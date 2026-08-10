#ifndef RACERS_H
#define RACERS_H

#include "defines.h"

/**
 * @brief How many racer slots to process this frame.
 *
 * Several per-player passes -- kart setup, kart rendering, particles, shadows --
 * run over players one through four unconditionally and over players five
 * through eight only when the screen mode is not 3P/4P split-screen.
 *
 * That test reads like a level-of-detail decision and is not one. It is a proxy
 * for "does this race have more than four karts". The proxy holds in vanilla
 * only because 3P/4P split-screen occurs exclusively in Versus and Battle,
 * which cap at four racers, while Grand Prix fills all eight slots and always
 * runs in 1P or 2P. Under those constraints, skipping slots five through eight
 * in quarter-screen modes costs nothing and saves the work.
 *
 * An eight-player mode breaks the proxy in the worst direction: eight occupied
 * slots and the smallest viewport at the same time. Treating the condition as a
 * viewport-size test there would leave players five through eight unrendered --
 * karts that collide, take items and finish the race while being invisible.
 *
 * Centralising the decision here keeps today's behaviour byte-for-byte while
 * putting the eventual fix in one place: this function should come to depend on
 * how many racer slots are occupied, not on how the screen is divided.
 *
 * @param screenMode gActiveScreenMode, or gScreenModeSelection at configuration
 *                   time. Passed rather than read so callers stay explicit about
 *                   which they mean -- the active mode is forced to 1P during
 *                   cutscenes even in a multiplayer session.
 * @return the number of leading racer slots to process, 4 or NUM_PLAYERS.
 */
static inline s32 racers_to_process(s32 screenMode) {
    // @port 3P/4P Grand Prix races eight karts in this port (CPUs fill the empty
    // slots), so the proxy no longer holds in any mode: every slot is processed.
    (void) screenMode;
    return NUM_PLAYERS;
}

#endif /* RACERS_H */
