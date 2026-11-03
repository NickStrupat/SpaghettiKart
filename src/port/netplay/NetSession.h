#ifndef NETPLAY_NET_SESSION_H
#define NETPLAY_NET_SESSION_H

#include <libultraship/libultraship.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gekkonet.h>

#include "RaceConfig.h"
#include "SyntheticDriver.h"

/**
 * Requests a player sends along with its inputs. The simulation acts on them on the frame they belong to, so every
 * peer does the same thing at the same time.
 */
enum class PadControl : u8 {
    None = 0,
    Leave = 1,      // Any player: everyone quits to the menus
    Restart = 2,    // Player 1 (the host): race this course again
    NextCourse = 3, // Player 1, Grand Prix: move on as if the race had finished; a testing aid
};

enum class BarrierStatus { Waiting, Passed, Failed };

struct TestControl {
    u32 SegmentFrame; // Shown frame within each segment
    PadControl Control;
};

/**
 * One netplay race series: from the first race until the players leave it. Everything but LocalPlayer and the
 * address entries must be the same on every peer.
 *
 * A series is split into segments, one GekkoNet session per loaded race. When a race ends (a Grand Prix moving on,
 * a restart), every peer freezes the simulation on the frame that started the transition, waits until all peers
 * have confirmed that same frame, then loads the next race outside of any session: loading is never rolled back.
 */
struct SessionSettings {
    s32 LocalPlayer = 0;
    /** Each player's peer address, in player order, as GekkoNet sees them: opaque bytes. The local entry is unused. */
    std::vector<std::vector<u8>> Addresses;
    /** Carries GekkoNet's packets for one segment. Owned by the caller; must outlive the segment. */
    std::function<GekkoNetAdapter*(u32 segment)> GetAdapter;
    /**
     * Agree with the other peers that a segment ended on the given frame. Without these, the series ends with the
     * first race.
     */
    std::function<void(u32 segment, s32 frame)> ReachBarrier;
    std::function<BarrierStatus(u32 segment, s32 frame)> GetBarrierStatus;
    RaceConfig Config;

    // Testing aids
    std::optional<u32> BotSeed; // Drive the local player with seeded synthetic inputs
    SyntheticDriver::Style BotStyle = SyntheticDriver::Style::Racer;
    u32 TotalFrames = 0;        // End the race after this many shown frames; 0 never does
    u32 DelayMs = 0;            // Simulated one-way delay added to received packets
    u32 JitterMs = 0;           // Random extra delay, up to this many ms
    u32 LossPercent = 0;        // Percentage of received packets to drop
    std::string LogPath;        // After each segment, write each frame's checksum to <LogPath>.seg<N>
    u32 DesyncAtFrame = 0;      // Corrupt the local state at this frame, to test desync detection
    std::vector<TestControl> TestControls; // Requests the local player makes in every segment
    bool DumpSegmentStarts = false;        // Write the state each segment starts from to <LogPath>.seg<N>.start
    u32 LeaveAfterRaces = 0;               // Leave the series early in the race after this many; 0 never does
};

enum class SessionResult {
    Running,
    Completed,     // Reached SessionSettings::TotalFrames
    RaceOver,      // The series ended: the players left the race for the menus or the podium
    PeerLeft,      // Another player disconnected or ended the race
    Desync,        // The peers' simulations diverged
    ConnectFailed, // Not every player connected in time
    Failed,        // Could not start
};

/** @returns true for results that end a session as planned. */
inline bool IsSuccess(SessionResult result) {
    return (result == SessionResult::Completed) || (result == SessionResult::RaceOver);
}

/**
 * Runs a netplay race with GekkoNet. Each frame the session sends the local player's input, receives remote inputs,
 * and tells the game which frames to save, load (roll back to) and advance. Remote inputs that arrive late are
 * predicted; when a prediction was wrong the session loads an earlier snapshot and re-simulates with the real inputs.
 */
namespace NetSession {

/** 1 (the default): each peer shows only its own player's view, full screen. 0: the race's split screen. */
constexpr const char* kOwnViewCVar = "gNetplay.OwnView";
/** 1 (the default): each peer plays only its own player's camera's sounds. 0: every camera's, as split screen does. */
constexpr const char* kOwnAudioCVar = "gNetplay.OwnAudio";

/**
 * Applies the race settings and starts loading the race; the GekkoNet session starts once it has loaded.
 * Call after the frame's game logic: the menus overwrite the track selection while they run.
 */
void Start(const SessionSettings& settings);
/** Ends the session and restores the player's own settings. The game stays on the race screen. */
void Stop(SessionResult result, const std::string& message);

/** Asks every peer to leave the race; the series ends for all on the same frame. */
void RequestLeave();
/** Player 1 only: asks every peer to race the current course again. */
void RequestRestart();
/** @returns true when RequestRestart() can be used: this is player 1 and a race is running. */
bool CanRestart();

/** @returns true from Start() until Stop(). */
bool IsActive();
/** @returns true while this peer waits for the others to confirm the end of a segment. */
bool IsAtBarrier();
/** @returns true when the series ended because a Grand Prix finished: the game is going to the podium. */
bool IsHeadingToPodium();
s32 GetLocalPlayer();
u32 GetShownFrames();
/** @returns true when the series is within a prediction window of TotalFrames, so a peer ending is expected. */
bool IsNearPlannedEnd();
/** How the last session ended, and why. Running while one is active. */
SessionResult GetResult();
const std::string& GetResultMessage();

// The race loop; see Netplay.h.
void BeginFrame();
bool IsInRace();
bool ConsumeLocalPads(OSContPad* pads);
void RunRaceFrame();
/** Ends the session when the race is over. Call after the frame's game logic. */
void EndFrame();

} // namespace NetSession

#endif // NETPLAY_NET_SESSION_H
