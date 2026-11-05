// asio first: on Windows it must include winsock2.h before anything includes windows.h.
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif
#define ASIO_STANDALONE
#include <asio.hpp>

#include "RelayClient.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <optional>
#include <random>

#include "NetLog.h"

using namespace RelayProtocol;
using asio::ip::udp;
using Clock = std::chrono::steady_clock;

namespace {

constexpr u8 kLobbyChannel = 1;
constexpr u8 kGekkoChannel = 2;
constexpr size_t kGekkoHeaderSize = 3; // Channel, session tag
// GekkoNet sessions that can run at once, each with its own tag: a shared console's last menu session finishes while
// its race's session starts.
constexpr size_t kNumGekkoLanes = 2;
constexpr auto kRequestRetryInterval = std::chrono::milliseconds(500);
constexpr auto kRequestTimeout = std::chrono::seconds(10);
constexpr auto kConnectionTimeout = std::chrono::seconds(10);
constexpr int kLeaveRepeats = 3; // Leave is never answered, so send it a few times in case of loss
constexpr s32 kPingSmoothing = 4;  // Each round trip moves the ping a quarter of the way
// Punches go out this often until a member answers directly, then less often, to keep the routers' mappings open.
constexpr auto kPunchInterval = std::chrono::milliseconds(250);
constexpr auto kPunchKeepAliveInterval = std::chrono::milliseconds(500);
// A direct connection silent for this long is given up for the relay, well before GekkoNet's 5 s disconnect.
constexpr auto kDirectTimeout = std::chrono::milliseconds(1500);

/** One GekkoNet session's packets: the tag they carry, and those received for it. */
struct GekkoLane {
    bool Used = false;
    u16 Tag = 0;
    u32 LastRequest = 0; // When GetGekkoAdapter() last handed it out; the least recent one is reused
    std::vector<GekkoNetResult*> Packets;
    std::vector<GekkoNetResult*> HandedOut;
};

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

} // namespace

struct RelayClient::Impl {
    Status State = Status::Idle;
    std::string Error;
    std::string Address;
    std::future<ResolveResult> Resolving;

    asio::io_context Io;
    std::unique_ptr<udp::socket> Socket;
    udp::endpoint Relay;

    // The pending Create or Join
    bool Creating = false;
    RoomCode Code = {};
    BuildId Build = {};
    u64 Content = 0;
    Name HostName = {};
    bool InRace = false;
    u32 Request = 0;
    Clock::time_point RequestStarted;
    Clock::time_point LastRequestSent;

    u64 Token = 0;
    s32 Slot = -1;
    u8 Occupied = 0;
    s32 PingMs = -1;

    // Direct connections to the other members
    bool DirectEnabled = true;
    u64 Key = 0;                  // This client's, which its direct packets carry
    udp::endpoint LocalEndpoint;  // This client's own address on its network
    std::array<Peer, kMaxSlots> Peers;
    std::optional<std::chrono::milliseconds> BlockDirectAfter; // Testing: direct connections fail after this
    Clock::time_point JoinedAt;
    Clock::time_point LastKeepAliveSent;
    Clock::time_point LastHeardFromRelay;
    Clock::time_point Epoch = Clock::now();

    std::vector<LobbyPacket> LobbyPackets;
    std::array<GekkoLane, kNumGekkoLanes> GekkoLanes;
    u32 GekkoRequests = 0;

    std::mt19937 Random{ std::random_device{}() };

    ~Impl() {
        FreeGekkoPackets();
    }

    void Start(const std::string& address, bool creating, const RoomCode& code, const BuildId& build, u64 content,
               const std::string& hostName) {
        Close();
        Address = address;
        Creating = creating;
        Code = code;
        Build = build;
        Content = content;
        HostName = MakeName(hostName);
        Error.clear();
        State = Status::Resolving;
        Resolving = std::async(std::launch::async, Resolve, address);
    }

    void Fail(const std::string& error) {
        Close();
        Error = error;
        State = Status::Failed;
    }

