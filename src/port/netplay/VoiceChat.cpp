#include "VoiceChat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include <opus.h>

#include "Lobby.h"
#include "NetLog.h"
#include "VoiceDevice.h"

using Clock = std::chrono::steady_clock;
using VoiceChat::MicStatus;

namespace {

constexpr size_t kSamplesPerMs = VoiceDevice::kSampleRate / 1000;
// Opus's usual frame: 20 ms
constexpr size_t kFrameSamples = 20 * kSamplesPerMs;
// Bits a second while someone talks. Speech is clear well below this; music from a player's room is not the point.
constexpr s32 kBitrate = 24000;
// How much loss the codec prepares for: it spends part of each frame on a coarse copy of the frame before.
constexpr s32 kExpectedLossPercent = 10;
// A frame's size is sent in one byte. At kBitrate a frame is about 60 bytes.
constexpr size_t kMaxFrameBytes = 255;
// The codec packs silence into a frame this small, or smaller: not worth sending.
constexpr s32 kSilentFrameBytes = 2;
// A packet: u16 the first frame's number, then each frame as u8 size and its bytes. Frames are numbered as sent, so
// a gap in the numbers is a lost frame rather than a pause in the talking.
constexpr size_t kPacketHeaderSize = 2;
constexpr size_t kMaxPacketFrames = 4;
constexpr size_t kMaxPacketSize = kPacketHeaderSize + kMaxPacketFrames * (1 + kMaxFrameBytes);
// After a stall (a course loading), the microphone has more waiting than anyone wants to hear late: only the newest
// is sent.
constexpr size_t kMaxCapturedSamples = 200 * kSamplesPerMs;
// Up to this many lost frames in a row are patched over; a longer gap is left as a gap.
constexpr s32 kMaxConcealedFrames = 3;
// What is kept queued in the device: it has to cover the time until the next game frame tops it up.
constexpr size_t kPlaybackTarget = 60 * kSamplesPerMs;
// A member's voice starts playing once this much of it has arrived, so that uneven arrival does not chop it up; it
// starts over whenever its queue runs dry, which every pause in the talking does.
constexpr size_t kStartThreshold = 40 * kSamplesPerMs;
// A member's queue grows when its game stalls and then catches up, or slowly, from its sound card's clock running
// ahead of this one's: beyond this much the oldest is dropped, down to kQueueAfterDrop, rather than playing late.
constexpr size_t kMaxQueued = 300 * kSamplesPerMs;
constexpr size_t kQueueAfterDrop = 60 * kSamplesPerMs;
// Someone counts as talking this long after their last frame, so that the mark does not flicker between words.
constexpr auto kTalkingHold = std::chrono::milliseconds(400);
// The microphone stays open this long with nobody to talk to: a direct connection that drops often comes back.
constexpr auto kCloseDelay = std::chrono::seconds(5);

/** Another member's voice. */
struct Speaker {
    OpusDecoder* Decoder = nullptr;
    bool Started = false;   // Whether a frame arrived since the decoder was made: NextFrame is known
    u16 NextFrame = 0;      // The number of the frame expected next
    std::deque<s16> Samples; // Unpacked, waiting to be mixed
    bool Playing = false;
    bool Heard = false; // Whether LastHeard is set
    Clock::time_point LastHeard;
    bool Muted = false;

