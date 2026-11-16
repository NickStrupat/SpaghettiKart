// The Mac's voice device (VoiceDevice.h): macOS's voice processing audio unit, the one its own calls use. It records
// the microphone and plays the others' voices, and takes what the Mac plays out of what the microphone hears, so that
// players on speakers do not send each other's voices back. It also evens out loudness and dampens steady noise.
//
// The unit runs its own audio threads; two rings pass samples between them and the game thread.
#include "VoiceDevice.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_OSX

#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>

#include "NetLog.h"

// VoiceMicPermissionMac.mm
extern "C" int sk_mic_permission(void);
extern "C" void sk_mic_request_permission(void);

using Clock = std::chrono::steady_clock;

namespace {

constexpr int kPermissionNotAsked = 0;
constexpr int kPermissionAllowed = 1;
// The unit's two sides: element 1 is the microphone, element 0 the speakers.
constexpr AudioUnitElement kMicrophoneBus = 1;
constexpr AudioUnitElement kSpeakerBus = 0;
// A little over a second of samples; a power of two, for the rings' index arithmetic.
constexpr size_t kRingCapacity = 65536;
// The most frames the unit hands over at once, generously.
constexpr size_t kMaxCallbackFrames = 8192;
// With no samples from the microphone for this long, the unit is started over: it can stop for good when the
// Mac's sound devices change (headphones connecting).
constexpr auto kSilentRestart = std::chrono::seconds(3);

/** Samples from one thread to another: one writes, one reads, neither waits. */
class SampleRing {
  public:
    SampleRing() : mSamples(kRingCapacity) {
    }

    size_t GetSize() const {
        return mWritten.load(std::memory_order_acquire) - mRead.load(std::memory_order_acquire);
    }

    /** Samples that do not fit are dropped. */
    void Write(const s16* samples, size_t count) {
        const size_t written = mWritten.load(std::memory_order_relaxed);
        count = std::min(count, kRingCapacity - (written - mRead.load(std::memory_order_acquire)));
        for (size_t i = 0; i < count; i++) {
            mSamples[(written + i) & (kRingCapacity - 1)] = samples[i];
        }
        mWritten.store(written + count, std::memory_order_release);
    }

    /** @returns how many samples were there to read, at most `count`. */
    size_t Read(s16* samples, size_t count) {
        const size_t read = mRead.load(std::memory_order_relaxed);
        count = std::min(count, mWritten.load(std::memory_order_acquire) - read);
        for (size_t i = 0; i < count; i++) {
            samples[i] = mSamples[(read + i) & (kRingCapacity - 1)];
        }
        mRead.store(read + count, std::memory_order_release);
        return count;
    }

    /** Empties the ring; only while nothing else reads or writes it. */
    void Clear() {
        mRead.store(mWritten.load());
    }

  private:
    std::vector<s16> mSamples;
    std::atomic<size_t> mWritten{ 0 };
    std::atomic<size_t> mRead{ 0 };
};

class VoiceDeviceMac : public VoiceDevice {
  public:
    ~VoiceDeviceMac() override {
        StopUnit();
    }

    void Open() override {
        if (mState != State::Closed) {
            return;
        }
        const int permission = sk_mic_permission();
        if (permission == kPermissionNotAsked) {
            sk_mic_request_permission();
            mState = State::Starting;
        } else if (permission == kPermissionAllowed) {
            StartUnit();
        } else {
            mState = State::Denied;
        }
    }

    void Close() override {
        StopUnit();
        mState = State::Closed;
    }

    void Update() override {
        if (mState == State::Starting) {
            // Waiting for the player's answer to macOS's question.
            const int permission = sk_mic_permission();
            if (permission == kPermissionAllowed) {
                StartUnit();
            } else if (permission != kPermissionNotAsked) {
                mState = State::Denied;
            }
        } else if (mState == State::Open) {
            const Clock::time_point now = Clock::now();
            const u64 recorded = mRecorded.load(std::memory_order_relaxed);
            if (recorded != mLastRecorded) {
                mLastRecorded = recorded;
                mLastRecordedAt = now;
            } else if (now - mLastRecordedAt > kSilentRestart) {
                NetLog("Voice chat: the microphone went quiet; starting it over");
                StopUnit();
                StartUnit();
            }
        }
    }

    State GetState() const override {
        return mState;
    }

    size_t Capture(s16* samples, size_t capacity) override {
        return mCaptured.Read(samples, capacity);
    }

    size_t GetQueuedPlayback() const override {
        return mPlayback.GetSize();
    }

    void Play(const s16* samples, size_t count) override {
        if (mState == State::Open) {
            mPlayback.Write(samples, count);
        }
    }

