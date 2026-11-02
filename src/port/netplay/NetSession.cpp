#include "NetSession.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>

#include "Checksum.h"
#include "SimState.h"
#include "SyntheticDriver.h"

extern "C" {
#include "main.h"
#include "buffers.h"
#include "defines.h"
}

namespace {

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
// Peers load the race at different speeds; give up if they have not all connected after this long.
constexpr auto kConnectTimeout = std::chrono::seconds(30);

static_assert(sizeof(PadInput) == 4, "PadInput is sent to peers as raw bytes");

enum class SessionState { Off, WaitingForRace, Connecting, Running };

struct DelayedPacket {
    GekkoNetResult* Result;
    std::chrono::steady_clock::time_point ReleaseTime;
};

struct NetSessionState {
    SessionState State = SessionState::Off;
    SessionSettings Settings;
    std::optional<SyntheticDriver> Bot;
    SessionResult Result = SessionResult::Running;
    std::string ResultMessage;

    GekkoSession* Gekko = nullptr;
    GekkoNetAdapter* Socket = nullptr;
    std::chrono::steady_clock::time_point ConnectStarted;
    // GekkoNet stores only a frame number per state; the snapshots themselves live here.
    std::array<SimSnapshot, kNumSnapshots> Snapshots;
    std::array<s32, kNumSnapshots> SnapshotFrames;
    // Last checksum saved for each frame. After a rollback it is the corrected one.
    std::vector<u32> FrameChecksums;
    GameplayCVarOverride CVars;
    u32 ShownFrames = 0;
    // Time sync: a peer running ahead of the others skips advancing now and then so they catch up.
    bool WaitingThisFrame = false;
    bool WaitedLastFrame = false;
    u32 WaitsLeft = 0;
    u32 FramesUntilTimeSync = kTimeSyncInterval;
    u32 WaitedFrames = 0;

