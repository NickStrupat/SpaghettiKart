#ifndef NETPLAY_SIM_STATE_H
#define NETPLAY_SIM_STATE_H

#include <libultraship/libultraship.h>

/**
 * Netplay rollback savestates.
 *
 * The simulation's state is:
 *   - the globals and static locals of the simulation's source files, grouped into one section (SimSection.h),
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

/** Bracket the simulation half of a race frame. In rollback mode, routes its allocations into the sim arena. */
void SimState_BeginSimFrame(void);
void SimState_EndSimFrame(void);

/**
 * Advances the race by one frame from the given pads: simulates, draws and collects garbage, in the same order as
 * race_logic_loop(). Draw code also mutates simulation state, so every simulated frame must be drawn.
 * @param pads Four pads, one per local controller slot.
 * @param shown True for the frame that will be displayed. Other frames, such as those re-simulated after a
 *              rollback, are drawn into the off-screen gfx pool and discarded, without sounds or interpolation.
 */
void SimState_AdvanceFrame(OSContPad* pads, bool shown);

/** Draws the current race frame again and undoes the draw code's state changes, for frames netplay cannot advance. */
void SimState_RedrawFrame(void);

/** @returns true while SimState_AdvanceFrame() runs a frame that will not be shown, so audio can skip its sounds. */
bool SimState_IsResimulating(void);

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

} // namespace SimState

#endif // __cplusplus

#endif // NETPLAY_SIM_STATE_H
