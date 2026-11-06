#ifndef NETPLAY_CANONICAL_STICK_H
#define NETPLAY_CANONICAL_STICK_H

#include <libultraship/libultraship.h>

/**
 * Fewer rollbacks from a wobbling analog stick.
 *
 * GekkoNet predicts a missing remote input by repeating the player's last one and compares inputs byte for byte, so
 * every wobble of a stick is a wrong prediction and a rollback. Many stick positions mean the same to a race, though:
 * MK64 ignores the stick near the centre, steering stops growing near the edge, and the Y axis only matters for
 * backing up and throwing items. Before a race input is sent, its stick is replaced with one fixed position among
 * those the race treats the same.
 *
 * Every peer runs on the replaced inputs, so this cannot desync anything. It changes how the game plays only if race
 * code reads the stick in a way GetMeaning() in CanonicalStick.cpp does not cover: keep it in step with the readers
 * of a controller's rawStickX and rawStickY.
 */
namespace CanonicalStick {

/** Replaces a stick position with the one standing for every position the race treats the same. */
void Apply(s8* x, s8* y);

} // namespace CanonicalStick

#endif // NETPLAY_CANONICAL_STICK_H
