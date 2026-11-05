/**
 * Netplay relay server: lists rooms, matches players by room code and forwards their packets.
 *
 *   netplay-relay [--port 7777] [--delay-ms N] [--jitter-ms N] [--loss PERCENT] [--no-rate-limits]
 *
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
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "RelayProtocol.h"

using namespace RelayProtocol;
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

struct Options {
    uint16_t Port = kDefaultPort;
    uint32_t DelayMs = 0;
    uint32_t JitterMs = 0;
    uint32_t LossPercent = 0;
    bool RateLimits = true;
};

struct Client {
    uint64_t Token = 0;
    uint64_t Key = 0; // Shown to the other members, which recognize this client's direct packets by it
    udp::endpoint Endpoint;
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

std::string ToString(const RoomCode& code) {
    return std::string(code.data(), code.size());
}

class RelayServer {
  public:
    RelayServer(asio::io_context& io, const Options& options)
        : mOptions(options), mSocket(io, udp::endpoint(udp::v4(), options.Port)), mHousekeeping(io),
          mDelayTimer(io), mRandom(std::random_device{}()) {
        Receive();
        ScheduleHousekeeping();
    }

    void Shutdown() {
        uint8_t packet[kHeaderSize + 1];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::RoomClosed);
        writer.Write8((uint8_t) CloseReason::ServerShutdown);
        for (const auto& [token, client] : mClients) {
            asio::error_code error;
            mSocket.send_to(asio::buffer(packet, writer.GetSize()), client.Endpoint, 0, error);
        }
        Log("shutting down: closed %zu rooms", mRooms.size());
        mSocket.close();
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
                Handle(mReceiveBuffer.data(), size, mSender);
            }
            Receive();
        });
    }

    void Handle(const uint8_t* data, size_t size, const udp::endpoint& from) {
        const Clock::time_point now = Clock::now();
        AddressLimits& limits = mLimits[from.address()];
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
    bool AnswerRepeatedRequest(const udp::endpoint& from, uint32_t request) {
        const auto it = mRequests.find({ from, request });
        if (it == mRequests.end()) {
            return false;
        }
        SendJoined(mClients.at(it->second));
        return true;
    }

    void HandleCreate(ByteReader& reader, const udp::endpoint& from, AddressLimits& limits, Clock::time_point now) {
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

    void HandleJoin(ByteReader& reader, const udp::endpoint& from, AddressLimits& limits, Clock::time_point now) {
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

    void HandleRelay(ByteReader& reader, const udp::endpoint& from, Clock::time_point now) {
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

    void HandleKeepAlive(ByteReader& reader, const udp::endpoint& from, Clock::time_point now) {
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
    void HandleListRooms(ByteReader& reader, const udp::endpoint& from, AddressLimits& limits, Clock::time_point now) {
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
    Client* FindClient(uint64_t token, const udp::endpoint& from, Clock::time_point now) {
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

    Client& AddClient(Room& room, uint8_t slot, const udp::endpoint& from, uint32_t request, Clock::time_point now) {
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

    void SendError(const udp::endpoint& to, uint32_t request, ErrorCode error) {
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
            const bool shared = member.SharesAddresses && member.Endpoint.address().is_v4();
            writer.Write64(member.Key);
            writer.Write32(shared ? member.Endpoint.address().to_v4().to_uint() : 0);
            writer.Write16(shared ? member.Endpoint.port() : 0);
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

    void Send(const udp::endpoint& to, const uint8_t* data, size_t size) {
        if ((mOptions.LossPercent > 0) && (mRandom() % 100 < mOptions.LossPercent)) {
            return;
        }
        if ((mOptions.DelayMs == 0) && (mOptions.JitterMs == 0)) {
            asio::error_code error;
            mSocket.send_to(asio::buffer(data, size), to, 0, error);
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

    void ScheduleDelayed() {
        mDelayTimer.expires_at(mDelayed.begin()->first);
        mDelayTimer.async_wait([this](asio::error_code error) {
            if (error == asio::error::operation_aborted) {
                return;
            }
            const Clock::time_point now = Clock::now();
            while (!mDelayed.empty() && (mDelayed.begin()->first <= now)) {
                const DelayedPacket& packet = mDelayed.begin()->second;
                asio::error_code sendError;
                mSocket.send_to(asio::buffer(packet.Data), packet.To, 0, sendError);
                mDelayed.erase(mDelayed.begin());
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
            for (auto it = mLimits.begin(); it != mLimits.end();) {
                const Clock::time_point lastUse = std::max(
                    { it->second.Packets.GetLastUse(), it->second.Creates.GetLastUse(), it->second.Joins.GetLastUse() });
                it = (now - lastUse > kRateLimitExpiry) ? mLimits.erase(it) : std::next(it);
            }
            ScheduleHousekeeping();
        });
    }

    struct DelayedPacket {
        udp::endpoint To;
        std::vector<uint8_t> Data;
    };

    Options mOptions;
    udp::socket mSocket;
    asio::steady_timer mHousekeeping;
    asio::steady_timer mDelayTimer;
    std::mt19937_64 mRandom;
    std::array<uint8_t, 2048> mReceiveBuffer;
    udp::endpoint mSender;

    std::unordered_map<std::string, Room> mRooms;
    std::unordered_map<uint64_t, Client> mClients;
    std::map<std::pair<udp::endpoint, uint32_t>, uint64_t> mRequests;
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
    if (!ParseOptions(argc, argv, &options)) {
        printf("Usage: %s [--port %u] [--delay-ms N] [--jitter-ms N] [--loss PERCENT] [--no-rate-limits]\n", argv[0],
               kDefaultPort);
        return 2;
    }

    try {
        asio::io_context io;
        RelayServer server(io, options);
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](asio::error_code, int) { server.Shutdown(); });
        Log("netplay relay listening on UDP port %u (protocol %u, delay %u ms, jitter %u ms, loss %u%%)", options.Port,
            kVersion, options.DelayMs, options.JitterMs, options.LossPercent);
        io.run();
    } catch (const std::exception& e) {
        Log("error: %s", e.what());
        return 1;
    }
    return 0;
}
