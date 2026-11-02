#ifndef NETPLAY_DETERMINISM_HARNESS_H
#define NETPLAY_DETERMINISM_HARNESS_H

#include <libultraship/libultraship.h>

/**
 * Determinism harness: plays a race from a fixed RaceConfig with scripted inputs and logs a gameplay
 * checksum for every frame. Running the same log on another build or platform reports the first frame
 * whose simulation differs. Netplay can only work if this passes between every pair of platforms.
 *
 * Driven by environment variables, so it can run unattended:
 *   SK_DETERMINISM_RECORD=<file>   Play a race with seeded synthetic inputs and write the log.
 *   SK_DETERMINISM_VERIFY=<file>   Replay the log's inputs and compare checksums.
 * Recording options (all optional):
 *   SK_DETERMINISM_FRAMES=1800     Race frames to run (30 per second).
 *   SK_DETERMINISM_MODE=gp|vs|battle
 *   SK_DETERMINISM_PLAYERS=1..4    Human players driven by synthetic inputs.
 *   SK_DETERMINISM_TRACK=mk:luigi_raceway   Track resource name (VS and Battle).
 *   SK_DETERMINISM_SEED=1          Seeds both the game RNG and the synthetic inputs.
 * Either mode:
 *   SK_DETERMINISM_TRACE=<file>    Write per-component checksums for the first frames, and diagnostics on failure.
 *   SK_DETERMINISM_ROLLBACK=8      Rollback stress test: after every frame, load the state from 8 frames earlier
 *                                  and re-simulate up to the current frame, checking each re-simulated checksum.
 * The game exits when the run finishes: exit code 0 on success, 1 on a mismatch or error.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Reads the environment. Call once after the engine has started. */
void Harness_Init(void);

/** Detects the start of the harness race. Call every frame after the gamestate update. */
void Harness_BeginFrame(void);

/**
 * Replaces this frame's pads with the harness's pads while a harness race runs.
 * @returns true when the pads were replaced and must be applied to every controller unconditionally.
 */
bool Harness_OverridePads(OSContPad* pads);

/**
 * Starts the harness race once the game has booted, then records or verifies the checksum of each race frame.
 * Call after the frame's game logic.
 */
void Harness_EndFrame(void);

/** @returns true once the run is done and the game should exit. */
bool Harness_IsFinished(void);

/** @returns the process exit code for a finished run. */
s32 Harness_GetExitCode(void);

#ifdef __cplusplus
}
#endif

#endif // NETPLAY_DETERMINISM_HARNESS_H
