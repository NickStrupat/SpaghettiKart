#ifndef NETPLAY_RESYNC_H
#define NETPLAY_RESYNC_H

#include <libultraship/libultraship.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "SimState.h"

/**
 * Desync recovery: when the players' simulations have gone apart, every player takes over the host's state and the
 * race goes on from there, instead of ending.
 *
 * A snapshot is raw memory, pointers included. Every copy of the game runs without address space layout
 * randomization (SimState_DisableAddressRandomization()) and keeps its sim arena at one fixed address, so those
 * pointers hold in another copy of the same build. Pointers into the regular heap, such as to the loaded game files,
 * do not: the host marks each one, and the other players keep their own values there.
 *
 * NetSession.cpp runs a recovery:
 *   1. A player that detects a desync tells every other player (Request); each freezes its race.
 *   2. The host packs its state of a frame no input can change any more (Pack()) and goes back to that frame.
 *   3. Each other player pulls the packed state in chunks (Collector, Server), unpacks it onto its own (Unpack())
 *      and checks that its checksum is now the host's.
 *   4. Everyone confirms that frame through the segment barrier, and a new segment starts from it.
 * If any of it fails, the race ends, as it did before recovery existed.
 */
namespace Resync {

enum class MessageKind : u8 {
    Request = 1, // u32 segment: a player found a desync in this segment
    Pull = 2,    // u32 segment, u16 first chunk, u16 chunk count: a player asks the host for these chunks
    Chunk = 3,   // u32 segment, s32 frame, u16 chunk, u16 chunks in all, the chunk's bytes
};

struct Message {
    MessageKind Kind = MessageKind::Request;
    u32 Segment = 0;
    s32 Frame = 0;  // Chunk
    u16 Index = 0;  // Pull: the first chunk asked for; Chunk: this chunk
    u16 Count = 0;  // Pull: how many chunks; Chunk: chunks in all
    std::vector<u8> Data;
};

/** Sends a message to one player's peer, or with -1 to every other player (SessionSettings::SendResync). */
using SendFunction = std::function<void(s32 player, const std::vector<u8>& message)>;

std::vector<u8> MakeRequest(u32 segment);
/** @returns false for a message that is not a well-formed resync message. */
bool ReadMessage(const std::vector<u8>& bytes, Message* message);

/** @returns false, with the reason, when this copy of the game cannot take part in a recovery. */
bool IsAvailable(std::string* reason);

/** Packs a snapshot, compressed, with the place of every pointer in it into the regular heap. */
std::vector<u8> Pack(const SimSnapshot& snapshot, u32 checksum);

/**
 * Unpacks the host's state onto this player's own: the host's bytes, except where the host's point into its heap,
 * where this player's values from `own` stay.
 * @param own This player's state of the same frame, or as near to it as it has.
 * @param checksum The host's gameplay checksum of the state, to check the result against.
 * @returns false, with the reason, when the host's state cannot be used here.
 */
bool Unpack(const std::vector<u8>& packed, const SimSnapshot& own, SimSnapshot* state, u32* checksum,
            std::string* error);

/** The host's side of a recovery: answers each pull with the chunks it asks for. */
class Server {
  public:
    Server(u32 segment, s32 frame, std::vector<u8> packed);
    void HandlePull(s32 player, const Message& pull, const SendFunction& send) const;

  private:
    u32 mSegment;
    s32 mFrame;
    std::vector<u8> mPacked;
    u16 mNumChunks;
};

/** Another player's side of a recovery: pulls the host's packed state until every chunk has arrived. */
class Collector {
  public:
    explicit Collector(u32 segment);
    void Add(const Message& chunk);
    /** Asks the host for the next missing chunks, once the last ones asked for arrived or seem lost. */
    void Update(const SendFunction& send);
    bool IsComplete() const;
    /** The frame the host's state is from; -1 until a chunk has arrived. */
    s32 GetFrame() const;
    std::vector<u8> TakePacked();

  private:
    u32 mSegment;
    s32 mFrame = -1;
    std::vector<std::vector<u8>> mChunks;
    size_t mNumReceived = 0;
    u16 mPullEnd = 0;
    std::chrono::steady_clock::time_point mPulledAt;
    bool mPulled = false;
};

} // namespace Resync

#endif // NETPLAY_RESYNC_H
