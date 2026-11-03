#include "Netplay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gekkonet.h>

#include "Lobby.h"
#include "NetLog.h"
#include "NetSession.h"
#include "RaceConfig.h"
#include "port/Game.h"

extern "C" {
#include "main.h"
#include "defines.h"
}

namespace {

// Frames to wait after boot before a test starts, so the engine and menus finish initializing.
constexpr u32 kBootFrames = 30;
// After returning to the menus, let them initialize before loading a race.
constexpr u32 kMenuSettleFrames = 3;
// Lobby tests watch this much of the podium after a Grand Prix: 15 s.
constexpr u32 kTestPodiumFrames = 450;
constexpr u32 kTestLingerFrames = 60;

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
    u32 TotalFrames = 0;
    u32 DelayMs = 0;
    u32 JitterMs = 0;
    u32 LossPercent = 0;
    u32 DesyncAtFrame = 0;
    std::string LogPath;

    // Direct test
    std::vector<std::string> PeerAddresses;
    s32 LocalPlayer = 0;
    RaceConfig Config;
    GekkoNetAdapter* DirectSocket = nullptr;

    std::vector<TestControl> TestControls;

    // Lobby test
    std::string RelayAddress;
    bool HostRoom = false;
    std::string RoomCode;
    std::string RoomFile;
    u32 Players = 2;
    u32 Races = 1;
    u32 RacesStarted = 0;
    u32 RacesEnded = 0;
    bool JoinedRoom = false;
    Lobby::Settings TestSettings;

    u32 PodiumFrames = 0;
    bool TestResetBeforeRace = false;
    u32 TestStartDelay = 0; // Host: frames to idle in the room before starting

    // Lobby races
    std::optional<Lobby::RaceStart> PendingRace;
    u32 RunningRaceId = 0;
    bool ResetRequested = false;
    u32 MenuFrames = 0;
    bool SessionWasActive = false;
};

NetplayState sNetplay;

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
    }
    settings->TotalFrames = sNetplay.TotalFrames;
    settings->DelayMs = sNetplay.DelayMs;
    settings->JitterMs = sNetplay.JitterMs;
    settings->LossPercent = sNetplay.LossPercent;
    settings->DesyncAtFrame = sNetplay.DesyncAtFrame;
    settings->LogPath = sNetplay.LogPath;
    settings->TestControls = sNetplay.TestControls;
    settings->DumpSegmentStarts = GetEnvU32("SK_NETPLAY_DUMP", 0) != 0;
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

void OnSessionEnded();

void StartDirectRace() {
    const std::string& ownAddress = sNetplay.PeerAddresses[sNetplay.LocalPlayer];
    const unsigned short port = (unsigned short) atoi(ownAddress.c_str() + ownAddress.rfind(':') + 1);
    try {
        sNetplay.DirectSocket = gekko_default_adapter(port);
    } catch (const std::exception& e) {
        FinishTest(false, "Could not open UDP port " + std::to_string(port) + ": " + e.what());
        return;
    }

    SessionSettings settings;
    settings.LocalPlayer = sNetplay.LocalPlayer;
    for (const std::string& address : sNetplay.PeerAddresses) {
        settings.Addresses.emplace_back(address.begin(), address.end());
    }
    settings.GetAdapter = [](u32) { return sNetplay.DirectSocket; };
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
                Lobby::Host(sNetplay.RelayAddress, "Host");
            } else {
                const std::string code = sNetplay.RoomCode.empty() ? ReadRoomFile() : sNetplay.RoomCode;
                if (code.size() == RelayProtocol::kCodeLength) {
                    Lobby::Join(sNetplay.RelayAddress, code, "Guest");
                }
            }
        }
        return;
    }

    if (state != Lobby::State::InRoom) {
        FinishTest(false, "Left the room: " + Lobby::GetError());
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
        (CountMembers() >= sNetplay.Players) && Lobby::GetStartBlocker().empty()) {
        sNetplay.RacesStarted++;
        NetLog("Starting race %u of %u", sNetplay.RacesStarted, sNetplay.Races);
        Lobby::StartRace();
    }
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
    sNetplay.Players = GetEnvU32("SK_NETPLAY_PLAYERS", 2);
    sNetplay.Races = GetEnvU32("SK_NETPLAY_RACES", 1);
    const RaceConfig config = RaceConfig::FromEnvironment("SK_NETPLAY_", (s8) sNetplay.Players);
    sNetplay.TestSettings.Mode = config.ModeSelection;
    sNetplay.TestSettings.TrackName = config.TrackName;
    sNetplay.TestSettings.CupIndex = (s32) GetEnvU32("SK_NETPLAY_CUP", 0);
    sNetplay.Test = TestMode::Lobby;
}

} // namespace

extern "C" void Netplay_Init(void) {
    if (const char* peers = GetEnv("SK_NETPLAY_PEERS")) {
        InitDirectTest(peers);
    } else if (const char* relay = GetEnv("SK_NETPLAY_RELAY")) {
        InitLobbyTest(relay);
    }
    if (sNetplay.Test == TestMode::None) {
        return;
    }
    sNetplay.UseBot = GetEnvU32("SK_NETPLAY_BOT", 0) != 0;
    sNetplay.TotalFrames = GetEnvU32("SK_NETPLAY_FRAMES", 0);
    sNetplay.DelayMs = GetEnvU32("SK_NETPLAY_DELAY_MS", 0);
    sNetplay.JitterMs = GetEnvU32("SK_NETPLAY_JITTER_MS", 0);
    sNetplay.LossPercent = GetEnvU32("SK_NETPLAY_LOSS", 0);
    sNetplay.DesyncAtFrame = GetEnvU32("SK_NETPLAY_DESYNC_AT", 0);
    if (const char* logPath = GetEnv("SK_NETPLAY_LOG")) {
        sNetplay.LogPath = logPath;
    }
    sNetplay.TestResetBeforeRace = GetEnvU32("SK_NETPLAY_TEST_RESET", 0) != 0;
    sNetplay.TestStartDelay = GetEnvU32("SK_NETPLAY_TEST_START_DELAY", 0);
    if (const char* controls = GetEnv("SK_NETPLAY_TEST_CONTROLS")) {
        sNetplay.TestControls = ParseTestControls(controls);
    }
}

extern "C" void Netplay_BeginFrame(void) {
    Lobby::Update();
    HandleRaceEndedElsewhere();
    NetSession::BeginFrame();
}

extern "C" bool Netplay_IsInRace(void) {
    return NetSession::IsInRace();
}

extern "C" bool Netplay_ConsumeLocalPads(OSContPad* pads) {
    return NetSession::ConsumeLocalPads(pads);
}

extern "C" void Netplay_RunRaceFrame(void) {
    NetSession::RunRaceFrame();
}

extern "C" void Netplay_EndFrame(void) {
    NetSession::EndFrame();
    if (sNetplay.SessionWasActive && !NetSession::IsActive()) {
        OnSessionEnded();
    }

    if (sNetplay.BootFramesLeft > 0) {
        // Start the direct test after this frame's menu update, which would otherwise overwrite the track selection.
        if ((--sNetplay.BootFramesLeft == 0) && (sNetplay.Test == TestMode::Direct) && !sNetplay.Finished) {
            StartDirectRace();
        }
    } else {
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
    Lobby::Leave();
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

bool IsRaceRunning() {
    return NetSession::IsActive() || sNetplay.PendingRace.has_value();
}

} // namespace Netplay
