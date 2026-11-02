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

    /** Opens a new room on the relay at "host[:port]"; this client becomes its host, in slot 0. */
    void Create(const std::string& relayAddress, const RelayProtocol::BuildId& build, u64 content);
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

#endif // NETPLAY_RELAY_CLIENT_H