    void Reset() {
        if (Decoder != nullptr) {
            opus_decoder_destroy(Decoder);
        }
        *this = Speaker{};
    }
};

struct VoiceState {
    bool DeviceMade = false; // Whether Create() was tried; Device stays null where there is none
    std::unique_ptr<VoiceDevice> Device;
    VoiceDevice::State LoggedState = VoiceDevice::State::Closed;
    OpusEncoder* Encoder = nullptr;
    std::vector<s16> Captured; // Recorded, not yet a whole frame
    u16 NextFrame = 0;         // The number of the next frame sent
    bool Sent = false;         // Whether LastSent is set
    Clock::time_point LastSent;
    bool Needed = false; // Whether LastNeeded is set
    Clock::time_point LastNeeded;
    MicStatus Mic = MicStatus::Off;
    std::array<Speaker, Lobby::kMaxMembers> Speakers;
};

VoiceState sVoice;

bool EnsureDevice(bool real) {
    if (!sVoice.DeviceMade) {
        sVoice.DeviceMade = true;
        sVoice.Device = VoiceDevice::Create(real);
    }
    return sVoice.Device != nullptr;
}

void CloseDevice() {
    if ((sVoice.Device != nullptr) && (sVoice.Device->GetState() != VoiceDevice::State::Closed)) {
        sVoice.Device->Close();
        NetLog("Voice chat: microphone closed");
    }
    sVoice.LoggedState = VoiceDevice::State::Closed;
    if (sVoice.Encoder != nullptr) {
        opus_encoder_destroy(sVoice.Encoder);
        sVoice.Encoder = nullptr;
    }
    sVoice.Captured.clear();
    sVoice.Sent = false;
    sVoice.Needed = false;
    for (Speaker& speaker : sVoice.Speakers) {
        const bool muted = speaker.Muted;
        speaker.Reset();
        speaker.Muted = muted;
    }
}

bool MakeEncoder() {
    int error = OPUS_OK;
    sVoice.Encoder = opus_encoder_create(VoiceDevice::kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if ((sVoice.Encoder == nullptr) || (error != OPUS_OK)) {
        NetLog("Voice chat: the codec failed to start (%s)", opus_strerror(error));
        sVoice.Encoder = nullptr;
        return false;
    }
    opus_encoder_ctl(sVoice.Encoder, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(sVoice.Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(sVoice.Encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(sVoice.Encoder, OPUS_SET_PACKET_LOSS_PERC(kExpectedLossPercent));
    // Silence comes out as frames of kSilentFrameBytes or less, which are not sent.
    opus_encoder_ctl(sVoice.Encoder, OPUS_SET_DTX(1));
    return true;
}

/** Whether voice goes between this game and the member in this slot. */
bool IsHeardBy(u8 slot) {
    if ((slot >= Lobby::kMaxMembers) || ((s32) slot == Lobby::GetLocalSlot())) {
        return false;
    }
    const Lobby::Member& member = Lobby::GetMembers()[slot];
    return member.Present && member.Known && member.Voice && Lobby::IsDirect(slot);
}

void SendPacket(const u8* packet, size_t size) {
    for (u8 slot = 0; slot < Lobby::kMaxMembers; slot++) {
        if (IsHeardBy(slot)) {
            Lobby::SendVoice(slot, packet, size);
        }
    }
}

/** Takes what the microphone recorded, and sends the whole frames of it that are not silence. */
void SendCaptured(bool send, Clock::time_point now) {
    s16 chunk[kFrameSamples * 4];
    while (true) {
        const size_t count = sVoice.Device->Capture(chunk, std::size(chunk));
        if (count == 0) {
            break;
        }
        sVoice.Captured.insert(sVoice.Captured.end(), chunk, chunk + count);
    }
    if (!send || (sVoice.Encoder == nullptr)) {
        sVoice.Captured.clear();
        return;
    }
    size_t taken = 0;
    if (sVoice.Captured.size() > kMaxCapturedSamples) {
        taken = sVoice.Captured.size() - kMaxCapturedSamples;
    }

    u8 packet[kMaxPacketSize];
    size_t size = kPacketHeaderSize;
    size_t frames = 0;
    while (sVoice.Captured.size() - taken >= kFrameSamples) {
        u8 frame[kMaxFrameBytes];
        const s32 bytes = opus_encode(sVoice.Encoder, &sVoice.Captured[taken], kFrameSamples, frame, sizeof(frame));
        taken += kFrameSamples;
        if (bytes <= kSilentFrameBytes) {
            continue;
        }
        if (frames == 0) {
            packet[0] = (u8) sVoice.NextFrame;
            packet[1] = (u8) (sVoice.NextFrame >> 8);
        }
        packet[size++] = (u8) bytes;
        memcpy(packet + size, frame, (size_t) bytes);
        size += (size_t) bytes;
        sVoice.NextFrame++;
        if (++frames == kMaxPacketFrames) {
            SendPacket(packet, size);
            size = kPacketHeaderSize;
            frames = 0;
        }
        sVoice.Sent = true;
        sVoice.LastSent = now;
    }
    if (frames > 0) {
        SendPacket(packet, size);
    }
    sVoice.Captured.erase(sVoice.Captured.begin(), sVoice.Captured.begin() + (std::ptrdiff_t) taken);
}

/**
 * Unpacks one frame of a member's voice onto its queue.
 * @param data The frame, or nullptr to guess at a lost one from what came before.
 * @param recover true: unpacks the coarse copy this frame carries of the one before it, which was lost.
 */
void Decode(Speaker& speaker, const u8* data, size_t size, bool recover) {
    s16 samples[kFrameSamples];
    const int count = opus_decode(speaker.Decoder, data, (opus_int32) size, samples, kFrameSamples, recover ? 1 : 0);
    if ((count == (int) kFrameSamples) && !speaker.Muted) {
        speaker.Samples.insert(speaker.Samples.end(), samples, samples + count);
    }
}

void ReceiveFrame(Speaker& speaker, u16 number, const u8* data, size_t size) {
    if (speaker.Decoder == nullptr) {
        int error = OPUS_OK;
        speaker.Decoder = opus_decoder_create(VoiceDevice::kSampleRate, 1, &error);
        if ((speaker.Decoder == nullptr) || (error != OPUS_OK)) {
            speaker.Decoder = nullptr;
            return;
        }
    }
    if (speaker.Started) {
        // How many frames were lost before this one; below 0 for a frame that arrived late or twice.
        const s16 lost = (s16) (number - speaker.NextFrame);
        if (lost < 0) {
            return;
        }
        if ((lost > 0) && (lost <= kMaxConcealedFrames)) {
            for (s16 i = 0; i < lost - 1; i++) {
                Decode(speaker, nullptr, 0, false);
            }
            Decode(speaker, data, size, true);
        }
    }
    Decode(speaker, data, size, false);
    speaker.Started = true;
    speaker.NextFrame = (u16) (number + 1);
}

void Receive(const Lobby::VoiceData& voice, Clock::time_point now) {
    // Only from members this game sends its own voice to: the two sides hear each other, or neither does.
    if (!IsHeardBy(voice.Source) || (voice.Data.size() < kPacketHeaderSize)) {
        return;
    }
    Speaker& speaker = sVoice.Speakers[voice.Source];
    u16 number = (u16) (voice.Data[0] | (voice.Data[1] << 8));
    size_t position = kPacketHeaderSize;
    while (position < voice.Data.size()) {
        const size_t size = voice.Data[position++];
        if ((size == 0) || (position + size > voice.Data.size())) {
            break;
        }
        ReceiveFrame(speaker, number++, &voice.Data[position], size);
        position += size;
        if (!speaker.Heard) {
            NetLog("Voice chat: hearing slot %u", voice.Source);
        }
        speaker.Heard = true;
        speaker.LastHeard = now;
    }
}

/** Tops the device's queue up to kPlaybackTarget with the members' voices mixed, or with silence. */
void Mix() {
    const size_t queued = sVoice.Device->GetQueuedPlayback();
    if (queued >= kPlaybackTarget) {
        return;
    }
    const size_t count = kPlaybackTarget - queued;
    s32 mix[kPlaybackTarget] = {};
    for (Speaker& speaker : sVoice.Speakers) {
        std::deque<s16>& samples = speaker.Samples;
        if (samples.size() > kMaxQueued) {
            samples.erase(samples.begin(), samples.end() - (std::ptrdiff_t) kQueueAfterDrop);
        }
        if (!speaker.Playing && (samples.size() >= kStartThreshold)) {
            speaker.Playing = true;
        }
        if (!speaker.Playing) {
            continue;
        }
        const size_t available = std::min(count, samples.size());
        for (size_t i = 0; i < available; i++) {
            mix[i] += samples[i];
        }
        samples.erase(samples.begin(), samples.begin() + (std::ptrdiff_t) available);
        if (available < count) {
            speaker.Playing = false;
        }
    }
    const f32 volume = std::clamp(CVarGetFloat(VoiceChat::kVolumeCVar, 1.0f), 0.0f, 4.0f);
    s16 out[kPlaybackTarget];
    for (size_t i = 0; i < count; i++) {
        out[i] = (s16) std::clamp((s32) ((f32) mix[i] * volume), (s32) INT16_MIN, (s32) INT16_MAX);
    }
    sVoice.Device->Play(out, count);
}

void LogDeviceState(VoiceDevice::State state) {
    if (state == sVoice.LoggedState) {
        return;
    }
    sVoice.LoggedState = state;
    switch (state) {
        case VoiceDevice::State::Open:
            NetLog("Voice chat: microphone open");
            break;
        case VoiceDevice::State::Denied:
            NetLog("Voice chat: the microphone is not allowed");
            break;
        case VoiceDevice::State::Failed:
            NetLog("Voice chat: the microphone failed to start");
            break;
        default:
            break;
    }
}

} // namespace

namespace VoiceChat {

void Update(bool real) {
    const bool inRoom = Lobby::GetState() == Lobby::State::InRoom;
    // Taken even when nothing plays it, or it would pile up.
    const std::vector<Lobby::VoiceData> arrived = inRoom ? Lobby::TakeVoice() : std::vector<Lobby::VoiceData>();
    if (!inRoom || (CVarGetInteger(kEnabledCVar, 1) == 0)) {
        Lobby::SetVoice(false);
        CloseDevice();
        sVoice.Mic = MicStatus::Off;
        return;
    }
    if (!EnsureDevice(real)) {
        Lobby::SetVoice(false);
        sVoice.Mic = MicStatus::Unavailable;
        return;
    }
    sVoice.Device->Update();
    VoiceDevice::State state = sVoice.Device->GetState();
    if ((state == VoiceDevice::State::Denied) || (state == VoiceDevice::State::Failed)) {
        // Without a microphone this game takes no part: the others are told, and do not send it their voices.
        Lobby::SetVoice(false);
        LogDeviceState(state);
        sVoice.Mic = (state == VoiceDevice::State::Denied) ? MicStatus::Denied : MicStatus::Failed;
        return;
    }
    Lobby::SetVoice(true);

    const Clock::time_point now = Clock::now();
    const std::array<Lobby::Member, Lobby::kMaxMembers>& members = Lobby::GetMembers();
    bool anyone = false;
    for (u8 slot = 0; slot < Lobby::kMaxMembers; slot++) {
        if (!members[slot].Present) {
            sVoice.Speakers[slot].Reset();
        }
        anyone = anyone || IsHeardBy(slot);
    }
    if (anyone) {
        sVoice.Needed = true;
        sVoice.LastNeeded = now;
    }
    if (!sVoice.Needed || (now - sVoice.LastNeeded > kCloseDelay)) {
        CloseDevice();
        sVoice.Mic = MicStatus::Idle;
        return;
    }
    if (state == VoiceDevice::State::Closed) {
        sVoice.Device->Open();
        state = sVoice.Device->GetState();
    }
    LogDeviceState(state);
    if (state != VoiceDevice::State::Open) {
        sVoice.Mic = MicStatus::Starting;
        return;
    }
    if ((sVoice.Encoder == nullptr) && !MakeEncoder()) {
        sVoice.Mic = MicStatus::Failed;
        return;
    }

    for (const Lobby::VoiceData& voice : arrived) {
        Receive(voice, now);
    }
    const bool muted = CVarGetInteger(kMicMutedCVar, 0) != 0;
    SendCaptured(!muted && anyone, now);
    Mix();
    sVoice.Mic = muted ? MicStatus::Muted : MicStatus::Live;
}

void Shutdown() {
    CloseDevice();
    sVoice.Device.reset();
    sVoice.DeviceMade = false;
    sVoice.Mic = MicStatus::Off;
}

MicStatus GetMicStatus() {
    return sVoice.Mic;
}

bool IsTalking() {
    return (sVoice.Mic == MicStatus::Live) && sVoice.Sent && (Clock::now() - sVoice.LastSent < kTalkingHold);
}

MemberStatus GetMemberStatus(u8 slot) {
    if ((slot >= Lobby::kMaxMembers) || ((s32) slot == Lobby::GetLocalSlot())) {
        return MemberStatus::None;
    }
    const Lobby::Member& member = Lobby::GetMembers()[slot];
    if (!member.Present || !member.Known) {
        return MemberStatus::None;
    }
    if (!member.Voice) {
        return MemberStatus::VoiceOff;
    }
    if (!Lobby::IsDirect(slot)) {
        return MemberStatus::NotDirect;
    }
    const Speaker& speaker = sVoice.Speakers[slot];
    const bool talking = speaker.Heard && (Clock::now() - speaker.LastHeard < kTalkingHold);
    return talking ? MemberStatus::Talking : MemberStatus::Quiet;
}

bool IsMuted(u8 slot) {
    return (slot < Lobby::kMaxMembers) && sVoice.Speakers[slot].Muted;
}

void SetMuted(u8 slot, bool muted) {
    if (slot < Lobby::kMaxMembers) {
        sVoice.Speakers[slot].Muted = muted;
        if (muted) {
            sVoice.Speakers[slot].Samples.clear();
        }
    }
}

} // namespace VoiceChat
