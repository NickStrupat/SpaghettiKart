// The desktop game's RelayLink (RelayLink.h): one UDP socket for the relay and for direct connections to other desktop
// games, and WebRTC for direct connections to browsers (WebRtcPeers.h), which have no UDP. The browser build uses
// RelayLinkWeb.cpp instead.
#ifndef __EMSCRIPTEN__

// asio first: on Windows it must include winsock2.h before anything includes windows.h.
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif
#define ASIO_STANDALONE
#include <asio.hpp>

#include "RelayLink.h"

#include <chrono>
#include <future>

#include "NetLog.h"
#include "WebRtcPeers.h"

using namespace RelayProtocol;
using asio::ip::udp;
using Clock = std::chrono::steady_clock;

namespace {

constexpr s32 kPingSmoothing = 4; // Each round trip moves the ping a quarter of the way
// Punches go out this often until a member answers directly, then less often, to keep the routers' mappings open.
constexpr auto kPunchInterval = std::chrono::milliseconds(250);
constexpr auto kPunchKeepAliveInterval = std::chrono::milliseconds(500);
// A direct connection silent for this long is given up for the relay, well before GekkoNet's 5 s disconnect.
constexpr auto kDirectTimeout = std::chrono::milliseconds(1500);

/** Another member, as far as connecting to it directly goes. */
struct Peer {
    u64 Key = 0; // 0 while the slot is free or the relay has not described it yet
    // Where it may be reached: where the relay sees it, and its own address on its network. Port 0 when unknown.
    std::array<udp::endpoint, 2> Candidates;
    bool HasDirect = false;
    udp::endpoint Direct; // Where its direct packets come from
    Clock::time_point LastHeard;
    Clock::time_point LastPunch;
    s32 PingMs = -1;
};

std::string ToString(const udp::endpoint& endpoint) {
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

udp::endpoint MakeEndpoint(u32 address, u16 port) {
    return (port == 0) ? udp::endpoint() : udp::endpoint(asio::ip::address_v4(address), port);
}

struct ResolveResult {
    udp::endpoint Endpoint;
    std::string Error;
};

ResolveResult Resolve(const std::string& address) {
    const size_t colon = address.rfind(':');
    const std::string host = address.substr(0, colon);
    const std::string port = (colon == std::string::npos) ? std::to_string(kDefaultPort) : address.substr(colon + 1);
    try {
        asio::io_context io;
        udp::resolver resolver(io);
        const auto results = resolver.resolve(udp::v4(), host, port);
        if (results.empty()) {
            return { {}, "Could not find the relay server " + address };
        }
        return { results.begin()->endpoint(), "" };
    } catch (const std::exception& e) {
        return { {}, "Could not find the relay server " + address + ": " + e.what() };
    }
}

class RelayLinkUdp : public RelayLink {
  public:
    explicit RelayLinkUdp(const Callbacks& callbacks)
        : mCallbacks(callbacks), mRtc(WebRtcPeers::Role::Desktop, { callbacks.FromMember, callbacks.SendSignal }) {
    }

    void Open(const std::string& relayAddress) override {
        mState = State::Connecting;
        mResolving = std::async(std::launch::async, Resolve, relayAddress);
    }

    State GetState() const override {
        return mState;
    }

    const std::string& GetError() const override {
        return mError;
    }

    void SendToRelay(const u8* data, size_t size) override {
        SendTo(mRelay, data, size);
    }

    void Receive() override {
        mRtc.Receive();
        if (mState == State::Connecting) {
            FinishOpening();
        }
        if (mSocket == nullptr) {
            return;
        }
        u8 buffer[2048];
        udp::endpoint sender;
        while (mSocket != nullptr) {
            asio::error_code error;
            const size_t size = mSocket->receive_from(asio::buffer(buffer), sender, 0, error);
            if (error == asio::error::would_block) {
                return;
            }
            // Other errors (such as ICMP "port unreachable" on Windows) concern one packet; keep reading.
            if (error) {
                continue;
            }
            if (sender == mRelay) {
                mCallbacks.FromRelay(buffer, size);
            } else {
                HandleDirect(buffer, size, sender, Clock::now());
            }
        }
    }

    void GetLocalAddress(u32* address, u16* port) const override {
        const bool hasLocal = mLocalEndpoint.address().is_v4();
        *address = hasLocal ? mLocalEndpoint.address().to_v4().to_uint() : 0;
        *port = hasLocal ? mLocalEndpoint.port() : 0;
    }

    void SetMembers(s32 ownSlot, u64 ownKey, const Members& members) override {
        mRtc.SetMembers(ownSlot, members);
        mSlot = ownSlot;
        mKey = ownKey;
        for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
            const Member& member = members[slot];
            Peer& peer = mPeers[slot];
            if ((member.Key == 0) || (slot == ownSlot)) {
                peer = Peer();
                continue;
            }
            if (peer.Key != member.Key) {
                // Someone new in this slot
                peer = Peer();
                peer.Key = member.Key;
            }
            const std::array<udp::endpoint, 2> candidates = {
                Reachable(MakeEndpoint(member.PublicAddress, member.PublicPort)),
                Reachable(MakeEndpoint(member.LocalAddress, member.LocalPort))
            };
            if ((candidates != peer.Candidates) && (candidates[0].port() != 0)) {
                const bool same = (candidates[1].port() == 0) || (candidates[1] == candidates[0]);
                NetLog("Slot %d can be reached at %s%s%s", slot, ToString(candidates[0]).c_str(), same ? "" : " or ",
                       same ? "" : ToString(candidates[1]).c_str());
            }
            peer.Candidates = candidates;
        }
    }

    /** Punches towards every member not reached directly yet, keeps the reached ones open, and notices lost ones. */
    void UpdateDirect(bool allowed) override {
        mRtc.Update(allowed);
        mAllowed = allowed;
        if (!allowed) {
            for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
                LoseDirect(slot);
            }
            return;
        }
        const Clock::time_point now = Clock::now();
        for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
            Peer& peer = mPeers[slot];
            if ((slot == mSlot) || (peer.Key == 0)) {
                continue;
            }
            if (peer.HasDirect && (now - peer.LastHeard > kDirectTimeout)) {
                LoseDirect(slot);
            }
            if (now - peer.LastPunch < (peer.HasDirect ? kPunchKeepAliveInterval : kPunchInterval)) {
                continue;
            }
            peer.LastPunch = now;
            u8 packet[kHeaderSize + 1 + 8 + 4];
            ByteWriter writer(packet, sizeof(packet));
            writer.WriteHeader(MessageType::Punch);
            writer.Write8((u8) mSlot);
            writer.Write64(mKey);
            writer.Write32(GetMilliseconds(now));
            for (size_t i = 0; i < peer.Candidates.size(); i++) {
                const udp::endpoint& candidate = peer.Candidates[i];
                if ((candidate.port() != 0) && ((i == 0) || (candidate != peer.Candidates[0]))) {
                    SendTo(candidate, packet, writer.GetSize());
                }
            }
            if (peer.HasDirect && (peer.Direct != peer.Candidates[0]) && (peer.Direct != peer.Candidates[1])) {
                SendTo(peer.Direct, packet, writer.GetSize());
            }
        }
    }

    bool HasDirect(u8 slot) const override {
        return HasPunchedDirect(slot) || mRtc.HasDirect(slot);
    }

    s32 GetDirectPingMs(u8 slot) const override {
        return HasPunchedDirect(slot) ? mPeers[slot].PingMs : mRtc.GetPingMs(slot);
    }

    void SendDirect(u8 slot, const u8* data, size_t size) override {
        if (!HasPunchedDirect(slot)) {
            mRtc.Send(slot, data, size);
            return;
        }
        u8 packet[kMaxPacketSize];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::Direct);
        writer.Write8((u8) mSlot);
        writer.Write64(mKey);
        writer.WriteBytes(data, size);
        if (!writer.Failed()) {
            SendTo(mPeers[slot].Direct, packet, writer.GetSize());
        }
    }

    void HandleSignal(u8 slot, const u8* data, size_t size) override {
        // Only browsers send these: desktop games connect to each other by punching.
        mRtc.HandleSignal(slot, data, size);
    }

  private:
    /** @returns true while punching reached this member, a desktop game. */
    bool HasPunchedDirect(u8 slot) const {
        return mAllowed && (slot < kMaxSlots) && mPeers[slot].HasDirect;
    }

    void FinishOpening() {
        if (mResolving.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        const ResolveResult result = mResolving.get();
        if (!result.Error.empty()) {
            Fail(result.Error);
            return;
        }
        try {
            mSocket = std::make_unique<udp::socket>(mIo, udp::endpoint(udp::v4(), 0));
            mSocket->non_blocking(true);
        } catch (const std::exception& e) {
            Fail(std::string("Could not open a network socket: ") + e.what());
            return;
        }
        mRelay = result.Endpoint;
        mLocalEndpoint = FindLocalEndpoint();
        mState = State::Open;
    }

    void Fail(const std::string& error) {
        mSocket.reset();
        mError = error;
        mState = State::Failed;
    }

    /** @returns this computer's address on its network, the one packets to the relay leave from, and the port. */
    udp::endpoint FindLocalEndpoint() {
        asio::error_code error;
        udp::socket probe(mIo);
        probe.open(udp::v4(), error);
        // Connecting a UDP socket sends nothing; it only picks the route.
        probe.connect(mRelay, error);
        const udp::endpoint local = probe.local_endpoint(error);
        const unsigned short port = mSocket->local_endpoint(error).port();
        return error ? udp::endpoint() : udp::endpoint(local.address(), port);
    }

    /**
     * Where this client can send to one of a member's addresses. A member on the relay's own computer has only
     * loopback addresses (the relay sees it there, and its packets to the relay never leave the computer), which mean
     * this client's own computer. It is reached at the relay's address instead, the way this client reaches the relay:
     * on the same network, that computer's address there; across the internet, its router's, on the member's own port,
     * which most home routers keep for the packets the member sends out.
     */
    udp::endpoint Reachable(const udp::endpoint& candidate) const {
        return candidate.address().is_loopback() ? udp::endpoint(mRelay.address(), candidate.port()) : candidate;
    }

    void LoseDirect(s32 slot) {
        Peer& peer = mPeers[slot];
        if (peer.HasDirect) {
            peer.HasDirect = false;
            NetLog("Direct connection to slot %d lost; using the relay", slot);
        }
    }

    void SendTo(const udp::endpoint& to, const u8* data, size_t size) {
        if (mSocket != nullptr) {
            asio::error_code error;
            mSocket->send_to(asio::buffer(data, size), to, 0, error);
        }
    }

    u32 GetMilliseconds(Clock::time_point now) const {
        return (u32) std::chrono::duration_cast<std::chrono::milliseconds>(now - mEpoch).count();
    }

    /** A packet from another member, not through the relay. */
    void HandleDirect(const u8* data, size_t size, const udp::endpoint& sender, Clock::time_point now) {
        ByteReader reader(data, size);
        MessageType type;
        if (!reader.ReadHeader(&type) || (mSlot < 0) || !mAllowed) {
            return;
        }
        const u8 source = reader.Read8();
        const u64 key = reader.Read64();
        if (reader.Failed() || (source >= kMaxSlots) || ((s32) source == mSlot) || (mPeers[source].Key == 0) ||
            (mPeers[source].Key != key)) {
            return;
        }
        Peer& peer = mPeers[source];
        switch (type) {
            case MessageType::Punch: {
                const u32 time = reader.Read32();
                if (reader.Failed()) {
                    return;
                }
                u8 packet[kHeaderSize + 1 + 8 + 4];
                ByteWriter writer(packet, sizeof(packet));
                writer.WriteHeader(MessageType::PunchReply);
                writer.Write8((u8) mSlot);
                writer.Write64(mKey);
                writer.Write32(time);
                SendTo(sender, packet, writer.GetSize());
                break;
            }
            case MessageType::PunchReply: {
                const u32 echo = reader.Read32();
                if (reader.Failed()) {
                    return;
                }
                const s32 sample = (s32) (GetMilliseconds(now) - echo);
                peer.PingMs = (peer.PingMs < 0) ? sample : (peer.PingMs * (kPingSmoothing - 1) + sample) / kPingSmoothing;
                break;
            }
            case MessageType::Direct:
                mCallbacks.FromMember(source, reader.GetRemaining(), reader.GetRemainingSize());
                break;
            default:
                return;
        }
        // Any of these shows the way between the two works both ways: the sender's router let out what it sent here.
        // The first address that works is kept; others that also work (both a public and a local one) are spares.
        if (!peer.HasDirect) {
            peer.HasDirect = true;
            peer.Direct = sender;
            // Punch back at once, which measures the round trip for the automatic input delay.
            peer.LastPunch = Clock::time_point();
            NetLog("Direct connection to slot %u at %s", source, ToString(sender).c_str());
        }
        if (sender == peer.Direct) {
            peer.LastHeard = now;
        }
    }

    Callbacks mCallbacks;
    WebRtcPeers mRtc; // Direct connections to browsers
    State mState = State::Connecting;
    std::string mError;
    std::future<ResolveResult> mResolving;
    asio::io_context mIo;
    std::unique_ptr<udp::socket> mSocket;
    udp::endpoint mRelay;
    udp::endpoint mLocalEndpoint; // This client's own address on its network
    s32 mSlot = -1;
    u64 mKey = 0; // This client's, which its direct packets carry
    std::array<Peer, kMaxSlots> mPeers;
    bool mAllowed = false;
    Clock::time_point mEpoch = Clock::now();
};

} // namespace

std::unique_ptr<RelayLink> RelayLink::Create(const Callbacks& callbacks) {
    return std::make_unique<RelayLinkUdp>(callbacks);
}

#endif // __EMSCRIPTEN__
