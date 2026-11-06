#ifndef NETPLAY_KART_SMOOTHING_H
#define NETPLAY_KART_SMOOTHING_H

#include <libultraship/libultraship.h>

#include <string>

/**
 * Other karts glide after a rollback instead of jumping.
 *
 * A rollback replaces predicted inputs with real ones, so the karts they steered can be somewhere else on the newest
 * frame than the shown frames led up to. The shown frame draws each such kart where it now is plus how far the
 * rollback moved it, and that offset fades over a few frames. Only the drawing changes: SimState_EndDisplay() undoes
 * whatever the shown frame changed, and the simulation never reads the offsets.
 *
 * Karts that a camera on screen follows stay where they are, or the view would slide around them: the player's own
 * kart in the own view, every player's in split screen. A correction too big to come from a few frames of other
 * inputs (a kart put back on the track) is shown as the jump it is.
 */
namespace KartSmoothing {

/** 1 (the default): other karts glide after a rollback. 0: they jump. */
constexpr const char* kCVar = "gNetplay.SmoothKarts";

/** Forgets every offset and the counts. Call when a session starts. */
void Reset();

/** Remembers where every kart is. Call before a session update, while the state is still the frame last shown. */
void BeginUpdate();

/**
 * Adds how far a rollback moved each kart to its offset. Call once the update has re-simulated the frames up to
 * the one it started from, before it advances further.
 */
void EndRollback();

/**
 * Moves the karts by their offsets for the shown draw, then fades the offsets. Call once per game frame, after
 * SimState_BeginDisplay(), so the move is undone with the rest of the draw.
 * @param viewPlayer The player whose view fills the screen, or -1 for split screen.
 */
void Apply(s32 viewPlayer);

/** What the session smoothed, for its log. */
std::string GetSummary();

} // namespace KartSmoothing

#endif // NETPLAY_KART_SMOOTHING_H