    void Close() {
        if ((Socket != nullptr) && (State == Status::InRoom)) {
            u8 packet[kHeaderSize + 8];
            ByteWriter writer(packet, sizeof(packet));
            writer.WriteHeader(MessageType::Leave);
            writer.Write64(Token);
            for (int i = 0; i < kLeaveRepeats; i++) {
                SendToRelay(packet, writer.GetSize());
            }
        }
        Socket.reset();
        Token = 0;
        Slot = -1;
        Occupied = 0;
        PingMs = -1;
        Key = 0;
        Peers = {};
        LobbyPackets.clear();
        FreeGekkoPackets();
        State = Status::Idle;
    }

    void FreeGekkoPackets() {
        for (GekkoLane& lane : GekkoLanes) {
            FreeGekkoPackets(lane);
        }
    }

    static void FreeGekkoPackets(GekkoLane& lane) {
        for (GekkoNetResult* packet : lane.Packets) {
            FreeGekkoPacket(packet);
        }
        lane.Packets.clear();
    }

    static void FreeGekkoPacket(GekkoNetResult* packet) {
        free(packet->addr.data);
        free(packet->data);
        free(packet);
    }

    void SendToRelay(const u8* data, size_t size) {
        if (Socket != nullptr) {
            asio::error_code error;
            Socket->send_to(asio::buffer(data, size), Relay, 0, error);
        }
    }

    void SendRequest(Clock::time_point now) {
        u8 packet[kHeaderSize + 4 + kCodeLength + kBuildIdLength + 8 + kNameLength];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(Creating ? MessageType::Create : MessageType::Join);
        writer.Write32(Request);
        if (!Creating) {
            writer.WriteBytes(Code.data(), Code.size());
        }
        writer.WriteBytes(Build.data(), Build.size());
        writer.Write64(Content);
        if (Creating) {
            writer.WriteBytes(HostName.data(), HostName.size());
        }
        SendToRelay(packet, writer.GetSize());
        LastRequestSent = now;
    }

    void OpenSocket(const udp::endpoint& relay, Clock::time_point now) {
        try {
            Socket = std::make_unique<udp::socket>(Io, udp::endpoint(udp::v4(), 0));
            Socket->non_blocking(true);
        } catch (const std::exception& e) {
            Fail(std::string("Could not open a network socket: ") + e.what());
            return;
        }
        Relay = relay;
        LocalEndpoint = FindLocalEndpoint();
        Request = Random() | 1;
        RequestStarted = now;
        State = Status::Joining;
        SendRequest(now);
    }

    /** @returns this computer's address on its network, the one packets to the relay leave from, and the port. */
    udp::endpoint FindLocalEndpoint() {
        asio::error_code error;
        udp::socket probe(Io);
        probe.open(udp::v4(), error);
        // Connecting a UDP socket sends nothing; it only picks the route.
        probe.connect(Relay, error);
        const udp::endpoint local = probe.local_endpoint(error);
        const unsigned short port = Socket->local_endpoint(error).port();
        return error ? udp::endpoint() : udp::endpoint(local.address(), port);
    }

    void Update() {
        const Clock::time_point now = Clock::now();
        if (State == Status::Resolving) {
            if (Resolving.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return;
            }
            const ResolveResult result = Resolving.get();
            if (!result.Error.empty()) {
                Fail(result.Error);
                return;
            }
            OpenSocket(result.Endpoint, now);
        }

        Receive(now);

        if (State == Status::Joining) {
            if (now - RequestStarted > kRequestTimeout) {
                Fail("No answer from the relay server at " + Address + " (is it running, and as new as the game?)");
            } else if (now - LastRequestSent > kRequestRetryInterval) {
                SendRequest(now);
            }
        } else if (State == Status::InRoom) {
            if (now - LastHeardFromRelay > kConnectionTimeout) {
                Fail("Lost the connection to the relay server");
            } else if (now - LastKeepAliveSent > std::chrono::milliseconds(kKeepAliveIntervalMs)) {
                u8 packet[kHeaderSize + 8 + 4 + 1 + 4 + 2];
                ByteWriter writer(packet, sizeof(packet));
                writer.WriteHeader(MessageType::KeepAlive);
                writer.Write64(Token);
                // 0 means "not an answer to a keep-alive" in RoomState, so never send it.
                writer.Write32(GetMilliseconds(now) | 1);
                writer.Write8((DirectEnabled ? kShareAddresses : 0) | (InRace ? kInRace : 0));
                const bool hasLocal = LocalEndpoint.address().is_v4();
                writer.Write32(hasLocal ? LocalEndpoint.address().to_v4().to_uint() : 0);
                writer.Write16(hasLocal ? LocalEndpoint.port() : 0);
                SendToRelay(packet, writer.GetSize());
                LastKeepAliveSent = now;
            }
            if (State == Status::InRoom) {
                UpdateDirect(now);
            }
        }
    }

