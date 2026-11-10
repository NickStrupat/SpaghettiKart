#ifndef NETPLAY_RELAY_LINK_H
#define NETPLAY_RELAY_LINK_H

#include <libultraship/libultraship.h>

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "tools/netplay-relay/RelayProtocol.h"

/**
 * How a RelayClient reaches the relay and the other members of its room; RelayClient keeps the protocol and a link only
 * moves packets. The desktop game uses one UDP socket for both, punching through routers to reach members directly
 * (RelayLinkUdp.cpp). A browser has no UDP: it reaches the relay with a WebSocket and members with WebRTC data channels,
 * set up through messages the relay passes along (RelayLinkWeb.cpp).
 *
 * Single-threaded: call everything from the game thread.
 */
class RelayLink {
  public:
    enum class State { Connecting, Open, Failed };

    /** A member as the relay describes it (RoomState). Addresses are 0 when the member does not share them. */
    struct Member {
        u64 Key = 0; // 0 while the slot is free
        u32 PublicAddress = 0;
        u16 PublicPort = 0;
        u32 LocalAddress = 0;
        u16 LocalPort = 0;
    };
    using Members = std::array<Member, RelayProtocol::kMaxSlots>;

    struct Callbacks {
        /** A datagram from the relay. */
        std::function<void(const u8* data, size_t size)> FromRelay;
        /** What a member sent directly: a payload as Relay carries it (RelayClient's channel byte first). */
        std::function<void(u8 slot, const u8* data, size_t size)> FromMember;
        /** Asks the client to pass a message to a member through the relay, for links that set up connections so. */
        std::function<void(u8 slot, const u8* data, size_t size)> SendSignal;
    };

    /** The link this platform uses. */
    static std::unique_ptr<RelayLink> Create(const Callbacks& callbacks);

    virtual ~RelayLink() = default;

    /** Starts connecting to the relay at "host[:port]". */
    virtual void Open(const std::string& relayAddress) = 0;
    virtual State GetState() const = 0;
    /** Why the link is Failed. */
    virtual const std::string& GetError() const = 0;
    virtual void SendToRelay(const u8* data, size_t size) = 0;
    /** Delivers what has arrived through the callbacks. */
    virtual void Receive() = 0;
    /** This computer's address on its network and the port members behind the same router reach it at; 0 if none. */
    virtual void GetLocalAddress(u32* address, u16* port) const = 0;

    /** The room's members, this client's slot and its key, whenever the relay describes them. */
    virtual void SetMembers(s32 ownSlot, u64 ownKey, const Members& members) = 0;
    /**
     * Once per frame in a room: connects to the members not reached directly yet, keeps the reached ones, and notices
     * lost ones. With allowed false, drops every direct connection and ignores direct packets.
     */
    virtual void UpdateDirect(bool allowed) = 0;
    /** @returns true while this member is reached directly. */
    virtual bool HasDirect(u8 slot) const = 0;
    /** Round trip straight to this member, or -1 while not connected directly or not measured yet. */
    virtual s32 GetDirectPingMs(u8 slot) const = 0;
    /** Sends a payload straight to a member reached directly. */
    virtual void SendDirect(u8 slot, const u8* data, size_t size) = 0;
    /** A message from a member that the client passed along through the relay (Callbacks::SendSignal). */
    virtual void HandleSignal(u8 slot, const u8* data, size_t size) = 0;
};

#endif // NETPLAY_RELAY_LINK_H
