// The browser build's RelayLink (RelayLink.h, docs/WEB.md). A page cannot send UDP: it reaches the relay with a
// WebSocket, which carries the same messages, one per binary message, and reaches the other members with WebRTC data
// channels, set up through messages the relay passes along. The desktop game uses RelayLinkUdp.cpp instead.
#ifdef __EMSCRIPTEN__

#include "RelayLink.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

#include <emscripten/websocket.h>

#include "NetLog.h"

using namespace RelayProtocol;
using Clock = std::chrono::steady_clock;

// src/port/web/WebRtcLink.js
extern "C" {
void sk_rtc_open(int slot, int initiator, const char* iceServersJson);
void sk_rtc_close(int slot);
void sk_rtc_signal(int slot, const u8* data, int size);
int sk_rtc_send(int slot, const u8* data, int size);
int sk_rtc_poll(int* slot, int* kind, u8* buffer, int capacity);
int sk_page_is_secure(void);
}

namespace {

// Where browsers learn their public address for WebRTC. Without one, members only connect directly on the same
// network; everyone else plays through the relay.
constexpr const char* kIceServers = "[{\"urls\":\"stun:stun.l.google.com:19302\"}]";

// Events from WebRtcLink.js
constexpr int kEventData = 0;
constexpr int kEventSignal = 1;
constexpr int kEventOpen = 2;
constexpr int kEventClosed = 3;

// Kinds of message on a data channel: the first byte of each.
constexpr u8 kChannelPayload = 0;
constexpr u8 kChannelPing = 1;
constexpr u8 kChannelPong = 2;

constexpr s32 kPingSmoothing = 4; // Each round trip moves the ping a quarter of the way
constexpr auto kPingInterval = std::chrono::milliseconds(500);
// A direct connection silent for this long is given up for the relay, well before GekkoNet's 5 s disconnect.
constexpr auto kDirectTimeout = std::chrono::milliseconds(1500);
// The member with the lower slot, which makes the offer, tries again this long after a connection failed or closed.
constexpr auto kRetryInterval = std::chrono::seconds(10);

// WebRTC's setup messages (a description of each side's connection, a few hundred bytes to over a kilobyte) can be
// longer than one relay payload, so they travel in parts: message number, part, number of parts, then the bytes.
constexpr size_t kSignalHeaderSize = 3;
constexpr size_t kSignalPartSize = kMaxPayload - 1 - kSignalHeaderSize; // 1: RelayClient's channel byte
constexpr size_t kMaxSignalParts = 16;

struct Peer {
    u64 Key = 0; // 0 while the slot is free
    bool Connecting = false;
    bool Open = false;
    Clock::time_point LastHeard;
    Clock::time_point LastPing;
    Clock::time_point LastAttempt;
    s32 PingMs = -1;
    // The setup message being received in parts
    u8 SignalId = 0;
    std::vector<std::vector<u8>> SignalParts;
    u8 NextSignalId = 0;
};

class RelayLinkWeb : public RelayLink {
  public:
    explicit RelayLinkWeb(const Callbacks& callbacks) : mCallbacks(callbacks) {
    }

    ~RelayLinkWeb() override {
        for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
            CloseDirect(slot);
        }
        if (mSocket > 0) {
            emscripten_websocket_close(mSocket, 1000, "");
            emscripten_websocket_delete(mSocket);
        }
    }

    void Open(const std::string& relayAddress) override {
        mAddress = relayAddress;
        const std::string url = MakeUrl(relayAddress);
        EmscriptenWebSocketCreateAttributes attributes;
        emscripten_websocket_init_create_attributes(&attributes);
        attributes.url = url.c_str();
        attributes.protocols = nullptr;
        attributes.createOnMainThread = true;
        mSocket = emscripten_websocket_new(&attributes);
        if (mSocket <= 0) {
            Fail("Could not connect to the relay server at " + relayAddress);
            return;
        }
        mState = State::Connecting;
        emscripten_websocket_set_onopen_callback(mSocket, this, OnOpen);
        emscripten_websocket_set_onmessage_callback(mSocket, this, OnMessage);
        emscripten_websocket_set_onerror_callback(mSocket, this, OnError);
        emscripten_websocket_set_onclose_callback(mSocket, this, OnClose);
    }

    State GetState() const override {
        return mState;
    }

    const std::string& GetError() const override {
        return mError;
    }

    void SendToRelay(const u8* data, size_t size) override {
        if (mState == State::Open) {
            emscripten_websocket_send_binary(mSocket, const_cast<u8*>(data), (uint32_t) size);
        }
    }

