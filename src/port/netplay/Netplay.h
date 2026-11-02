#ifndef NETPLAY_NETPLAY_H
#define NETPLAY_NETPLAY_H

#include <libultraship/libultraship.h>

/**
 * Online races with rollback netcode (GekkoNet).
 *
 * During a netplay race the session owns the race loop: every frame it sends the local player's input,
 * receives remote inputs, and tells the game which frames to save, load (roll back to) and advance.
 * Remote inputs that arrive late are predicted; when a prediction was wrong the session loads an
 * earlier snapshot and re-simulates the frames since then with the corrected inputs.
 *
 * Until the lobby exists (phase 3), a session is started from environment variables for testing:
 *   SK_NETPLAY_PLAYER=0                 This peer's player index.
 *   SK_NETPLAY_PEERS=127.0.0.1:7000,127.0.0.1:7001
 *                                       One UDP address per player, in player order. Each peer binds the
 *                                       port of its own entry.
 * Optional:
 *   SK_NETPLAY_MODE / _TRACK / _SEED    Race settings, as RaceConfig::FromEnvironment() reads them.
 *   SK_NETPLAY_BOT=1                    Drive the local player with seeded synthetic inputs.
 *   SK_NETPLAY_FRAMES=1800              Exit after this many shown race frames (0 runs until quit).
 *   SK_NETPLAY_DELAY_MS=60              Simulated one-way delay added to received packets.
 *   SK_NETPLAY_JITTER_MS=20             Random extra delay, up to this many ms.
 *   SK_NETPLAY_LOSS=5                   Percentage of received packets to drop.
 *   SK_NETPLAY_LOG=<file>               On exit, write each frame's checksum, to compare peers.
 *   SK_NETPLAY_DESYNC_AT=300            Corrupt this peer's state at that frame, to test desync detection.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Reads the environment. Call once after the engine has started. */
void Netplay_Init(void);

/** Starts the session once its race has loaded. Call every frame after the gamestate update. */
void Netplay_BeginFrame(void);

/** @returns true while a netplay session runs the race. */
bool Netplay_IsInRace(void);

/**
 * Hands the local player's input (pads[0]) to the session.
 * @returns true when netplay took the pads; controllers are then updated as the session advances frames.
 */
bool Netplay_ConsumeLocalPads(OSContPad* pads);

/**
 * Runs one netplay race frame in place of race_logic_loop(): rolls back and re-simulates as the session asks,
 * then simulates and draws the next frame, or redraws the current one while waiting for remote input.
 */
void Netplay_RunRaceFrame(void);

/** Starts the test race once the game has booted. Call after the frame's game logic. */
void Netplay_EndFrame(void);

/** @returns true once a test session is done and the game should exit. */
bool Netplay_IsFinished(void);

/** @returns the process exit code for a finished test session: 0 on success, 1 on desync or disconnect. */
s32 Netplay_GetExitCode(void);

#ifdef __cplusplus
}
#endif

#endif // NETPLAY_NETPLAY_H
