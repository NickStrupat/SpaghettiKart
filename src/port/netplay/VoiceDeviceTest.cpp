// The voice device of test runs (VoiceDevice.h, SK_NETPLAY_TEST_VOICE=<note>, 1 to 4): no microphone and no speakers.
// Its microphone hums this game's note in short bursts, as syllables, and what it is asked to play is searched for the
// four notes, so that tools/netplay-sim.py can tell whose voice reached whom.
//
// Time is counted in game frames rather than by the clock: each Capture(), one per game frame, records one frame's
// worth of samples and plays as many. A fast-forwarded test (SK_FAST) then carries voice as a real-time one does.

#include "VoiceDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

#include "NetLog.h"

namespace {

constexpr u32 kNumNotes = 4;
constexpr std::array<f64, kNumNotes> kNoteHz = { 500.0, 700.0, 900.0, 1100.0 };
constexpr s32 kSamplesPerGameFrame = VoiceDevice::kSampleRate / 30;
// A burst and the pause after it, in samples: four syllables a second
constexpr s32 kBurstSamples = VoiceDevice::kSampleRate * 15 / 100;
constexpr s32 kSyllableSamples = VoiceDevice::kSampleRate / 4;
constexpr f64 kHumAmplitude = 8000.0;
// Played samples are searched a syllable at a time. A note counts as present from a fraction of the hum's loudness:
// the codec and the mixing change it, silence and the other notes stay far below.
constexpr f64 kHeardAmplitude = 1000.0;
constexpr s32 kBlocksToHear = 4;  // A second of a note, before it is reported
constexpr s32 kBlocksToStop = 12; // Three seconds without it, before that is reported

class VoiceDeviceTest : public VoiceDevice {
  public:
    explicit VoiceDeviceTest(u32 note) : mNote(std::clamp(note, 1u, kNumNotes) - 1) {
    }

    void Open() override {
        mState = State::Open;
    }

    void Close() override {
        mState = State::Closed;
        mPlayback.clear();
        for (u32 note = 0; note < kNumNotes; note++) {
            if (mFilters[note].Heard) {
                NetLog("Voice test: note %u stopped", note + 1);
            }
        }
        mFilters = MakeFilters();
        mBlockSamples = 0;
    }

    State GetState() const override {
        return mState;
    }

    size_t Capture(s16* samples, size_t capacity) override {
        if (mState != State::Open) {
            return 0;
        }
        const size_t count = std::min(capacity, (size_t) kSamplesPerGameFrame - mCapturedThisFrame);
        if (count == 0) {
            // The frame's samples are taken: the next call starts the next frame's.
            mCapturedThisFrame = 0;
            return 0;
        }
        const f64 step = 2.0 * M_PI * kNoteHz[mNote] / kSampleRate;
        for (size_t i = 0; i < count; i++) {
            const bool humming = (mHummed % kSyllableSamples) < kBurstSamples;
            samples[i] = humming ? (s16) (kHumAmplitude * std::sin(step * (f64) mHummed)) : 0;
            mHummed++;
        }
        mCapturedThisFrame += count;
        Listen(count);
        return count;
    }

    size_t GetQueuedPlayback() const override {
        return mPlayback.size();
    }

    void Play(const s16* samples, size_t count) override {
        if (mState == State::Open) {
            mPlayback.insert(mPlayback.end(), samples, samples + count);
        }
    }

  private:
    /** Plays `count` samples to nobody, searching them for the notes; silence when nothing waits. */
    void Listen(size_t count) {
        for (size_t i = 0; i < count; i++) {
            s16 sample = 0;
            if (!mPlayback.empty()) {
                sample = mPlayback.front();
                mPlayback.pop_front();
            }
            // Goertzel's filter, one per note: the strength of one pitch without a whole spectrum.
            for (u32 note = 0; note < kNumNotes; note++) {
                Filter& filter = mFilters[note];
                const f64 next = (f64) sample + filter.Coefficient * filter.Last - filter.BeforeLast;
                filter.BeforeLast = filter.Last;
                filter.Last = next;
            }
            if (++mBlockSamples == kSyllableSamples) {
                EndBlock();
            }
        }
    }

    void EndBlock() {
        for (u32 note = 0; note < kNumNotes; note++) {
            Filter& filter = mFilters[note];
            const f64 power = filter.Last * filter.Last + filter.BeforeLast * filter.BeforeLast -
                              filter.Coefficient * filter.Last * filter.BeforeLast;
            const f64 amplitude = 2.0 * std::sqrt(std::max(power, 0.0)) / kSyllableSamples;
            filter.Last = 0.0;
            filter.BeforeLast = 0.0;
            const bool present = amplitude >= kHeardAmplitude;
            filter.Run = (present == filter.RunPresent) ? filter.Run + 1 : 1;
            filter.RunPresent = present;
            if (present && !filter.Heard && (filter.Run >= kBlocksToHear)) {
                filter.Heard = true;
                NetLog("Voice test: hearing note %u", note + 1);
            } else if (!present && filter.Heard && (filter.Run >= kBlocksToStop)) {
                filter.Heard = false;
                NetLog("Voice test: note %u stopped", note + 1);
            }
        }
        mBlockSamples = 0;
    }

    struct Filter {
        f64 Coefficient = 0.0;
        f64 Last = 0.0;
        f64 BeforeLast = 0.0;
        bool Heard = false;      // As last reported
        bool RunPresent = false; // Whether the note was in the last block, and for how many blocks that has held
        s32 Run = 0;
    };

    static std::array<Filter, kNumNotes> MakeFilters() {
        std::array<Filter, kNumNotes> filters;
        for (u32 note = 0; note < kNumNotes; note++) {
            filters[note].Coefficient = 2.0 * std::cos(2.0 * M_PI * kNoteHz[note] / kSampleRate);
        }
        return filters;
    }

    u32 mNote;
    State mState = State::Closed;
    u64 mHummed = 0;
    size_t mCapturedThisFrame = 0;
    std::deque<s16> mPlayback;
    std::array<Filter, kNumNotes> mFilters = MakeFilters();
    s32 mBlockSamples = 0;
};

} // namespace

std::unique_ptr<VoiceDevice> CreateTestVoiceDevice(u32 note) {
    return std::make_unique<VoiceDeviceTest>(note);
}
