#include "Netplay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gekkonet.h>

#include "HangWatchdog.h"
#include "Lobby.h"
#include "MenuSession.h"
#include "SimArena.h"
#include "SimState.h"
#include "NetLog.h"
#include "NetSession.h"
#include "RaceConfig.h"
#include "SoundRollback.h"
#include "TestScreenshots.h"
#include "port/Game.h"

extern "C" {
#include "main.h"
#include "menus.h"
#include "defines.h"
#include "sounds.h"
#include "audio/external.h"
}

namespace {

// Frames to wait after boot before a test starts, so the engine and menus finish initializing.
constexpr u32 kBootFrames = 30;
// After returning to the menus, let them initialize before loading a race.
constexpr u32 kMenuSettleFrames = 3;
// A shared console's last menu session keeps handing out input this long at most, once the race has loaded.
constexpr auto kMenuHandoverTimeout = std::chrono::seconds(10);
// The menus wait for every player's input each frame: their input delay covers the latency, plus a frame.
constexpr u8 kMaxMenuInputDelay = 6;
constexpr f32 kFrameMs = 1000.0f / 30.0f;
// Lobby tests watch this much of the podium after a Grand Prix: 15 s.
constexpr u32 kTestPodiumFrames = 450;
constexpr s32 kNumControllers = 4;
// SK_TEST_INPUT holds each button press this many frames, so the menus see it whatever frame they check on.
constexpr u32 kTestPressFrames = 2;
constexpr u32 kTestLingerFrames = 60;
// A shared console test runs the menus this many frames after its races.
constexpr u32 kTestMenuFramesAfterRaces = 150;

/** A button SK_TEST_INPUT presses, and the game frame it presses it on. */
struct TestPress {
    u32 Frame;
    u16 Button;
    u32 FramesHeld = 0; // Each press happens once, even when a new console restarts the count
};

enum class TestMode {
    None,   // Races come from the lobby window
    Direct, // SK_NETPLAY_PEERS: peers connect directly, without a relay or lobby
    Lobby,  // SK_NETPLAY_RELAY: bots meet in a room on a relay and race
};

struct NetplayState {
    TestMode Test = TestMode::None;
    u32 BootFramesLeft = kBootFrames;
    bool Finished = false;
    // A lobby test stays in the room this long after it finishes, so peers still at the end of the race hear of it.
    u32 LingerFrames = 0;
    s32 ExitCode = EXIT_SUCCESS;

    // Options for both test modes
    bool UseBot = false;
    SyntheticDriver::Style BotStyle = SyntheticDriver::Style::Racer;
    u32 TotalFrames = 0;
    u32 DelayMs = 0;
    u32 JitterMs = 0;
    u32 LossPercent = 0;
    u32 DesyncAtFrame = 0;
    std::string LogPath;

    // Direct test
    bool MenuTest = false; // SK_NETPLAY_MENUS: run the menus in lockstep instead of a race
    u32 MenuTestStartFrame = kBootFrames; // SK_NETPLAY_TEST_MENUS_AT
    bool MenuTestStarted = false;
    std::vector<std::string> PeerAddresses;
    s32 LocalPlayer = 0;
    RaceConfig Config;
    GekkoNetAdapter* DirectSocket = nullptr;

    std::vector<TestControl> TestControls;
    u32 TestSeriesRaces = 0;

    // Lobby test
    std::string RelayAddress;
    bool HostRoom = false;
    bool TestConsole = false; // SK_NETPLAY_CONSOLE: the room plays on a shared console
    u32 TestJoinAt = 0;       // SK_NETPLAY_TEST_JOIN_AT: a guest joins on this game frame
    std::vector<u32> TestConsoleSizes; // SK_NETPLAY_TEST_CONSOLE_PLAYERS: consoles to see, in order, before passing
    size_t TestConsolesSeen = 0;
    bool LeftOnPurpose = false; // Left the room with Z on a console's Game Select
    std::string RoomCode;
    std::string RoomFile;
    u32 Players = 2;
    u32 Races = 1;
    u32 RacesStarted = 0;
    u32 RacesEnded = 0;
    bool JoinedRoom = false;
    Lobby::Settings TestSettings;

    u32 PodiumFrames = 0;
    std::vector<TestPress> TestInput;        // SK_TEST_INPUT
    std::vector<TestPress> TestConsoleInput; // SK_TEST_CONSOLE_INPUT
    bool LocalZHeld = false;          // On a console's Game Select, pressing Z leaves the room
    bool ReturnToOnlineMenu = false;  // After leaving a room, the menus reset to the ONLINE screen
    u32 GameFrames = 0;               // Game frames run since boot
    u32 ConsoleStartFrame = 0;        // SK_TEST_CONSOLE_INPUT counts from the latest shared console's start
    bool TestResetBeforeRace = false;
    u32 TestStartDelay = 0; // Host: frames to idle in the room before starting
    u32 TestMenuAtFrame = 0; // Open the settings menu, as Escape does, on this shown frame of the first race
    u32 TestHangAtFrame = 0; // Freeze the game thread for a while on this shown frame of the first race
    u32 TestSoundRollbackAt = 0; // Run SoundRollback::SelfTest() on this shown frame of the first race
    u32 TestEnginesAt = 0;       // Check that every other player's engine is heard on this shown frame of the first race

