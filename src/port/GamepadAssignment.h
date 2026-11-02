#pragma once

#include <libultraship/libultraship.h>

#include <string>
#include <vector>

/**
 * Lets player 1 listen to one chosen gamepad instead of all of them, so two copies of the game on one computer can
 * each have their own controller. The choice is a CVar, so each settings folder (SHIP_HOME) keeps its own.
 */
namespace GamepadAssignment {

/** 0: player 1 listens to every gamepad (the default). n: only the nth connected gamepad. */
constexpr const char* kCVar = "gPlayerOneGamepad";

struct Gamepad {
    s32 Number; // 1-based, in connection order
    std::string Name;
    bool AnyButtonHeld;
};

/** Enforces the choice; call once per frame, as gamepads can connect and disconnect at any time. */
void Apply();

/** @returns the connected gamepads, in the order the choice counts them. */
std::vector<Gamepad> GetGamepads();

} // namespace GamepadAssignment
