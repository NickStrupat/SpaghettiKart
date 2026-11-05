#include "NetSession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <vector>

#include "Checksum.h"
#include "NetLog.h"
#include "RaceEvents.h"
#include "SimState.h"
#include "SoundRollback.h"
#include "SyntheticDriver.h"

extern "C" {
#include "main.h"
#include "buffers.h"
#include "defines.h"
#include "code_800029B0.h"
#include "racing/race_logic.h"
#include "audio/external.h"
#include "menus.h"
}
#include "port/Game.h"

namespace {

// How far the session may run ahead of the slowest peer's confirmed input: 8 frames is 266 ms at 30 fps.
// GekkoNet predicts at most this many frames past the last input it received, so once a peer has advanced a frame,
// every input more than this many frames back is final.
constexpr unsigned char kPredictionWindow = 8;
// A race frame lasts 1/30 s.
constexpr f32 kFrameMs = 1000.0f / 30.0f;
// The automatic input delay covers the whole frames of the latency, favoring responsive controls: rollbacks hide the
// rest, under a frame. Beyond this many frames rollbacks hide all of it, since more delay would feel sluggish.
constexpr s32 kMaxAutoInputDelay = 3;
// Before any latency is measured
constexpr u8 kUnknownLatencyInputDelay = 1;
// Rollback needs the last prediction window + 2 states. A desync is only detected once the peers have exchanged
// that frame's checksum, so keep about a second of snapshots to still have the desynced frame for its report.
constexpr size_t kNumSnapshots = 32;
static_assert(kNumSnapshots >= kPredictionWindow + 2);
constexpr size_t kMaxPlayers = 4;
// GekkoNet averages frame advantage over 26 frames; re-check only once the average reflects earlier waits.
constexpr u32 kTimeSyncInterval = 26;
// The stats overlay sums up the last two seconds.
constexpr size_t kLiveStatsUpdates = 60;
// Peers load the race at different speeds; give up if they have not all connected after this long.
constexpr auto kConnectTimeout = std::chrono::seconds(30);
// Give up on peers that never confirm the end of a segment.
constexpr auto kBarrierTimeout = std::chrono::seconds(15);
// Without a barrier (direct test peers), keep the session up this many frames after the end so the others can
// confirm it too.
constexpr u32 kDirectBarrierFrames = 30;
// A leave request that has not ended the race by then (the session is stuck) ends it locally.
constexpr auto kLeaveTimeout = std::chrono::seconds(8);
// SessionSettings::LeaveAfterRaces leaves on this frame of the next race, once it is under way.
constexpr u32 kLeaveAfterRacesFrame = 30;
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

/** What one session update did, for the stats overlay. */
struct UpdateStats {
    u8 RollbackFrames = 0;
    bool Stalled = false;
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
    // Failure reports are written to <ReportPrefix>.<what>: the test log path, or a file per series in the game's
    // folder, where they can be fetched from both computers.
    std::string ReportPrefix;
    u32 ShownFrames = 0;
    u32 SegmentFrames = 0;
    u8 InputDelay = 0;

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