    // Lobby races
    std::optional<Lobby::RaceStart> PendingRace;
    u32 RunningRaceId = 0;
    bool ResetRequested = false;
    u32 MenuFrames = 0;
    bool SessionWasActive = false;
};

NetplayState sNetplay;

/** The shared console this peer plays on (Lobby::ConsoleStart): its menus, then its races, one session at a time. */
struct ConsoleState {
    bool Active = false;
    Lobby::ConsoleStart Start;
    u32 Segment = 0; // The current or last session's; menu and race sessions share the sequence
    // Races set up before the menu session started; a menu session that follows a race starts while it fades out.
    u32 RaceSetupsAtMenus = 0;
    GameplayCVarOverride CVars;
    bool AfterPodium = false; // The menus restart from Game Select once the podium is over
    // The menus loaded a race: their session hands out input until every peer has run the same last menu frame.
    bool HandoverPending = false;
    u32 HandoverSegment = 0;
    s32 HandoverFrame = 0;
    std::chrono::steady_clock::time_point HandoverStarted;
};

ConsoleState sConsole;

const char* GetEnv(const char* name) {
    const char* value = getenv(name);
    return ((value != nullptr) && (value[0] != '\0')) ? value : nullptr;
}

u32 GetEnvU32(const char* name, u32 fallback) {
    const char* value = GetEnv(name);
    return (value != nullptr) ? (u32) strtoul(value, nullptr, 10) : fallback;
}

void FinishTest(bool success, const std::string& message) {
    if (!message.empty()) {
        NetLog("%s: %s", success ? "PASS" : "FAIL", message.c_str());
    }
    fflush(stdout);
    sNetplay.ExitCode = success ? EXIT_SUCCESS : EXIT_FAILURE;
    sNetplay.Finished = true;
    sNetplay.LingerFrames = (success && (sNetplay.Test == TestMode::Lobby)) ? kTestLingerFrames : 0;
}

bool IsInMenus() {
    const bool menu = (gGamestate == START_MENU_FROM_QUIT) || (gGamestate == MAIN_MENU_FROM_QUIT) ||
                      (gGamestate == PLAYER_SELECT_MENU_FROM_QUIT) || (gGamestate == COURSE_SELECT_MENU_FROM_QUIT) ||
                      (gGamestate == COURSE_DATA_MENU);
    return menu && (gGamestateNext == gGamestate);
}

void ApplyTestOptions(SessionSettings* settings, u32 botSeed) {
    if (sNetplay.UseBot) {
        settings->BotSeed = botSeed;
        settings->BotStyle = sNetplay.BotStyle;
    }
    settings->TotalFrames = sNetplay.TotalFrames;
    settings->DelayMs = sNetplay.DelayMs;
    settings->JitterMs = sNetplay.JitterMs;
    settings->LossPercent = sNetplay.LossPercent;
    settings->DesyncAtFrame = sNetplay.DesyncAtFrame;
    settings->LogPath = sNetplay.LogPath;
    settings->TestControls = sNetplay.TestControls;
    settings->LeaveAfterRaces = sNetplay.TestSeriesRaces;
    settings->DumpSegmentStarts = GetEnvU32("SK_NETPLAY_DUMP", 0) != 0;
    settings->PressLAtFrame = GetEnvU32("SK_NETPLAY_TEST_PRESS_L_AT", 0);
}

/** Parses "restart@300,next@200,leave@500": a control and the frame of each segment to send it on. */
std::vector<TestControl> ParseTestControls(const char* text) {
    std::vector<TestControl> controls;
    std::string list = text;
    size_t start = 0;
    while (start < list.size()) {
        const size_t comma = std::min(list.find(',', start), list.size());
        const std::string item = list.substr(start, comma - start);
        const size_t at = item.find('@');
        const std::string name = item.substr(0, at);
        const u32 frame = (at == std::string::npos) ? 0 : (u32) strtoul(item.c_str() + at + 1, nullptr, 10);
        if (name == "leave") {
            controls.push_back({ frame, PadControl::Leave });
        } else if (name == "restart") {
            controls.push_back({ frame, PadControl::Restart });
        } else if (name == "next") {
            controls.push_back({ frame, PadControl::NextCourse });
        }
        start = comma + 1;
    }
    return controls;
}

/** Parses "Z@60,A@120,R_JPAD@150": a button and the game frame to press it on. */
std::vector<TestPress> ParseTestInput(const char* text) {
    static const std::pair<const char*, u16> kButtons[] = {
        { "A", A_BUTTON },   { "B", B_BUTTON },        { "Z", Z_TRIG },          { "START", START_BUTTON },
        { "L", L_TRIG },     { "R", R_TRIG },          { "U_JPAD", U_JPAD },     { "D_JPAD", D_JPAD },
        { "L_JPAD", L_JPAD }, { "R_JPAD", R_JPAD },    { "U_CBUTTONS", U_CBUTTONS }, { "D_CBUTTONS", D_CBUTTONS },
        { "L_CBUTTONS", L_CBUTTONS }, { "R_CBUTTONS", R_CBUTTONS },
    };
    std::vector<TestPress> presses;
    std::string list = text;
    size_t start = 0;
    while (start < list.size()) {
        const size_t comma = std::min(list.find(',', start), list.size());
        const std::string item = list.substr(start, comma - start);
        const size_t at = item.find('@');
        const std::string name = item.substr(0, at);
        const u32 frame = (at == std::string::npos) ? 0 : (u32) strtoul(item.c_str() + at + 1, nullptr, 10);
        for (const auto& [buttonName, button] : kButtons) {
            if (name == buttonName) {
                presses.push_back({ frame, button });
            }
        }
        start = comma + 1;
    }
    return presses;
}

void ApplyTestPresses(std::vector<TestPress>& presses, u32 frame, OSContPad* pad) {
    for (TestPress& press : presses) {
        if ((frame >= press.Frame) && (press.FramesHeld < kTestPressFrames)) {
            pad->button |= press.Button;
            press.FramesHeld++;
        }
    }
}

/** Adds the buttons SK_TEST_INPUT and SK_TEST_CONSOLE_INPUT hold on this game frame to the local pad. */
void ApplyTestInput(OSContPad* pad) {
    ApplyTestPresses(sNetplay.TestInput, sNetplay.GameFrames, pad);
    if (sConsole.Active) {
        ApplyTestPresses(sNetplay.TestConsoleInput, sNetplay.GameFrames - sNetplay.ConsoleStartFrame, pad);
    }
}

void OnSessionEnded();
void OnConsoleRaceEnded(SessionResult result);

bool OpenDirectSocket() {
    const std::string& ownAddress = sNetplay.PeerAddresses[sNetplay.LocalPlayer];
    const unsigned short port = (unsigned short) atoi(ownAddress.c_str() + ownAddress.rfind(':') + 1);
    try {
        sNetplay.DirectSocket = gekko_default_adapter(port);
    } catch (const std::exception& e) {
        FinishTest(false, "Could not open UDP port " + std::to_string(port) + ": " + e.what());
        return false;
    }
    return true;
}

/** SK_NETPLAY_MENUS: the direct peers run the menus in lockstep, driven by SK_TEST_INPUT. */
void StartDirectMenus() {
    if (!OpenDirectSocket()) {
        return;
    }
    MenuSessionSettings settings;
    settings.LocalPlayer = sNetplay.LocalPlayer;
    for (const std::string& address : sNetplay.PeerAddresses) {
        settings.Addresses.emplace_back(address.begin(), address.end());
    }
    settings.GetAdapter = []() { return sNetplay.DirectSocket; };
    settings.LogPath = sNetplay.LogPath;
    MenuSession::Start(settings);
}

void StartDirectRace() {
    if (!OpenDirectSocket()) {
        return;
    }

    SessionSettings settings;
    settings.LocalPlayer = sNetplay.LocalPlayer;
    for (const std::string& address : sNetplay.PeerAddresses) {
        settings.Addresses.emplace_back(address.begin(), address.end());
    }
    settings.GetAdapter = [](u32) { return sNetplay.DirectSocket; };
    // Direct peers run on one machine; their only latency is the simulated one.
    settings.GetLatencyMs = []() { return (s32) (sNetplay.DelayMs + sNetplay.JitterMs / 2); };
    settings.Config = sNetplay.Config;
    ApplyTestOptions(&settings, (sNetplay.Config.RandomSeed + 1) * 0x9E3779B9u * (u32) (sNetplay.LocalPlayer + 1));
    NetSession::Start(settings);
    if (!NetSession::IsActive()) {
        OnSessionEnded();
    }
}

void StartLobbyRace(const Lobby::RaceStart& start) {
    SessionSettings settings;
    settings.LocalPlayer = start.LocalPlayer;
    for (u8 slot : start.Slots) {
        settings.Addresses.push_back({ slot });
    }
    const u32 raceId = start.Id;
    settings.GetAdapter = [raceId](u32 segment) { return Lobby::GetGekkoAdapter(raceId, segment); };
    settings.ReachBarrier = [raceId](u32 segment, s32 frame) { Lobby::ReachBarrier(raceId, segment, frame); };
    settings.GetBarrierStatus = [raceId](u32 segment, s32 frame) {
        return Lobby::GetBarrierStatus(raceId, segment, frame);
    };
    settings.IsRelayed = [slots = start.Slots](s32 player) { return !Lobby::IsDirect(slots[player]); };
    settings.GetLatencyMs = [slots = start.Slots]() {
        // Test peers add their simulated delay to every packet they receive.
        const s32 latency = Lobby::GetLatencyMs(slots);
        return (latency < 0) ? latency : latency + (s32) (sNetplay.DelayMs + sNetplay.JitterMs / 2);
    };
    settings.Config = start.Config;
    if (sNetplay.Test == TestMode::Lobby) {
        ApplyTestOptions(&settings, (start.Config.RandomSeed + 1) * 0x9E3779B9u * (u32) (start.LocalPlayer + 1));
        if (!settings.LogPath.empty()) {
            settings.LogPath += ".race" + std::to_string(sNetplay.RacesEnded + 1);
        }
    }
    sNetplay.RunningRaceId = start.Id;
    NetSession::Start(settings);
    if (!NetSession::IsActive()) {
        OnSessionEnded();
    }
}

void OnSessionEnded() {
    const SessionResult result = NetSession::GetResult();
    const std::string& message = NetSession::GetResultMessage();
    if (sConsole.Active) {
        Lobby::SetLastRaceMessage(message);
        OnConsoleRaceEnded(result);
        if (sNetplay.Test == TestMode::Lobby) {
            sNetplay.RacesEnded++;
            if (!IsSuccess(result)) {
                FinishTest(false, "");
            }
        }
        return;
    }
    if (sNetplay.Test == TestMode::Direct) {
        gekko_default_adapter_destroy();
        FinishTest(IsSuccess(result), "");
        return;
    }

    Lobby::ReportRaceEnded(sNetplay.RunningRaceId, result, message);
    Lobby::SetLastRaceMessage(message);
    sNetplay.RunningRaceId = 0;
    // A finished Grand Prix goes on to its podium, which each peer shows on its own. Otherwise everyone goes back
    // to the menus, where the room picks the next race.
    if (!NetSession::IsHeadingToPodium()) {
        CM_RequestReset();
    }

    if (sNetplay.Test == TestMode::Lobby) {
        sNetplay.RacesEnded++;
        if (!IsSuccess(result)) {
            FinishTest(false, "");
        } else if (sNetplay.RacesEnded >= sNetplay.Races) {
            FinishTest(true, "");
        }
    }
}

void HandleRaceEndedElsewhere() {
    const std::optional<Lobby::RaceEnd> end = Lobby::TakeRaceEndedElsewhere();
    if (!end.has_value()) {
        return;
    }
    if (NetSession::IsAtBarrier() && IsSuccess(end->Result)) {
        // A peer that confirmed the end of the race went ahead; this peer follows once it hears of it.
        return;
    }
    if (NetSession::IsActive() && (end->Id == sNetplay.RunningRaceId)) {
        // A peer that reached the end of a test leaves first.
        NetSession::Stop(NetSession::IsNearPlannedEnd() ? SessionResult::Completed : end->Result, end->Message);
    } else if (sNetplay.PendingRace.has_value() && (sNetplay.PendingRace->Id == end->Id)) {
        sNetplay.PendingRace.reset();
        sNetplay.ResetRequested = false;
        Lobby::ReportRaceEnded(end->Id, end->Result, end->Message);
        Lobby::SetLastRaceMessage(end->Message);
    }
}

/** Starts the race the lobby asked for, from the menus, where every peer starts from the same state. */
void UpdatePendingRace() {
    if (!sNetplay.PendingRace.has_value() && !NetSession::IsActive()) {
        sNetplay.PendingRace = Lobby::TakeRaceStart();
    }
    if (!sNetplay.PendingRace.has_value()) {
        return;
    }
    const bool testReset = sNetplay.TestResetBeforeRace && !sNetplay.ResetRequested;
    if (!IsInMenus() || testReset) {
        if (!sNetplay.ResetRequested) {
            NetLog("Returning to the menus to start race %u", sNetplay.PendingRace->Id);
            CM_RequestReset();
            sNetplay.ResetRequested = true;
        }
        sNetplay.MenuFrames = 0;
        return;
    }
    if (++sNetplay.MenuFrames < kMenuSettleFrames) {
        return;
    }
    const Lobby::RaceStart start = *sNetplay.PendingRace;
    sNetplay.PendingRace.reset();
    sNetplay.ResetRequested = false;
    StartLobbyRace(start);
}

void WriteRoomFile(const std::string& code) {
    // Written to a temporary file first, so that a guest polling for it never reads half a code.
    const std::string temporary = sNetplay.RoomFile + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        file << code;
    }
    std::rename(temporary.c_str(), sNetplay.RoomFile.c_str());
}

