#ifndef SCREEN_GRID_H
#define SCREEN_GRID_H

#include "defines.h"
#include "mk64.h" /* SCREEN_WIDTH, SCREEN_HEIGHT */

/**
 * @file Where a player's viewport sits, as a grid position.
 *
 * Several HUD sites need to know which part of the screen a player occupies so
 * they can slide a marker inward, centre an item over a cell, or pick a corner
 * to anchor to. Every one of them currently answers that question with an
 * expression that hardcodes a 2x2 grid -- (playerId & 1) for the column, or
 * (playerId == 0 || playerId == 2) for the left-hand side. Those are correct
 * for four players and silently wrong for eight, where the low bit of a player
 * id says nothing about which of four columns they are in.
 *
 * Every split the game uses is a grid, so the layouts differ only in how many
 * columns and rows they have:
 *
 *     1P          1 x 1
 *     2P vertical    2 x 1
 *     2P horizontal  1 x 2
 *     3P/4P       2 x 2
 *     8P          4 x 2
 *
 * With that, column and row are just playerId % columns and playerId / columns,
 * and a cell centre is span/(2n) + index*(span/n) on each axis. That formula
 * reproduces every hand-written centre in set_screen(), including the 4x2 table
 * there -- 40/120/200/280 across and 60/180 down.
 *
 * Deriving the numbers rather than tabulating them a second time means the two
 * cannot drift apart.
 */

static inline s32 screen_grid_columns(s32 screenMode) {
    switch (screenMode) {
        case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL:
        case SCREEN_MODE_3P_4P_SPLITSCREEN:
            return 2;
        case SCREEN_MODE_8P:
            return 4;
        case SCREEN_MODE_1P:
        case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL:
        default:
            return 1;
    }
}

static inline s32 screen_grid_rows(s32 screenMode) {
    switch (screenMode) {
        case SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL:
        case SCREEN_MODE_3P_4P_SPLITSCREEN:
        case SCREEN_MODE_8P:
            return 2;
        case SCREEN_MODE_1P:
        case SCREEN_MODE_2P_SPLITSCREEN_VERTICAL:
        default:
            return 1;
    }
}

/** Which column of the grid this player's viewport occupies, leftmost is 0. */
static inline s32 screen_player_column(s32 screenMode, s32 playerId) {
    return playerId % screen_grid_columns(screenMode);
}

/** Which row of the grid this player's viewport occupies, topmost is 0. */
static inline s32 screen_player_row(s32 screenMode, s32 playerId) {
    return playerId / screen_grid_columns(screenMode);
}

static inline s32 screen_cell_center_x(s32 screenMode, s32 playerId) {
    s32 columns = screen_grid_columns(screenMode);
    s32 width = SCREEN_WIDTH / columns;

    return (width / 2) + (screen_player_column(screenMode, playerId) * width);
}

static inline s32 screen_cell_center_y(s32 screenMode, s32 playerId) {
    s32 rows = screen_grid_rows(screenMode);
    s32 height = SCREEN_HEIGHT / rows;
    s32 row = screen_player_row(screenMode, playerId);

    if (row >= rows) {
        row = rows - 1;
    }
    return (height / 2) + (row * height);
}

/**
 * True when this player's viewport sits in the right half of the screen.
 *
 * This is what the (playerId & 1) tests were reaching for. It gives the same
 * answer they do for every existing mode -- in a 2x2 grid the odd players are
 * exactly the right-hand column -- while staying meaningful with four columns,
 * where the two rightmost count as the right half.
 */
static inline s32 screen_cell_is_right_half(s32 screenMode, s32 playerId) {
    return screen_cell_center_x(screenMode, playerId) >= (SCREEN_WIDTH / 2);
}

#endif /* SCREEN_GRID_H */
