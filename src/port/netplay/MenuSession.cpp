#include "MenuSession.h"

#include <array>
#include <chrono>
#include <cstdio>

#include "NetLog.h"
#include "SimState.h"
#include "SyntheticDriver.h"
#include "port/Game.h"

extern "C" {
#include "main.h"
#include "menus.h"
#include "menu_items.h"
#include "code_80086E70.h"
#include "buffers/random.h"
}

namespace {

constexpr size_t kMaxPlayers = 4;
// Peers start at different moments; give up if they have not all connected after this long.
constexpr auto kConnectTimeout = std::chrono::seconds(30);

struct MenuSessionState {
    bool Active = false;
    bool Running = false; // Every player connected
    bool Frozen = false;
    MenuSessionSettings Settings;
    GekkoSession* Gekko = nullptr;
    bool FrameReady = false;
    std::array<PadInput, kMaxPlayers> Inputs = {};
    u32 Frames = 0;
    FILE* Log = nullptr;
    std::chrono::steady_clock::time_point ConnectStarted;
    std::string StopReason;
};

MenuSessionState sMenu;

// 64-bit FNV-1a
constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ULL;
constexpr u64 kFnvPrime = 0x100000001B3ULL;

template <typename T> void HashValue(u64& hash, const T& value) {
    const u8* bytes = reinterpret_cast<const u8*>(&value);
    for (size_t i = 0; i < sizeof(T); i++) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

/** Hashes the state the menus run on: which screen, every cursor and choice, and what drives their timing. */
u64 MenuChecksum() {
    u64 hash = kFnvOffsetBasis;
    HashValue(hash, gGamestate);
    HashValue(hash, gGamestateNext);
    HashValue(hash, gMenuSelection);
    HashValue(hash, gMainMenuSelection);
    HashValue(hash, gSubMenuSelection);
    HashValue(hash, gPlayerSelectMenuSelection);
    HashValue(hash, gDebugMenuSelection);
    HashValue(hash, gFadeModeSelection);
    HashValue(hash, gMenuFadeType);
    HashValue(hash, gMenuTimingCounter);
    HashValue(hash, gCycleFlashMenu);
    HashValue(hash, gGlobalTimer);
    HashValue(hash, gRandomSeed16);
    HashValue(hash, gPlayerCount);
    HashValue(hash, gPlayerCountSelection1);
    HashValue(hash, gScreenModeSelection);
    HashValue(hash, gModeSelection);
    HashValue(hash, gCCSelection);
    for (size_t i = 0; i < NUM_ROWS_GAME_MODE_MENU; i++) {
        HashValue(hash, gGameModeMenuColumn[i]);
    }
    HashValue(hash, gGameModeSubMenuColumn);
    for (size_t i = 0; i < kMaxPlayers; i++) {
        HashValue(hash, gCharacterGridSelections[i]);
        HashValue(hash, gCharacterGridIsSelected[i]);
        HashValue(hash, gCharacterSelections[i]);
        HashValue(hash, gControllers[i].button);
        HashValue(hash, gControllers[i].buttonPressed);
    }
    HashValue(hash, gCupSelection);
    HashValue(hash, gCourseIndexInCup);
    HashValue(hash, gCurrentCourseId);
    return hash;
}

void HandleSessionEvents() {
    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(sMenu.Gekko, &count);
    for (int i = 0; (i < count) && sMenu.Active; i++) {
        const GekkoSessionEvent* event = events[i];
        switch (event->type) {
            case GekkoPlayerConnected:
                NetLog("Menus: player %d connected", event->data.connected.handle + 1);
                break;
            case GekkoSessionStarted:
                NetLog("Menus: every player connected");
                sMenu.Running = true;
                break;
            case GekkoPlayerDisconnected:
                MenuSession::Stop("Player " + std::to_string(event->data.disconnected.handle + 1) + " disconnected");
                break;
            default:
                break;
        }
    }
}

} // namespace

namespace MenuSession {

void Start(const MenuSessionSettings& settings) {
    Stop("A new menu session started");
    sMenu = MenuSessionState{};
    sMenu.Settings = settings;
    sMenu.Active = true;
    sMenu.StopReason.clear();

    if (settings.ResetToGameSelect) {
        // Every peer starts from the same state: the globals as they were at boot, then the Game Select screen.
        SimState_RestoreBootState();
        SimState_ResetControllers();
        CM_RequestReset();
    }
    // The frame counter is not simulation state, but the menus' flashing colors follow it. It only advances on frames
    // that run, so from here on every peer counts the same.
    gGlobalTimer = 0;

    gekko_create(&sMenu.Gekko, GekkoGameSession);
    GekkoConfig config = {};
    config.num_players = (unsigned char) settings.Addresses.size();
    config.max_spectators = 0;
    // No prediction: GekkoNet's lockstep mode, which never rolls back.
    config.max_input_prediction_window = 0;
    config.input_size = sizeof(PadInput);
    config.state_size = sizeof(u32);
    config.limited_saving = true;
    config.desync_detection = false;
    gekko_start(sMenu.Gekko, &config);
    gekko_net_adapter_set(sMenu.Gekko, settings.GetAdapter());
    // Actor handles follow the order they are added in, so handle == player index.
    for (s32 player = 0; player < (s32) settings.Addresses.size(); player++) {
        if (player == settings.LocalPlayer) {
            gekko_add_actor(sMenu.Gekko, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(sMenu.Gekko, player, settings.InputDelay);
        } else {
            std::vector<u8> address = settings.Addresses[player];
            GekkoNetAddress gekkoAddress = { address.data(), (unsigned int) address.size() };
            gekko_add_actor(sMenu.Gekko, GekkoRemotePlayer, &gekkoAddress);
        }
    }
    if (!settings.LogPath.empty()) {
        sMenu.Log = fopen((settings.LogPath + ".menu").c_str(), "w");
    }
    sMenu.ConnectStarted = std::chrono::steady_clock::now();
    NetLog("Menus: session started, player %d of %zu, input delay %u%s", settings.LocalPlayer,
           settings.Addresses.size(), settings.InputDelay, settings.ResetToGameSelect ? ", from Game Select" : "");
}

void Stop(const std::string& reason) {
    if (!sMenu.Active) {
        return;
    }
    NetLog("Menus: session ended after %u frames: %s", sMenu.Frames, reason.c_str());
    gekko_destroy(&sMenu.Gekko);
    if (sMenu.Log != nullptr) {
        fclose(sMenu.Log);
        sMenu.Log = nullptr;
    }
    sMenu.Active = false;
    sMenu.Running = false;
    sMenu.Frozen = false;
    sMenu.FrameReady = false;
    sMenu.StopReason = reason;
    fflush(stdout);
}

bool IsActive() {
    return sMenu.Active;
}

bool IsRunning() {
    return sMenu.Active && sMenu.Running;
}

u32 GetFrames() {
    return sMenu.Frames;
}

const std::string& GetStopReason() {
    return sMenu.StopReason;
}

void BeginFrame(const OSContPad& localPad) {
    if (!sMenu.Active) {
        return;
    }
    sMenu.FrameReady = false;
    HandleSessionEvents();
    if (!sMenu.Active) {
        return;
    }
    if (!sMenu.Running && (std::chrono::steady_clock::now() - sMenu.ConnectStarted > kConnectTimeout)) {
        Stop("Not every player connected");
        return;
    }

    PadInput input;
    input.StickX = localPad.stick_x;
    input.StickY = localPad.stick_y;
    input.Buttons = localPad.button;
    gekko_add_local_input(sMenu.Gekko, sMenu.Settings.LocalPlayer, &input);

    int count = 0;
    GekkoGameEvent** events = gekko_update_session(sMenu.Gekko, &count);
    for (int i = 0; i < count; i++) {
        if (events[i]->type != GekkoAdvanceEvent) {
            continue;
        }
        const auto& advance = events[i]->data.adv;
        const PadInput* inputs = reinterpret_cast<const PadInput*>(advance.inputs);
        const size_t numPlayers = advance.input_len / sizeof(PadInput);
        sMenu.Inputs = {};
        for (size_t player = 0; (player < numPlayers) && (player < kMaxPlayers); player++) {
            sMenu.Inputs[player] = inputs[player];
        }
        sMenu.FrameReady = !sMenu.Frozen;
    }
}

bool IsWaiting() {
    return sMenu.Active && !sMenu.Frozen && !sMenu.FrameReady;
}

void Freeze() {
    if (sMenu.Active && !sMenu.Frozen) {
        NetLog("Menus: frozen after %u frames", sMenu.Frames);
        sMenu.Frozen = true;
        sMenu.FrameReady = false;
    }
}

bool IsFrozen() {
    return sMenu.Active && sMenu.Frozen;
}

bool TakePads(OSContPad* pads) {
    if (!sMenu.Active || !sMenu.FrameReady) {
        return false;
    }
    for (size_t player = 0; player < kMaxPlayers; player++) {
        memset(&pads[player], 0, sizeof(OSContPad));
        pads[player].stick_x = sMenu.Inputs[player].StickX;
        pads[player].stick_y = sMenu.Inputs[player].StickY;
        pads[player].button = sMenu.Inputs[player].Buttons;
    }
    return true;
}

void EndFrame() {
    if (!sMenu.Active || !sMenu.FrameReady) {
        return;
    }
    sMenu.Frames++;
    if (sMenu.Log != nullptr) {
        fprintf(sMenu.Log,
                "%u %016llx menu %d main %d sub %d select %d players %d mode %d cc %d columns %d %d %d %d "
                "characters %d %d %d %d grid %d %d %d %d cup %d course %d id %d timer %d random %04x flash %d "
                "fade %d %d\n",
                sMenu.Frames, (unsigned long long) MenuChecksum(), gMenuSelection, gMainMenuSelection,
                gSubMenuSelection, gPlayerSelectMenuSelection, gPlayerCount, gModeSelection, gCCSelection,
                gGameModeMenuColumn[0], gGameModeMenuColumn[1], gGameModeMenuColumn[2], gGameModeMenuColumn[3],
                gCharacterSelections[0], gCharacterSelections[1], gCharacterSelections[2], gCharacterSelections[3],
                gCharacterGridSelections[0], gCharacterGridSelections[1], gCharacterGridSelections[2],
                gCharacterGridSelections[3], gCupSelection, gCourseIndexInCup, gCurrentCourseId, gGlobalTimer,
                gRandomSeed16, gCycleFlashMenu, gFadeModeSelection, gMenuFadeType);
    }
    sMenu.FrameReady = false;
}

} // namespace MenuSession