    // Simulated network conditions
    std::deque<DelayedPacket> DelayedPackets;
    std::vector<GekkoNetResult*> ReadyPackets;
    u32 RandomState = 0x2545F491;
};

NetSessionState sNet;

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

// Adapter wrapping the real one that delays and drops received packets, to test rollback on localhost.
void LagSend(GekkoNetAddress* addr, const char* data, int length) {
    sNet.Socket->send_data(addr, data, length);
}

GekkoNetResult** LagReceive(int* length) {
    const auto now = std::chrono::steady_clock::now();
    int count = 0;
    GekkoNetResult** received = sNet.Socket->receive_data(&count);
    for (int i = 0; i < count; i++) {
        if ((NextRandom() % 100) < sNet.Settings.LossPercent) {
            FreePacket(received[i]);
            continue;
        }
        const u32 jitterMs = sNet.Settings.JitterMs;
        const u32 jitter = (jitterMs > 0) ? (NextRandom() % (jitterMs + 1)) : 0;
        sNet.DelayedPackets.push_back({ received[i], now + std::chrono::milliseconds(sNet.Settings.DelayMs + jitter) });
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
    FILE* log = fopen(sNet.Settings.LogPath.c_str(), "w");
    if (log == nullptr) {
        printf("[Netplay] Could not write %s\n", sNet.Settings.LogPath.c_str());
        return;
    }
    for (size_t frame = 0; frame < sNet.FrameChecksums.size(); frame++) {
        fprintf(log, "%zu %08x\n", frame, sNet.FrameChecksums[frame]);
    }
    fclose(log);
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
    const std::string& logPath = sNet.Settings.LogPath;
    const std::string path = (logPath.empty() ? std::string("netplay") : logPath) + ".desync-p" +
                             std::to_string(sNet.Settings.LocalPlayer) + "-f" + std::to_string(frame) + ".txt";
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

void StartGekkoSession() {
    const SessionSettings& settings = sNet.Settings;
    gekko_create(&sNet.Gekko, GekkoGameSession);
    GekkoConfig config = {};
    config.num_players = (unsigned char) settings.Addresses.size();
    config.max_spectators = 0;
    config.max_input_prediction_window = kPredictionWindow;
    config.input_size = sizeof(PadInput);
    config.state_size = sizeof(s32);
    config.limited_saving = false;
    config.desync_detection = true;
    gekko_start(sNet.Gekko, &config);

    sNet.Socket = settings.Adapter;
    const bool simulateNetwork = (settings.DelayMs > 0) || (settings.JitterMs > 0) || (settings.LossPercent > 0);
    gekko_net_adapter_set(sNet.Gekko, simulateNetwork ? &sLagAdapter : sNet.Socket);

    // Actor handles follow the order they are added in, so handle == player index.
    for (s32 player = 0; player < (s32) settings.Addresses.size(); player++) {
        if (player == settings.LocalPlayer) {
            gekko_add_actor(sNet.Gekko, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(sNet.Gekko, player, kLocalInputDelay);
        } else {
            std::vector<u8> address = settings.Addresses[player];
            GekkoNetAddress gekkoAddress = { address.data(), (unsigned int) address.size() };
            gekko_add_actor(sNet.Gekko, GekkoRemotePlayer, &gekkoAddress);
        }
    }

    sNet.SnapshotFrames.fill(-1);
    sNet.ConnectStarted = std::chrono::steady_clock::now();
    sNet.State = SessionState::Connecting;
    printf("[Netplay] Session started: player %d of %zu\n", settings.LocalPlayer, settings.Addresses.size());
    fflush(stdout);
}

void HandleSessionEvents() {
    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(sNet.Gekko, &count);
    for (int i = 0; (i < count) && (sNet.Gekko != nullptr); i++) {
        const GekkoSessionEvent* event = events[i];
        switch (event->type) {
            case GekkoPlayerConnected:
                printf("[Netplay] Player %d connected\n", event->data.connected.handle);
                break;
            case GekkoSessionStarted:
                printf("[Netplay] All players connected; race starting\n");
                sNet.State = SessionState::Running;
                break;
            case GekkoPlayerDisconnected: {
                const s32 player = event->data.disconnected.handle;
                printf("[Netplay] Player %d disconnected\n", player);
                // A peer that reached the end of a test leaves first.
                if (NetSession::IsNearPlannedEnd()) {
                    NetSession::Stop(SessionResult::Completed, "Race complete");
                } else {
                    NetSession::Stop(SessionResult::PeerLeft, "Player " + std::to_string(player + 1) + " disconnected");
                }
                break;
            }
            case GekkoDesyncDetected: {
                const auto& desync = event->data.desynced;
                printf("[Netplay] DESYNC at frame %d: local checksum %08x, player %d has %08x\n", desync.frame,
                       desync.local_checksum, desync.remote_handle, desync.remote_checksum);
                WriteDesyncReport(desync.frame);
                NetSession::Stop(SessionResult::Desync,
                                 "The game went out of sync with player " + std::to_string(desync.remote_handle + 1) +
                                     " at frame " + std::to_string(desync.frame));
                break;
            }
            default:
                break;
        }
    }
    if (count > 0) {
        fflush(stdout);
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

namespace NetSession {

void Start(const SessionSettings& settings) {
    if (sNet.State != SessionState::Off) {
        Stop(SessionResult::Failed, "A new race started");
    }
    sNet.Settings = settings;
    sNet.Result = SessionResult::Running;
    sNet.ResultMessage.clear();
    sNet.FrameChecksums.clear();
    sNet.ShownFrames = 0;
    sNet.WaitingThisFrame = false;
    sNet.WaitedLastFrame = false;
    sNet.WaitsLeft = 0;
    sNet.FramesUntilTimeSync = kTimeSyncInterval;
    sNet.WaitedFrames = 0;
    sNet.RandomState = 0x2545F491 ^ ((u32) (settings.LocalPlayer + 1) * 7919);
    sNet.Bot.reset();
    if (settings.BotSeed.has_value()) {
        sNet.Bot.emplace(*settings.BotSeed);
    }

    if (!SimState_IsSupported()) {
        sNet.Result = SessionResult::Failed;
        sNet.ResultMessage = "This build cannot save simulation state, which netplay needs (build it with clang)";
        return;
    }
    sNet.CVars.Apply(settings.Config);
    SimState_SetRollbackMode(true);
    RaceConfig_StartRace(settings.Config);
    sNet.State = SessionState::WaitingForRace;
}

void Stop(SessionResult result, const std::string& message) {
    if (sNet.State == SessionState::Off) {
        return;
    }
    const char* verdict = IsSuccess(result) ? "PASS" : "FAIL";
    if (sNet.Gekko != nullptr) {
        GekkoRollbackStats rollbacks = {};
        gekko_rollback_stats(sNet.Gekko, &rollbacks);
        GekkoNetworkStats network = {};
        gekko_network_stats(sNet.Gekko, (sNet.Settings.LocalPlayer == 0) ? 1 : 0, &network);
        printf("[Netplay] %s: %s. %u frames shown, %u rollbacks (avg %.1f, max %u frames), %u stalls, %u time-sync "
               "waits, ping %.0f ms\n",
               verdict, message.c_str(), sNet.ShownFrames, rollbacks.total_count, rollbacks.avg_frames,
               rollbacks.max_frames, rollbacks.total_stalls, sNet.WaitedFrames, network.avg_ping);
        gekko_destroy(&sNet.Gekko);
        for (DelayedPacket& packet : sNet.DelayedPackets) {
            FreePacket(packet.Result);
        }
        sNet.DelayedPackets.clear();
    } else {
        printf("[Netplay] %s: %s\n", verdict, message.c_str());
    }
    if (!sNet.Settings.LogPath.empty()) {
        WriteChecksumLog();
    }
    sNet.CVars.Restore();
    SimState_SetRollbackMode(false);
    // The game exits with _Exit(), which skips flushing stdio.
    fflush(stdout);
    sNet.Socket = nullptr;
    sNet.Result = result;
    sNet.ResultMessage = message;
    sNet.State = SessionState::Off;
}

bool IsActive() {
    return sNet.State != SessionState::Off;
}

s32 GetLocalPlayer() {
    return sNet.Settings.LocalPlayer;
}

u32 GetShownFrames() {
    return sNet.ShownFrames;
}

bool IsNearPlannedEnd() {
    return (sNet.Settings.TotalFrames > 0) && (sNet.ShownFrames + kPredictionWindow >= sNet.Settings.TotalFrames);
}

SessionResult GetResult() {
    return sNet.Result;
}

const std::string& GetResultMessage() {
    return sNet.ResultMessage;
}

void BeginFrame() {
    if ((sNet.State == SessionState::WaitingForRace) && (gGamestate == RACING)) {
        StartGekkoSession();
    }
}

bool IsInRace() {
    return ((sNet.State == SessionState::Connecting) || (sNet.State == SessionState::Running)) &&
           (gGamestate == RACING);
}

bool ConsumeLocalPads(OSContPad* pads) {
    if (!IsInRace()) {
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
    gekko_add_local_input(sNet.Gekko, sNet.Settings.LocalPlayer, &input);
    return true;
}

void RunRaceFrame() {
    HandleSessionEvents();
    if ((sNet.State == SessionState::Connecting) &&
        (std::chrono::steady_clock::now() - sNet.ConnectStarted > kConnectTimeout)) {
        Stop(SessionResult::ConnectFailed, "Not every player connected");
    }
    if (!IsInRace()) {
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
    const SessionSettings& settings = sNet.Settings;
    if ((settings.DesyncAtFrame > 0) && (sNet.ShownFrames + 1 == settings.DesyncAtFrame)) {
        gRandomSeed16 ^= 1;
    }
    sNet.ShownFrames++;
    if ((settings.TotalFrames > 0) && (sNet.ShownFrames >= settings.TotalFrames)) {
        Stop(SessionResult::Completed, "Race complete");
    }
}

void EndFrame() {
    // The game leaves a race (results menu, quit) by setting gGamestate to 255 and gGamestateNext to where it goes.
    if (((sNet.State == SessionState::Connecting) || (sNet.State == SessionState::Running)) &&
        (gGamestate != RACING)) {
        Stop(SessionResult::RaceOver, "The race is over");
    }
}

} // namespace NetSession
