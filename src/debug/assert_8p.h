#ifndef ASSERT_8P_H
#define ASSERT_8P_H

/**
 * @file Runtime invariants for the eight-player work.
 *
 * This codebase does not crash when an assumption breaks. Every failure mode
 * found so far -- a mode falling past a switch, a player count matching no arm,
 * a screen-mode test standing in for a racer count, PIF RAM overrunning its
 * neighbours -- produces a game that still runs and still looks plausible while
 * being wrong. Playtesting reports success in all four cases.
 *
 * These checks exist to convert that class of silence into a printed line. They
 * are cheap, they run during a race, and each distinct failure is reported once
 * per race so a broken frame does not flood the console.
 *
 * Everything asserted here holds in vanilla with one to four players. The point
 * is that they stop holding the moment the fifth appears, and they say so.
 */

typedef enum Assert8pId {
    /* A racer slot is occupied but sits beyond what the per-player passes cover,
       so that kart is simulated and never drawn. */
    ASSERT_8P_ROSTER_COVERAGE,
    /* Finishing positions are not a bijection over the active racers: two karts
       share a position, or a position is missing. */
    ASSERT_8P_RANK_BIJECTION,
    /* More viewports are wanted than gScreenContexts can hold. */
    ASSERT_8P_SCREEN_CAPACITY,
    /* More humans than the emulated controller hardware can carry. */
    ASSERT_8P_CONTROLLER_LIMIT,

    ASSERT_8P_ID_COUNT
} Assert8pId;

/** Reset the once-per-race latches and print the starting roster. */
void assert_8p_race_start(void);

/** Check the per-frame invariants. Safe to call every frame during a race. */
void assert_8p_frame(void);

#endif /* ASSERT_8P_H */
