#include <stdio.h>

#include <macros.h>
#include <defines.h>
#include <common_structs.h>
#include <racers.h>
#include <screen_class.h>

#include "assert_8p.h"
#include "main.h"
#include "menus.h"
#include "code_800029B0.h"
#include "racing/race_logic.h"

/**
 * The emulated Serial Interface packs one 8-byte request frame per controller
 * into __osContPifRam.ramarray, declared u32[15] in src/os/controller.h. Four
 * frames plus the terminator fit in those 60 bytes; eight do not. Raising the
 * controller count without moving players 5-8 onto a path that bypasses PIF
 * emulation overruns the buffer and corrupts whatever follows it in memory,
 * silently. This is the ceiling that check exists to announce.
 */
#define PIF_CONTROLLER_CAPACITY 4

/* One bit per Assert8pId. Cleared at race start so a failure is reported once
   per race rather than once per frame. */
static u32 sReported = 0;

static s32 already_reported(Assert8pId id) {
    if (sReported & (1U << id)) {
        return 1;
    }
    sReported |= (1U << id);
    return 0;
}

/** True for racers the ranking system itself considers active. Mirrors the
    filter in update_race_position_data() so the two agree on the population. */
static s32 is_ranked_racer(const Player* player) {
    return ((player->type & PLAYER_EXISTS) != 0) && ((player->type & PLAYER_CINEMATIC_MODE) == 0) &&
           ((player->type & PLAYER_INVISIBLE_OR_BOMB) == 0);
}

static s32 occupied_slot_count(void) {
    s32 count = 0;
    s32 i;

    for (i = 0; i < NUM_PLAYERS; i++) {
        if ((gPlayers[i].type & PLAYER_EXISTS) != 0) {
            count++;
        }
    }
    return count;
}

static s32 human_slot_count(void) {
    s32 count = 0;
    s32 i;

    for (i = 0; i < NUM_PLAYERS; i++) {
        if (((gPlayers[i].type & PLAYER_EXISTS) != 0) && ((gPlayers[i].type & PLAYER_HUMAN) != 0)) {
            count++;
        }
    }
    return count;
}

/**
 * Every occupied racer slot must fall within the range the per-player passes
 * cover, or that kart is simulated without ever being drawn.
 *
 * This is the invariant that nearly went wrong: the passes in render_player.c
 * gate players 5-8 on the screen mode, which reads as a detail decision but is
 * really asking how many karts exist. Under eight players in a small viewport
 * those two questions give opposite answers, and the result is a kart that
 * collides, takes items and finishes the race while remaining invisible.
 */
static void check_roster_coverage(void) {
    s32 covered = racers_to_process(gActiveScreenMode);
    s32 i;

    for (i = 0; i < NUM_PLAYERS; i++) {
        if (((gPlayers[i].type & PLAYER_EXISTS) != 0) && (i >= covered)) {
            if (!already_reported(ASSERT_8P_ROSTER_COVERAGE)) {
                printf("[8P-ASSERT] roster coverage: slot %d is occupied but per-player passes cover only %d slots "
                       "(screen mode %d, class %d). That kart is simulated and never drawn.\n",
                       i, covered, gActiveScreenMode, (s32) screen_mode_class(gActiveScreenMode));
            }
            return;
        }
    }
}

/**
 * Finishing positions must be a bijection over the ranked racers: every active
 * kart holds a distinct position inside [0, n). A count test that matches no arm
 * tends to leave positions stale or duplicated rather than produce an obvious
 * error, so a plausible-looking race can still be ranking eight karts as four.
 */