    std::array<UpdateStats, kLiveStatsUpdates> RecentUpdates;
    size_t NumRecentUpdates = 0;

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
    const std::string path = sNet.ReportPrefix + ".desync-p" + std::to_string(sNet.Settings.LocalPlayer) + "-s" +
                             std::to_string(sNet.Segment) + "-f" + std::to_string(frame) + ".txt";
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

/**
 * Writes the checksum breakdown of every frame still kept, for comparing peers after a failure: the peer that did not
 * detect a desync never learns its frame.
 */
void WriteKeptFramesReport() {
    if (sNet.Gekko == nullptr) {
        return;
    }
    const std::string path = sNet.ReportPrefix + ".seg" + std::to_string(sNet.Segment) + ".kept.txt";
    FILE* report = fopen(path.c_str(), "w");
    if (report == nullptr) {
        return;
    }
    std::vector<s32> frames;
    for (size_t i = 0; i < kNumSnapshots; i++) {
        if (sNet.SnapshotFrames[i] >= 0) {
            frames.push_back(sNet.SnapshotFrames[i]);
        }
    }
    std::sort(frames.begin(), frames.end());
    static SimSnapshot sCurrent;
    SimState::Save(sCurrent);
    for (s32 frame : frames) {
        SimState::Load(sNet.Snapshots[GetSnapshotIndex(frame)]);
        fprintf(report, "frame %d\n", frame);
        Sim_WriteChecksumBreakdown(report);
    }
    SimState::Load(sCurrent);
    fclose(report);
    NetLog("Kept frames %d-%d written to %s", frames.empty() ? -1 : frames.front(),
           frames.empty() ? -1 : frames.back(), path.c_str());
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
           "%u time-sync waits, ping %.0f ms, %s",
           verdict, message.c_str(), sNet.Segment, sNet.SegmentFrames, rollbacks.total_count, rollbacks.avg_frames,
           rollbacks.max_frames, rollbacks.total_stalls, sNet.WaitedFrames, network.avg_ping,
           SoundRollback::GetSummary().c_str());
    gekko_destroy(&sNet.Gekko);
    for (DelayedPacket& packet : sNet.DelayedPackets) {
        FreePacket(packet.Result);
    }
    sNet.DelayedPackets.clear();
    if (!sNet.Settings.LogPath.empty()) {
        WriteChecksumLog();
        SoundRollback::WriteLog(sNet.Settings.LogPath + ".seg" + std::to_string(sNet.Segment) + ".sounds");
    }
    SoundRollback::Stop();
    fflush(stdout);
}

/** The player's choice, or for automatic delay, the latency to the slowest other player in frames. */
u8 ChooseInputDelay(s32* latencyMs) {
    *latencyMs = (sNet.Settings.GetLatencyMs != nullptr) ? sNet.Settings.GetLatencyMs() : -1;
    const s32 chosen = CVarGetInteger(NetSession::kInputDelayCVar, NetSession::kAutoInputDelay);
    if (chosen != NetSession::kAutoInputDelay) {
        return (u8) std::clamp(chosen, 0, NetSession::kMaxInputDelay);
    }
    if (*latencyMs < 0) {
        return kUnknownLatencyInputDelay;
    }
    return (u8) std::min((s32) (*latencyMs / kFrameMs), kMaxAutoInputDelay);
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

    s32 latencyMs;
    sNet.InputDelay = ChooseInputDelay(&latencyMs);
    // Actor handles follow the order they are added in, so handle == player index.
    for (s32 player = 0; player < (s32) settings.Addresses.size(); player++) {
        if (player == settings.LocalPlayer) {
            gekko_add_actor(sNet.Gekko, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(sNet.Gekko, player, sNet.InputDelay);
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
    sNet.NumRecentUpdates = 0;
    sNet.ConnectStarted = std::chrono::steady_clock::now();
    sNet.State = SessionState::Connecting;
    NetLog("Segment %u started: player %d of %zu, input delay %u (latency %d ms)", sNet.Segment,
           settings.LocalPlayer, settings.Addresses.size(), sNet.InputDelay, latencyMs);
    RaceEvents::Reset();
    SoundRollback::Start(!settings.LogPath.empty());
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
    const u32 leaveAfter = sNet.Settings.LeaveAfterRaces;
    const u32 seriesRace = sNet.Segment - sNet.Settings.FirstSegment + 1;
    if ((leaveAfter > 0) && (seriesRace > leaveAfter) && (sNet.SegmentFrames == kLeaveAfterRacesFrame)) {
        NetSession::RequestLeave();
    }
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

/** @returns where this series' failure reports go: the test log, or netplay/<date-time> in the game's folder. */
std::string MakeReportPrefix(const SessionSettings& settings) {
    if (!settings.LogPath.empty()) {
        return settings.LogPath;
    }
    const std::string directory = Ship::Context::GetPathRelativeToAppDirectory("netplay");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    char name[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S", std::localtime(&now));
    return directory + "/" + name;
}

/**
 * The player this peer watches: its own, or on a shared console where it sits out the race (the race has fewer
 * players than the console has controllers), player 1.
 */
s32 GetWatchedPlayer() {
    return (sNet.Settings.LocalPlayer < gPlayerCount) ? sNet.Settings.LocalPlayer : 0;
}

s32 GetViewPlayer() {
    return (CVarGetInteger(NetSession::kOwnViewCVar, 1) != 0) ? GetWatchedPlayer() : -1;
}

} // namespace

namespace NetSession {

void Start(const SessionSettings& settings) {
    if (sNet.State != SessionState::Off) {
        Stop(SessionResult::Failed, "A new race started");
    }
    sNet.Settings = settings;
    sNet.ReportPrefix = MakeReportPrefix(settings);
    sNet.Result = SessionResult::Running;
    sNet.ResultMessage.clear();
    sNet.ExitGamestate = RACING;
    sNet.Segment = settings.FirstSegment;
    sNet.ShownFrames = 0;
    sNet.LeaveRequested = false;
    sNet.RandomState = 0x2545F491 ^ ((u32) (settings.LocalPlayer + 1) * 7919);
    sNet.Bot.reset();
    if (settings.BotSeed.has_value()) {
        sNet.Bot.emplace(*settings.BotSeed, settings.BotStyle);
    }

    if (!SimState_IsSupported()) {
        sNet.Result = SessionResult::Failed;
        sNet.ResultMessage = "This build cannot save simulation state, which netplay needs (build it with clang)";
        return;
    }
    sNet.CVars.Apply(settings.Config);
    if (settings.RaceSetUpByMenus) {
        // The race loaded this frame; its session starts before the race runs a frame.
        SimState_SetRollbackMode(true);
        SimState_SetHostControlsMenus(true);
        sNet.SetupCountAtWait = SimState_GetRaceSetupCount() - 1;
    } else {
        SimState_RestoreBaseline();
        SimState_SetRollbackMode(true);
        SimState_SetHostControlsMenus(true);
        sNet.SetupCountAtWait = SimState_GetRaceSetupCount();
        RaceConfig_StartRace(settings.Config);
    }
    sNet.State = SessionState::WaitingForRace;
}

void Stop(SessionResult result, const std::string& message) {
    if (sNet.State == SessionState::Off) {
        return;
    }
    if (!IsSuccess(result)) {
        WriteKeptFramesReport();
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

u32 GetSegment() {
    return sNet.Segment;
}

u8 GetInputDelay() {
    return sNet.InputDelay;
}

bool GetLiveStats(LiveStats* stats) {
    if ((sNet.State != SessionState::Running) || (sNet.Gekko == nullptr)) {
        return false;
    }
    stats->LocalPlayer = sNet.Settings.LocalPlayer;
    stats->PingMs.assign(sNet.Settings.Addresses.size(), -1);
    stats->Relayed.assign(sNet.Settings.Addresses.size(), false);
    for (s32 player = 0; player < (s32) stats->PingMs.size(); player++) {
        if (player != sNet.Settings.LocalPlayer) {
            GekkoNetworkStats network = {};
            gekko_network_stats(sNet.Gekko, player, &network);
            stats->PingMs[player] = (s32) network.avg_ping;
            stats->Relayed[player] = (sNet.Settings.IsRelayed != nullptr) && sNet.Settings.IsRelayed(player);
        }
    }
    stats->InputDelay = sNet.InputDelay;

    const size_t numUpdates = std::min(sNet.NumRecentUpdates, kLiveStatsUpdates);
    u32 rollbacks = 0;
    u32 rolledBackFrames = 0;
    stats->MaxRollbackFrames = 0;
    stats->Stalls = 0;
    for (size_t i = 0; i < numUpdates; i++) {
        const UpdateStats& update = sNet.RecentUpdates[i];
        rollbacks += (update.RollbackFrames > 0) ? 1 : 0;
        rolledBackFrames += update.RollbackFrames;
        stats->MaxRollbackFrames = std::max(stats->MaxRollbackFrames, (u32) update.RollbackFrames);
        stats->Stalls += update.Stalled ? 1 : 0;
    }
    stats->RollbackShare = (numUpdates > 0) ? (f32) rollbacks / numUpdates : 0.0f;
    stats->AvgRollbackFrames = (rollbacks > 0) ? (f32) rolledBackFrames / rollbacks : 0.0f;
    return true;
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
    // Sounds follow the view: this player's camera only, unless the player turned that off.
    const bool ownAudio = IsActive() && (CVarGetInteger(kOwnAudioCVar, 1) != 0);
    audio_set_only_camera(ownAudio ? (s8) GetWatchedPlayer() : -1);

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
        input = sNet.Bot->Next(GetWatchedPlayer());
    } else {
        input.StickX = pads[0].stick_x;
        input.StickY = pads[0].stick_y;
        // Pausing would only pause this peer.
        input.Buttons = pads[0].button & ~START_BUTTON;
    }
    if ((sNet.Settings.PressLAtFrame > 0) && (sNet.SegmentFrames + 1 == sNet.Settings.PressLAtFrame)) {
        input.Buttons |= L_TRIG;
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
        SimState_DrawFrame(GetViewPlayer());
        return;
    }
    if (sNet.WaitingThisFrame) {
        gekko_network_poll(sNet.Gekko);
        SimState_DrawFrame(GetViewPlayer());
        UpdateBarrier(-1);
        return;
    }

    int count = 0;
    GekkoGameEvent** events = gekko_update_session(sNet.Gekko, &count);

    // The last advance that is not a rollback is this update's new frame; earlier ones are drawn off-screen.
    int shownAdvance = -1;
    UpdateStats update;
    for (int i = 0; i < count; i++) {
        if (events[i]->type == GekkoAdvanceEvent) {
            if (events[i]->data.adv.rolling_back) {
                update.RollbackFrames++;
            } else {
                shownAdvance = i;
            }
        }
    }
    update.Stalled = (shownAdvance < 0);
    sNet.RecentUpdates[sNet.NumRecentUpdates++ % kLiveStatsUpdates] = update;

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
                SoundRollback::BeginFrame(event->data.adv.frame);
                SimState_AdvanceFrame(pads.data(), i == shownAdvance);
                SoundRollback::EndFrame();
                // Every time the frame is simulated, re-simulations included, or a rollback would undo it.
                if ((sNet.Settings.DesyncAtFrame > 0) && (event->data.adv.frame == (s32) sNet.Settings.DesyncAtFrame)) {
                    gRandomSeed16 ^= 1;
                }
                ApplyControls(controls.data());
                break;
            }
            case GekkoSaveEvent:
                SaveFrame(event->data.save);
                break;
            case GekkoLoadEvent:
                LoadFrame(event->data.load);
                SoundRollback::Rollback();
                break;
            default:
                break;
        }
    }
    SoundRollback::Settle((shownAdvance >= 0) ? events[shownAdvance]->data.adv.frame : -1);

    // The new frame, or while waiting for remote input or frozen, the current one again.
    SimState_DrawFrame(GetViewPlayer());
    if (IsTransitionPending()) {
        // Whatever started the transition, a restart request has nothing left to do.
        sNet.PendingControl = PadControl::None;
    }
    const s32 shownFrame = (shownAdvance >= 0) ? events[shownAdvance]->data.adv.frame : -1;
    if (shownAdvance >= 0) {
        const SessionSettings& settings = sNet.Settings;
        sNet.SegmentFrames++;
        sNet.ShownFrames++;
        RaceEvents::Update(sNet.SegmentFrames);
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
