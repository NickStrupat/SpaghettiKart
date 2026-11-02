#include "Netplay.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <gekkonet.h>

#include "Checksum.h"
#include "RaceConfig.h"
#include "SimState.h"
#include "SyntheticDriver.h"

extern "C" {
#include "main.h"
#include "buffers.h"
#include "defines.h"
}

namespace {

// Frames to wait after boot before starting the test race, so the engine and menus finish initializing.
constexpr u32 kBootFrames = 30;
// How far the session may run ahead of the slowest peer's confirmed input: 8 frames is 266 ms at 30 fps.
constexpr unsigned char kPredictionWindow = 8;
// Frames the local input is held back, trading a little latency for fewer rollbacks.
constexpr unsigned char kLocalInputDelay = 1;
// Rollback needs the last prediction window + 2 states. A desync is only detected once the peers have exchanged
// that frame's checksum, so keep about a second of snapshots to still have the desynced frame for its report.
constexpr size_t kNumSnapshots = 32;
static_assert(kNumSnapshots >= kPredictionWindow + 2);
constexpr size_t kMaxPlayers = 4;
// GekkoNet averages frame advantage over 26 frames; re-check only once the average reflects earlier waits.
constexpr u32 kTimeSyncInterval = 26;

static_assert(sizeof(PadInput) == 4, "PadInput is sent to peers as raw bytes");

enum class SessionState { Off, WaitingForBoot, WaitingForRace, Running, Finished };

struct DelayedPacket {
    GekkoNetResult* Result;
    std::chrono::steady_clock::time_point ReleaseTime;
};

struct NetSession {
    SessionState State = SessionState::Off;

    // Test configuration, from the environment
    s32 LocalPlayer = 0;
    std::vector<std::string> PeerAddresses;
    RaceConfig Config;
    u32 TotalFrames = 0;
    u32 DelayMs = 0;
    u32 JitterMs = 0;
    u32 LossPercent = 0;
    std::string LogPath;
    std::optional<SyntheticDriver> Bot;
    // Test of desync detection: corrupt this peer's state after this shown frame.
    u32 DesyncAtFrame = 0;

    GekkoSession* Gekko = nullptr;
    GekkoNetAdapter* Socket = nullptr;
    // GekkoNet stores only a frame number per state; the snapshots themselves live here.
    std::array<SimSnapshot, kNumSnapshots> Snapshots;
    std::array<s32, kNumSnapshots> SnapshotFrames;
    // Last checksum saved for each frame. After a rollback it is the corrected one.
    std::vector<u32> FrameChecksums;
    GameplayCVarOverride CVars;
    u32 BootFramesLeft = kBootFrames;
    u32 ShownFrames = 0;
    // Time sync: a peer running ahead of the others skips advancing now and then so they catch up.
    bool WaitingThisFrame = false;
    bool WaitedLastFrame = false;
    u32 WaitsLeft = 0;
    u32 FramesUntilTimeSync = kTimeSyncInterval;
    u32 WaitedFrames = 0;
    s32 ExitCode = EXIT_SUCCESS;