std::string ReadRoomFile() {
    std::ifstream file(sNetplay.RoomFile);
    std::string code;
    file >> code;
    return code;
}

size_t CountMembers() {
    size_t count = 0;
    for (const Lobby::Member& member : Lobby::GetMembers()) {
        count += member.Present ? 1 : 0;
    }
    return count;
}

/** @returns true once every member's relay ping has arrived, which the automatic input delay needs. */
bool AreAllPingsKnown() {
    for (const Lobby::Member& member : Lobby::GetMembers()) {
        if (member.Present && (member.PingMs < 0)) {
            return false;
        }
    }
    return true;
}

/** Drives the lobby like players would: the host opens a room and starts races, guests join and get ready. */
void UpdateLobbyTest() {
    if (sNetplay.Finished) {
        return;
    }
    const Lobby::State state = Lobby::GetState();
    if (!sNetplay.JoinedRoom) {
        if (state == Lobby::State::InRoom) {
            sNetplay.JoinedRoom = true;
            NetLog("In room %s as slot %d", Lobby::GetRoomCode().c_str(), Lobby::GetLocalSlot());
            fflush(stdout);
            if (Lobby::IsHost()) {
                Lobby::SetSettings(sNetplay.TestSettings);
                if (!sNetplay.RoomFile.empty()) {
                    WriteRoomFile(Lobby::GetRoomCode());
                }
            }
        } else if (state == Lobby::State::Offline) {
            if (!Lobby::GetError().empty()) {
                FinishTest(false, Lobby::GetError());
            } else if (sNetplay.HostRoom) {
                Lobby::Host(sNetplay.RelayAddress, "Host", sNetplay.TestConsole);
            } else if (sNetplay.GameFrames >= sNetplay.TestJoinAt) {
                const std::string code = sNetplay.RoomCode.empty() ? ReadRoomFile() : sNetplay.RoomCode;
                if (code.size() == RelayProtocol::kCodeLength) {
                    Lobby::Join(sNetplay.RelayAddress, code, "Guest");
                }
            }
        }
        return;
    }

    if (state != Lobby::State::InRoom) {
        if (sNetplay.LeftOnPurpose) {
            FinishTest(true, "");
        } else {
            FinishTest(false, "Left the room: " + Lobby::GetError());
        }
        return;
    }
    // The podium waits for a button press, which bots do not make.
    if (gGamestate == ENDING) {
        if (++sNetplay.PodiumFrames == kTestPodiumFrames) {
            NetLog("Leaving the podium");
            CM_RequestReset();
        }
    } else {
        sNetplay.PodiumFrames = 0;
    }
    if (sNetplay.TestConsole) {
        // The players drive the console's menus with SK_TEST_INPUT, and bots its races. Once the races are over, the
        // menus run a little longer, to check they stayed in sync.
        const bool consolesSeen = sNetplay.TestConsolesSeen == sNetplay.TestConsoleSizes.size();
        if ((sNetplay.RacesEnded >= sNetplay.Races) && consolesSeen && MenuSession::IsRunning() &&
            (MenuSession::GetFrames() >= kTestMenuFramesAfterRaces)) {
            NetLog("Test: menus in sync for %u frames after the races", MenuSession::GetFrames());
            FinishTest(true, "");
        }
        return;
    }
    const Lobby::Member& self = Lobby::GetMembers()[Lobby::GetLocalSlot()];
    const bool racing = NetSession::IsActive() || sNetplay.PendingRace.has_value();
    if (!Lobby::IsHost() && !self.Ready && !racing && IsInMenus()) {
        Lobby::SetReady(true);
    }
    if (sNetplay.TestStartDelay > 0) {
        sNetplay.TestStartDelay--;
        return;
    }
    if (Lobby::IsHost() && !racing && (sNetplay.RacesStarted < sNetplay.Races) &&
        (CountMembers() >= sNetplay.Players) && AreAllPingsKnown() && Lobby::GetStartBlocker().empty()) {
        sNetplay.RacesStarted++;
        NetLog("Starting race %u of %u", sNetplay.RacesStarted, sNetplay.Races);
        Lobby::StartRace();
    }
}

