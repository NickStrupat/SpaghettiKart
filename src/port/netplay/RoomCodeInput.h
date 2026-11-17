#ifndef NETPLAY_ROOM_CODE_INPUT_H
#define NETPLAY_ROOM_CODE_INPUT_H

#include <libultraship/libultraship.h>

#include <string>

/**
 * Typing a room code with a controller: a code is 7 stick directions (RelayProtocol::kCodeAlphabet, "UDLR"), each one
 * entered by flicking the stick, or pressing the D-pad or a C button (the keyboard's arrow keys are the C buttons by
 * default). B erases the last one. Shared by the ONLINE screen and the netplay window.
 *
 * A flick counts once the stick is well over along one axis and much less along the other, so a direction that is
 * not quite straight still counts as the nearest one. The stick has to come back near the middle before the next flick
 * counts, so holding it never enters a direction twice.
 */
namespace RoomCodeInput {

/** What one frame of a pad entered: nothing, a letter of the code, or an erase. */
enum class Entry { None, Up, Down, Left, Right, Erase };

class Reader {
  public:
    /**
     * @param stickX, stickY The stick this frame, as the game reads it (about -80 to 80, up positive).
     * @param pressed The buttons pressed this frame, not held from the last.
     */
    Entry Read(s8 stickX, s8 stickY, u16 pressed);
    /** Starts over: a stick already pushed counts only after it comes back to the middle. */
    void Reset();

  private:
    bool mStickArmed = false;
};

/** @returns the code letter of a direction ('U', 'D', 'L', 'R'), or 0 for None and Erase. */
char ToLetter(Entry entry);

/**
 * Applies an entry to a code being typed.
 * @returns true when the entry changed the code.
 */
bool Apply(Entry entry, std::string* code);

} // namespace RoomCodeInput

#endif // NETPLAY_ROOM_CODE_INPUT_H