    // Simulated network conditions
    std::deque<DelayedPacket> DelayedPackets;
    std::vector<GekkoNetResult*> ReadyPackets;
    u32 RandomState = 0x2545F491;
};

NetSession sNet;

const char* GetEnv(const char* name) {
    const char* value = getenv(name);
    return ((value != nullptr) && (value[0] != '\0')) ? value : nullptr;
}

u32 GetEnvU32(const char* name, u32 fallback) {
    const char* value = GetEnv(name);
    return (value != nullptr) ? (u32) strtoul(value, nullptr, 10) : fallback;
}

// xorshift32, for simulated packet loss and jitter
u32 NextRandom() {
    sNet.RandomState ^= sNet.RandomState << 13;
    sNet.RandomState ^= sNet.RandomState >> 17;
    sNet.RandomState ^= sNet.RandomState << 5;
    return sNet.RandomState;
}

void FreePacket(GekkoNetResult* packet) {
    sNet.Socket->free_data(packet->addr.data);
    sNet.Socket->free_data(packet->data);
    sNet.Socket->free_data(packet);
}

// Adapter wrapping the UDP socket that delays and drops received packets, to test rollback on localhost.
void LagSend(GekkoNetAddress* addr, const char* data, int length) {
    sNet.Socket->send_data(addr, data, length);
}

GekkoNetResult** LagReceive(int* length) {
    const auto now = std::chrono::steady_clock::now();
    int count = 0;
    GekkoNetResult** received = sNet.Socket->receive_data(&count);
    for (int i = 0; i < count; i++) {
        if ((NextRandom() % 100) < sNet.LossPercent) {
            FreePacket(received[i]);
            continue;
        }
        const u32 jitter = (sNet.JitterMs > 0) ? (NextRandom() % (sNet.JitterMs + 1)) : 0;
        sNet.DelayedPackets.push_back({ received[i], now + std::chrono::milliseconds(sNet.DelayMs + jitter) });
    }

    // Jitter can reorder packets, as real networks do.
    sNet.ReadyPackets.clear();
    for (auto it = sNet.DelayedPackets.begin(); it != sNet.DelayedPackets.end();) {
        if (it->ReleaseTime <= now) {
            sNet.ReadyPackets.push_back(it->Result);
            it = sNet.DelayedPackets.erase(it);
        } else {
            ++it;
        }
    }
    *length = (int) sNet.ReadyPackets.size();
    return sNet.ReadyPackets.data();
}

void LagFree(void* data) {
    sNet.Socket->free_data(data);
}

GekkoNetAdapter sLagAdapter = { LagSend, LagReceive, LagFree };

size_t GetSnapshotIndex(s32 frame) {
    return (size_t) (((frame % (s32) kNumSnapshots) + (s32) kNumSnapshots) % (s32) kNumSnapshots);
}

void WriteChecksumLog() {
    FILE* log = fopen(sNet.LogPath.c_str(), "w");
    if (log == nullptr) {
        printf("[Netplay] Could not write %s\n", sNet.LogPath.c_str());
        return;
    }
    for (size_t frame = 0; frame < sNet.FrameChecksums.size(); frame++) {
        fprintf(log, "%zu %08x\n", frame, sNet.FrameChecksums[frame]);
    }
    fclose(log);
}

void Finish(s32 exitCode, const char* verdict) {
    if (sNet.Gekko != nullptr) {
        GekkoRollbackStats rollbacks = {};
        gekko_rollback_stats(sNet.Gekko, &rollbacks);
        GekkoNetworkStats network = {};
        gekko_network_stats(sNet.Gekko, (sNet.LocalPlayer == 0) ? 1 : 0, &network);
        printf("[Netplay] %s: %u frames shown, %u rollbacks (avg %.1f, max %u frames), %u stalls, %u time-sync "
               "waits, ping %.0f ms\n",
               verdict, sNet.ShownFrames, rollbacks.total_count, rollbacks.avg_frames, rollbacks.max_frames,
               rollbacks.total_stalls, sNet.WaitedFrames, network.avg_ping);
        gekko_destroy(&sNet.Gekko);
        for (DelayedPacket& packet : sNet.DelayedPackets) {
            FreePacket(packet.Result);
        }
        sNet.DelayedPackets.clear();
        gekko_default_adapter_destroy();
    } else {
        printf("[Netplay] %s\n", verdict);
    }
    if (!sNet.LogPath.empty()) {
        WriteChecksumLog();
    }
    sNet.CVars.Restore();
    SimState_SetRollbackMode(false);
    // The game exits with _Exit(), which skips flushing stdio.
    fflush(stdout);
    sNet.ExitCode = exitCode;
    sNet.State = SessionState::Finished;
}

/**
 * Writes the per-component checksums of a desynced frame, if its snapshot is still kept, so that the files
 * written by each peer can be diffed to find the diverging player, actor, object or camera.
 */
void WriteDesyncReport(s32 frame) {
    const size_t index = GetSnapshotIndex(frame);
    if (sNet.SnapshotFrames[index] != frame) {
        printf("[Netplay] Frame %d is no longer kept; no desync report\n", frame);
        return;
    }
    const std::string path = (sNet.LogPath.empty() ? std::string("netplay") : sNet.LogPath) + ".desync-p" +
                             std::to_string(sNet.LocalPlayer) + "-f" + std::to_string(frame) + ".txt";
    FILE* report = fopen(path.c_str(), "w");
    if (report == nullptr) {
        return;
    }
    static SimSnapshot sCurrent;
    SimState::Save(sCurrent);
    SimState::Load(sNet.Snapshots[index]);
    fprintf(report, "frame %d\n", frame);
    Sim_WriteChecksumBreakdown(report);
    SimState::Load(sCurrent);
    fclose(report);
    printf("[Netplay] Desync report written to %s\n", path.c_str());
}

void StartSession() {
    const std::string& ownAddress = sNet.PeerAddresses[sNet.LocalPlayer];
    const size_t colon = ownAddress.rfind(':');
    const unsigned short port = (unsigned short) atoi(ownAddress.c_str() + colon + 1);

    gekko_create(&sNet.Gekko, GekkoGameSession);
    GekkoConfig config = {};
    config.num_players = (unsigned char) sNet.PeerAddresses.size();
    config.max_spectators = 0;
    config.max_input_prediction_window = kPredictionWindow;
    config.input_size = sizeof(PadInput);
    config.state_size = sizeof(s32);
    config.limited_saving = false;
    config.desync_detection = true;
    gekko_start(sNet.Gekko, &config);

    try {
        sNet.Socket = gekko_default_adapter(port);
    } catch (const std::exception& e) {
        printf("[Netplay] Could not open UDP port %u: %s\n", port, e.what());
        Finish(EXIT_FAILURE, "FAIL");
        return;
    }
    const bool simulateNetwork = (sNet.DelayMs > 0) || (sNet.JitterMs > 0) || (sNet.LossPercent > 0);
    gekko_net_adapter_set(sNet.Gekko, simulateNetwork ? &sLagAdapter : sNet.Socket);

    // Actor handles follow the order they are added in, so handle == player index.
    for (s32 player = 0; player < (s32) sNet.PeerAddresses.size(); player++) {
        if (player == sNet.LocalPlayer) {
            gekko_add_actor(sNet.Gekko, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(sNet.Gekko, player, kLocalInputDelay);
        } else {
            GekkoNetAddress address = { (void*) sNet.PeerAddresses[player].c_str(),
                                        (unsigned int) sNet.PeerAddresses[player].size() };
            gekko_add_actor(sNet.Gekko, GekkoRemotePlayer, &address);
        }
    }

    sNet.SnapshotFrames.fill(-1);
    sNet.State = SessionState::Running;
    printf("[Netplay] Session started: player %d of %zu on port %u\n", sNet.LocalPlayer, sNet.PeerAddresses.size(),
           port);
}

void HandleSessionEvents() {
    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(sNet.Gekko, &count);
    for (int i = 0; (i < count) && (sNet.State == SessionState::Running); i++) {
        const GekkoSessionEvent* event = events[i];
        switch (event->type) {
            case GekkoPlayerConnected:
                printf("[Netplay] Player %d connected\n", event->data.connected.handle);
                break;
            case GekkoSessionStarted:
                printf("[Netplay] All players connected; race starting\n");
                break;
            case GekkoPlayerDisconnected: {
                printf("[Netplay] Player %d disconnected\n", event->data.disconnected.handle);
                // A peer that reached the end of the test leaves first.
                const bool done = (sNet.TotalFrames > 0) && (sNet.ShownFrames + kPredictionWindow >= sNet.TotalFrames);
                Finish(done ? EXIT_SUCCESS : EXIT_FAILURE, done ? "PASS" : "FAIL");
                break;
            }
            case GekkoDesyncDetected:
                printf("[Netplay] DESYNC at frame %d: local checksum %08x, player %d has %08x\n",
                       event->data.desynced.frame, event->data.desynced.local_checksum,
                       event->data.desynced.remote_handle, event->data.desynced.remote_checksum);
                WriteDesyncReport(event->data.desynced.frame);
                Finish(EXIT_FAILURE, "FAIL");
                break;
            default:
                break;
        }
    }
}

void UnpackInputs(const GekkoGameEvent::GekkoEventData::GekkoAdvance& advance, OSContPad* pads) {
    const PadInput* inputs = reinterpret_cast<const PadInput*>(advance.inputs);
    const size_t numPlayers = advance.input_len / sizeof(PadInput);
    for (size_t i = 0; i < kMaxPlayers; i++) {
        memset(&pads[i], 0, sizeof(OSContPad));
        if (i < numPlayers) {
            pads[i].stick_x = inputs[i].StickX;
            pads[i].stick_y = inputs[i].StickY;
            pads[i].button = inputs[i].Buttons;
        }
    }
}

void SaveFrame(const GekkoGameEvent::GekkoEventData::GekkoSave& save) {
    const s32 frame = save.frame;
    SimState::Save(sNet.Snapshots[GetSnapshotIndex(frame)]);
    sNet.SnapshotFrames[GetSnapshotIndex(frame)] = frame;
    *save.state_len = sizeof(s32);
    memcpy(save.state, &frame, sizeof(s32));

    const u64 checksum = Sim_GameplayChecksum();
    *save.checksum = (u32) (checksum ^ (checksum >> 32));
    if (frame >= 0) {
        if (sNet.FrameChecksums.size() <= (size_t) frame) {
            sNet.FrameChecksums.resize(frame + 1);
        }
        sNet.FrameChecksums[frame] = *save.checksum;
    }
}

void LoadFrame(const GekkoGameEvent::GekkoEventData::GekkoLoad& load) {
    s32 frame;
    memcpy(&frame, load.state, sizeof(s32));
    SimState::Load(sNet.Snapshots[GetSnapshotIndex(frame)]);
}

} // namespace

extern "C" void Netplay_Init(void) {
    const char* peers = GetEnv("SK_NETPLAY_PEERS");
    if (peers == nullptr) {
        return;
    }

    std::string list = peers;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const size_t end = (comma == std::string::npos) ? list.size() : comma;
        sNet.PeerAddresses.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    sNet.LocalPlayer = (s32) GetEnvU32("SK_NETPLAY_PLAYER", 0);

    if ((sNet.PeerAddresses.size() < 2) || (sNet.PeerAddresses.size() > kMaxPlayers) ||
        (sNet.LocalPlayer >= (s32) sNet.PeerAddresses.size())) {
        Finish(EXIT_FAILURE, "FAIL: SK_NETPLAY_PEERS needs 2-4 addresses and SK_NETPLAY_PLAYER must index one");
        return;
    }
    if (!SimState_IsSupported()) {
        Finish(EXIT_FAILURE, "FAIL: this build cannot save simulation state (needs clang)");
        return;
    }

    sNet.Config = RaceConfig::FromEnvironment("SK_NETPLAY_", (s8) sNet.PeerAddresses.size());
    sNet.TotalFrames = GetEnvU32("SK_NETPLAY_FRAMES", 0);
    sNet.DelayMs = GetEnvU32("SK_NETPLAY_DELAY_MS", 0);
    sNet.JitterMs = GetEnvU32("SK_NETPLAY_JITTER_MS", 0);
    sNet.LossPercent = GetEnvU32("SK_NETPLAY_LOSS", 0);
    sNet.DesyncAtFrame = GetEnvU32("SK_NETPLAY_DESYNC_AT", 0);
    if (const char* logPath = GetEnv("SK_NETPLAY_LOG")) {
        sNet.LogPath = logPath;
    }
    if (GetEnvU32("SK_NETPLAY_BOT", 0) != 0) {
        sNet.Bot.emplace((sNet.Config.RandomSeed + 1) * 0x9E3779B9u * (u32) (sNet.LocalPlayer + 1));
    }
    sNet.RandomState ^= (u32) (sNet.LocalPlayer + 1) * 7919;
    sNet.State = SessionState::WaitingForBoot;
}

extern "C" void Netplay_BeginFrame(void) {
    if ((sNet.State == SessionState::WaitingForRace) && (gGamestate == RACING)) {
        StartSession();
    }
}

extern "C" bool Netplay_IsInRace(void) {
    return (sNet.State == SessionState::Running) && (gGamestate == RACING);
}

extern "C" bool Netplay_ConsumeLocalPads(OSContPad* pads) {
    if (!Netplay_IsInRace()) {
        return false;
    }

    // Waiting when ahead keeps every peer's prediction distance, and so its rollbacks, about equal.
    // Waits are spread to at most every other frame, so the correction never looks like a freeze.
    if ((sNet.WaitsLeft == 0) && (--sNet.FramesUntilTimeSync == 0)) {
        sNet.FramesUntilTimeSync = kTimeSyncInterval;
        const f32 framesAhead = gekko_frames_ahead(sNet.Gekko);
        sNet.WaitsLeft = (framesAhead >= 1.0f) ? (u32) framesAhead : 0;
    }
    sNet.WaitingThisFrame = (sNet.WaitsLeft > 0) && !sNet.WaitedLastFrame;
    sNet.WaitedLastFrame = sNet.WaitingThisFrame;
    if (sNet.WaitingThisFrame) {
        sNet.WaitsLeft--;
        sNet.WaitedFrames++;
        return true;
    }

    PadInput input;
    if (sNet.Bot.has_value()) {
        input = sNet.Bot->Next();
    } else {
        input.StickX = pads[0].stick_x;
        input.StickY = pads[0].stick_y;
        // Pausing would only pause this peer.
        input.Buttons = pads[0].button & ~START_BUTTON;
    }
    gekko_add_local_input(sNet.Gekko, sNet.LocalPlayer, &input);
    return true;
}

extern "C" void Netplay_RunRaceFrame(void) {
    HandleSessionEvents();
    if (sNet.State != SessionState::Running) {
        SimState_RedrawFrame();
        return;
    }
    if (sNet.WaitingThisFrame) {
        gekko_network_poll(sNet.Gekko);
        SimState_RedrawFrame();
        return;
    }

    int count = 0;
    GekkoGameEvent** events = gekko_update_session(sNet.Gekko, &count);

    // The last advance that is not a rollback is this update's new frame; earlier ones are drawn off-screen.
    int shownAdvance = -1;
    for (int i = 0; i < count; i++) {
        if ((events[i]->type == GekkoAdvanceEvent) && !events[i]->data.adv.rolling_back) {
            shownAdvance = i;
        }
    }

    for (int i = 0; i < count; i++) {
        const GekkoGameEvent* event = events[i];
        switch (event->type) {
            case GekkoAdvanceEvent: {
                std::array<OSContPad, kMaxPlayers> pads;
                UnpackInputs(event->data.adv, pads.data());
                SimState_AdvanceFrame(pads.data(), i == shownAdvance);
                break;
            }
            case GekkoSaveEvent:
                SaveFrame(event->data.save);
                break;
            case GekkoLoadEvent:
                LoadFrame(event->data.load);
                break;
            default:
                break;
        }
    }

    if (shownAdvance < 0) {
        // Waiting for remote input: show the current frame again.
        SimState_RedrawFrame();
        return;
    }
    if ((sNet.DesyncAtFrame > 0) && (sNet.ShownFrames + 1 == sNet.DesyncAtFrame)) {
        gRandomSeed16 ^= 1;
    }
    if ((sNet.TotalFrames > 0) && (++sNet.ShownFrames >= sNet.TotalFrames)) {
        Finish(EXIT_SUCCESS, "PASS");
    }
}

extern "C" void Netplay_EndFrame(void) {
    if ((sNet.State == SessionState::WaitingForBoot) && (--sNet.BootFramesLeft == 0)) {
        // Start the race after this frame's menu update, which would otherwise overwrite the track selection.
        sNet.CVars.Apply(sNet.Config);
        SimState_SetRollbackMode(true);
        RaceConfig_StartRace(sNet.Config);
        sNet.State = SessionState::WaitingForRace;
    }
}

extern "C" bool Netplay_IsFinished(void) {
    return sNet.State == SessionState::Finished;
}

extern "C" s32 Netplay_GetExitCode(void) {
    return sNet.ExitCode;
}