static void check_rank_bijection(void) {
    s32 seen[NUM_PLAYERS];
    s32 active = 0;
    s32 i;

    /* Positions are only meaningful once the race is running. During staging
       they have not been computed yet and every racer reads as position zero,
       which would look exactly like a duplicate. A check that cries wolf on the
       first frame of every race teaches you to ignore it, so don't run it until
       the data it inspects exists. */
    if (gRaceState != RACE_IN_PROGRESS) {
        return;
    }

    for (i = 0; i < NUM_PLAYERS; i++) {
        seen[i] = 0;
    }

    for (i = 0; i < NUM_PLAYERS; i++) {
        s32 rank;

        if (!is_ranked_racer(&gPlayers[i])) {
            continue;
        }
        active++;

        rank = (s32) gPlayers[i].currentRank;
        if ((rank < 0) || (rank >= NUM_PLAYERS)) {
            if (!already_reported(ASSERT_8P_RANK_BIJECTION)) {
                printf("[8P-ASSERT] rank bijection: player %d holds position %d, outside [0,%d).\n", i, rank,
                       NUM_PLAYERS);
            }
            return;
        }

        if (seen[rank] != 0) {
            if (!already_reported(ASSERT_8P_RANK_BIJECTION)) {
                printf("[8P-ASSERT] rank bijection: position %d held by more than one racer (player %d duplicates "
                       "it).\n",
                       rank, i);
            }
            return;
        }
        seen[rank] = 1;
    }

    /* With n active racers the held positions should be exactly 0..n-1. A gap
       means somebody was ranked out of range or skipped entirely. */
    for (i = 0; i < active; i++) {
        if (seen[i] == 0) {
            if (!already_reported(ASSERT_8P_RANK_BIJECTION)) {
                printf("[8P-ASSERT] rank bijection: %d racers active but position %d is unheld.\n", active, i);
            }
            return;
        }
    }
}

/**
 * Every human must have a screen context that is both allocated and actually
 * dimensioned.
 *
 * Allocation alone is not the interesting property. Widening gScreenContexts is
 * a one-character change, and once it is wide enough a pure capacity test passes
 * for eight humans while contexts 4-7 still hold the zeros they were defined
 * with -- an array big enough to be indexed safely and a viewport with no size.
 * That renders as a player whose view never appears, which is precisely the kind
 * of quiet wrongness this file exists to catch. So check the geometry, not the
 * bound: a zero-area viewport means the per-mode layout switch has no arm for
 * the current mode.
 */
static void check_screen_contexts(void) {
    s32 i;

    /* Layout is assigned during render setup, so only meaningful once racing. */
    if (gRaceState != RACE_IN_PROGRESS) {
        return;
    }

    for (i = 0; i < NUM_PLAYERS; i++) {
        if (((gPlayers[i].type & PLAYER_EXISTS) == 0) || ((gPlayers[i].type & PLAYER_HUMAN) == 0)) {
            continue;
        }

        if (i >= ARRAY_COUNT(gScreenContexts)) {
            if (!already_reported(ASSERT_8P_SCREEN_CAPACITY)) {
                printf("[8P-ASSERT] screen contexts: human in slot %d but gScreenContexts holds only %d.\n", i,
                       ARRAY_COUNT(gScreenContexts));
            }
            return;
        }

        if ((gScreenContexts[i].screenWidth <= 0) || (gScreenContexts[i].screenHeight <= 0)) {
            if (!already_reported(ASSERT_8P_SCREEN_CAPACITY)) {
                printf("[8P-ASSERT] screen contexts: human in slot %d has a %dx%d viewport -- the layout switch has "
                       "no arm for screen mode %d.\n",
                       i, (s32) gScreenContexts[i].screenWidth, (s32) gScreenContexts[i].screenHeight,
                       gActiveScreenMode);
            }
            return;
        }
    }
}

/**
 * More humans than the emulated controller hardware can carry. See the note on
 * PIF_CONTROLLER_CAPACITY above -- the limit is the emulated PIF RAM buffer, not
 * an array size, so exceeding it corrupts memory rather than dropping input.
 */
static void check_controller_limit(void) {
    s32 humans = human_slot_count();

    if (humans > PIF_CONTROLLER_CAPACITY) {
        if (!already_reported(ASSERT_8P_CONTROLLER_LIMIT)) {
            printf("[8P-ASSERT] controller limit: %d humans but emulated PIF RAM carries %d controller frames. "
                   "Players %d+ need a path that bypasses SI/PIF emulation.\n",
                   humans, PIF_CONTROLLER_CAPACITY, PIF_CONTROLLER_CAPACITY + 1);
        }
    }
}

void assert_8p_race_start(void) {
    s32 occupied = occupied_slot_count();
    s32 humans = human_slot_count();

    sReported = 0;

    printf("[8P-ASSERT] race start: %d slots occupied (%d human, %d CPU), screen mode %d (class %d), "
           "gPlayerCount %d, per-player passes cover %d slots.\n",
           occupied, humans, occupied - humans, gActiveScreenMode, (s32) screen_mode_class(gActiveScreenMode),
           (s32) gPlayerCount, racers_to_process(gActiveScreenMode));
}

void assert_8p_frame(void) {
    check_roster_coverage();
    check_rank_bijection();
    check_screen_contexts();
    check_controller_limit();
}