std::vector<std::vector<u8>> GetConsoleAddresses() {
    std::vector<std::vector<u8>> addresses;
    for (u8 slot : sConsole.Start.Slots) {
        addresses.push_back({ slot });
    }
    return addresses;
}

std::string GetConsoleLogPath(u32 segment) {
    if (sNetplay.LogPath.empty()) {
        return "";
    }
    return sNetplay.LogPath + ".console" + std::to_string(sConsole.Start.Id) + ".seg" + std::to_string(segment);
}

u8 ChooseMenuInputDelay() {
    const s32 latencyMs = Lobby::GetLatencyMs(sConsole.Start.Slots);
    if (latencyMs < 0) {
        return 2;
    }
    return (u8) std::min((s32) (latencyMs / kFrameMs) + 1, (s32) kMaxMenuInputDelay);
}

void StartConsoleMenus(u32 segment, bool resetToGameSelect) {
    sConsole.Segment = segment;
    MenuSessionSettings settings;
    settings.LocalPlayer = sConsole.Start.LocalPlayer;
    settings.Addresses = GetConsoleAddresses();
    const u32 id = sConsole.Start.Id;
    settings.GetAdapter = [id, segment]() { return Lobby::GetGekkoAdapter(id, segment); };
    settings.InputDelay = ChooseMenuInputDelay();
    settings.ResetToGameSelect = resetToGameSelect;
    settings.LogPath = GetConsoleLogPath(segment);
    // Races set up from these menus build in the sim arena, for rollback.
    SimState_SetRollbackMode(true);
    sConsole.RaceSetupsAtMenus = SimState_GetRaceSetupCount();
    MenuSession::Start(settings);
}

