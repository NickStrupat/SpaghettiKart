#ifndef NETPLAY_VOICE_DEVICE_H
#define NETPLAY_VOICE_DEVICE_H

#include <libultraship/libultraship.h>

#include <memory>

/**
 * A microphone and the speakers, for voice chat (VoiceChat.h). Samples are 16 bit, one channel, kSampleRate a second,
 * both ways. The microphone's samples come with this computer's own sound already removed from them (echo
 * cancellation), where the platform can do that: players on speakers would otherwise send each other's voices, and the
 * game's music, back.
 *
 * VoiceChat moves the samples once per game frame, on the game thread; a device keeps its own audio thread behind two
 * queues. Every function is for the game thread.
 */
class VoiceDevice {
  public:
    static constexpr s32 kSampleRate = 48000;

    enum class State {
        Closed,
        Starting, // Waiting for the player to allow the microphone, or for the device to come up
        Open,
        Denied,   // The player, or the system, refused the microphone
        Failed,
    };

    /**
     * The device this platform uses, or nullptr where there is none. With SK_NETPLAY_TEST_VOICE set to a number, a
     * device for tests instead (VoiceDeviceTest.cpp): its microphone hums a note, and it reports the notes it is asked
     * to play. SK_NETPLAY_TEST_VOICE=mic gives a test run the platform's device, real microphone and all.
     * @param real Whether a real microphone may be used; false for test runs, which then get the test device or none.
     */
    static std::unique_ptr<VoiceDevice> Create(bool real);

    virtual ~VoiceDevice() = default;

    /** Starts the microphone and the speakers; the first time, the system asks the player to allow the microphone. */
    virtual void Open() = 0;
    virtual void Close() = 0;
    /** Once per game frame: lets a device that is Starting finish, and one that stopped working start over. */
    virtual void Update() {
    }
    virtual State GetState() const = 0;

    /**
     * Takes what the microphone recorded since the last game frame: call it until it returns 0.
     * @returns the number of samples written, at most `capacity`. What is never taken is dropped after about a second.
     */
    virtual size_t Capture(s16* samples, size_t capacity) = 0;
    /** @returns the number of samples waiting to be played. */
    virtual size_t GetQueuedPlayback() const = 0;
    /** Queues samples to play. Samples that do not fit in the queue (about a second) are dropped. */
    virtual void Play(const s16* samples, size_t count) = 0;
};

#endif // NETPLAY_VOICE_DEVICE_H
