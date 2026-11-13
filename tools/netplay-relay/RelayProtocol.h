#ifndef NETPLAY_RELAY_PROTOCOL_H
#define NETPLAY_RELAY_PROTOCOL_H

/**
 * Wire format between SpaghettiKart and the netplay relay server. Shared by both; depends only on the standard library.
 *
 * Players meet in rooms named by a 6-character code, which they pick from the relay's list of open rooms. The relay
 * forwards packets between the members of a room and knows nothing about the game: lobby and rollback traffic are
 * opaque payloads to it.
 *
 * Every datagram starts with a 4-byte header: magic (u16), protocol version (u8), message type (u8).
 * All integers are little-endian.
 *
 *   Client -> relay
 *     Create     u32 request, char build[32], u64 content, char name[16]
 *                Open a room; the creator gets slot 0 and hosts. The name is the host's, shown in the room list.
 *     Join       u32 request, char code[6], char build[32], u64 content
 *     Relay      u64 token, u8 destination (slot or kBroadcast), payload
 *     KeepAlive  u64 token, u32 echo, u8 KeepAliveFlags, u32 local IPv4, u16 local port
 *                Sent every second; the relay answers with RoomState. The local address is the client's own on its
 *                network, for members behind the same router.
 *     Leave      u64 token
 *     ListRooms  u32 request, char build[32], u64 content          Ask for the open rooms of this build.
 *
 *   Relay -> client
 *     Joined     u32 request, u64 token, char code[6], u8 slot     Answer to Create or Join.
 *     Error      u32 request, u8 ErrorCode                         request is 0 for errors about a token.
 *     Relayed    u8 source slot, payload
 *     RoomState  u32 echo, u8 your slot, u8 occupied-slot mask, then for each occupied slot in order:
 *                u64 key, u32 public IPv4, u16 public port, u32 local IPv4, u16 local port
 *                On KeepAlive (echo copied) and on membership changes (0). The public address is where the relay sees
 *                the member; both addresses are 0 for members that do not share them.
 *     RoomClosed u8 CloseReason
 *     RoomList   u32 request, u8 count, then per room: char code[6], char host name[16], u8 members, u8 RoomFlags
 *                The rooms with the asker's build and content that have a free slot, at most kMaxListedRooms.
 *                Then u8 other builds, u8 other content: how many rooms with a free slot are of another build, and
 *                of this build with other game files or mods (relays since 2026-11-13; older ones end the list
 *                before them, and older games ignore them).
 *
 *   Client -> client, directly
 *     Punch      u8 source slot, u64 source key, u32 time          To each address a member may have, until one answers,
 *                                                                  then as a keep-alive.
 *     PunchReply u8 source slot, u64 source key, u32 echo          To where the Punch came from.
 *     Direct     u8 source slot, u64 source key, payload           What Relay would carry, without the relay.
 *
 * Create and Join are resent until answered; the relay answers a repeated request with the same reply.
 * The token identifies the client in later messages, so a client whose address changes (NAT rebinding) keeps its slot.
 * It stays between client and relay. Members recognize each other's direct packets by the key the relay hands out.
 *
 * Direct packets cross most home routers because each member sends from the socket it talks to the relay with: the
 * router maps it to the same public address the relay sees, and lets in replies from where it sent punches to.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace RelayProtocol {

constexpr uint16_t kMagic = 0x4B53; // "SK"
constexpr uint8_t kVersion = 3;
constexpr uint16_t kDefaultPort = 7777;
constexpr size_t kHeaderSize = 4;
constexpr size_t kCodeLength = 6;
constexpr size_t kBuildIdLength = 32;
constexpr size_t kNameLength = 16;
constexpr size_t kMaxSlots = 4;
constexpr uint8_t kBroadcast = 0xFF;
// Keeps datagrams under the 1280-byte IPv6 minimum MTU, so they are never fragmented.
constexpr size_t kMaxPayload = 1200;
constexpr size_t kMaxPacketSize = kHeaderSize + 16 + kMaxPayload;
// One RoomList datagram: 24 bytes per room stays well under kMaxPayload.
constexpr size_t kMaxListedRooms = 40;
// No 0/O or 1/I, which are easy to mix up when reading a code out loud.
constexpr char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr uint32_t kKeepAliveIntervalMs = 1000;
// The relay drops clients it has not heard from for this long.
constexpr uint32_t kClientTimeoutMs = 15000;

enum class MessageType : uint8_t {
    Create = 1,
    Join = 2,
    Relay = 3,
    KeepAlive = 4,
    Leave = 5,
    ListRooms = 6,

    Punch = 0x41,
    PunchReply = 0x42,
    Direct = 0x43,

    Joined = 0x81,
    Error = 0x82,
    Relayed = 0x83,
    RoomState = 0x84,
    RoomClosed = 0x85,
    RoomList = 0x86,
};

enum KeepAliveFlags : uint8_t {
    kShareAddresses = 1 << 0, // Give the other members this client's addresses, so they can connect directly
    kInRace = 1 << 1,         // The host is racing: the room list shows the room as racing
};

enum RoomFlags : uint8_t {
    kRoomRacing = 1 << 0,
};

enum class ErrorCode : uint8_t {
    None = 0,
    NoSuchRoom = 1,
    RoomFull = 2,
    BuildMismatch = 3,
    ContentMismatch = 4,
    RateLimited = 5,
    ServerFull = 6,
    UnknownClient = 7,
};

enum class CloseReason : uint8_t {
    HostLeft = 1,
    ServerShutdown = 2,
};

inline const char* GetErrorText(ErrorCode error) {
    switch (error) {
        case ErrorCode::NoSuchRoom:
            return "No room with that code";
        case ErrorCode::RoomFull:
            return "The room is full";
        case ErrorCode::BuildMismatch:
            return "The host runs a different version of the game";
        case ErrorCode::ContentMismatch:
            return "The host has different game files or mods";
        case ErrorCode::RateLimited:
            return "Too many requests; try again in a minute";
        case ErrorCode::ServerFull:
            return "The relay server is full";
        case ErrorCode::UnknownClient:
            return "The relay server no longer knows this client";
        default:
            return "Unknown error";
    }
}

using RoomCode = std::array<char, kCodeLength>;
using BuildId = std::array<char, kBuildIdLength>;
using Name = std::array<char, kNameLength>;

inline BuildId MakeBuildId(const std::string& text) {
    BuildId id = {};
    memcpy(id.data(), text.data(), (text.size() < id.size()) ? text.size() : id.size());
    return id;
}

/** A name padded with zeros, cut to kNameLength. */
inline Name MakeName(const std::string& text) {
    Name name = {};
    memcpy(name.data(), text.data(), (text.size() < name.size()) ? text.size() : name.size());
    return name;
}