/** The menus have just loaded a race: its rollback session takes over before it runs a frame. */
void StartConsoleRace(u32 segment) {
    SessionSettings settings;
    settings.LocalPlayer = sConsole.Start.LocalPlayer;
    settings.Addresses = GetConsoleAddresses();
    const u32 id = sConsole.Start.Id;
    settings.GetAdapter = [id](u32 raceSegment) { return Lobby::GetGekkoAdapter(id, raceSegment); };
    settings.ReachBarrier = [id](u32 raceSegment, s32 frame) { Lobby::ReachBarrier(id, raceSegment, frame); };
    settings.GetBarrierStatus = [id](u32 raceSegment, s32 frame) {
        return Lobby::GetBarrierStatus(id, raceSegment, frame);
    };
    settings.IsRelayed = [slots = sConsole.Start.Slots](s32 player) { return !Lobby::IsDirect(slots[player]); };
    settings.GetLatencyMs = [slots = sConsole.Start.Slots]() {
        const s32 latency = Lobby::GetLatencyMs(slots);
        return (latency < 0) ? latency : latency + (s32) (sNetplay.DelayMs + sNetplay.JitterMs / 2);
    };
    settings.Config = sConsole.Start.Rules;
    settings.RaceSetUpByMenus = true;
    settings.FirstSegment = segment;
    if (sNetplay.Test == TestMode::Lobby) {
        ApplyTestOptions(&settings, (sConsole.Start.Id + segment) * 0x9E3779B9u * (u32) (settings.LocalPlayer + 1));
        if (!settings.LogPath.empty()) {
            // Named like a lobby race's, for tools/netplay-sim.py; segments number on through the console.
            settings.LogPath += ".race" + std::to_string(sConsole.Start.Id);
        }
    }
    sConsole.Segment = segment;
    NetSession::Start(settings);
}

void StartConsole(const Lobby::ConsoleStart& start) {
    MenuSession::Stop("A new console started");
    NetSession::Stop(SessionResult::PeerLeft, "A new console started");
    sConsole.Start = start;
    sConsole.Active = true;
    sConsole.AfterPodium = false;
    sConsole.HandoverPending = false;
    sConsole.CVars.Apply(start.Rules);
    const std::vector<u32>& sizes = sNetplay.TestConsoleSizes;
    if ((sNetplay.TestConsolesSeen < sizes.size()) && (start.Slots.size() == sizes[sNetplay.TestConsolesSeen])) {
        sNetplay.TestConsolesSeen++;
    }
    // Each player's console test input says what to do on any console, from its start.
    sNetplay.ConsoleStartFrame = sNetplay.GameFrames;
    for (TestPress& press : sNetplay.TestConsoleInput) {
        press.FramesHeld = 0;
    }
    NetLog("Console %u: player %d of %zu", start.Id, start.LocalPlayer + 1, start.Slots.size());
    StartConsoleMenus(1, true);
}

void EndConsole() {
    if (!sConsole.Active) {
        return;
    }
    NetLog("Console %u ended", sConsole.Start.Id);
    MenuSession::Stop("Left the console");
    NetSession::Stop(SessionResult::PeerLeft, "Left the console");
    sConsole.CVars.Restore();
    sConsole.Active = false;
    SimState_SetRollbackMode(false);
    sNetplay.ReturnToOnlineMenu = true;
    CM_RequestReset();
}

/** A console race series ended: the menus go on from where it left them, the same on every peer. */
void OnConsoleRaceEnded(SessionResult result) {
    const u32 next = NetSession::GetSegment() + 1;
    if (NetSession::IsHeadingToPodium()) {
        sConsole.AfterPodium = true;
        sConsole.Segment = next;
    } else if (IsSuccess(result)) {
        StartConsoleMenus(next, false);
    } else {
        // A player left or the games went out of sync: everyone starts over from Game Select. If a player left, the
        // host starts a new console without it.
        StartConsoleMenus(next, true);
    }
}