  private:
    /** Makes and starts the unit; mState says how that went. */
    void StartUnit() {
        mState = State::Failed;
        const AudioComponentDescription description = { kAudioUnitType_Output, kAudioUnitSubType_VoiceProcessingIO,
                                                        kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if ((component == nullptr) || !Check(AudioComponentInstanceNew(component, &mUnit), "make the unit")) {
            mUnit = nullptr;
            return;
        }
        const UInt32 on = 1;
        // Both ways, in the device's own format, whatever the Mac's microphone and speakers use: the unit converts.
        AudioStreamBasicDescription format = {};
        format.mSampleRate = kSampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = 1;
        format.mBitsPerChannel = 16;
        format.mBytesPerFrame = sizeof(s16);
        format.mBytesPerPacket = sizeof(s16);
        format.mFramesPerPacket = 1;
        const AURenderCallbackStruct record = { OnRecorded, this };
        const AURenderCallbackStruct play = { OnPlay, this };
        const bool ready =
            Check(AudioUnitSetProperty(mUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, kMicrophoneBus,
                                       &on, sizeof(on)),
                  "turn the microphone on") &&
            Check(AudioUnitSetProperty(mUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, kSpeakerBus,
                                       &on, sizeof(on)),
                  "turn the speakers on") &&
            Check(AudioUnitSetProperty(mUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, kMicrophoneBus,
                                       &format, sizeof(format)),
                  "set the microphone's format") &&
            Check(AudioUnitSetProperty(mUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kSpeakerBus,
                                       &format, sizeof(format)),
                  "set the speakers' format") &&
            Check(AudioUnitSetProperty(mUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
                                       kMicrophoneBus, &record, sizeof(record)),
                  "listen to the microphone") &&
            Check(AudioUnitSetProperty(mUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, kSpeakerBus,
                                       &play, sizeof(play)),
                  "feed the speakers");
        if (ready) {
            // The unit turns down everything else the Mac plays while it runs, the game's own sound included: as
            // little as it lets us ask for. Not fatal when refused.
            const AUVoiceIOOtherAudioDuckingConfiguration ducking = { false, kAUVoiceIOOtherAudioDuckingLevelMin };
            AudioUnitSetProperty(mUnit, kAUVoiceIOProperty_OtherAudioDuckingConfiguration, kAudioUnitScope_Global, 0,
                                 &ducking, sizeof(ducking));
        }
        mCaptured.Clear();
        mPlayback.Clear();
        if (!ready || !Check(AudioUnitInitialize(mUnit), "set the unit up")) {
            AudioComponentInstanceDispose(mUnit);
            mUnit = nullptr;
            return;
        }
        if (!Check(AudioOutputUnitStart(mUnit), "start the unit")) {
            AudioUnitUninitialize(mUnit);
            AudioComponentInstanceDispose(mUnit);
            mUnit = nullptr;
            return;
        }
        mLastRecorded = mRecorded.load();
        mLastRecordedAt = Clock::now();
        mState = State::Open;
    }

    void StopUnit() {
        if (mUnit != nullptr) {
            // Stopping waits for the callbacks to finish, so nothing uses the rings after this.
            AudioOutputUnitStop(mUnit);
            AudioUnitUninitialize(mUnit);
            AudioComponentInstanceDispose(mUnit);
            mUnit = nullptr;
        }
        mCaptured.Clear();
        mPlayback.Clear();
    }

    static bool Check(OSStatus status, const char* what) {
        if (status != noErr) {
            NetLog("Voice chat: could not %s (error %d)", what, (int) status);
        }
        return status == noErr;
    }

    /** On the unit's thread: the microphone has `frames` samples ready. */
    static OSStatus OnRecorded(void* context, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                               UInt32 bus, UInt32 frames, AudioBufferList*) {
        VoiceDeviceMac* device = static_cast<VoiceDeviceMac*>(context);
        if (frames > kMaxCallbackFrames) {
            return noErr;
        }
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 1;
        list.mBuffers[0].mDataByteSize = frames * sizeof(s16);
        list.mBuffers[0].mData = device->mRecordBuffer;
        const OSStatus status = AudioUnitRender(device->mUnit, flags, time, bus, frames, &list);
        if (status == noErr) {
            device->mCaptured.Write(device->mRecordBuffer, frames);
            device->mRecorded.fetch_add(frames, std::memory_order_relaxed);
        }
        return status;
    }

    /** On the unit's thread: the speakers want `frames` samples; silence when the game has queued too few. */
    static OSStatus OnPlay(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                           AudioBufferList* data) {
        VoiceDeviceMac* device = static_cast<VoiceDeviceMac*>(context);
        s16* samples = static_cast<s16*>(data->mBuffers[0].mData);
        const size_t wanted = std::min((size_t) frames, (size_t) data->mBuffers[0].mDataByteSize / sizeof(s16));
        const size_t played = device->mPlayback.Read(samples, wanted);
        memset(samples + played, 0, data->mBuffers[0].mDataByteSize - played * sizeof(s16));
        return noErr;
    }

    State mState = State::Closed;
    AudioUnit mUnit = nullptr;
    SampleRing mCaptured; // From the unit's thread to the game's
    SampleRing mPlayback; // From the game's thread to the unit's
    s16 mRecordBuffer[kMaxCallbackFrames];
    // Samples the microphone delivered, counted on the unit's thread; the game thread watches that it keeps growing.
    std::atomic<u64> mRecorded{ 0 };
    u64 mLastRecorded = 0;
    Clock::time_point mLastRecordedAt;
};

} // namespace

std::unique_ptr<VoiceDevice> CreatePlatformVoiceDevice() {
    return std::make_unique<VoiceDeviceMac>();
}

#endif // macOS
