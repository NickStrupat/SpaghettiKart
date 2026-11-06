#ifndef NETPLAY_SIM_STATE_H
#define NETPLAY_SIM_STATE_H

#include <libultraship/libultraship.h>

/**
 * Netplay rollback savestates.
 *
 * The simulation's state is:
 *   - the globals and static locals of the simulation's source files, grouped into sections (SimSection.h),
 *   - the C++ objects in the sim arena (SimArena.h),
 *   - the headers of the World containers that own those objects.
 * A snapshot copies all three; loading it copies them back.
 *
 * Rollback mode must be enabled before a race is set up. From then on, race setup and every simulated frame
 * allocate their C++ objects in the sim arena. Offline play leaves it off and is unaffected.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @returns true when this build can save and restore simulation state (clang builds). */
bool SimState_IsSupported(void);

/** Enables or disables rollback mode. Takes effect at the next race setup. */
void SimState_SetRollbackMode(bool enabled);
bool SimState_IsRollbackMode(void);

/** Bracket setup_race(). In rollback mode, rebuilds the World and track inside the sim arena. */
void SimState_BeginRaceSetup(void);
void SimState_EndRaceSetup(void);
/** @returns how many races have been set up, to notice when a new one has loaded. */
u32 SimState_GetRaceSetupCount(void);

/**
 * When enabled, menus that read the combined controller (results, "continue" prompts) take input from player 1
 * only, instead of from every player at once: in netplay, player 1 is the host, and two players pushing the same
 * cursor in different directions would fight over it.
 */
void SimState_SetHostControlsMenus(bool enabled);
/** Clears every controller's held and pressed buttons, so each peer starts a session from the same state. */
void SimState_ResetControllers(void);

/**
 * Puts the simulation's globals back to how they were before this run's first race was set up, keeping the state
 * that outlives races: the gamestate, save data, controller paks and the sound mode.
 * A race leaves values behind that the next race's setup never resets, and each peer has its own history of
 * attract demos and offline races, so netplay starts every race series from this baseline.
 */
void SimState_RestoreBaseline(void);

/**
 * Keeps a copy of the simulation's globals as they are at boot, before the first frame. Every peer boots the same
 * code and data, so netplay starts a room's menus from this copy (MenuSession.h).
 */
void SimState_CaptureBootState(void);
/** Puts the globals back to the boot copy, keeping the same state as SimState_RestoreBaseline(). */
void SimState_RestoreBootState(void);

/** Bracket the simulation half of a race frame. In rollback mode, routes its allocations into the sim arena. */
void SimState_BeginSimFrame(void);
void SimState_EndSimFrame(void);

/**
 * Advances the race by one frame from the given pads: simulates, draws and collects garbage, in the same order as
 * race_logic_loop(). Draw code also mutates simulation state, so every frame is drawn the same way on every peer:
 * split screen, into the off-screen gfx pool, then discarded. SimState_DrawFrame() draws what is displayed.
 * @param pads Four pads, one per local controller slot.
 * @param shown True for the frame that will be displayed. Other frames, such as those re-simulated after a
 *              rollback, run without sounds.
 */
void SimState_AdvanceFrame(OSContPad* pads, bool shown);

/**
 * Saves the state for SimState_EndDisplay() to put back, unless this display already has. Changes made from then on,
 * such as where the shown frame draws karts, are undone with the draw's own.
 */
void SimState_BeginDisplay(void);

/**
 * Draws the current race frame for display, without sounds. The draw's state changes are undone by
 * SimState_EndDisplay(), as the display list points into the state it drew.
 * @param viewPlayer The player whose view fills the screen (race_render_own_view()), or -1 for split screen.
 */
void SimState_DrawFrame(s32 viewPlayer);

/** Undoes SimState_DrawFrame(). Call every frame once its display list has run. */
void SimState_EndDisplay(void);

/** @returns true while a frame is simulated again, after a rollback. */
bool SimState_IsResimulating(void);
/** @returns true inside SimState_DrawFrame(): the frame's sounds played when it was simulated. */
bool SimState_IsDrawingShownFrame(void);

#ifdef __cplusplus
}

#include <cstdio>
#include <vector>

/** One saved copy of the simulation state. Reuse snapshots to avoid reallocating their buffers. */
struct SimSnapshot {
    std::vector<u8> Section;
    std::vector<u8> Arena;
    std::vector<u8> World;
};

namespace SimState {

void Save(SimSnapshot& snapshot);
void Load(const SimSnapshot& snapshot);

/**
 * Writes the byte ranges where two snapshots differ, to find state that rollback fails to reproduce.
 * Section ranges are given as link-time addresses, so `nm` on the executable names the variables.
 */
void WriteDifferences(const SimSnapshot& a, const SimSnapshot& b, FILE* out);

/**
 * Writes a snapshot to a file, to compare the states of two peers with tools/netplay-diff.py: a text header
 * "SKSNAP <section count> (<name> <link address> <size>)... <arena size> <world size>", then the raw bytes.
 */
void WriteSnapshotFile(const SimSnapshot& snapshot, FILE* out);

} // namespace SimState

#endif // __cplusplus

#endif // NETPLAY_SIM_STATE_H
