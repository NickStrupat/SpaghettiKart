#include "NetSession.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>

#include "Checksum.h"
#include "NetLog.h"
#include "SimState.h"
#include "SyntheticDriver.h"

extern "C" {
#include "main.h"
#include "buffers.h"
#include "defines.h"
#include "code_800029B0.h"
#include "racing/race_logic.h"
}
#include "port/Game.h"

namespace {

// How far the session may run ahead of the slowest peer's confirmed input: 8 frames is 266 ms at 30 fps.
// GekkoNet predicts at most this many frames past the last input it received, so once a peer has advanced a frame,
// every input more than this many frames back is final.
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
// Give up on peers that never confirm the end of a segment.
constexpr auto kBarrierTimeout = std::chrono::seconds(15);
// Without a barrier (direct test peers), keep the session up this many frames after the end so the others can
// confirm it too.
constexpr u32 kDirectBarrierFrames = 30;
// A leave request that has not ended the race by then (the session is stuck) ends it locally.
constexpr auto kLeaveTimeout = std::chrono::seconds(8);
// PadControl travels in two bits of the button word that no N64 button uses.
constexpr u16 kControlShift = 6;
constexpr u16 kControlMask = 0x3 << kControlShift;

static_assert(sizeof(PadInput) == 4, "PadInput is sent to peers as raw bytes");

enum class SessionState { Off, WaitingForRace, Connecting, Running };

struct DelayedPacket {
    GekkoNetResult* Result;
    std::chrono::steady_clock::time_point ReleaseTime;
};

/** Which control ended the race, kept with each snapshot because rollbacks can undo it. */
struct ExitCause {
    s8 Player = -1;
    PadControl Control = PadControl::None;
};

struct Barrier {
    bool Reached = false;
    s32 Frame = 0;
    u32 FramesWaited = 0;
    std::chrono::steady_clock::time_point Since;
};

struct NetSessionState {
    SessionState State = SessionState::Off;
    SessionSettings Settings;
    std::optional<SyntheticDriver> Bot;
    SessionResult Result = SessionResult::Running;
    std::string ResultMessage;
    // Where the game went after the last segment: the podium after a Grand Prix, or the menus.
    s32 ExitGamestate = RACING;

    u32 Segment = 0;
    // A segment's session starts once a race set up after this count has loaded.
    u32 SetupCountAtWait = 0;
    GekkoSession* Gekko = nullptr;
    GekkoNetAdapter* Socket = nullptr;
    std::chrono::steady_clock::time_point ConnectStarted;
    // GekkoNet stores only a frame number per state; the snapshots themselves live here.
    std::array<SimSnapshot, kNumSnapshots> Snapshots;
    std::array<s32, kNumSnapshots> SnapshotFrames;
    // Per snapshot: whether the game had started leaving the race, and which control made it.
    std::array<bool, kNumSnapshots> TransitionAt;
    std::array<ExitCause, kNumSnapshots> ExitAt;
    ExitCause Exit;
    Barrier SegmentEnd;
    // Last checksum saved for each frame of the segment. After a rollback it is the corrected one.
    std::vector<u32> FrameChecksums;
    GameplayCVarOverride CVars;
    u32 ShownFrames = 0;
    u32 SegmentFrames = 0;

