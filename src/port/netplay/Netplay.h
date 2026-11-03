#ifndef NETPLAY_NETPLAY_H
#define NETPLAY_NETPLAY_H

#include <libultraship/libultraship.h>

/**
 * Online races with rollback netcode. Players meet in a room on a relay server (see Lobby.h and the Netplay window);
 * NetSession.h runs the race itself.
 *
 * Automated tests start without the window, from environment variables. Either peers connect directly:
 *   SK_NETPLAY_PEERS=127.0.0.1:7000,127.0.0.1:7001
 *                                       One UDP address per player, in player order. Each peer binds the
 *                                       port of its own entry.
 *   SK_NETPLAY_PLAYER=0                 This peer's player index.
 *   SK_NETPLAY_MODE / _TRACK / _SEED    Race settings, as RaceConfig::FromEnvironment() reads them.
 * or bots meet through a relay (tools/netplay-relay):
 *   SK_NETPLAY_RELAY=127.0.0.1:7777     Relay server address.
 *   SK_NETPLAY_HOST=1                   Create the room; otherwise join it.
 *   SK_NETPLAY_ROOM_FILE=<file>         The host writes the room code here; guests wait for it.
 *   SK_NETPLAY_ROOM=ABC234              Or: the code of the room to join.
 *   SK_NETPLAY_PLAYERS=2                Host: start once this many players are ready.
 *   SK_NETPLAY_RACES=1                  Host: races to run before exiting.
 *   SK_NETPLAY_MODE / _TRACK / _CUP     Host: race settings.
 * Options for both:
 *   SK_NETPLAY_BOT=1                    Drive the local player with seeded synthetic inputs.
 *   SK_NETPLAY_FRAMES=1800              End the series after this many shown frames (0 runs until it ends).
 *   SK_NETPLAY_DELAY_MS=60              Simulated one-way delay added to received packets.
 *   SK_NETPLAY_JITTER_MS=20             Random extra delay, up to this many ms.
 *   SK_NETPLAY_LOSS=5                   Percentage of received packets to drop.
 *   SK_NETPLAY_LOG=<file>               After each segment, write each frame's checksum, to compare peers.
 *   SK_NETPLAY_DESYNC_AT=300            Corrupt this peer's state at that frame, to test desync detection.
 *   SK_NETPLAY_TEST_CONTROLS=restart@300,next@200,leave@500
 *                                       Send these requests on that frame of every race (see PadControl).
 *   SK_NETPLAY_DUMP=1                   Write the state each segment starts from, for tools/netplay-diff.py.
 *   SK_NETPLAY_TEST_RESET=1             Return to the menus before each lobby race, as a peer outside them does.
 *   SK_NETPLAY_TEST_START_DELAY=900     Host: idle this many frames in the room before starting (the title screen
 *                                       starts its attract demo race after 10 s).
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Reads the environment. Call once after the engine has started. */
void Netplay_Init(void);

/** Talks to the lobby and starts the session once its race has loaded. Call every frame after the gamestate update. */
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

/** Ends races that are over and starts the ones the lobby asks for. Call after the frame's game logic. */
void Netplay_EndFrame(void);

/** @returns true once a test session is done and the game should exit. */
bool Netplay_IsFinished(void);

/** @returns the process exit code for a finished test session: 0 on success, 1 on desync or disconnect. */
s32 Netplay_GetExitCode(void);

/** Ends any race and leaves the room. Call before the engine saves the config, so overridden settings are not kept. */
void Netplay_Shutdown(void);

#ifdef __cplusplus
}

namespace Netplay {
/** Ends the race for everyone, on the same frame, and returns them all to the room. */
void LeaveRace();
/** Host only: everyone races the current course again. */
void RestartRace();
bool CanRestartRace();
/** @returns true while this player is in, or loading, a netplay race. */
bool IsRaceRunning();
} // namespace Netplay
#endif

#endif // NETPLAY_NETPLAY_H
