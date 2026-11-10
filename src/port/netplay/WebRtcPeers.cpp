#include "WebRtcPeers.h"

#include <algorithm>
#include <cstring>

#include "NetLog.h"
#include "WebRtcTransport.h"

using namespace RelayProtocol;

namespace {

// Where each side learns its public address. Without one, members only connect directly on the same network;
// everyone else plays through the relay.
constexpr const char* kIceServers = "[{\"urls\":\"stun:stun.l.google.com:19302\"}]";

// Kinds of message on a data channel: the first byte of each.
constexpr u8 kChannelPayload = 0;
constexpr u8 kChannelPing = 1;
constexpr u8 kChannelPong = 2;
constexpr size_t kPingSize = 5; // Kind, then the sender's time in milliseconds (u32, little-endian)

constexpr s32 kPingSmoothing = 4; // Each round trip moves the ping a quarter of the way
constexpr auto kPingInterval = std::chrono::milliseconds(500);
// A direct connection silent for this long is given up for the relay, well before GekkoNet's 5 s disconnect.
constexpr auto kDirectTimeout = std::chrono::milliseconds(1500);
// The offering side tries again this long after a connection failed or closed.
constexpr auto kRetryInterval = std::chrono::seconds(10);

// Setup messages (a description of each side's connection, a few hundred bytes to over a kilobyte) can be longer than
// one relay payload, so they travel in parts: message number, part, number of parts, then the bytes.
constexpr size_t kSignalHeaderSize = 3;
constexpr size_t kSignalPartSize = kMaxPayload - 1 - kSignalHeaderSize; // 1: RelayClient's channel byte
constexpr size_t kMaxSignalParts = 16;

void WriteU32(u8* out, u32 value) {
    for (int i = 0; i < 4; i++) {
        out[i] = (u8) (value >> (i * 8));
    }
}

u32 ReadU32(const u8* data) {
    return (u32) data[0] | ((u32) data[1] << 8) | ((u32) data[2] << 16) | ((u32) data[3] << 24);
}

} // namespace

WebRtcPeers::WebRtcPeers(Role role, const Callbacks& callbacks) : mRole(role), mCallbacks(callbacks) {
}

WebRtcPeers::~WebRtcPeers() {
    for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
        CloseDirect(slot);
    }
}

void WebRtcPeers::SetMembers(s32 ownSlot, const RelayLink::Members& members) {
    mSlot = ownSlot;
    for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
        Peer& peer = mPeers[slot];
        const u64 key = (slot == ownSlot) ? 0 : members[slot].Key;
        if (peer.Key != key) {
            // Someone left, or someone new took the slot
            CloseDirect(slot);
            peer = Peer();
            peer.Key = key;
        }
        peer.SharesAddress = members[slot].PublicPort != 0;
    }
}

bool WebRtcPeers::ShouldOffer(s32 slot) const {
    return (mRole == Role::Browser) && (mPeers[slot].SharesAddress || (mSlot < slot));
}

void WebRtcPeers::Update(bool allowed) {
    mAllowed = allowed;
    const Clock::time_point now = Clock::now();
    for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
        Peer& peer = mPeers[slot];
        if (!allowed || (peer.Key == 0)) {
            CloseDirect(slot);
            continue;
        }
        // A browser that answers gets its connection ready for the offer; a desktop game makes one when an offer comes.
        if (!peer.Connecting && (now - peer.LastAttempt >= kRetryInterval)) {
            if (ShouldOffer(slot)) {
                StartDirect(slot, true);
            } else if ((mRole == Role::Browser) && !peer.SharesAddress) {
                StartDirect(slot, false);
            }
        }
        if (peer.Open && (now - peer.LastPing >= kPingInterval)) {
            SendPing(slot, now);
        }
        const bool direct = peer.Open && HasHeardRecently(peer, now);
        if (direct != peer.WasDirect) {
            peer.WasDirect = direct;
            NetLog(direct ? "Direct connection to slot %d over WebRTC"
                          : "Direct connection to slot %d lost; using the relay",
                   slot);
        }
    }
}

void WebRtcPeers::Receive() {
    u8 buffer[kMaxPacketSize + 1024];
    int slot;
    int kind;
    int size;
    while ((size = sk_rtc_poll(&slot, &kind, buffer, sizeof(buffer))) >= 0) {
        if ((slot >= 0) && (slot < (s32) kMaxSlots)) {
            HandleEvent(slot, kind, buffer, (size_t) size);
        }
    }
}

bool WebRtcPeers::HasDirect(u8 slot) const {
    return mAllowed && (slot < kMaxSlots) && mPeers[slot].Open && HasHeardRecently(mPeers[slot], Clock::now());
}

s32 WebRtcPeers::GetPingMs(u8 slot) const {
    return HasDirect(slot) ? mPeers[slot].PingMs : -1;
}

void WebRtcPeers::Send(u8 slot, const u8* data, size_t size) {
    if (!HasDirect(slot) || (size > kMaxPayload)) {
        return;
    }
    u8 message[kMaxPayload + 1];
    message[0] = kChannelPayload;
    memcpy(message + 1, data, size);
    sk_rtc_send(slot, message, (int) (size + 1));
}

