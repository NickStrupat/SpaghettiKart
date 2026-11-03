#ifndef NETPLAY_RACE_EVENTS_H
#define NETPLAY_RACE_EVENTS_H

#include <libultraship/libultraship.h>

/**
 * Logs what happens in a race, for unattended tests: laps, finishes and places, Battle eliminations, and the race
 * reaching its results. Lines start with "[Netplay] Race:", for tools/netplay-sim.py.
 */
namespace RaceEvents {

/** Forgets the previous race. Call when a race starts. */
void Reset();

/**
 * Logs what changed since the last call. Call once per displayed frame.
 * @param frame The frame number to report.
 */
void Update(u32 frame);

} // namespace RaceEvents

#endif // NETPLAY_RACE_EVENTS_H