    bool IsDirectBlocked(Clock::time_point now) const {
        return !DirectEnabled || (BlockDirectAfter.has_value() && (now - JoinedAt >= *BlockDirectAfter));
    }

    /** Punches towards every member not reached directly yet, keeps the reached ones open, and notices lost ones. */
    void UpdateDirect(Clock::time_point now) {
        if (IsDirectBlocked(now)) {
            for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
                LoseDirect(slot);
            }
            return;
        }
        for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
            Peer& peer = Peers[slot];
            if ((slot == Slot) || (peer.Key == 0)) {
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
            writer.Write8((u8) Slot);
            writer.Write64(Key);
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

    void LoseDirect(s32 slot) {
        Peer& peer = Peers[slot];
        if (peer.HasDirect) {
            peer.HasDirect = false;
            NetLog("Direct connection to slot %d lost; using the relay", slot);
        }
    }

    bool HasDirect(u8 slot, Clock::time_point now) const {
        return (slot < kMaxSlots) && Peers[slot].HasDirect && !IsDirectBlocked(now);
    }

    /** Takes the relay's description of the room's members. */
    void ReadMembers(ByteReader& reader, u8 occupied) {
        for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
            if ((occupied & (1 << slot)) == 0) {
                Peers[slot] = Peer();
                continue;
            }
            const u64 key = reader.Read64();
            const u32 publicAddress = reader.Read32();
            const u16 publicPort = reader.Read16();
            const u32 localAddress = reader.Read32();
            const u16 localPort = reader.Read16();
            if (reader.Failed()) {
                return;
            }
            if (slot == Slot) {
                Key = key;
                continue;
            }
            Peer& peer = Peers[slot];
            if (peer.Key != key) {
                // Someone new in this slot
                peer = Peer();
                peer.Key = key;
            }
            const std::array<udp::endpoint, 2> candidates = { Reachable(MakeEndpoint(publicAddress, publicPort)),
                                                              Reachable(MakeEndpoint(localAddress, localPort)) };
            if ((candidates != peer.Candidates) && (candidates[0].port() != 0)) {
                const bool same = (candidates[1].port() == 0) || (candidates[1] == candidates[0]);
                NetLog("Slot %d can be reached at %s%s%s", slot, ToString(candidates[0]).c_str(), same ? "" : " or ",
                       same ? "" : ToString(candidates[1]).c_str());
            }
            peer.Candidates = candidates;
        }
    }

    /**
     * Where this client can send to one of a member's addresses. A member on the relay's own computer has only
     * loopback addresses (the relay sees it there, and its packets to the relay never leave the computer), which mean
     * this client's own computer. It is reached at the relay's address instead, the way this client reaches the relay:
     * on the same network, that computer's address there; across the internet, its router's, on the member's own port,
     * which most home routers keep for the packets the member sends out.
     */
    udp::endpoint Reachable(const udp::endpoint& candidate) const {
        return candidate.address().is_loopback() ? udp::endpoint(Relay.address(), candidate.port()) : candidate;
    }

    void SendTo(const udp::endpoint& to, const u8* data, size_t size) {
        if (Socket != nullptr) {
            asio::error_code error;
            Socket->send_to(asio::buffer(data, size), to, 0, error);
        }
    }

    u32 GetMilliseconds(Clock::time_point now) const {
        return (u32) std::chrono::duration_cast<std::chrono::milliseconds>(now - Epoch).count();
    }

