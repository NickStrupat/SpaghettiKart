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

// The serial number is the same in every process. Without one, the GUID still tells models and connection types apart,
// though not two identical gamepads.
std::string GetId(int32_t instanceId) {
    SDL_GameController* controller = SDL_GameControllerFromInstanceID(instanceId);
    if (controller == nullptr) {
        return "";
    }
    const char* serial = SDL_GameControllerGetSerial(controller);
    if ((serial != nullptr) && (serial[0] != '\0')) {
        return serial;
    }
    char guid[33] = "";
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller)), guid, sizeof(guid));
    return guid;
}

std::string sAppliedChoice;

} // namespace

void Apply() {
    const std::string choice = CVarGetString(kCVar, "");
    if (choice.empty() && sAppliedChoice.empty()) {
        // Leave the Input Editor's own device toggles alone.
        return;
    }
    auto manager = GetDeviceManager();
    for (const auto& [instanceId, name] : manager->GetConnectedSDLGamepadNames()) {
        // With the chosen gamepad unplugged, player 1 listens to none rather than to someone else's.
        const bool wanted = choice.empty() || (GetId(instanceId) == choice);
        if (wanted == manager->PortIsIgnoringInstanceId(kPlayerOnePort, instanceId)) {
            if (wanted) {
                manager->UnignoreInstanceIdForPort(kPlayerOnePort, instanceId);
            } else {
                manager->IgnoreInstanceIdForPort(kPlayerOnePort, instanceId);
            }
        }
    }
    sAppliedChoice = choice;
}

std::vector<Gamepad> GetGamepads() {
    std::vector<Gamepad> gamepads;
    for (const auto& [instanceId, name] : GetDeviceManager()->GetConnectedSDLGamepadNames()) {
        bool held = false;
        if (SDL_GameController* controller = SDL_GameControllerFromInstanceID(instanceId)) {
            for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++) {
                held |= SDL_GameControllerGetButton(controller, (SDL_GameControllerButton) button) != 0;
            }
        }
        gamepads.push_back({ 0, GetId(instanceId), name, held });
    }
    std::sort(gamepads.begin(), gamepads.end(), [](const Gamepad& a, const Gamepad& b) { return a.Id < b.Id; });
    for (size_t i = 0; i < gamepads.size(); i++) {
        gamepads[i].Number = (s32) i + 1;
    }
    return gamepads;
}

} // namespace GamepadAssignment