    void Receive() override {
        // The WebSocket's callbacks queued what arrived between frames.
        while (!mIncoming.empty()) {
            const std::vector<u8> message = std::move(mIncoming.front());
            mIncoming.pop_front();
            mCallbacks.FromRelay(message.data(), message.size());
        }
        u8 buffer[kMaxPacketSize + 1024];
        int slot;
        int kind;
        int size;
        while ((size = sk_rtc_poll(&slot, &kind, buffer, sizeof(buffer))) >= 0) {
            if ((slot >= 0) && (slot < (s32) kMaxSlots)) {
                HandleRtcEvent(slot, kind, buffer, (size_t) size);
            }
        }
    }

    void GetLocalAddress(u32* address, u16* port) const override {
        // Members reach a browser with WebRTC, never at an address.
        *address = 0;
        *port = 0;
    }

    void SetMembers(s32 ownSlot, u64, const Members& members) override {
        mSlot = ownSlot;
        for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
            Peer& peer = mPeers[slot];
            const u64 key = (slot == ownSlot) ? 0 : members[slot].Key;
            if (peer.Key == key) {
                continue;
            }
            // Someone left, or someone new took the slot
            CloseDirect(slot);
            peer = Peer();
            peer.Key = key;
        }
    }

    void UpdateDirect(bool allowed) override {
        mAllowed = allowed;
        const Clock::time_point now = Clock::now();
        for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
            Peer& peer = mPeers[slot];
            if (!allowed || (peer.Key == 0)) {
                CloseDirect(slot);
                continue;
            }
            // The lower slot offers; the other side's connection waits for that offer.
            if (!peer.Connecting && (now - peer.LastAttempt >= kRetryInterval)) {
                StartDirect(slot, mSlot < slot);
            }
            if (peer.Open && (now - peer.LastPing >= kPingInterval)) {
                SendPing(slot, now);
            }
            const bool direct = peer.Open && HasHeardRecently(peer, now);
            if (direct != mWasDirect[slot]) {
                mWasDirect[slot] = direct;
                NetLog(direct ? "Direct connection to slot %d over WebRTC"
                              : "Direct connection to slot %d lost; using the relay",
                       slot);
            }
        }
    }

    bool HasDirect(u8 slot) const override {
        return mAllowed && (slot < kMaxSlots) && mPeers[slot].Open && HasHeardRecently(mPeers[slot], Clock::now());
    }

    s32 GetDirectPingMs(u8 slot) const override {
        return HasDirect(slot) ? mPeers[slot].PingMs : -1;
    }

    void SendDirect(u8 slot, const u8* data, size_t size) override {
        if (!HasDirect(slot) || (size > kMaxPayload)) {
            return;
        }
        u8 message[kMaxPayload + 1];
        message[0] = kChannelPayload;
        memcpy(message + 1, data, size);
        sk_rtc_send(slot, message, (int) (size + 1));
    }

    void HandleSignal(u8 slot, const u8* data, size_t size) override {
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
        // An offer from a member whose connection this side has not started (or gave up on) starts one.
        if (!peer.Connecting) {
            StartDirect(slot, false);
        }
        sk_rtc_signal(slot, whole.data(), (int) whole.size());
    }

  private:
    static std::string MakeUrl(const std::string& address) {
        if ((address.rfind("ws://", 0) == 0) || (address.rfind("wss://", 0) == 0)) {
            return address;
        }
        const std::string hostAndPort =
            (address.find(':') == std::string::npos) ? address + ":" + std::to_string(kDefaultPort) : address;
        // A page served over HTTPS may only open secure WebSockets: the relay then needs a proxy that adds TLS.
        return (sk_page_is_secure() ? "wss://" : "ws://") + hostAndPort + "/";
    }

    void Fail(const std::string& error) {
        mError = error;
        mState = State::Failed;
    }

    static EM_BOOL OnOpen(int, const EmscriptenWebSocketOpenEvent*, void* userData) {
        static_cast<RelayLinkWeb*>(userData)->mState = State::Open;
        return EM_TRUE;
    }

    static EM_BOOL OnMessage(int, const EmscriptenWebSocketMessageEvent* event, void* userData) {
        if (!event->isText) {
            static_cast<RelayLinkWeb*>(userData)->mIncoming.emplace_back(event->data, event->data + event->numBytes);
        }
        return EM_TRUE;
    }

    static EM_BOOL OnError(int, const EmscriptenWebSocketErrorEvent*, void* userData) {
        static_cast<RelayLinkWeb*>(userData)->Disconnected();
        return EM_TRUE;
    }

    static EM_BOOL OnClose(int, const EmscriptenWebSocketCloseEvent*, void* userData) {
        static_cast<RelayLinkWeb*>(userData)->Disconnected();
        return EM_TRUE;
    }

    void Disconnected() {
        if (mState == State::Failed) {
            return;
        }
        Fail((mState == State::Open) ? "Lost the connection to the relay server"
                                     : "Could not connect to the relay server at " + mAddress);
    }

    static bool HasHeardRecently(const Peer& peer, Clock::time_point now) {
        return now - peer.LastHeard <= kDirectTimeout;
    }

    void StartDirect(s32 slot, bool initiator) {
        NetLog("WebRTC: connecting to slot %d (%s)", slot, initiator ? "offering" : "waiting for its offer");
        Peer& peer = mPeers[slot];
        peer.Connecting = true;
        peer.Open = false;
        peer.LastAttempt = Clock::now();
        sk_rtc_open(slot, initiator ? 1 : 0, kIceServers);
    }

    void CloseDirect(s32 slot) {
        Peer& peer = mPeers[slot];
        if (peer.Connecting) {
            NetLog("WebRTC: closing the connection to slot %d", slot);
            sk_rtc_close(slot);
        }
        if (mWasDirect[slot]) {
            NetLog("Direct connection to slot %d lost; using the relay", slot);
            mWasDirect[slot] = false;
        }
        peer.Connecting = false;
        peer.Open = false;
        peer.PingMs = -1;
    }

    void SendPing(s32 slot, Clock::time_point now) {
        mPeers[slot].LastPing = now;
        u8 message[5] = { kChannelPing };
        const u32 time = GetMilliseconds(now);
        memcpy(message + 1, &time, sizeof(time));
        sk_rtc_send(slot, message, sizeof(message));
    }

    void HandleRtcEvent(s32 slot, int kind, const u8* data, size_t size) {
        Peer& peer = mPeers[slot];
        const Clock::time_point now = Clock::now();
        switch (kind) {
            case kEventSignal:
                SendSignal(slot, data, size);
                break;
            case kEventOpen:
                peer.Open = true;
                peer.LastHeard = now;
                // Measures the round trip at once, for the automatic input delay.
                SendPing(slot, now);
                break;
            case kEventClosed:
                // Tried again after kRetryInterval by the lower slot; the other waits for its offer.
                peer.Open = false;
                peer.Connecting = false;
                peer.LastAttempt = now;
                break;
            case kEventData:
                HandleChannelMessage(slot, data, size, now);
                break;
            default:
                break;
        }
    }

    void HandleChannelMessage(s32 slot, const u8* data, size_t size, Clock::time_point now) {
        if ((size == 0) || !mAllowed) {
            return;
        }
        Peer& peer = mPeers[slot];
        peer.LastHeard = now;
        if ((data[0] == kChannelPing) && (size == 5)) {
            u8 reply[5];
            memcpy(reply, data, sizeof(reply));
            reply[0] = kChannelPong;
            sk_rtc_send(slot, reply, sizeof(reply));
        } else if ((data[0] == kChannelPong) && (size == 5)) {
            u32 echo;
            memcpy(&echo, data + 1, sizeof(echo));
            const s32 sample = (s32) (GetMilliseconds(now) - echo);
            peer.PingMs = (peer.PingMs < 0) ? sample : (peer.PingMs * (kPingSmoothing - 1) + sample) / kPingSmoothing;
        } else if (data[0] == kChannelPayload) {
            mCallbacks.FromMember((u8) slot, data + 1, size - 1);
        }
    }

    /** Passes a setup message to the member through the relay, in parts if it is long. */
    void SendSignal(s32 slot, const u8* data, size_t size) {
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

    u32 GetMilliseconds(Clock::time_point now) const {
        return (u32) std::chrono::duration_cast<std::chrono::milliseconds>(now - mEpoch).count();
    }

    Callbacks mCallbacks;
    State mState = State::Connecting;
    std::string mError;
    std::string mAddress;
    EMSCRIPTEN_WEBSOCKET_T mSocket = 0;
    std::deque<std::vector<u8>> mIncoming;
    s32 mSlot = -1;
    std::array<Peer, kMaxSlots> mPeers;
    std::array<bool, kMaxSlots> mWasDirect = {};
    bool mAllowed = false;
    Clock::time_point mEpoch = Clock::now();
};

} // namespace

std::unique_ptr<RelayLink> RelayLink::Create(const Callbacks& callbacks) {
    return std::make_unique<RelayLinkWeb>(callbacks);
}

#endif // __EMSCRIPTEN__