/** The text of a zero-padded name. */
inline std::string ToString(const Name& name) {
    size_t length = 0;
    while ((length < name.size()) && (name[length] != 0)) {
        length++;
    }
    return std::string(name.data(), length);
}

/** Appends little-endian values to a fixed buffer. Writes past the end are dropped and mark the writer as failed. */
class ByteWriter {
  public:
    ByteWriter(uint8_t* buffer, size_t capacity) : mBuffer(buffer), mCapacity(capacity) {
    }

    void Write8(uint8_t value) {
        WriteBytes(&value, 1);
    }
    void Write16(uint16_t value) {
        const uint8_t bytes[] = { (uint8_t) value, (uint8_t) (value >> 8) };
        WriteBytes(bytes, sizeof(bytes));
    }
    void Write32(uint32_t value) {
        Write16((uint16_t) value);
        Write16((uint16_t) (value >> 16));
    }
    void Write64(uint64_t value) {
        Write32((uint32_t) value);
        Write32((uint32_t) (value >> 32));
    }
    void WriteBytes(const void* data, size_t size) {
        if (mFailed || (size > mCapacity - mSize)) {
            mFailed = true;
            return;
        }
        memcpy(mBuffer + mSize, data, size);
        mSize += size;
    }
    void WriteHeader(MessageType type) {
        Write16(kMagic);
        Write8(kVersion);
        Write8((uint8_t) type);
    }

    size_t GetSize() const {
        return mSize;
    }
    bool Failed() const {
        return mFailed;
    }

  private:
    uint8_t* mBuffer;
    size_t mCapacity;
    size_t mSize = 0;
    bool mFailed = false;
};

/** Reads little-endian values. Reads past the end return zeros and mark the reader as failed. */
class ByteReader {
  public:
    ByteReader(const uint8_t* data, size_t size) : mData(data), mSize(size) {
    }

    uint8_t Read8() {
        uint8_t value = 0;
        ReadBytes(&value, 1);
        return value;
    }
    uint16_t Read16() {
        uint8_t bytes[2] = {};
        ReadBytes(bytes, sizeof(bytes));
        return (uint16_t) (bytes[0] | (bytes[1] << 8));
    }
    uint32_t Read32() {
        const uint32_t low = Read16();
        return low | ((uint32_t) Read16() << 16);
    }
    uint64_t Read64() {
        const uint64_t low = Read32();
        return low | ((uint64_t) Read32() << 32);
    }
    void ReadBytes(void* out, size_t size) {
        if (mFailed || (size > mSize - mOffset)) {
            mFailed = true;
            memset(out, 0, size);
            return;
        }
        memcpy(out, mData + mOffset, size);
        mOffset += size;
    }
    /** Reads the header and checks magic and version. @returns false for packets that are not ours. */
    bool ReadHeader(MessageType* type) {
        const uint16_t magic = Read16();
        const uint8_t version = Read8();
        *type = (MessageType) Read8();
        return !mFailed && (magic == kMagic) && (version == kVersion);
    }

    const uint8_t* GetRemaining() const {
        return mData + mOffset;
    }
    size_t GetRemainingSize() const {
        return mFailed ? 0 : mSize - mOffset;
    }
    bool Failed() const {
        return mFailed;
    }

  private:
    const uint8_t* mData;
    size_t mSize;
    size_t mOffset = 0;
    bool mFailed = false;
};

} // namespace RelayProtocol

#endif // NETPLAY_RELAY_PROTOCOL_H