    void Receive(Clock::time_point now) {
        if (Socket == nullptr) {
            return;
        }
        u8 buffer[2048];
        udp::endpoint sender;
        while (Socket != nullptr) {
            asio::error_code error;
            const size_t size = Socket->receive_from(asio::buffer(buffer), sender, 0, error);
            if (error == asio::error::would_block) {
                return;
            }
            // Other errors (such as ICMP "port unreachable" on Windows) concern one packet; keep reading.
            if (error) {
                continue;
            }
            if (sender == Relay) {
                Handle(buffer, size, now);
            } else {
                HandleDirect(buffer, size, sender, now);
            }
        }
    }

    void Handle(const u8* data, size_t size, Clock::time_point now) {
        ByteReader reader(data, size);
        MessageType type;
        if (!reader.ReadHeader(&type)) {
            return;
        }
        switch (type) {
            case MessageType::Joined: {
                const u32 request = reader.Read32();
                const u64 token = reader.Read64();
                RoomCode code;
                reader.ReadBytes(code.data(), code.size());
                const u8 slot = reader.Read8();
                if (!reader.Failed() && (State == Status::Joining) && (request == Request)) {
                    Token = token;
                    Code = code;
                    Slot = slot;
                    Occupied = (u8) (1 << slot);
                    State = Status::InRoom;
                    LastHeardFromRelay = now;
                    LastKeepAliveSent = now - std::chrono::milliseconds(kKeepAliveIntervalMs);
                    JoinedAt = now;
                }
                break;
            }
            case MessageType::Error: {
                const u32 request = reader.Read32();
                const ErrorCode error = (ErrorCode) reader.Read8();
                if (reader.Failed()) {
                    break;
                }
                if ((State == Status::Joining) && (request == Request)) {
                    Fail(GetErrorText(error));
                } else if ((State == Status::InRoom) && (request == 0) && (error == ErrorCode::UnknownClient)) {
                    Fail("The relay server dropped this connection");
                }
                break;
            }
            case MessageType::RoomState: {
                const u32 echo = reader.Read32();
                const u8 slot = reader.Read8();
                const u8 occupied = reader.Read8();
                if (!reader.Failed() && (State == Status::InRoom)) {
                    Slot = slot;
                    Occupied = occupied;
                    ReadMembers(reader, occupied);
                    LastHeardFromRelay = now;
                    if (echo != 0) {
                        // Averaged, as a single round trip can be held up.
                        const s32 sample = (s32) (GetMilliseconds(now) - echo);
                        PingMs = (PingMs < 0) ? sample : (PingMs * (kPingSmoothing - 1) + sample) / kPingSmoothing;
                    }
                }
                break;
            }
            case MessageType::RoomClosed: {
                const CloseReason reason = (CloseReason) reader.Read8();
                if (State == Status::InRoom) {
                    // The relay already forgot this client; don't send Leave.
                    State = Status::Idle;
                    Fail((reason == CloseReason::HostLeft) ? "The host closed the room"
                                                           : "The relay server shut down");
                }
                break;
            }
            case MessageType::Relayed: {
                const u8 source = reader.Read8();
                if (reader.Failed() || (State != Status::InRoom)) {
                    break;
                }
                LastHeardFromRelay = now;
                HandlePayload(source, reader);
                break;
            }
            default:
                break;
        }
    }