    // Requests the local player sends with its inputs
    PadControl PendingControl = PadControl::None;
    bool LeaveRequested = false;
    std::chrono::steady_clock::time_point LeaveRequestedAt;

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

/**
 * Every way out of a race starts the quit transition (gIsInQuitToMenuTransition), a fade that ignores input and
 * ends by changing the gamestate. A segment freezes once it starts: later frames do nothing until every peer has
 * confirmed the frame, then the fade runs and the next race loads outside the session.
 */
bool IsTransitionPending() {
    return (gIsInQuitToMenuTransition != 0) || (gGamestate != RACING);
}

void WriteChecksumLog() {
    const std::string path = sNet.Settings.LogPath + ".seg" + std::to_string(sNet.Segment);
    FILE* log = fopen(path.c_str(), "w");
    if (log == nullptr) {
        NetLog("Could not write %s", path.c_str());
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
        NetLog("Frame %d is no longer kept; no desync report", frame);
        return;
    }
    const std::string& logPath = sNet.Settings.LogPath;
    const std::string path = (logPath.empty() ? std::string("netplay") : logPath) + ".desync-p" +
                             std::to_string(sNet.Settings.LocalPlayer) + "-s" + std::to_string(sNet.Segment) +
                             "-f" + std::to_string(frame) + ".txt";
    FILE* report = fopen(path.c_str(), "w");
    if (report == nullptr) {
        return;
    }
    static SimSnapshot sCurrent;
    SimState::Save(sCurrent);
    SimState::Load(sNet.Snapshots[index]);
    fprintf(report, "segment %u frame %d\n", sNet.Segment, frame);
    Sim_WriteChecksumBreakdown(report);
    SimState::Load(sCurrent);
    fclose(report);
    NetLog("Desync report written to %s", path.c_str());
}

/** Prints the segment's statistics, tears down its GekkoNet session and writes its checksum log. */
void EndGekkoSession(const char* verdict, const std::string& message) {
    if (sNet.Gekko == nullptr) {
        NetLog("%s: %s", verdict, message.c_str());
        return;
    }
    GekkoRollbackStats rollbacks = {};
    gekko_rollback_stats(sNet.Gekko, &rollbacks);
    GekkoNetworkStats network = {};
    gekko_network_stats(sNet.Gekko, (sNet.Settings.LocalPlayer == 0) ? 1 : 0, &network);
    NetLog("%s: %s. Segment %u: %u frames shown, %u rollbacks (avg %.1f, max %u frames), %u stalls, "
           "%u time-sync waits, ping %.0f ms",
           verdict, message.c_str(), sNet.Segment, sNet.SegmentFrames, rollbacks.total_count, rollbacks.avg_frames,
           rollbacks.max_frames, rollbacks.total_stalls, sNet.WaitedFrames, network.avg_ping);
    gekko_destroy(&sNet.Gekko);
    for (DelayedPacket& packet : sNet.DelayedPackets) {
        FreePacket(packet.Result);
    }
    sNet.DelayedPackets.clear();
    if (!sNet.Settings.LogPath.empty()) {
        WriteChecksumLog();
    }
    fflush(stdout);
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

    sNet.Socket = settings.GetAdapter(sNet.Segment);
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

    // The menus and the end of the last race left each peer's controllers in a different state.
    SimState_ResetControllers();
    if (settings.DumpSegmentStarts && !settings.LogPath.empty()) {
        const std::string path = settings.LogPath + ".seg" + std::to_string(sNet.Segment) + ".start";
        if (FILE* dump = fopen(path.c_str(), "wb")) {
            static SimSnapshot sStart;
            SimState::Save(sStart);
            SimState::WriteSnapshotFile(sStart, dump);
            fclose(dump);
        }
    }
    sNet.SnapshotFrames.fill(-1);
    sNet.TransitionAt.fill(false);
    sNet.Exit = ExitCause{};
    sNet.SegmentEnd = Barrier{};
    sNet.FrameChecksums.clear();
    sNet.SegmentFrames = 0;
    sNet.PendingControl = PadControl::None;
    sNet.WaitingThisFrame = false;
    sNet.WaitedLastFrame = false;
    sNet.WaitsLeft = 0;
    sNet.FramesUntilTimeSync = kTimeSyncInterval;
    sNet.WaitedFrames = 0;
    sNet.ConnectStarted = std::chrono::steady_clock::now();
    sNet.State = SessionState::Connecting;
    NetLog("Segment %u started: player %d of %zu", sNet.Segment, settings.LocalPlayer,
           settings.Addresses.size());
    fflush(stdout);
}

/** Ends the segment once every peer confirmed it: the next race loads, or the series ends. */
void EndSegment() {
    // The confirmed state is frozen at the frame the transition began, so this is where every peer goes.
    const s32 next = gGotoMode;
    const bool continues = (next == RACING) && (sNet.Settings.GetBarrierStatus != nullptr);
    if (continues) {
        EndGekkoSession("PASS", "Next race");
        sNet.Segment++;
        sNet.SetupCountAtWait = SimState_GetRaceSetupCount();
        sNet.State = SessionState::WaitingForRace;
        return;
    }

    std::string message = "The race is over";
    if (sNet.Exit.Control == PadControl::Leave) {
        message = "Player " + std::to_string(sNet.Exit.Player + 1) + " left the race";
    } else if (next == ENDING) {
        message = "The Grand Prix is over";
    }
    sNet.ExitGamestate = next;
    NetSession::Stop(SessionResult::RaceOver, message);
}

/** Notices when the segment's end can no longer change, then waits for the other peers to notice too. */
void UpdateBarrier(s32 shownFrame) {
    Barrier& barrier = sNet.SegmentEnd;
    if (!barrier.Reached && (shownFrame >= 0)) {
        const s32 confirmed = shownFrame - kPredictionWindow - 1;
        const size_t index = GetSnapshotIndex(confirmed);
        if ((confirmed >= 0) && (sNet.SnapshotFrames[index] == confirmed) && sNet.TransitionAt[index]) {
            barrier.Reached = true;
            barrier.Frame = confirmed;
            barrier.Since = std::chrono::steady_clock::now();
            NetLog("Segment %u ends at frame %d", sNet.Segment, confirmed);
            fflush(stdout);
            if (sNet.Settings.ReachBarrier != nullptr) {
                sNet.Settings.ReachBarrier(sNet.Segment, confirmed);
            }
        }
    }
    if (!barrier.Reached) {
        return;
    }

    barrier.FramesWaited++;
    BarrierStatus status;
    if (sNet.Settings.GetBarrierStatus != nullptr) {
        status = sNet.Settings.GetBarrierStatus(sNet.Segment, barrier.Frame);
    } else {
        status = (barrier.FramesWaited >= kDirectBarrierFrames) ? BarrierStatus::Passed : BarrierStatus::Waiting;
    }
    if ((status == BarrierStatus::Waiting) && (std::chrono::steady_clock::now() - barrier.Since > kBarrierTimeout)) {
        status = BarrierStatus::Failed;
    }
    if (status == BarrierStatus::Passed) {
        EndSegment();
    } else if (status == BarrierStatus::Failed) {
        NetSession::Stop(SessionResult::PeerLeft, "Not every player reached the end of the race");
    }
}

void HandleSessionEvents() {
    int count = 0;
    GekkoSessionEvent** events = gekko_session_events(sNet.Gekko, &count);
    for (int i = 0; (i < count) && (sNet.Gekko != nullptr); i++) {
        const GekkoSessionEvent* event = events[i];
        switch (event->type) {
            case GekkoPlayerConnected:
                NetLog("Player %d connected", event->data.connected.handle + 1);
                break;
            case GekkoSessionStarted:
                NetLog("All players connected; race starting");
                sNet.State = SessionState::Running;
                break;
            case GekkoPlayerDisconnected: {
                const s32 player = event->data.disconnected.handle;
                NetLog("Player %d disconnected", player + 1);
                if (sNet.SegmentEnd.Reached) {
                    // Peers leave a segment as soon as they know everyone confirmed its end; the barrier decides.
                    break;
                }
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
                NetLog("DESYNC at frame %d: local checksum %08x, player %d has %08x", desync.frame,
                       desync.local_checksum, desync.remote_handle + 1, desync.remote_checksum);
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

void UnpackInputs(const GekkoGameEvent::GekkoEventData::GekkoAdvance& advance, OSContPad* pads,
                  PadControl* controls) {
    const PadInput* inputs = reinterpret_cast<const PadInput*>(advance.inputs);
    const size_t numPlayers = advance.input_len / sizeof(PadInput);
    for (size_t i = 0; i < kMaxPlayers; i++) {
        memset(&pads[i], 0, sizeof(OSContPad));
        controls[i] = PadControl::None;
        if (i < numPlayers) {
            pads[i].stick_x = inputs[i].StickX;
            pads[i].stick_y = inputs[i].StickY;
            pads[i].button = inputs[i].Buttons & ~kControlMask;
            controls[i] = (PadControl) ((inputs[i].Buttons & kControlMask) >> kControlShift);
        }
    }
}

/** Acts on the players' requests at the end of a frame. The first request wins; the game then starts leaving. */
void ApplyControls(const PadControl* controls) {
    if (IsTransitionPending()) {
        return;
    }
    for (s8 player = 0; player < (s8) kMaxPlayers; player++) {
        const bool host = (player == 0);
        switch (controls[player]) {
            case PadControl::Leave:
                func_80290338(); // The pause menu's "Quit"
                break;
            case PadControl::Restart:
                if (!host) {
                    continue;
                }
                func_802903B0(); // The results menu's "Retry"
                break;
            case PadControl::NextCourse:
                if (!host || (gModeSelection != GRAND_PRIX)) {
                    continue;
                }
                // What confirming the Grand Prix results does. set_next_course() only checks for the cup's last
                // course after a race the players finished, which this skips.
                gIsInQuitToMenuTransition = 1;
                gQuitToMenuTransitionCounter = 5;
                gRaceState = RACE_EXIT;
                if (GetCupCursorPosition() == GetCupSize() - 1) {
                    gGotoMode = ENDING;
                } else {
                    set_next_course();
                }
                break;
            default:
                continue;
        }
        sNet.Exit = ExitCause{ player, controls[player] };
        return;
    }
}

void SaveFrame(const GekkoGameEvent::GekkoEventData::GekkoSave& save) {
    const s32 frame = save.frame;
    const size_t index = GetSnapshotIndex(frame);
    SimState::Save(sNet.Snapshots[index]);
    sNet.SnapshotFrames[index] = frame;
    sNet.TransitionAt[index] = IsTransitionPending();
    sNet.ExitAt[index] = sNet.Exit;
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
    const size_t index = GetSnapshotIndex(frame);
    SimState::Load(sNet.Snapshots[index]);
    sNet.Exit = sNet.ExitAt[index];
}

PadControl GetLocalControl() {
    for (const TestControl& test : sNet.Settings.TestControls) {
        if (test.SegmentFrame == sNet.SegmentFrames) {
            if (test.Control == PadControl::Leave) {
                NetSession::RequestLeave();
            } else {
                sNet.PendingControl = test.Control;
            }
        }
    }
    return sNet.LeaveRequested ? PadControl::Leave : sNet.PendingControl;
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
    sNet.ExitGamestate = RACING;
    sNet.Segment = 1;
    sNet.ShownFrames = 0;
    sNet.LeaveRequested = false;
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
    SimState_RestoreBaseline();
    SimState_SetRollbackMode(true);
    SimState_SetHostControlsMenus(true);
    sNet.SetupCountAtWait = SimState_GetRaceSetupCount();
    RaceConfig_StartRace(settings.Config);
    sNet.State = SessionState::WaitingForRace;
}

void Stop(SessionResult result, const std::string& message) {
    if (sNet.State == SessionState::Off) {
        return;
    }
    EndGekkoSession(IsSuccess(result) ? "PASS" : "FAIL", message);
    sNet.CVars.Restore();
    SimState_SetRollbackMode(false);
    SimState_SetHostControlsMenus(false);
    // The game exits with _Exit(), which skips flushing stdio.
    fflush(stdout);
    sNet.Socket = nullptr;
    sNet.LeaveRequested = false;
    sNet.PendingControl = PadControl::None;
    sNet.Result = result;
    sNet.ResultMessage = message;
    sNet.State = SessionState::Off;
}

void RequestLeave() {
    if ((sNet.State != SessionState::Off) && !sNet.LeaveRequested) {
        sNet.LeaveRequested = true;
        sNet.LeaveRequestedAt = std::chrono::steady_clock::now();
    }
}

void RequestRestart() {
    if (CanRestart()) {
        sNet.PendingControl = PadControl::Restart;
    }
}

bool CanRestart() {
    return (sNet.State == SessionState::Running) && (sNet.Settings.LocalPlayer == 0) && !sNet.SegmentEnd.Reached &&
           (sNet.Settings.GetBarrierStatus != nullptr);
}

bool IsActive() {
    return sNet.State != SessionState::Off;
}

bool IsAtBarrier() {
    return (sNet.State != SessionState::Off) && sNet.SegmentEnd.Reached;
}

bool IsHeadingToPodium() {
    return sNet.ExitGamestate == ENDING;
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
    // Wait for a newly set up race: right after a segment ends the old one is still on screen, fading out.
    if ((sNet.State == SessionState::WaitingForRace) && (gGamestate == RACING) &&
        (SimState_GetRaceSetupCount() != sNet.SetupCountAtWait)) {
        StartGekkoSession();
    }
    if (sNet.LeaveRequested && (std::chrono::steady_clock::now() - sNet.LeaveRequestedAt > kLeaveTimeout)) {
        Stop(SessionResult::PeerLeft, "Left the race");
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
    input.Buttons = (input.Buttons & ~kControlMask) | (u16) ((u16) GetLocalControl() << kControlShift);
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
        UpdateBarrier(-1);
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

    bool drawn = false;
    for (int i = 0; i < count; i++) {
        const GekkoGameEvent* event = events[i];
        switch (event->type) {
            case GekkoAdvanceEvent: {
                // Once the game starts leaving the race, the segment is frozen: frames pass without changing it.
                if (IsTransitionPending()) {
                    break;
                }
                std::array<OSContPad, kMaxPlayers> pads;
                std::array<PadControl, kMaxPlayers> controls;
                UnpackInputs(event->data.adv, pads.data(), controls.data());
                SimState_AdvanceFrame(pads.data(), i == shownAdvance);
                ApplyControls(controls.data());
                drawn |= (i == shownAdvance);
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

    if (!drawn) {
        // Waiting for remote input, or frozen: show the current frame again.
        SimState_RedrawFrame();
    }
    if (IsTransitionPending()) {
        // Whatever started the transition, a restart request has nothing left to do.
        sNet.PendingControl = PadControl::None;
    }
    const s32 shownFrame = (shownAdvance >= 0) ? events[shownAdvance]->data.adv.frame : -1;
    if (shownAdvance >= 0) {
        const SessionSettings& settings = sNet.Settings;
        if ((settings.DesyncAtFrame > 0) && (sNet.ShownFrames + 1 == settings.DesyncAtFrame)) {
            gRandomSeed16 ^= 1;
        }
        sNet.SegmentFrames++;
        sNet.ShownFrames++;
        if ((settings.TotalFrames > 0) && (sNet.ShownFrames >= settings.TotalFrames)) {
            Stop(SessionResult::Completed, "Race complete");
            return;
        }
    }
    UpdateBarrier(shownFrame);
}

void EndFrame() {
    // Segments freeze before the game leaves the race; this catches a way out that slipped past them.
    if (((sNet.State == SessionState::Connecting) || (sNet.State == SessionState::Running)) &&
        (gGamestate != RACING)) {
        Stop(SessionResult::RaceOver, "The race is over");
    }
}

} // namespace NetSession
