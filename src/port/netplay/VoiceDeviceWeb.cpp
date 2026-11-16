// The browser build's voice device (VoiceDevice.h, docs/WEB.md): the browser's microphone, with its echo
// cancellation, and a sound context for the others' voices (src/port/web/VoiceDevice.js). An AudioWorklet on the
// browser's audio thread (src/port/web/VoiceWorklet.js) moves the samples through two rings in the game's shared
// memory, the way the game's own sound reaches the browser (src/port/audio/WebAudioOutput.h).
#ifdef __EMSCRIPTEN__

#include "VoiceDevice.h"

#include <algorithm>
#include <atomic>

// src/port/web/VoiceDevice.js
extern "C" {
void sk_voice_open(const void* capturedWrite, const void* capturedRead, const s16* captured, const void* playbackWrite,
                   const void* playbackRead, const s16* playback, int capacity, int sampleRate);
void sk_voice_close(void);
int sk_voice_state(void);
}

namespace {

// Each ring's size in samples, a power of two: a little over a second.
constexpr u32 kCapacity = 65536;

/**
 * Shared with the worklet, which reads and writes it through the page's shared memory. Both counts only grow
 * (wrapping at 2^32): their difference is what waits. One side advances Write, the other Read.
 */
struct Ring {
    std::atomic<u32> Write{ 0 };
    std::atomic<u32> Read{ 0 };
    s16 Samples[kCapacity];
};

// Static: the worklet keeps their addresses for as long as it runs, which can be a moment past Close().
Ring sCaptured; // The worklet writes, the game reads
Ring sPlayback; // The game writes, the worklet reads

class VoiceDeviceWeb : public VoiceDevice {
  public:
    ~VoiceDeviceWeb() override {
        sk_voice_close();
    }

    void Open() override {
        static_assert(std::atomic<u32>::is_always_lock_free, "the worklet reads the counts with Atomics");
        if (GetState() != State::Closed) {
            return;
        }
        // What an earlier microphone recorded is not sent now. The worklet does the same for the playback ring.
        sCaptured.Read.store(sCaptured.Write.load());
        sk_voice_open(&sCaptured.Write, &sCaptured.Read, sCaptured.Samples, &sPlayback.Write, &sPlayback.Read,
                      sPlayback.Samples, (int) kCapacity, kSampleRate);
    }

    void Close() override {
        sk_voice_close();
    }

    State GetState() const override {
        // The page's numbers are this enum's (VoiceDevice.js).
        return (State) sk_voice_state();
    }

    size_t Capture(s16* samples, size_t capacity) override {
        const u32 read = sCaptured.Read.load(std::memory_order_relaxed);
        const size_t count = std::min<size_t>(capacity, sCaptured.Write.load(std::memory_order_acquire) - read);
        for (size_t i = 0; i < count; i++) {
            samples[i] = sCaptured.Samples[(read + (u32) i) & (kCapacity - 1)];
        }
        sCaptured.Read.store(read + (u32) count, std::memory_order_release);
        return count;
    }

    size_t GetQueuedPlayback() const override {
        return sPlayback.Write.load(std::memory_order_acquire) - sPlayback.Read.load(std::memory_order_acquire);
    }

    void Play(const s16* samples, size_t count) override {
        if (GetState() != State::Open) {
            return;
        }
        const u32 write = sPlayback.Write.load(std::memory_order_relaxed);
        count = std::min<size_t>(count, kCapacity - (write - sPlayback.Read.load(std::memory_order_acquire)));
        for (size_t i = 0; i < count; i++) {
            sPlayback.Samples[(write + (u32) i) & (kCapacity - 1)] = samples[i];
        }
        // The samples are in place before the worklet can see the new count.
        sPlayback.Write.store(write + (u32) count, std::memory_order_release);
    }
};

} // namespace

std::unique_ptr<VoiceDevice> CreatePlatformVoiceDevice() {
    return std::make_unique<VoiceDeviceWeb>();
}

#endif // __EMSCRIPTEN__
