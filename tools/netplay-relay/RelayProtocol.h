#ifndef NETPLAY_RELAY_PROTOCOL_H
#define NETPLAY_RELAY_PROTOCOL_H

/**
 * Wire format between SpaghettiKart and the netplay relay server. Shared by both; depends only on the standard library.
 *
 * Players meet in rooms named by a 6-character code. The relay forwards packets between the members of a room and
 * knows nothing about the game: lobby and rollback traffic are opaque payloads to it.
 *
 * Every datagram starts with a 4-byte header: magic (u16), protocol version (u8), message type (u8).
 * All integers are little-endian.
 *
 *   Client -> relay
 *     Create     u32 request, char build[32], u64 content         Open a room; the creator gets slot 0 and hosts.
 *     Join       u32 request, char code[6], char build[32], u64 content
 *     Relay      u64 token, u8 destination (slot or kBroadcast), payload
 *     KeepAlive  u64 token, u32 echo                              Sent every second; the relay answers with RoomState.
 *     Leave      u64 token
 *
 *   Relay -> client
 *     Joined     u32 request, u64 token, char code[6], u8 slot     Answer to Create or Join.
 *     Error      u32 request, u8 ErrorCode                         request is 0 for errors about a token.
 *     Relayed    u8 source slot, payload
 *     RoomState  u32 echo, u8 your slot, u8 occupied-slot mask     On KeepAlive (echo copied) and on membership changes (0).
 *     RoomClosed u8 CloseReason
 *
 * Create and Join are resent until answered; the relay answers a repeated request with the same reply.
 * The token identifies the client in later messages, so a client whose address changes (NAT rebinding) keeps its slot.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace RelayProtocol {

constexpr uint16_t kMagic = 0x4B53; // "SK"
constexpr uint8_t kVersion = 1;
constexpr uint16_t kDefaultPort = 7777;
constexpr size_t kHeaderSize = 4;
constexpr size_t kCodeLength = 6;
constexpr size_t kBuildIdLength = 32;
constexpr size_t kMaxSlots = 4;
constexpr uint8_t kBroadcast = 0xFF;
// Keeps datagrams under the 1280-byte IPv6 minimum MTU, so they are never fragmented.
constexpr size_t kMaxPayload = 1200;
constexpr size_t kMaxPacketSize = kHeaderSize + 16 + kMaxPayload;
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

    Joined = 0x81,
    Error = 0x82,
    Relayed = 0x83,
    RoomState = 0x84,
    RoomClosed = 0x85,
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

inline BuildId MakeBuildId(const std::string& text) {
    BuildId id = {};
    memcpy(id.data(), text.data(), (text.size() < id.size()) ? text.size() : id.size());
    return id;
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