    /** A packet from another member, not through the relay. */
    void HandleDirect(const u8* data, size_t size, const udp::endpoint& sender, Clock::time_point now) {
        ByteReader reader(data, size);
        MessageType type;
        if (!reader.ReadHeader(&type) || (State != Status::InRoom) || IsDirectBlocked(now)) {
            return;
        }
        const u8 source = reader.Read8();
        const u64 key = reader.Read64();
        if (reader.Failed() || (source >= kMaxSlots) || ((s32) source == Slot) || (Peers[source].Key == 0) ||
            (Peers[source].Key != key)) {
            return;
        }
        Peer& peer = Peers[source];
        switch (type) {
            case MessageType::Punch: {
                const u32 time = reader.Read32();
                if (reader.Failed()) {
                    return;
                }
                u8 packet[kHeaderSize + 1 + 8 + 4];
                ByteWriter writer(packet, sizeof(packet));
                writer.WriteHeader(MessageType::PunchReply);
                writer.Write8((u8) Slot);
                writer.Write64(Key);
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
                HandlePayload(source, reader);
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

    /** What Relay or Direct carried: a lobby message or a GekkoNet packet. */
    void HandlePayload(u8 source, ByteReader& reader) {
        const u8 channel = reader.Read8();
        if (reader.Failed()) {
            return;
        }
        if (channel == kLobbyChannel) {
            const u8* payload = reader.GetRemaining();
            LobbyPackets.push_back({ source, std::vector<u8>(payload, payload + reader.GetRemainingSize()) });
        } else if (channel == kGekkoChannel) {
            const u16 tag = reader.Read16();
            for (GekkoLane& lane : GekkoLanes) {
                if (!reader.Failed() && lane.Used && (tag == lane.Tag)) {
                    QueueGekkoPacket(lane, source, reader.GetRemaining(), reader.GetRemainingSize());
                }
            }
        }
    }

    static void QueueGekkoPacket(GekkoLane& lane, u8 source, const u8* data, size_t size) {
        GekkoNetResult* packet = static_cast<GekkoNetResult*>(malloc(sizeof(GekkoNetResult)));
        packet->addr.size = 1;
        packet->addr.data = malloc(1);
        *static_cast<u8*>(packet->addr.data) = source;
        packet->data_len = (unsigned int) size;
        packet->data = malloc(size > 0 ? size : 1);
        memcpy(packet->data, data, size);
        lane.Packets.push_back(packet);
    }

    /** Sends directly to a member reached that way, or through the relay. */
    void SendPayload(u8 destination, const u8* header, size_t headerSize, const void* data, size_t size) {
        if ((State != Status::InRoom) || (headerSize + size > kMaxPayload)) {
            return;
        }
        u8 packet[kMaxPacketSize];
        ByteWriter writer(packet, sizeof(packet));
        const bool direct = HasDirect(destination, Clock::now());
        if (direct) {
            writer.WriteHeader(MessageType::Direct);
            writer.Write8((u8) Slot);
            writer.Write64(Key);
        } else {
            writer.WriteHeader(MessageType::Relay);
            writer.Write64(Token);
            writer.Write8(destination);
        }
        writer.WriteBytes(header, headerSize);
        writer.WriteBytes(data, size);
        if (direct) {
            SendTo(Peers[destination].Direct, packet, writer.GetSize());
        } else {
            SendToRelay(packet, writer.GetSize());
        }
    }
};

namespace {

// GekkoNet's adapter is a table of plain functions, so they reach the client through this, and each lane has its own.
RelayClient::Impl* sGekkoClient = nullptr;

template <size_t Lane> void GekkoSend(GekkoNetAddress* address, const char* data, int length) {
    if ((sGekkoClient == nullptr) || (address->size != 1)) {
        return;
    }
    const u16 tag = sGekkoClient->GekkoLanes[Lane].Tag;
    const u8 header[kGekkoHeaderSize] = { kGekkoChannel, (u8) tag, (u8) (tag >> 8) };
    sGekkoClient->SendPayload(*static_cast<const u8*>(address->data), header, sizeof(header), data, (size_t) length);
}

template <size_t Lane> GekkoNetResult** GekkoReceive(int* length) {
    if (sGekkoClient == nullptr) {
        *length = 0;
        return nullptr;
    }
    sGekkoClient->Receive(Clock::now());
    // GekkoNet frees each packet with GekkoFree; the array stays ours until the next call.
    GekkoLane& lane = sGekkoClient->GekkoLanes[Lane];
    lane.HandedOut.swap(lane.Packets);
    lane.Packets.clear();
    *length = (int) lane.HandedOut.size();
    return lane.HandedOut.data();
}

void GekkoFree(void* data) {
    free(data);
}

std::array<GekkoNetAdapter, kNumGekkoLanes> sGekkoAdapters = { {
    { GekkoSend<0>, GekkoReceive<0>, GekkoFree },
    { GekkoSend<1>, GekkoReceive<1>, GekkoFree },
} };
static_assert(kNumGekkoLanes == 2, "one adapter per lane");

} // namespace

RelayClient::RelayClient() : mImpl(std::make_unique<Impl>()) {
}

RelayClient::~RelayClient() {
    mImpl->Close();
    if (sGekkoClient == mImpl.get()) {
        sGekkoClient = nullptr;
    }
}

void RelayClient::Create(const std::string& relayAddress, const BuildId& build, u64 content,
                         const std::string& hostName) {
    mImpl->Start(relayAddress, true, {}, build, content, hostName);
}

void RelayClient::Join(const std::string& relayAddress, const std::string& code, const BuildId& build, u64 content) {
    RoomCode roomCode = {};
    for (size_t i = 0; (i < roomCode.size()) && (i < code.size()); i++) {
        roomCode[i] = (char) toupper((unsigned char) code[i]);
    }
    mImpl->Start(relayAddress, false, roomCode, build, content, "");
}

void RelayClient::Leave() {
    mImpl->Close();
    mImpl->Error.clear();
}

void RelayClient::Update() {
    mImpl->Update();
}

RelayClient::Status RelayClient::GetStatus() const {
    return mImpl->State;
}

const std::string& RelayClient::GetError() const {
    return mImpl->Error;
}

std::string RelayClient::GetRoomCode() const {
    return (mImpl->State == Status::InRoom) ? std::string(mImpl->Code.data(), mImpl->Code.size()) : "";
}

s32 RelayClient::GetSlot() const {
    return mImpl->Slot;
}

u8 RelayClient::GetOccupiedSlots() const {
    return mImpl->Occupied;
}

s32 RelayClient::GetPingMs() const {
    return mImpl->PingMs;
}

void RelayClient::SetDirectEnabled(bool enabled) {
    mImpl->DirectEnabled = enabled;
}

void RelayClient::SetInRace(bool inRace) {
    mImpl->InRace = inRace;
}

void RelayClient::BlockDirectAfter(u32 milliseconds) {
    mImpl->BlockDirectAfter = std::chrono::milliseconds(milliseconds);
}

bool RelayClient::IsDirect(u8 slot) const {
    return mImpl->HasDirect(slot, Clock::now());
}

s32 RelayClient::GetDirectPingMs(u8 slot) const {
    return IsDirect(slot) ? mImpl->Peers[slot].PingMs : -1;
}

void RelayClient::SendLobby(u8 destination, const void* data, size_t size) {
    const u8 header = kLobbyChannel;
    mImpl->SendPayload(destination, &header, sizeof(header), data, size);
}

std::vector<RelayClient::LobbyPacket> RelayClient::TakeLobbyPackets() {
    std::vector<LobbyPacket> packets;
    packets.swap(mImpl->LobbyPackets);
    return packets;
}

GekkoNetAdapter* RelayClient::GetGekkoAdapter(u16 sessionTag) {
    sGekkoClient = mImpl.get();
    auto& lanes = mImpl->GekkoLanes;
    size_t chosen = 0;
    for (size_t i = 0; i < lanes.size(); i++) {
        if (lanes[i].Used && (lanes[i].Tag == sessionTag)) {
            chosen = i;
            break;
        }
        // Otherwise the lane handed out least recently: the older of the sessions that may still run.
        if (!lanes[i].Used || (lanes[chosen].Used && (lanes[i].LastRequest < lanes[chosen].LastRequest))) {
            chosen = i;
        }
    }
    GekkoLane& lane = lanes[chosen];
    if (!lane.Used || (lane.Tag != sessionTag)) {
        Impl::FreeGekkoPackets(lane);
        lane.Used = true;
        lane.Tag = sessionTag;
    }
    lane.LastRequest = ++mImpl->GekkoRequests;
    return &sGekkoAdapters[chosen];
}

struct RelayRoomList::Impl {
    asio::io_context Io;
    std::unique_ptr<udp::socket> Socket;
    std::string Address;
    udp::endpoint Relay;
    std::optional<std::future<ResolveResult>> Resolving;
    BuildId Build = {};
    u64 Content = 0;
    u32 Request = 0;
    bool Busy = false;
    Clock::time_point RequestStarted;
    Clock::time_point LastRequestSent;
    bool HasList = false;
    std::vector<Room> Rooms;
    std::string Error;
    std::mt19937 Random{ std::random_device{}() };

    void SendRequest(Clock::time_point now) {
        u8 packet[kHeaderSize + 4 + kBuildIdLength + 8];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::ListRooms);
        writer.Write32(Request);
        writer.WriteBytes(Build.data(), Build.size());
        writer.Write64(Content);
        asio::error_code error;
        Socket->send_to(asio::buffer(packet, writer.GetSize()), Relay, 0, error);
        LastRequestSent = now;
    }

    void Fail(const std::string& error) {
        Error = error;
        Busy = false;
    }

    void Update() {
        const Clock::time_point now = Clock::now();
        if (Resolving.has_value()) {
            if (Resolving->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return;
            }
            const ResolveResult result = Resolving->get();
            Resolving.reset();
            if (!result.Error.empty()) {
                Address.clear();
                Fail(result.Error);
                return;
            }
            try {
                Socket = std::make_unique<udp::socket>(Io, udp::endpoint(udp::v4(), 0));
                Socket->non_blocking(true);
            } catch (const std::exception& e) {
                Address.clear();
                Fail(std::string("Could not open a network socket: ") + e.what());
                return;
            }
            Relay = result.Endpoint;
            SendRequest(now);
        }
        if (Socket == nullptr) {
            return;
        }
        u8 buffer[2048];
        udp::endpoint sender;
        while (true) {
            asio::error_code error;
            const size_t size = Socket->receive_from(asio::buffer(buffer), sender, 0, error);
            if (error == asio::error::would_block) {
                break;
            }
            if (!error && (sender == Relay)) {
                Handle(buffer, size);
            }
        }
        if (Busy && (now - RequestStarted > kRequestTimeout)) {
            Fail("No answer from the relay server at " + Address + " (is it running, and as new as the game?)");
        } else if (Busy && (now - LastRequestSent > kRequestRetryInterval)) {
            SendRequest(now);
        }
    }

    void Handle(const u8* data, size_t size) {
        ByteReader reader(data, size);
        MessageType type;
        if (!reader.ReadHeader(&type) || !Busy || (reader.Read32() != Request)) {
            return;
        }
        if (type == MessageType::Error) {
            Fail(GetErrorText((ErrorCode) reader.Read8()));
            return;
        }
        if (type != MessageType::RoomList) {
            return;
        }
        const u8 count = reader.Read8();
        std::vector<Room> rooms;
        for (u8 i = 0; i < count; i++) {
            RoomCode code;
            reader.ReadBytes(code.data(), code.size());
            Name name;
            reader.ReadBytes(name.data(), name.size());
            Room room;
            room.Code = std::string(code.data(), code.size());
            room.HostName = ToString(name);
            room.Members = reader.Read8();
            room.Racing = (reader.Read8() & kRoomRacing) != 0;
            rooms.push_back(room);
        }
        if (reader.Failed()) {
            return;
        }
        Rooms = rooms;
        HasList = true;
        Error.clear();
        Busy = false;
    }
};

RelayRoomList::RelayRoomList() : mImpl(std::make_unique<Impl>()) {
}

RelayRoomList::~RelayRoomList() = default;

void RelayRoomList::Refresh(const std::string& relayAddress, const BuildId& build, u64 content) {
    Impl& impl = *mImpl;
    impl.Build = build;
    impl.Content = content;
    impl.Request = impl.Random() | 1;
    impl.RequestStarted = Clock::now();
    impl.Busy = true;
    impl.Error.clear();
    // The relay's address is looked up once; later refreshes ask it straight away.
    if ((relayAddress != impl.Address) || (impl.Socket == nullptr)) {
        impl.Address = relayAddress;
        impl.Socket.reset();
        impl.Resolving = std::async(std::launch::async, Resolve, relayAddress);
    } else if (!impl.Resolving.has_value()) {
        impl.SendRequest(impl.RequestStarted);
    }
}

void RelayRoomList::Update() {
    mImpl->Update();
}

bool RelayRoomList::IsBusy() const {
    return mImpl->Busy;
}

bool RelayRoomList::HasList() const {
    return mImpl->HasList;
}

const std::vector<RelayRoomList::Room>& RelayRoomList::GetRooms() const {
    return mImpl->Rooms;
}

const std::string& RelayRoomList::GetError() const {
    return mImpl->Error;
}