bool IsMenuGamestate(s32 gamestate) {
    return (gamestate == START_MENU_FROM_QUIT) || (gamestate == MAIN_MENU_FROM_QUIT) ||
           (gamestate == PLAYER_SELECT_MENU_FROM_QUIT) || (gamestate == COURSE_SELECT_MENU_FROM_QUIT);
}

bool IsOnGameSelect() {
    return IsMenuGamestate(gGamestate) && (gMenuSelection == MAIN_MENU) &&
           (gMainMenuSelection == MAIN_MENU_PLAYER_SELECT);
}

/** Starts, hands over and ends the shared console's sessions. Runs before the sessions' own frame updates. */
void UpdateConsole() {
    if (std::optional<Lobby::ConsoleStart> start = Lobby::TakeConsoleStart()) {
        StartConsole(*start);
    }
    if (sConsole.Active && !Lobby::IsSharedConsole()) {
        EndConsole();
    }
    if (!sConsole.Active) {
        return;
    }
    if (MenuSession::IsRunning() && !MenuSession::IsFrozen() && (gGamestate == RACING) &&
        (SimState_GetRaceSetupCount() != sConsole.RaceSetupsAtMenus)) {
        sConsole.HandoverPending = true;
        sConsole.HandoverSegment = sConsole.Segment;
        sConsole.HandoverFrame = (s32) MenuSession::GetFrames();
        sConsole.HandoverStarted = std::chrono::steady_clock::now();
        MenuSession::Freeze();
        Lobby::ReachBarrier(sConsole.Start.Id, sConsole.HandoverSegment, sConsole.HandoverFrame);
        StartConsoleRace(sConsole.Segment + 1);
    }
    if (sConsole.HandoverPending) {
        const BarrierStatus status =
            Lobby::GetBarrierStatus(sConsole.Start.Id, sConsole.HandoverSegment, sConsole.HandoverFrame);
        if ((status != BarrierStatus::Waiting) ||
            (std::chrono::steady_clock::now() - sConsole.HandoverStarted > kMenuHandoverTimeout)) {
            sConsole.HandoverPending = false;
            MenuSession::Stop("Every player is racing");
        }
    }
    if (sConsole.AfterPodium && IsMenuGamestate(gGamestate)) {
        sConsole.AfterPodium = false;
        StartConsoleMenus(sConsole.Segment, true);
    }
    Lobby::SetRacing(NetSession::IsActive());
}

void InitDirectTest(const char* peers) {
    std::string list = peers;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const size_t end = (comma == std::string::npos) ? list.size() : comma;
        sNetplay.PeerAddresses.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    sNetplay.LocalPlayer = (s32) GetEnvU32("SK_NETPLAY_PLAYER", 0);
    if ((sNetplay.PeerAddresses.size() < 2) || (sNetplay.PeerAddresses.size() > Lobby::kMaxMembers) ||
        (sNetplay.LocalPlayer >= (s32) sNetplay.PeerAddresses.size())) {
        FinishTest(false, "SK_NETPLAY_PEERS needs 2-4 addresses and SK_NETPLAY_PLAYER must index one");
        return;
    }
    sNetplay.Config = RaceConfig::FromEnvironment("SK_NETPLAY_", (s8) sNetplay.PeerAddresses.size());
    sNetplay.MenuTest = GetEnvU32("SK_NETPLAY_MENUS", 0) != 0;
    sNetplay.MenuTestStartFrame = std::max(GetEnvU32("SK_NETPLAY_TEST_MENUS_AT", kBootFrames), kBootFrames);
    sNetplay.Test = TestMode::Direct;
}

void InitLobbyTest(const char* relay) {
    sNetplay.RelayAddress = relay;
    sNetplay.HostRoom = GetEnvU32("SK_NETPLAY_HOST", 0) != 0;
    if (const char* code = GetEnv("SK_NETPLAY_ROOM")) {
        sNetplay.RoomCode = code;
    }
    if (const char* file = GetEnv("SK_NETPLAY_ROOM_FILE")) {
        sNetplay.RoomFile = file;
    }
    if (!sNetplay.HostRoom && sNetplay.RoomCode.empty() && sNetplay.RoomFile.empty()) {
        FinishTest(false, "A guest needs SK_NETPLAY_ROOM or SK_NETPLAY_ROOM_FILE");
        return;
    }
    sNetplay.TestConsole = GetEnvU32("SK_NETPLAY_CONSOLE", 0) != 0;
    sNetplay.TestJoinAt = GetEnvU32("SK_NETPLAY_TEST_JOIN_AT", 0);
    if (const char* sizes = GetEnv("SK_NETPLAY_TEST_CONSOLE_PLAYERS")) {
        for (const char* size = sizes; *size != 0; size++) {
            if (isdigit((unsigned char) *size)) {
                sNetplay.TestConsoleSizes.push_back((u32) (*size - '0'));
            }
        }
    }
    sNetplay.Players = GetEnvU32("SK_NETPLAY_PLAYERS", 2);
    sNetplay.Races = GetEnvU32("SK_NETPLAY_RACES", 1);
    if (const u32 blockDirectMs = GetEnvU32("SK_NETPLAY_TEST_BLOCK_DIRECT_MS", 0)) {
        Lobby::BlockDirectAfter(blockDirectMs);
    }
    const RaceConfig config = RaceConfig::FromEnvironment("SK_NETPLAY_", (s8) sNetplay.Players);
    sNetplay.TestSettings.Mode = config.ModeSelection;
    sNetplay.TestSettings.TrackName = config.TrackName;
    sNetplay.TestSettings.CupIndex = (s32) GetEnvU32("SK_NETPLAY_CUP", 0);
    sNetplay.Test = TestMode::Lobby;
}

} // namespace

