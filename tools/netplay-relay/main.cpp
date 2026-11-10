/**
 * Netplay relay server: lists rooms, matches players by room code and forwards their packets.
 *
 *   netplay-relay [--port 7777] [--ws-port 7777] [--delay-ms N] [--jitter-ms N] [--loss PERCENT] [--no-rate-limits]
 *
 * The desktop game talks to it over UDP on --port. Browsers, which cannot send UDP, connect with a WebSocket on TCP
 * port --ws-port (the same number by default; 0 turns WebSockets off) and send the same messages, one per binary
 * WebSocket message (WebSocket.h). Players of both kinds can share a room.
 * The delay, jitter and loss options degrade every packet the relay sends, to test rollback on one machine.
 * --no-rate-limits is for tests that run several fast-forwarded games from one address.
 * Protocol: see RelayProtocol.h.
 */

#include <asio.hpp>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "RelayProtocol.h"
#include "WebSocket.h"

using namespace RelayProtocol;
using asio::ip::tcp;
using asio::ip::udp;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t kMaxRooms = 5000;
constexpr size_t kMaxClients = kMaxRooms * kMaxSlots;
// Per source IP. A 4-player rollback session sends about 100 packets per second per player; a LAN party shares an IP.
constexpr double kPacketsPerSecond = 1500.0;
constexpr double kPacketBurst = 3000.0;
constexpr double kCreatesPerSecond = 10.0 / 60.0;
constexpr double kCreateBurst = 10.0;
// Slow enough that guessing a room code (32^6 possibilities) is hopeless.
constexpr double kJoinsPerSecond = 20.0 / 60.0;
constexpr double kJoinBurst = 20.0;
// The ONLINE screen asks again every few seconds while it is open.
constexpr double kListsPerSecond = 1.0;
constexpr double kListBurst = 10.0;
constexpr auto kHousekeepingInterval = std::chrono::seconds(1);
constexpr auto kRateLimitExpiry = std::chrono::seconds(120);
// A WebSocket that has sent nothing for this long is closed: a client in a room keeps it busy every second.
constexpr auto kWebSocketIdleTimeout = std::chrono::milliseconds(kClientTimeoutMs);
// Messages waiting to go out on one WebSocket before the connection counts as stuck and is closed.
constexpr size_t kMaxWebSocketQueue = 1024;

struct Options {
    uint16_t Port = kDefaultPort;
    uint16_t WebSocketPort = kDefaultPort;
    uint32_t DelayMs = 0;
    uint32_t JitterMs = 0;
    uint32_t LossPercent = 0;
    bool RateLimits = true;
};

/** Where a client is: a UDP endpoint, or a browser's WebSocket connection. */
struct Address {
    udp::endpoint Udp;
    uint64_t WebSocket = 0; // The connection's number; 0 for UDP

    bool operator==(const Address& other) const {
        return (Udp == other.Udp) && (WebSocket == other.WebSocket);
    }
    bool operator!=(const Address& other) const {
        return !(*this == other);
    }
    bool operator<(const Address& other) const {
        return std::tie(WebSocket, Udp) < std::tie(other.WebSocket, other.Udp);
    }
};

struct Client {
    uint64_t Token = 0;
    uint64_t Key = 0; // Shown to the other members, which recognize this client's direct packets by it
    Address Endpoint;
    // From KeepAlive: whether the other members may have this client's addresses, and its own on its network
    bool SharesAddresses = false;
    uint32_t LocalAddress = 0;
    uint16_t LocalPort = 0;
    std::string RoomKey;
    uint8_t Slot = 0;
    uint32_t Request = 0; // The Create or Join that made this client, to answer repeats
    Clock::time_point LastSeen;
};

struct Room {
    RoomCode Code = {};
    BuildId Build = {};
    uint64_t Content = 0;
    Name HostName = {};
    bool Racing = false; // From the host's KeepAlive
    Clock::time_point Created;
    std::array<uint64_t, kMaxSlots> Members = {}; // Client token per slot; 0 when free
};

class TokenBucket {
  public:
    TokenBucket(double burst) : mTokens(burst) {
    }

