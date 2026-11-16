#include "VoiceDevice.h"

#include <cstdlib>
#include <cstring>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// VoiceDeviceTest.cpp
std::unique_ptr<VoiceDevice> CreateTestVoiceDevice(u32 note);
// VoiceDeviceMac.cpp on macOS, VoiceDeviceWeb.cpp in a browser, or the fallback below
std::unique_ptr<VoiceDevice> CreatePlatformVoiceDevice();

std::unique_ptr<VoiceDevice> VoiceDevice::Create(bool real) {
    const char* test = getenv("SK_NETPLAY_TEST_VOICE");
    if ((test != nullptr) && (strcmp(test, "mic") == 0)) {
        return CreatePlatformVoiceDevice();
    }
    if ((test != nullptr) && (test[0] != '\0')) {
        return CreateTestVoiceDevice((u32) strtoul(test, nullptr, 10));
    }
    return real ? CreatePlatformVoiceDevice() : nullptr;
}

#if !(defined(__APPLE__) && TARGET_OS_OSX) && !defined(__EMSCRIPTEN__)
// No voice chat on this platform yet: it needs a microphone with echo cancellation.
std::unique_ptr<VoiceDevice> CreatePlatformVoiceDevice() {
    return nullptr;
}
#endif
