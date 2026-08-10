#ifndef SCREEN_CLASS_H
#define SCREEN_CLASS_H

#include "defines.h"

/**
 * @brief Viewport size classes.
 *
 * Much of the game branches on the screen mode when what it actually wants to
 * know is "how big is the viewport I am drawing into" -- to pick a level of
 * detail, to skip an effect that would be invisible in a small window, or to
 * gate a positional sound. Those sites test mode identity (== SCREEN_MODE_1P,
 * != SCREEN_MODE_3P_4P_SPLITSCREEN) which silently stops being correct the
 * moment a new mode is added.
 *
 * Classifying instead of comparing keeps those sites working when a mode is
 * added: a new, smaller mode maps onto the smallest existing class and every
 * ordered test below continues to mean what it did before.
 *
 * Values are ordered largest viewport to smallest, so magnitude comparisons
 * read naturally: (class <= SCREEN_CLASS_HALF) is "at least half the screen".
 */
typedef enum ScreenClass {
    SCREEN_CLASS_FULL = 0,    /* 1P -- the whole screen */
    SCREEN_CLASS_HALF = 1,    /* 2P -- horizontal or vertical split */
    SCREEN_CLASS_QUARTER = 2, /* 3P/4P -- one quadrant */
    SCREEN_CLASS_EIGHTH = 3,  /* reserved: 5P-8P, added with SCREEN_MODE_8P */
} ScreenClass;

/**
 * @brief Map a screen mode to its viewport size class.
 *
 * Pure function of the mode value. Callers pass whichever global applies:
 * gActiveScreenMode for "what am I drawing right now" (this is forced to
 * SCREEN_MODE_1P during cutscenes and ceremonies even in a multiplayer
 * session), or gScreenModeSelection for "what did the menu choose".
 *
 * An unrecognised mode classifies as the smallest viewport, so a mode added
 * without updating this function degrades to conservative detail rather than
 * claiming a full screen it does not have.
 */
static inline ScreenClass screen_mode_class(s32 screenMode) {
    switch (screenMode) {
        case SCREEN_MODE_1P:
            return SCREEN_CLASS_FULL;
        case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL:
        case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL:
            return SCREEN_CLASS_HALF;
        case SCREEN_MODE_3P_4P_SPLITSCREEN:
            return SCREEN_CLASS_QUARTER;
        default:
            return SCREEN_CLASS_EIGHTH;
    }
}

#endif /* SCREEN_CLASS_H */
