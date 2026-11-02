#include "GamepadAssignment.h"

#include <algorithm>

#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"

namespace GamepadAssignment {
namespace {

constexpr uint8_t kPlayerOnePort = 0;

std::shared_ptr<Ship::ConnectedPhysicalDeviceManager> GetDeviceManager() {
    return Ship::Context::GetRawInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager();
}

// SDL hands out increasing instance ids as gamepads connect, so sorting them gives connection order.
std::vector<int32_t> GetInstanceIds() {
    std::vector<int32_t> ids;
    for (const auto& [instanceId, name] : GetDeviceManager()->GetConnectedSDLGamepadNames()) {
        ids.push_back(instanceId);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

s32 sAppliedChoice = 0;

} // namespace

void Apply() {
    const s32 choice = CVarGetInteger(kCVar, 0);
    if ((choice == 0) && (sAppliedChoice == 0)) {
        // Leave the Input Editor's own device toggles alone.
        return;
    }
    auto manager = GetDeviceManager();
    const std::vector<int32_t> ids = GetInstanceIds();
    for (size_t i = 0; i < ids.size(); i++) {
        // With the chosen gamepad unplugged, player 1 listens to none rather than to someone else's.
        const bool wanted = (choice == 0) || ((s32) i + 1 == choice);
        if (wanted == manager->PortIsIgnoringInstanceId(kPlayerOnePort, ids[i])) {
            if (wanted) {
                manager->UnignoreInstanceIdForPort(kPlayerOnePort, ids[i]);
            } else {
                manager->IgnoreInstanceIdForPort(kPlayerOnePort, ids[i]);
            }
        }
    }
    sAppliedChoice = choice;
}

std::vector<Gamepad> GetGamepads() {
    auto names = GetDeviceManager()->GetConnectedSDLGamepadNames();
    std::vector<Gamepad> gamepads;
    for (int32_t instanceId : GetInstanceIds()) {
        bool held = false;
        if (SDL_GameController* controller = SDL_GameControllerFromInstanceID(instanceId)) {
            for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++) {
                held |= SDL_GameControllerGetButton(controller, (SDL_GameControllerButton) button) != 0;
            }
        }
        gamepads.push_back({ (s32) gamepads.size() + 1, names[instanceId], held });
    }
    return gamepads;
}

} // namespace GamepadAssignment