    bool Take(double perSecond, double burst, Clock::time_point now) {
        const double elapsed = std::chrono::duration<double>(now - mLast).count();
        mTokens = std::min(burst, mTokens + elapsed * perSecond);
        mLast = now;
        if (mTokens < 1.0) {
            return false;
        }
        mTokens -= 1.0;
        return true;
    }

    Clock::time_point GetLastUse() const {
        return mLast;
    }

  private:
    double mTokens;
    Clock::time_point mLast = Clock::now();
};

struct AddressLimits {
    TokenBucket Packets{ kPacketBurst };
    TokenBucket Creates{ kCreateBurst };
    TokenBucket Joins{ kJoinBurst };
    TokenBucket Lists{ kListBurst };
};

void Log(const char* format, ...) {
    char timestamp[32];
    const time_t now = time(nullptr);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", gmtime(&now));
    printf("%s ", timestamp);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

std::string ToString(const udp::endpoint& endpoint) {
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

std::string ToString(const Address& address) {
    return (address.WebSocket != 0) ? "WebSocket " + std::to_string(address.WebSocket) + " from " +
                                          address.Udp.address().to_string()
                                    : ToString(address.Udp);
}

/** A browser's connection. Until its handshake is done, it is an HTTP request. */
struct WebSocketConnection {
    WebSocketConnection(asio::io_context& io, uint64_t id) : Socket(io), Id(id) {
    }

    tcp::socket Socket;
    uint64_t Id;
    Address From;
    bool Open = false;
    std::vector<uint8_t> Received;
    std::array<uint8_t, 4096> ReadBuffer;
    std::deque<std::vector<uint8_t>> Outgoing; // The front one is being written
    bool Closing = false;                       // Closes once Outgoing has gone out
    Clock::time_point LastHeard;
};

std::string ToString(const RoomCode& code) {
    return std::string(code.data(), code.size());
}

class RelayServer {
  public:
    RelayServer(asio::io_context& io, const Options& options)
        : mIo(io), mOptions(options), mSocket(io, udp::endpoint(udp::v4(), options.Port)), mAcceptor(io),
          mHousekeeping(io), mDelayTimer(io), mRandom(std::random_device{}()) {
        Receive();
        if (options.WebSocketPort != 0) {
            mAcceptor.open(tcp::v4());
            mAcceptor.set_option(tcp::acceptor::reuse_address(true));
            mAcceptor.bind(tcp::endpoint(tcp::v4(), options.WebSocketPort));
            mAcceptor.listen();
            Accept();
        }
        ScheduleHousekeeping();
    }

    void Shutdown() {
        uint8_t packet[kHeaderSize + 1];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::RoomClosed);
        writer.Write8((uint8_t) CloseReason::ServerShutdown);
        for (const auto& [token, client] : mClients) {
            SendNow(client.Endpoint, packet, writer.GetSize());
        }
        Log("shutting down: closed %zu rooms", mRooms.size());
        mSocket.close();
        asio::error_code error;
        mAcceptor.close(error);
        for (auto& [id, connection] : mWebSockets) {
            // Lets the RoomClosed messages queued above go out before the connection closes.
            connection->Socket.shutdown(tcp::socket::shutdown_receive, error);
        }
        mHousekeeping.cancel();
        mDelayTimer.cancel();
    }

  private:
    void Receive() {
        mSocket.async_receive_from(asio::buffer(mReceiveBuffer), mSender, [this](asio::error_code error, size_t size) {
            if (error == asio::error::operation_aborted) {
                return;
            }
            if (!error) {
                Handle(mReceiveBuffer.data(), size, Address{ mSender, 0 });
            }
            Receive();
        });
    }

    void Accept() {
        auto connection = std::make_shared<WebSocketConnection>(mIo, ++mLastWebSocketId);
        mAcceptor.async_accept(connection->Socket, [this, connection](asio::error_code error) {
            if (error == asio::error::operation_aborted) {
                return;
            }
            if (!error && (mWebSockets.size() < kMaxClients)) {
                asio::error_code ignored;
                connection->Socket.set_option(tcp::no_delay(true), ignored);
                const tcp::endpoint remote = connection->Socket.remote_endpoint(ignored);
                connection->From = Address{ udp::endpoint(remote.address(), remote.port()), connection->Id };
                connection->LastHeard = Clock::now();
                mWebSockets[connection->Id] = connection;
                ReadWebSocket(connection);
            }
            Accept();
        });
    }

    void ReadWebSocket(const std::shared_ptr<WebSocketConnection>& connection) {
        connection->Socket.async_read_some(
            asio::buffer(connection->ReadBuffer), [this, connection](asio::error_code error, size_t size) {
                if (error) {
                    CloseWebSocket(connection->Id, "disconnected");
                    return;
                }
                connection->LastHeard = Clock::now();
                connection->Received.insert(connection->Received.end(), connection->ReadBuffer.begin(),
                                            connection->ReadBuffer.begin() + (std::ptrdiff_t) size);
                if (HandleWebSocketData(*connection)) {
                    ReadWebSocket(connection);
                } else {
                    CloseWebSocket(connection->Id, "closed");
                }
            });
    }

    /** Answers the handshake, then handles each complete message. @returns false when the connection should close. */
    bool HandleWebSocketData(WebSocketConnection& connection) {
        if (!connection.Open) {
            const std::string text(connection.Received.begin(), connection.Received.end());
            const size_t end = text.find("\r\n\r\n");
            if (end == std::string::npos) {
                return text.size() < WebSocket::kMaxHandshakeSize;
            }
            bool accepted = false;
            const std::string answer = WebSocket::AnswerHandshake(text.substr(0, end + 4), &accepted);
            QueueWebSocket(connection, std::vector<uint8_t>(answer.begin(), answer.end()));
            connection.Received.erase(connection.Received.begin(),
                                      connection.Received.begin() + (std::ptrdiff_t) (end + 4));
            if (!accepted) {
                return false;
            }
            connection.Open = true;
        }
        WebSocket::Frame frame;
        while (true) {
            const WebSocket::ParseResult result = WebSocket::ParseClientFrame(connection.Received, &frame);
            if (result == WebSocket::ParseResult::NeedMore) {
                return true;
            }
            // Relay messages are small: browsers send them whole, never in fragments.
            if ((result == WebSocket::ParseResult::Malformed) || !frame.Final) {
                return false;
            }
            switch (frame.Op) {
                case WebSocket::Opcode::Binary:
                    Handle(frame.Payload.data(), frame.Payload.size(), connection.From);
                    // Handling it may have closed this connection (a full queue).
                    if (mWebSockets.count(connection.Id) == 0) {
                        return false;
                    }
                    break;
                case WebSocket::Opcode::Ping:
                    QueueWebSocket(connection, WebSocket::MakeServerFrame(WebSocket::Opcode::Pong, frame.Payload.data(),
                                                                          frame.Payload.size()));
                    break;
                case WebSocket::Opcode::Close:
                    QueueWebSocket(connection, WebSocket::MakeServerFrame(WebSocket::Opcode::Close, nullptr, 0));
                    return false;
                default:
                    break;
            }
        }
    }

    void QueueWebSocket(WebSocketConnection& connection, std::vector<uint8_t> data) {
        if (connection.Closing) {
            return;
        }
        if (connection.Outgoing.size() >= kMaxWebSocketQueue) {
            CloseWebSocket(connection.Id, "too slow to read");
            return;
        }
        connection.Outgoing.push_back(std::move(data));
        if (connection.Outgoing.size() == 1) {
            WriteWebSocket(mWebSockets.at(connection.Id));
        }
    }

    void WriteWebSocket(const std::shared_ptr<WebSocketConnection>& connection) {
        asio::async_write(connection->Socket, asio::buffer(connection->Outgoing.front()),
                          [this, connection](asio::error_code error, size_t) {
                              if (error) {
                                  CloseWebSocket(connection->Id, "disconnected");
                                  return;
                              }
                              connection->Outgoing.pop_front();
                              if (!connection->Outgoing.empty()) {
                                  WriteWebSocket(connection);
                              } else if (connection->Closing) {
                                  asio::error_code ignored;
                                  connection->Socket.close(ignored);
                              }
                          });
    }

    /** Ends a browser's connection, and its place in a room: it cannot come back on another connection. */
    void CloseWebSocket(uint64_t id, const char* reason) {
        const auto it = mWebSockets.find(id);
        if (it == mWebSockets.end()) {
            return;
        }
        const std::shared_ptr<WebSocketConnection> connection = it->second;
        mWebSockets.erase(it);
        const Address from = connection->From;
        std::vector<uint64_t> tokens;
        for (const auto& [token, client] : mClients) {
            if (client.Endpoint == from) {
                tokens.push_back(token);
            }
        }
        for (uint64_t token : tokens) {
            RemoveClient(token, reason);
        }
        // Whatever is still queued (an answer to a bad handshake, a close frame) goes out before the socket closes:
        // the write in progress closes it when done.
        connection->Closing = true;
        asio::error_code error;
        connection->Socket.shutdown(tcp::socket::shutdown_receive, error);
        if (connection->Outgoing.empty()) {
            connection->Socket.close(error);
        }
    }

    void Handle(const uint8_t* data, size_t size, const Address& from) {
        const Clock::time_point now = Clock::now();
        AddressLimits& limits = mLimits[from.Udp.address()];
        if (mOptions.RateLimits && !limits.Packets.Take(kPacketsPerSecond, kPacketBurst, now)) {
            return;
        }

        ByteReader reader(data, size);
        MessageType type;
        if (!reader.ReadHeader(&type)) {
            return;
        }
        switch (type) {
            case MessageType::Create:
                HandleCreate(reader, from, limits, now);
                break;
            case MessageType::Join:
                HandleJoin(reader, from, limits, now);
                break;
            case MessageType::Relay:
                HandleRelay(reader, from, now);
                break;
            case MessageType::KeepAlive:
                HandleKeepAlive(reader, from, now);
                break;
            case MessageType::Leave:
                if (Client* client = FindClient(reader.Read64(), from, now)) {
                    RemoveClient(client->Token, "left");
                }
                break;
            case MessageType::ListRooms:
                HandleListRooms(reader, from, limits, now);
                break;
            default:
                break;
        }
    }

    // A repeated Create or Join (its reply was lost) gets the same reply.
    bool AnswerRepeatedRequest(const Address& from, uint32_t request) {
        const auto it = mRequests.find({ from, request });
        if (it == mRequests.end()) {
            return false;
        }
        SendJoined(mClients.at(it->second));
        return true;
    }

    void HandleCreate(ByteReader& reader, const Address& from, AddressLimits& limits, Clock::time_point now) {
        const uint32_t request = reader.Read32();
        Room room;
        reader.ReadBytes(room.Build.data(), room.Build.size());
        room.Content = reader.Read64();
        reader.ReadBytes(room.HostName.data(), room.HostName.size());
        room.Created = now;
        if (reader.Failed() || AnswerRepeatedRequest(from, request)) {
            return;
        }
        if (mOptions.RateLimits && !limits.Creates.Take(kCreatesPerSecond, kCreateBurst, now)) {
            SendError(from, request, ErrorCode::RateLimited);
            return;
        }
        if ((mRooms.size() >= kMaxRooms) || (mClients.size() >= kMaxClients)) {
            SendError(from, request, ErrorCode::ServerFull);
            return;
        }

        std::uniform_int_distribution<size_t> letter(0, sizeof(kCodeAlphabet) - 2);
        do {
            for (char& c : room.Code) {
                c = kCodeAlphabet[letter(mRandom)];
            }
        } while (mRooms.count(ToString(room.Code)) != 0);

        const std::string key = ToString(room.Code);
        Room& added = mRooms.emplace(key, room).first->second;
        Client& client = AddClient(added, 0, from, request, now);
        Log("room %s created by %s (%zu rooms)", key.c_str(), ToString(from).c_str(), mRooms.size());
        SendJoined(client);
        BroadcastRoomState(added);
    }

    void HandleJoin(ByteReader& reader, const Address& from, AddressLimits& limits, Clock::time_point now) {
        const uint32_t request = reader.Read32();
        RoomCode code;
        reader.ReadBytes(code.data(), code.size());
        BuildId build;
        reader.ReadBytes(build.data(), build.size());
        const uint64_t content = reader.Read64();
        if (reader.Failed() || AnswerRepeatedRequest(from, request)) {
            return;
        }
        if (mOptions.RateLimits && !limits.Joins.Take(kJoinsPerSecond, kJoinBurst, now)) {
            SendError(from, request, ErrorCode::RateLimited);
            return;
        }
        for (char& c : code) {
            c = (char) toupper((unsigned char) c);
        }

        const auto it = mRooms.find(ToString(code));
        if (it == mRooms.end()) {
            SendError(from, request, ErrorCode::NoSuchRoom);
            return;
        }
        Room& room = it->second;
        if (build != room.Build) {
            SendError(from, request, ErrorCode::BuildMismatch);
            return;
        }
        if (content != room.Content) {
            SendError(from, request, ErrorCode::ContentMismatch);
            return;
        }
        uint8_t slot = 0;
        while ((slot < kMaxSlots) && (room.Members[slot] != 0)) {
            slot++;
        }
        if ((slot == kMaxSlots) || (mClients.size() >= kMaxClients)) {
            SendError(from, request, ErrorCode::RoomFull);
            return;
        }

        Client& client = AddClient(room, slot, from, request, now);
        Log("room %s: %s joined as slot %u", it->first.c_str(), ToString(from).c_str(), slot);
        SendJoined(client);
        BroadcastRoomState(room);
    }

    void HandleRelay(ByteReader& reader, const Address& from, Clock::time_point now) {
        const uint64_t token = reader.Read64();
        const uint8_t destination = reader.Read8();
        const size_t payloadSize = reader.GetRemainingSize();
        if (reader.Failed() || (payloadSize > kMaxPayload)) {
            return;
        }
        Client* client = FindClient(token, from, now);
        if (client == nullptr) {
            return;
        }
        const Room& room = mRooms.at(client->RoomKey);

        uint8_t packet[kMaxPacketSize];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::Relayed);
        writer.Write8(client->Slot);
        writer.WriteBytes(reader.GetRemaining(), payloadSize);
        for (uint8_t slot = 0; slot < kMaxSlots; slot++) {
            const bool addressed = (destination == kBroadcast) || (destination == slot);
            if (addressed && (slot != client->Slot) && (room.Members[slot] != 0)) {
                Send(mClients.at(room.Members[slot]).Endpoint, packet, writer.GetSize());
            }
        }
    }

    void HandleKeepAlive(ByteReader& reader, const Address& from, Clock::time_point now) {
        const uint64_t token = reader.Read64();
        const uint32_t echo = reader.Read32();
        const uint8_t flags = reader.Read8();
        const uint32_t localAddress = reader.Read32();
        const uint16_t localPort = reader.Read16();
        if (reader.Failed()) {
            return;
        }
        if (Client* client = FindClient(token, from, now)) {
            client->SharesAddresses = (flags & kShareAddresses) != 0;
            client->LocalAddress = localAddress;
            client->LocalPort = localPort;
            Room& room = mRooms.at(client->RoomKey);
            if (client->Slot == 0) {
                room.Racing = (flags & kInRace) != 0;
            }
            SendRoomState(room, *client, echo);
        }
    }

    /** Answers with the joinable rooms of the asker's build and content, newest first. */
    void HandleListRooms(ByteReader& reader, const Address& from, AddressLimits& limits, Clock::time_point now) {
        const uint32_t request = reader.Read32();
        BuildId build;
        reader.ReadBytes(build.data(), build.size());
        const uint64_t content = reader.Read64();
        if (reader.Failed()) {
            return;
        }
        if (mOptions.RateLimits && !limits.Lists.Take(kListsPerSecond, kListBurst, now)) {
            SendError(from, request, ErrorCode::RateLimited);
            return;
        }
        std::vector<const Room*> rooms;
        for (const auto& [key, room] : mRooms) {
            const bool hasFreeSlot = std::count(room.Members.begin(), room.Members.end(), 0) > 0;
            if ((room.Build == build) && (room.Content == content) && hasFreeSlot) {
                rooms.push_back(&room);
            }
        }
        std::sort(rooms.begin(), rooms.end(), [](const Room* a, const Room* b) { return a->Created > b->Created; });
        rooms.resize(std::min(rooms.size(), kMaxListedRooms));

        uint8_t packet[kHeaderSize + 4 + 1 + kMaxListedRooms * (kCodeLength + kNameLength + 2)];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::RoomList);
        writer.Write32(request);
        writer.Write8((uint8_t) rooms.size());
        for (const Room* room : rooms) {
            writer.WriteBytes(room->Code.data(), room->Code.size());
            writer.WriteBytes(room->HostName.data(), room->HostName.size());
            writer.Write8((uint8_t) (kMaxSlots - std::count(room->Members.begin(), room->Members.end(), 0)));
            writer.Write8(room->Racing ? kRoomRacing : 0);
        }
        Send(from, packet, writer.GetSize());
    }

    /** Looks up the sender of a message and notes that it is alive. Unknown tokens get an error reply. */
    Client* FindClient(uint64_t token, const Address& from, Clock::time_point now) {
        const auto it = mClients.find(token);
        if (it == mClients.end()) {
            SendError(from, 0, ErrorCode::UnknownClient);
            return nullptr;
        }
        Client& client = it->second;
        if (client.Endpoint != from) {
            Log("room %s: slot %u moved from %s to %s", client.RoomKey.c_str(), client.Slot,
                ToString(client.Endpoint).c_str(), ToString(from).c_str());
            client.Endpoint = from;
        }
        client.LastSeen = now;
        return &client;
    }

    Client& AddClient(Room& room, uint8_t slot, const Address& from, uint32_t request, Clock::time_point now) {
        uint64_t token;
        do {
            token = mRandom();
        } while ((token == 0) || (mClients.count(token) != 0));

        Client& client = mClients[token];
        client.Token = token;
        client.Key = mRandom();
        client.Endpoint = from;
        client.RoomKey = ToString(room.Code);
        client.Slot = slot;
        client.Request = request;
        client.LastSeen = now;
        room.Members[slot] = token;
        mRequests[{ from, request }] = token;
        return client;
    }

    void RemoveClient(uint64_t token, const char* reason) {
        const auto it = mClients.find(token);
        if (it == mClients.end()) {
            return;
        }
        const Client client = it->second;
        mRequests.erase({ client.Endpoint, client.Request });
        mClients.erase(it);

        Room& room = mRooms.at(client.RoomKey);
        room.Members[client.Slot] = 0;
        if (client.Slot != 0) {
            Log("room %s: slot %u %s", client.RoomKey.c_str(), client.Slot, reason);
            BroadcastRoomState(room);
            return;
        }

        // The host keeps the authoritative lobby state, so the room closes with it.
        uint8_t packet[kHeaderSize + 1];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::RoomClosed);
        writer.Write8((uint8_t) CloseReason::HostLeft);
        for (uint64_t member : room.Members) {
            if (member != 0) {
                const Client& guest = mClients.at(member);
                Send(guest.Endpoint, packet, writer.GetSize());
                mRequests.erase({ guest.Endpoint, guest.Request });
                mClients.erase(member);
            }
        }
        Log("room %s closed: host %s (%zu rooms left)", client.RoomKey.c_str(), reason, mRooms.size() - 1);
        mRooms.erase(client.RoomKey);
    }

    void SendJoined(const Client& client) {
        const Room& room = mRooms.at(client.RoomKey);
        uint8_t packet[kHeaderSize + 4 + 8 + kCodeLength + 1];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::Joined);
        writer.Write32(client.Request);
        writer.Write64(client.Token);
        writer.WriteBytes(room.Code.data(), room.Code.size());
        writer.Write8(client.Slot);
        Send(client.Endpoint, packet, writer.GetSize());
    }

    void SendError(const Address& to, uint32_t request, ErrorCode error) {
        uint8_t packet[kHeaderSize + 4 + 1];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::Error);
        writer.Write32(request);
        writer.Write8((uint8_t) error);
        Send(to, packet, writer.GetSize());
    }

    void SendRoomState(const Room& room, const Client& client, uint32_t echo) {
        uint8_t occupied = 0;
        for (uint8_t slot = 0; slot < kMaxSlots; slot++) {
            if (room.Members[slot] != 0) {
                occupied |= (uint8_t) (1 << slot);
            }
        }
        uint8_t packet[kHeaderSize + 4 + 1 + 1 + kMaxSlots * (8 + 4 + 2 + 4 + 2)];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::RoomState);
        writer.Write32(echo);
        writer.Write8(client.Slot);
        writer.Write8(occupied);
        for (uint64_t token : room.Members) {
            if (token == 0) {
                continue;
            }
            const Client& member = mClients.at(token);
            // A browser has no address others could reach it at: it connects to them with WebRTC instead.
            const udp::endpoint& endpoint = member.Endpoint.Udp;
            const bool shared = member.SharesAddresses && (member.Endpoint.WebSocket == 0) && endpoint.address().is_v4();
            writer.Write64(member.Key);
            writer.Write32(shared ? endpoint.address().to_v4().to_uint() : 0);
            writer.Write16(shared ? endpoint.port() : 0);
            writer.Write32(shared ? member.LocalAddress : 0);
            writer.Write16(shared ? member.LocalPort : 0);
        }
        Send(client.Endpoint, packet, writer.GetSize());
    }

    void BroadcastRoomState(const Room& room) {
        for (uint64_t member : room.Members) {
            if (member != 0) {
                SendRoomState(room, mClients.at(member), 0);
            }
        }
    }

    void Send(const Address& to, const uint8_t* data, size_t size) {
        if ((mOptions.LossPercent > 0) && (mRandom() % 100 < mOptions.LossPercent)) {
            return;
        }
        if ((mOptions.DelayMs == 0) && (mOptions.JitterMs == 0)) {
            SendNow(to, data, size);
            return;
        }
        // Jitter reorders packets, as real networks do.
        const uint32_t jitter = (mOptions.JitterMs > 0) ? (uint32_t) (mRandom() % (mOptions.JitterMs + 1)) : 0;
        const Clock::time_point release = Clock::now() + std::chrono::milliseconds(mOptions.DelayMs + jitter);
        const bool earliest = mDelayed.empty() || (release < mDelayed.begin()->first);
        mDelayed.emplace(release, DelayedPacket{ to, std::vector<uint8_t>(data, data + size) });
        if (earliest) {
            ScheduleDelayed();
        }
    }

    /** Sends without the testing delay or loss: as a datagram, or as one binary WebSocket message. */
    void SendNow(const Address& to, const uint8_t* data, size_t size) {
        if (to.WebSocket == 0) {
            asio::error_code error;
            mSocket.send_to(asio::buffer(data, size), to.Udp, 0, error);
            return;
        }
        const auto it = mWebSockets.find(to.WebSocket);
        if ((it != mWebSockets.end()) && it->second->Open) {
            QueueWebSocket(*it->second, WebSocket::MakeServerFrame(WebSocket::Opcode::Binary, data, size));
        }
    }

    void ScheduleDelayed() {
        mDelayTimer.expires_at(mDelayed.begin()->first);
        mDelayTimer.async_wait([this](asio::error_code error) {
            if (error == asio::error::operation_aborted) {
                return;
            }
            const Clock::time_point now = Clock::now();
            while (!mDelayed.empty() && (mDelayed.begin()->first <= now)) {
                const DelayedPacket packet = mDelayed.begin()->second;
                mDelayed.erase(mDelayed.begin());
                SendNow(packet.To, packet.Data.data(), packet.Data.size());
            }
            if (!mDelayed.empty()) {
                ScheduleDelayed();
            }
        });
    }

    void ScheduleHousekeeping() {
        mHousekeeping.expires_after(kHousekeepingInterval);
        mHousekeeping.async_wait([this](asio::error_code error) {
            if (error == asio::error::operation_aborted) {
                return;
            }
            const Clock::time_point now = Clock::now();
            std::vector<uint64_t> timedOut;
            for (const auto& [token, client] : mClients) {
                if (now - client.LastSeen > std::chrono::milliseconds(kClientTimeoutMs)) {
                    timedOut.push_back(token);
                }
            }
            for (uint64_t token : timedOut) {
                RemoveClient(token, "timed out");
            }
            std::vector<uint64_t> idle;
            for (const auto& [id, connection] : mWebSockets) {
                if (now - connection->LastHeard > kWebSocketIdleTimeout) {
                    idle.push_back(id);
                }
            }
            for (uint64_t id : idle) {
                CloseWebSocket(id, "idle");
            }
            for (auto it = mLimits.begin(); it != mLimits.end();) {
                const Clock::time_point lastUse = std::max(
                    { it->second.Packets.GetLastUse(), it->second.Creates.GetLastUse(), it->second.Joins.GetLastUse() });
                it = (now - lastUse > kRateLimitExpiry) ? mLimits.erase(it) : std::next(it);
            }
            ScheduleHousekeeping();
        });
    }

    struct DelayedPacket {
        Address To;
        std::vector<uint8_t> Data;
    };

    asio::io_context& mIo;
    Options mOptions;
    udp::socket mSocket;
    tcp::acceptor mAcceptor;
    std::unordered_map<uint64_t, std::shared_ptr<WebSocketConnection>> mWebSockets;
    uint64_t mLastWebSocketId = 0;
    asio::steady_timer mHousekeeping;
    asio::steady_timer mDelayTimer;
    std::mt19937_64 mRandom;
    std::array<uint8_t, 2048> mReceiveBuffer;
    udp::endpoint mSender;

    std::unordered_map<std::string, Room> mRooms;
    std::unordered_map<uint64_t, Client> mClients;
    std::map<std::pair<Address, uint32_t>, uint64_t> mRequests;
    std::map<asio::ip::address, AddressLimits> mLimits;
    std::multimap<Clock::time_point, DelayedPacket> mDelayed;
};

