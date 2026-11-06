#pragma once

#include <libultraship/libultraship.h>

#include <string>
#include <vector>

/**
 * Lets player 1 listen to one chosen gamepad instead of all of them, so two copies of the game on one computer can
 * each have their own controller. The choice is a CVar, so each settings folder (SHIP_HOME) keeps its own.
 *
 * The choice names a gamepad by its serial number, not its position: SDL finds gamepads in a different order in every
 * process, so the first gamepad of one copy can be the second of the other, and change at the next launch.
 */
namespace GamepadAssignment {

/** "": player 1 listens to every gamepad (the default). Otherwise the Id of the only gamepad it listens to. */
constexpr const char* kCVar = "gPlayerOneGamepadId";

struct Gamepad {
    s32 Number;     // 1-based, in Id order, so every copy of the game numbers the same gamepads alike
    std::string Id; // the serial number (the Bluetooth address on PlayStation pads), or SDL's GUID without one
    std::string Name;
    bool AnyButtonHeld;
};

/** Enforces the choice; call once per frame, as gamepads can connect and disconnect at any time. */
void Apply();

/** @returns the connected gamepads, sorted by Id. */
std::vector<Gamepad> GetGamepads();

} // namespace GamepadAssignment