void WebRtcPeers::HandleSignal(u8 slot, const u8* data, size_t size) {
    if ((slot >= kMaxSlots) || (mPeers[slot].Key == 0) || (size < kSignalHeaderSize) || !mAllowed) {
        return;
    }
    Peer& peer = mPeers[slot];
    const u8 id = data[0];
    const u8 part = data[1];
    const u8 parts = data[2];
    if ((parts == 0) || (parts > kMaxSignalParts) || (part >= parts)) {
        return;
    }
    if ((id != peer.SignalId) || (peer.SignalParts.size() != parts)) {
        peer.SignalId = id;
        peer.SignalParts.assign(parts, {});
    }
    peer.SignalParts[part].assign(data + kSignalHeaderSize, data + size);
    std::vector<u8> whole;
    for (const std::vector<u8>& piece : peer.SignalParts) {
        if (piece.empty()) {
            return; // Not complete yet
        }
        whole.insert(whole.end(), piece.begin(), piece.end());
    }
    peer.SignalParts.clear();
    // An offer to a side that has no connection waiting (a desktop game, or one that gave up) starts one.
    if (!peer.Connecting) {
        StartDirect(slot, false);
    }
    sk_rtc_signal(slot, whole.data(), (int) whole.size());
}

void WebRtcPeers::StartDirect(s32 slot, bool initiator) {
    NetLog("WebRTC: connecting to slot %d (%s)", slot, initiator ? "offering" : "waiting for its offer");
    Peer& peer = mPeers[slot];
    peer.Connecting = true;
    peer.Open = false;
    peer.LastAttempt = Clock::now();
    sk_rtc_open(slot, initiator ? 1 : 0, kIceServers);
}

void WebRtcPeers::CloseDirect(s32 slot) {
    Peer& peer = mPeers[slot];
    if (peer.Connecting) {
        NetLog("WebRTC: closing the connection to slot %d", slot);
        sk_rtc_close(slot);
    }
    if (peer.WasDirect) {
        NetLog("Direct connection to slot %d lost; using the relay", slot);
        peer.WasDirect = false;
    }
    peer.Connecting = false;
    peer.Open = false;
    peer.PingMs = -1;
}

void WebRtcPeers::SendPing(s32 slot, Clock::time_point now) {
    mPeers[slot].LastPing = now;
    u8 message[kPingSize] = { kChannelPing };
    WriteU32(message + 1, GetMilliseconds(now));
    sk_rtc_send(slot, message, sizeof(message));
}

void WebRtcPeers::HandleEvent(s32 slot, int kind, const u8* data, size_t size) {
    Peer& peer = mPeers[slot];
    const Clock::time_point now = Clock::now();
    switch (kind) {
        case SK_RTC_EVENT_SIGNAL:
            SendSignal(slot, data, size);
            break;
        case SK_RTC_EVENT_OPEN:
            peer.Open = true;
            peer.LastHeard = now;
            // Measures the round trip at once, for the automatic input delay.
            SendPing(slot, now);
            break;
        case SK_RTC_EVENT_CLOSED:
            // The offering side tries again after kRetryInterval; the other waits for its offer.
            peer.Open = false;
            peer.Connecting = false;
            peer.LastAttempt = now;
            break;
        case SK_RTC_EVENT_DATA:
            HandleChannelMessage(slot, data, size, now);
            break;
        default:
            break;
    }
}

void WebRtcPeers::HandleChannelMessage(s32 slot, const u8* data, size_t size, Clock::time_point now) {
    if ((size == 0) || !mAllowed) {
        return;
    }
    Peer& peer = mPeers[slot];
    peer.LastHeard = now;
    if ((data[0] == kChannelPing) && (size == kPingSize)) {
        u8 reply[kPingSize];
        memcpy(reply, data, sizeof(reply));
        reply[0] = kChannelPong;
        sk_rtc_send(slot, reply, sizeof(reply));
    } else if ((data[0] == kChannelPong) && (size == kPingSize)) {
        const s32 sample = (s32) (GetMilliseconds(now) - ReadU32(data + 1));
        peer.PingMs = (peer.PingMs < 0) ? sample : (peer.PingMs * (kPingSmoothing - 1) + sample) / kPingSmoothing;
    } else if (data[0] == kChannelPayload) {
        mCallbacks.FromMember((u8) slot, data + 1, size - 1);
    }
}

void WebRtcPeers::SendSignal(s32 slot, const u8* data, size_t size) {
    Peer& peer = mPeers[slot];
    const size_t parts = (size + kSignalPartSize - 1) / kSignalPartSize;
    if ((parts == 0) || (parts > kMaxSignalParts)) {
        return;
    }
    const u8 id = peer.NextSignalId++;
    for (size_t part = 0; part < parts; part++) {
        const size_t offset = part * kSignalPartSize;
        const size_t length = std::min(kSignalPartSize, size - offset);
        u8 message[kSignalHeaderSize + kSignalPartSize] = { id, (u8) part, (u8) parts };
        memcpy(message + kSignalHeaderSize, data + offset, length);
        mCallbacks.SendSignal((u8) slot, message, kSignalHeaderSize + length);
    }
}

u32 WebRtcPeers::GetMilliseconds(Clock::time_point now) const {
    return (u32) std::chrono::duration_cast<std::chrono::milliseconds>(now - mEpoch).count();
}

bool WebRtcPeers::HasHeardRecently(const Peer& peer, Clock::time_point now) {
    return now - peer.LastHeard <= kDirectTimeout;
}
