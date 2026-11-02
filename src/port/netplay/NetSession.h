#ifndef NETPLAY_NET_SESSION_H
#define NETPLAY_NET_SESSION_H

#include <libultraship/libultraship.h>

#include <optional>
#include <string>
#include <vector>

#include <gekkonet.h>

#include "RaceConfig.h"

/** One netplay race. Everything but LocalPlayer and the addresses must be the same on every peer. */
struct SessionSettings {
    s32 LocalPlayer = 0;
    /** Each player's peer address, in player order, as GekkoNet sees them: opaque bytes. The local entry is unused. */
    std::vector<std::vector<u8>> Addresses;
    /** Carries GekkoNet's packets. Owned by the caller, and must outlive the session. */
    GekkoNetAdapter* Adapter = nullptr;
    RaceConfig Config;

    // Testing aids
    std::optional<u32> BotSeed; // Drive the local player with seeded synthetic inputs
    u32 TotalFrames = 0;        // End the race after this many shown frames; 0 never does
    u32 DelayMs = 0;            // Simulated one-way delay added to received packets
    u32 JitterMs = 0;           // Random extra delay, up to this many ms
    u32 LossPercent = 0;        // Percentage of received packets to drop
    std::string LogPath;        // When the race ends, write each frame's checksum here, to compare peers
    u32 DesyncAtFrame = 0;      // Corrupt the local state at this frame, to test desync detection
};

enum class SessionResult {
    Running,
    Completed,     // Reached SessionSettings::TotalFrames
    RaceOver,      // The race ended and the game left it
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

/**
 * Applies the race settings and starts loading the race; the GekkoNet session starts once it has loaded.
 * Call after the frame's game logic: the menus overwrite the track selection while they run.
 */
void Start(const SessionSettings& settings);
/** Ends the session and restores the player's own settings. The game stays on the race screen. */
void Stop(SessionResult result, const std::string& message);

/** @returns true from Start() until Stop(). */
bool IsActive();
s32 GetLocalPlayer();
u32 GetShownFrames();
/** @returns true when the session is within a prediction window of TotalFrames, so a peer ending is expected. */
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
