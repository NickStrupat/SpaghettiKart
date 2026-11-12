#ifdef __EMSCRIPTEN__
#include "WebAudioOutput.h"

#include <emscripten.h>

#include <algorithm>
#include <atomic>

namespace {

// The ring's size in frames, a power of two: about 0.6 s at the game's rate, far more than is ever kept waiting.
constexpr uint32_t kCapacity = 16384;

/**
 * Shared with the worklet, which reads it through the page's shared memory. Both counts only grow (wrapping at 2^32):
 * their difference is what waits to be played. The game writes Write; the worklet advances Read, with Atomics.
 */
struct Ring {
    std::atomic<uint32_t> Write{ 0 };
    std::atomic<uint32_t> Read{ 0 };
    int16_t Samples[kCapacity * 2];
};

Ring sRing;
std::atomic<bool> sActive{ false };

} // namespace

namespace WebAudioOutput {

bool IsSupported() {
    return EM_ASM_INT({
               return ((typeof AudioWorkletNode != 'undefined') && self.crossOriginIsolated &&
                       (typeof window.startWebAudio == 'function')) ? 1 : 0;
           }) != 0;
}

bool Start(int32_t sampleRate, int32_t buffered) {
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "the worklet reads the counts with Atomics");
    const int started = EM_ASM_INT(
        {
            return window.startWebAudio(HEAPU8.buffer, $0, $1, $2, $3, $4, $5) ? 1 : 0;
        },
        &sRing.Write, &sRing.Read, sRing.Samples, kCapacity, sampleRate, buffered);
    sActive.store(started != 0);
    return started != 0;
}

bool IsActive() {
    return sActive.load(std::memory_order_relaxed);
}

int32_t Buffered() {
    return (int32_t) (sRing.Write.load(std::memory_order_acquire) - sRing.Read.load(std::memory_order_acquire));
}

void Play(const int16_t* frames, size_t count) {
    const uint32_t write = sRing.Write.load(std::memory_order_relaxed);
    const uint32_t space = kCapacity - (write - sRing.Read.load(std::memory_order_acquire));
    count = std::min<size_t>(count, space);
    for (size_t i = 0; i < count; i++) {
        const uint32_t at = ((write + (uint32_t) i) & (kCapacity - 1)) * 2;
        sRing.Samples[at] = frames[i * 2];
        sRing.Samples[at + 1] = frames[i * 2 + 1];
    }
    // The frames are in place before the worklet can see the new count.
    sRing.Write.store(write + (uint32_t) count, std::memory_order_release);
}

} // namespace WebAudioOutput
#endif