extern "C" void Netplay_Init(void) {
    HangWatchdog::Start();
    SimState_CaptureBootState();
    TestScreenshots::Init();
    if (const char* input = GetEnv("SK_TEST_INPUT")) {
        sNetplay.TestInput = ParseTestInput(input);
    }
    if (const char* input = GetEnv("SK_TEST_CONSOLE_INPUT")) {
        sNetplay.TestConsoleInput = ParseTestInput(input);
    }
    if (const char* logPath = GetEnv("SK_NETPLAY_LOG")) {
        sNetplay.LogPath = logPath;
    }
    if (const char* peers = GetEnv("SK_NETPLAY_PEERS")) {
        InitDirectTest(peers);
    } else if (const char* relay = GetEnv("SK_NETPLAY_RELAY")) {
        InitLobbyTest(relay);
    }
    if (sNetplay.Test == TestMode::None) {
        return;
    }
    const u32 bot = GetEnvU32("SK_NETPLAY_BOT", 0);
    sNetplay.UseBot = bot != 0;
    sNetplay.BotStyle = (bot == 2) ? SyntheticDriver::Style::Random : SyntheticDriver::Style::Racer;
    sNetplay.TotalFrames = GetEnvU32("SK_NETPLAY_FRAMES", 0);
    sNetplay.DelayMs = GetEnvU32("SK_NETPLAY_DELAY_MS", 0);
    sNetplay.JitterMs = GetEnvU32("SK_NETPLAY_JITTER_MS", 0);
    sNetplay.LossPercent = GetEnvU32("SK_NETPLAY_LOSS", 0);
    sNetplay.DesyncAtFrame = GetEnvU32("SK_NETPLAY_DESYNC_AT", 0);
    sNetplay.TestResetBeforeRace = GetEnvU32("SK_NETPLAY_TEST_RESET", 0) != 0;
    sNetplay.TestStartDelay = GetEnvU32("SK_NETPLAY_TEST_START_DELAY", 0);
    sNetplay.TestMenuAtFrame = GetEnvU32("SK_NETPLAY_TEST_MENU_AT", 0);
    sNetplay.TestHangAtFrame = GetEnvU32("SK_NETPLAY_TEST_HANG_AT", 0);
    sNetplay.TestSoundRollbackAt = GetEnvU32("SK_NETPLAY_TEST_SOUND_ROLLBACK_AT", 0);
    sNetplay.TestEnginesAt = GetEnvU32("SK_NETPLAY_TEST_ENGINES_AT", 0);
    sNetplay.TestSeriesRaces = GetEnvU32("SK_NETPLAY_TEST_SERIES_RACES", 0);
    if (const char* controls = GetEnv("SK_NETPLAY_TEST_CONTROLS")) {
        sNetplay.TestControls = ParseTestControls(controls);
    }
}

namespace {

/**
 * @returns the first other player whose kart engine is not playing as heard from this player's camera
 * (func_800C94A4() starts it from where the kart is), or -1.
 */
s32 FindSilentEngine() {
    for (s32 player = 0; player < gPlayerCount; player++) {
        if (player == NetSession::GetLocalPlayer()) {
            continue;
        }
        const u32 engine = gPlayers[player].characterId + SOUND_ARG_LOAD(0x31, 0x02, 0x80, 0x00);
        Vec3f* slot = func_800C21E8(gPlayers[player].pos, engine);
        if ((slot == nullptr) || !is_sound_active(slot, engine)) {
            return player;
        }
    }
    return -1;
}

} // namespace

