#include "HangWatchdog.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#ifdef __APPLE__
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <libultraship/libultraship.h>

#include "NetLog.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kStallTime = std::chrono::seconds(5);
constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr const char* kSampleSeconds = "3";

std::atomic<bool> sRunning = false;
std::atomic<bool> sWatching = false;
std::atomic<Clock::rep> sLastBeat = 0;
std::thread sThread;
std::string sDirectory;

/** Runs sample on this process, which waits for it, and says where the call stacks went. */
void Record() {
#ifdef __APPLE__
    char name[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S", std::localtime(&now));
    const std::string path = sDirectory + "/" + name + ".hang.txt";
    const std::string pid = std::to_string(getpid());
    const char* argv[] = { "/usr/bin/sample", pid.c_str(), kSampleSeconds, "-file", path.c_str(), nullptr };
    pid_t child;
    if (posix_spawn(&child, argv[0], nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) {
        return;
    }
    int status;
    waitpid(child, &status, 0);
    NetLog("The game stopped responding for %lld s; its call stacks are in %s",
           (long long) std::chrono::duration_cast<std::chrono::seconds>(kStallTime).count(), path.c_str());
#endif
}

void Run() {
    bool recorded = false;
    while (sRunning) {
        std::this_thread::sleep_for(kPollInterval);
        const Clock::duration sinceBeat = Clock::now().time_since_epoch() - Clock::duration(sLastBeat.load());
        const bool stalled = sWatching && (sinceBeat > kStallTime);
        if (stalled && !recorded) {
            Record();
        }
        recorded = stalled;
    }
}

} // namespace

namespace HangWatchdog {

void Start() {
    if (sRunning) {
        return;
    }
    sDirectory = Ship::Context::GetPathRelativeToAppDirectory("netplay");
    std::error_code error;
    std::filesystem::create_directories(sDirectory, error);
    sLastBeat = Clock::now().time_since_epoch().count();
    sRunning = true;
    sThread = std::thread(Run);
}

void Stop() {
    if (!sRunning) {
        return;
    }
    sRunning = false;
    sThread.join();
}

void Beat(bool watching) {
    sLastBeat = Clock::now().time_since_epoch().count();
    sWatching = watching;
}

} // namespace HangWatchdog
