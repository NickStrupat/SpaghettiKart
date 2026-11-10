#ifndef NETPLAY_WEBRTC_PEERS_H
#define NETPLAY_WEBRTC_PEERS_H

#include <libultraship/libultraship.h>

#include <array>
#include <chrono>
#include <functional>
#include <vector>

#include "RelayLink.h"

/**
 * Direct connections over WebRTC data channels (WebRtcTransport.h) to the other members of a room: the browser's only
 * way to reach them without the relay, and the desktop game's way to reach browsers. Setup messages travel through the
 * relay, in parts when longer than one payload. A ping every 500 ms measures the round trip and notices a connection
 * gone quiet.
 *
 * One side offers, the other answers. A desktop game never offers: it keeps UDP punching for other desktops
 * (RelayLinkUdp.cpp) and answers browsers. So a browser offers to every desktop that shares its addresses (the only
 * members the relay shows an address for), and between two browsers the lower slot offers. A desktop that does not
 * share its addresses ignores offers too, which keeps its address private.
 *
 * Single-threaded: call everything from the game thread.
 */
class WebRtcPeers {
  public:
    enum class Role { Browser, Desktop };

    struct Callbacks {
        /** A payload a member sent (RelayClient's channel byte first). */
        std::function<void(u8 slot, const u8* data, size_t size)> FromMember;
        /** A setup message to pass to a member through the relay. */
        std::function<void(u8 slot, const u8* data, size_t size)> SendSignal;
    };

    WebRtcPeers(Role role, const Callbacks& callbacks);
    ~WebRtcPeers();

    void SetMembers(s32 ownSlot, const RelayLink::Members& members);
    /** Once per frame in a room: starts, keeps and pings connections. With allowed false, closes them all. */
    void Update(bool allowed);
    /** Takes what the transport received. */
    void Receive();
    bool HasDirect(u8 slot) const;
    s32 GetPingMs(u8 slot) const;
    void Send(u8 slot, const u8* data, size_t size);
    void HandleSignal(u8 slot, const u8* data, size_t size);

  private:
    using Clock = std::chrono::steady_clock;

    struct Peer {
        u64 Key = 0; // 0 while the slot is free
        bool SharesAddress = false; // A desktop game that takes WebRTC offers
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
        bool WasDirect = false;
    };

    bool ShouldOffer(s32 slot) const;
    void StartDirect(s32 slot, bool initiator);
    void CloseDirect(s32 slot);
    void SendPing(s32 slot, Clock::time_point now);
    void HandleEvent(s32 slot, int kind, const u8* data, size_t size);
    void HandleChannelMessage(s32 slot, const u8* data, size_t size, Clock::time_point now);
    void SendSignal(s32 slot, const u8* data, size_t size);
    u32 GetMilliseconds(Clock::time_point now) const;
    static bool HasHeardRecently(const Peer& peer, Clock::time_point now);

    Role mRole;
    Callbacks mCallbacks;
    s32 mSlot = -1;
    std::array<Peer, RelayProtocol::kMaxSlots> mPeers;
    bool mAllowed = false;
    Clock::time_point mEpoch = Clock::now();
};

#endif // NETPLAY_WEBRTC_PEERS_H