extern "C" void Netplay_BeginFrame(void) {
    TestScreenshots::BeginFrame();
    Lobby::Update();
    HandleRaceEndedElsewhere();
    UpdateConsole();
    NetSession::BeginFrame();
    if (MenuSession::IsActive()) {
        OSContPad pads[kNumControllers];
        osContGetReadData(pads);
        ApplyTestInput(&pads[0]);
        // On Game Select, Z leaves the room: this player's own choice, never sent to the others.
        const bool zPressed = ((pads[0].button & Z_TRIG) != 0) && !sNetplay.LocalZHeld;
        sNetplay.LocalZHeld = (pads[0].button & Z_TRIG) != 0;
        if (sConsole.Active && IsOnGameSelect()) {
            pads[0].button &= ~Z_TRIG;
            if (zPressed) {
                NetLog("Leaving the room");
                play_sound2(SOUND_MENU_GO_BACK);
                sNetplay.LeftOnPurpose = true;
                Lobby::Leave();
            }
        }
        MenuSession::BeginFrame(pads[0]);
        if (!MenuSession::IsActive() && (sNetplay.Test == TestMode::Direct)) {
            FinishTest(false, "");
        }
    }
    HangWatchdog::Beat((Lobby::GetState() != Lobby::State::Offline) || NetSession::IsActive());
    if ((sNetplay.TestHangAtFrame > 0) && (NetSession::GetShownFrames() == sNetplay.TestHangAtFrame)) {
        sNetplay.TestHangAtFrame = 0;
        NetLog("Test: freezing the game thread");
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    // Settings are not simulation state: one created by the simulation (pressing L sets the music volume) must not
    // live in the sim arena, where a rollback frees it from under the settings table.
    if (sNetplay.Test != TestMode::None) {
        auto musicVolume = Ship::Context::GetRawInstance()->GetConsoleVariables()->Get("gMainMusicVolume");
        if ((musicVolume != nullptr) && SimArena::Contains(musicVolume.get())) {
            FinishTest(false, "A setting was allocated in the sim arena");
        }
    }
    if ((sNetplay.TestSoundRollbackAt > 0) && (NetSession::GetShownFrames() == sNetplay.TestSoundRollbackAt)) {
        sNetplay.TestSoundRollbackAt = 0;
        const std::string failure = SoundRollback::SelfTest();
        NetLog("Test: sound rollback self-test %s%s", failure.empty() ? "passed" : "failed: ", failure.c_str());
        if (!failure.empty()) {
            FinishTest(false, "Sound rollback self-test failed: " + failure);
        }
    }
    if ((sNetplay.TestEnginesAt > 0) && (NetSession::GetShownFrames() == sNetplay.TestEnginesAt)) {
        sNetplay.TestEnginesAt = 0;
        const s32 silent = FindSilentEngine();
        if (silent < 0) {
            NetLog("Test: the other players' engines are heard");
        } else {
            FinishTest(false, "The engine of player " + std::to_string(silent + 1) + " is not heard");
        }
    }
    if ((sNetplay.TestMenuAtFrame > 0) && (NetSession::GetShownFrames() == sNetplay.TestMenuAtFrame)) {
        sNetplay.TestMenuAtFrame = 0;
        NetLog("Test: opening the settings menu");
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetMenu()->ToggleVisibility();
    }
}

extern "C" bool Netplay_IsInRace(void) {
    return NetSession::IsInRace();
}

extern "C" bool Netplay_IsWaitingForMenuInput(void) {
    return MenuSession::IsWaiting();
}

extern "C" s32 Netplay_GetExtraMode(s32 unlockedInSave) {
    return sConsole.Active ? (sConsole.Start.ExtraMode ? 1 : 0) : unlockedInSave;
}

extern "C" bool Netplay_IsSharedConsole(void) {
    return sConsole.Active;
}

extern "C" s32 Netplay_GetResetMenu(s32 menu) {
    if (sConsole.Active || MenuSession::IsActive()) {
        return MAIN_MENU;
    }
    if (sNetplay.ReturnToOnlineMenu || (Lobby::GetState() != Lobby::State::Offline)) {
        sNetplay.ReturnToOnlineMenu = false;
        return ONLINE_MENU;
    }
    return menu;
}

extern "C" bool Netplay_ConsumeLocalPads(OSContPad* pads) {
    if (NetSession::ConsumeLocalPads(pads)) {
        return true;
    }
    if (MenuSession::TakePads(pads)) {
        for (s32 i = 0; i < kNumControllers; i++) {
            apply_pad_to_controller(&gControllers[i], &pads[i]);
        }
        combine_controllers();
        return true;
    }
    if (!sNetplay.TestInput.empty() || !sNetplay.TestConsoleInput.empty()) {
        // Applied here: the game skips controller 1 when no gamepad is connected, as in unattended tests.
        ApplyTestInput(&pads[0]);
        for (s32 i = 0; i < kNumControllers; i++) {
            apply_pad_to_controller(&gControllers[i], &pads[i]);
        }
        combine_controllers();
        return true;
    }
    return false;
}

extern "C" void Netplay_RunRaceFrame(void) {
    NetSession::RunRaceFrame();
}

extern "C" void Netplay_EndFrame(void) {
    NetSession::EndFrame();
    MenuSession::EndFrame();
    TestScreenshots::EndFrame(sNetplay.GameFrames);
    sNetplay.GameFrames++;
    if (sNetplay.MenuTest && MenuSession::IsRunning() && (sNetplay.TotalFrames > 0) &&
        (MenuSession::GetFrames() >= sNetplay.TotalFrames)) {
        MenuSession::Stop("Test complete");
        FinishTest(true, "");
    }
    if (sNetplay.SessionWasActive && !NetSession::IsActive()) {
        OnSessionEnded();
    }

    if (sNetplay.BootFramesLeft > 0) {
        // Start the direct test after this frame's menu update, which would otherwise overwrite the track selection.
        if ((--sNetplay.BootFramesLeft == 0) && (sNetplay.Test == TestMode::Direct) && !sNetplay.Finished) {
            if (!sNetplay.MenuTest) {
                StartDirectRace();
            }
        }
    } else {
        if (sNetplay.MenuTest && !sNetplay.MenuTestStarted && (sNetplay.GameFrames >= sNetplay.MenuTestStartFrame)) {
            sNetplay.MenuTestStarted = true;
            StartDirectMenus();
        }
        if (sNetplay.Test == TestMode::Lobby) {
            UpdateLobbyTest();
        }
        if (!sNetplay.Finished) {
            UpdatePendingRace();
        }
    }
    if (sNetplay.Finished) {
        if (sNetplay.LingerFrames > 0) {
            sNetplay.LingerFrames--;
        } else {
            Lobby::Leave();
        }
    }
    sNetplay.SessionWasActive = NetSession::IsActive();
}

extern "C" bool Netplay_IsFinished(void) {
    return sNetplay.Finished && (sNetplay.LingerFrames == 0);
}

extern "C" s32 Netplay_GetExitCode(void) {
    return sNetplay.ExitCode;
}

extern "C" void Netplay_Shutdown(void) {
    NetSession::Stop(SessionResult::PeerLeft, "The game closed");
    MenuSession::Stop("The game closed");
    sConsole.CVars.Restore();
    Lobby::Leave();
    HangWatchdog::Stop();
}

namespace Netplay {

void LeaveRace() {
    NetSession::RequestLeave();
}

void RestartRace() {
    NetSession::RequestRestart();
}

bool CanRestartRace() {
    return NetSession::CanRestart();
}

s32 GetConsolePlayer(u8 slot) {
    if (!sConsole.Active) {
        return -1;
    }
    const std::vector<u8>& slots = sConsole.Start.Slots;
    const auto it = std::find(slots.begin(), slots.end(), slot);
    return (it == slots.end()) ? -1 : (s32) (it - slots.begin());
}

bool IsRaceRunning() {
    return NetSession::IsActive() || sNetplay.PendingRace.has_value();
}

} // namespace Netplay