bool ParseOptions(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--no-rate-limits") {
            options->RateLimits = false;
            continue;
        }
        if ((arg == "--help") || (arg == "-h") || (i + 1 >= argc)) {
            return false;
        }
        const uint32_t value = (uint32_t) strtoul(argv[++i], nullptr, 10);
        if (arg == "--port") {
            options->Port = (uint16_t) value;
        } else if (arg == "--ws-port") {
            options->WebSocketPort = (uint16_t) value;
        } else if (arg == "--delay-ms") {
            options->DelayMs = value;
        } else if (arg == "--jitter-ms") {
            options->JitterMs = value;
        } else if (arg == "--loss") {
            options->LossPercent = value;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    // Without --ws-port, WebSockets take the same number as UDP: one address serves both kinds of player.
    bool webSocketPortGiven = false;
    for (int i = 1; i < argc; i++) {
        webSocketPortGiven |= std::string(argv[i]) == "--ws-port";
    }
    if (!ParseOptions(argc, argv, &options)) {
        printf("Usage: %s [--port %u] [--ws-port %u] [--delay-ms N] [--jitter-ms N] [--loss PERCENT] "
               "[--no-rate-limits]\n",
               argv[0], kDefaultPort, kDefaultPort);
        return 2;
    }
    if (!webSocketPortGiven) {
        options.WebSocketPort = options.Port;
    }

    try {
        asio::io_context io;
        RelayServer server(io, options);
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](asio::error_code, int) { server.Shutdown(); });
        Log("netplay relay listening on UDP port %u and WebSocket port %u (protocol %u, delay %u ms, jitter %u ms, "
            "loss %u%%)",
            options.Port, options.WebSocketPort, kVersion, options.DelayMs, options.JitterMs, options.LossPercent);
        io.run();
    } catch (const std::exception& e) {
        Log("error: %s", e.what());
        return 1;
    }
    return 0;
}
