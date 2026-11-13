#ifndef NETPLAY_RELAY_CLIENT_H
#define NETPLAY_RELAY_CLIENT_H

#include <libultraship/libultraship.h>

#include <memory>
#include <string>
#include <vector>

#include <gekkonet.h>

#include "tools/netplay-relay/RelayProtocol.h"

/**
 * The game's connection to the netplay relay: creates or joins a room, keeps the membership alive, and carries
 * two channels of packets between room members: lobby messages and GekkoNet's rollback traffic.
 *
 * Packets to one member go to it directly once a direct connection works, and through the relay otherwise, or again
 * once the direct way goes quiet. Broadcasts always go through the relay. How packets move is a RelayLink's business
 * (RelayLink.h): UDP with punching through routers on the desktop, a WebSocket and WebRTC in a browser.
 * Single-threaded: call Update() once per frame from the game thread. Only host name lookup runs on a worker thread.
 */
class RelayClient {
  public:
    enum class Status { Idle, Resolving, Joining, InRoom, Failed };

    struct LobbyPacket {
        u8 Source; // Sender's slot
        std::vector<u8> Data;
    };

    RelayClient();
    ~RelayClient();

    /**
     * Opens a new room on the relay at "host[:port]"; this client becomes its host, in slot 0. The room list shows it
     * under the host's name.
     */
    void Create(const std::string& relayAddress, const RelayProtocol::BuildId& build, u64 content,
                const std::string& hostName);
    void Join(const std::string& relayAddress, const std::string& code, const RelayProtocol::BuildId& build,
              u64 content);
    /** Leaves the room, if in one, and returns to Idle. */
    void Leave();
    /** Receives packets, sends keep-alives and retries, and notices a lost connection. */
    void Update();

    Status GetStatus() const;
    /** Why the client is Failed. */
    const std::string& GetError() const;
    std::string GetRoomCode() const;
    s32 GetSlot() const;
    /** Bit n is set while slot n is occupied. */
    u8 GetOccupiedSlots() const;
    /** Round trip to the relay, or -1 before it is measured. */
    s32 GetPingMs() const;

    /**
     * Whether to connect to the other members directly when their routers allow it, which is faster than through the
     * relay. It shares this client's addresses with them. Set before Create() or Join(); on by default.
     */
    void SetDirectEnabled(bool enabled);
    /** Testing: direct connections stop working this long after joining, as if a router closed them. */
    void BlockDirectAfter(u32 milliseconds);
    /** Whether this client is racing. The room list shows a host's room as racing. */
    void SetInRace(bool inRace);
    /** @returns true while packets to this slot go to it directly rather than through the relay. */
    bool IsDirect(u8 slot) const;
    /** Round trip straight to this slot, or -1 while not connected directly or not measured yet. */
    s32 GetDirectPingMs(u8 slot) const;

    /** Sends a lobby message to one slot, or to every other member with RelayProtocol::kBroadcast. */
    void SendLobby(u8 destination, const void* data, size_t size);
    std::vector<LobbyPacket> TakeLobbyPackets();

    /**
     * A GekkoNet network adapter over this connection. Peer addresses are one byte: the peer's slot.
     * Only GekkoNet packets carrying this session tag are delivered, so packets from an earlier race are dropped.
     */
    GekkoNetAdapter* GetGekkoAdapter(u16 sessionTag);

    struct Impl; // Defined in RelayClient.cpp; public so that the GekkoNet adapter functions there can reach it

  private:
    std::unique_ptr<Impl> mImpl;
};

/**
 * Asks a relay for its open rooms, for the room list on the ONLINE screen. Separate from RelayClient, which is the
 * connection to one room: the list is wanted before joining any. Single-threaded, like RelayClient.
 */
class RelayRoomList {
  public:
    struct Room {
        std::string Code;
        std::string HostName;
        u8 Members = 0;
        bool Racing = false;
    };

    RelayRoomList();
    ~RelayRoomList();

    /** Asks the relay at "host[:port]" for the rooms of this build and content. The answer replaces the list. */
    void Refresh(const std::string& relayAddress, const RelayProtocol::BuildId& build, u64 content);
    /** Receives the answer and resends a lost request. Call once per frame. */
    void Update();
    /** @returns true while a request waits for its answer. */
    bool IsBusy() const;
    /** @returns true once a list has arrived. */
    bool HasList() const;
    /** The rooms of the last answer, newest first. */
    const std::vector<Room>& GetRooms() const;
    /** How many open rooms the last answer left out for being of another build (up to 255). */
    u8 GetOtherBuildRooms() const;
    /** How many open rooms of this build the last answer left out for other game files or mods (up to 255). */
    u8 GetOtherContentRooms() const;
    /** Why the last request failed, or empty. */
    const std::string& GetError() const;

    struct Impl;

  private:
    std::unique_ptr<Impl> mImpl;
};

#endif // NETPLAY_RELAY_CLIENT_H
