#include "RelayClient.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>

#include "NetLog.h"
#include "RelayLink.h"

using namespace RelayProtocol;
using Clock = std::chrono::steady_clock;

namespace {

constexpr u8 kLobbyChannel = 1;
constexpr u8 kGekkoChannel = 2;
// Messages a link sets up direct connections with (RelayLink::Callbacks::SendSignal): always through the relay.
constexpr u8 kSignalChannel = 3;
constexpr size_t kGekkoHeaderSize = 3; // Channel, session tag
// GekkoNet sessions that can run at once, each with its own tag: a shared console's last menu session finishes while
// its race's session starts.
constexpr size_t kNumGekkoLanes = 2;
constexpr auto kRequestRetryInterval = std::chrono::milliseconds(500);
constexpr auto kRequestTimeout = std::chrono::seconds(10);
constexpr auto kConnectionTimeout = std::chrono::seconds(10);
constexpr int kLeaveRepeats = 3; // Leave is never answered, so send it a few times in case of loss
constexpr s32 kPingSmoothing = 4;  // Each round trip moves the ping a quarter of the way

/** One GekkoNet session's packets: the tag they carry, and those received for it. */
struct GekkoLane {
    bool Used = false;
    u16 Tag = 0;
    u32 LastRequest = 0; // When GetGekkoAdapter() last handed it out; the least recent one is reused
    std::vector<GekkoNetResult*> Packets;
    std::vector<GekkoNetResult*> HandedOut;
};

} // namespace

struct RelayClient::Impl {
    Status State = Status::Idle;
    std::string Error;
    std::string Address;
    std::unique_ptr<RelayLink> Link;

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
        RelayLink::Callbacks callbacks;
        callbacks.FromRelay = [this](const u8* data, size_t size) { Handle(data, size, Clock::now()); };
        callbacks.FromMember = [this](u8 slot, const u8* data, size_t size) {
            ByteReader reader(data, size);
            HandlePayload(slot, reader);
        };
        callbacks.SendSignal = [this](u8 slot, const u8* data, size_t size) {
            const u8 header = kSignalChannel;
            SendThroughRelay(slot, &header, sizeof(header), data, size);
        };
        Link = RelayLink::Create(callbacks);
        Link->Open(address);
    }

    void Fail(const std::string& error) {
        Close();
        Error = error;
        State = Status::Failed;
    }

    void Close() {
        if ((Link != nullptr) && (State == Status::InRoom)) {
            u8 packet[kHeaderSize + 8];
            ByteWriter writer(packet, sizeof(packet));
            writer.WriteHeader(MessageType::Leave);
            writer.Write64(Token);
            for (int i = 0; i < kLeaveRepeats; i++) {
                SendToRelay(packet, writer.GetSize());
            }
        }
        Link.reset();
        Token = 0;
        Slot = -1;
        Occupied = 0;
        PingMs = -1;
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
        if (Link != nullptr) {
            Link->SendToRelay(data, size);
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

    void StartRequest(Clock::time_point now) {
        Request = Random() | 1;
        RequestStarted = now;
        State = Status::Joining;
        SendRequest(now);
    }

    void Update() {
        const Clock::time_point now = Clock::now();
        if (Link == nullptr) {
            return;
        }
        Link->Receive();
        if (Link == nullptr) {
            return; // Something received closed the connection
        }
        if (State == Status::Resolving) {
            if (Link->GetState() == RelayLink::State::Failed) {
                Fail(Link->GetError());
                return;
            }
            if (Link->GetState() == RelayLink::State::Open) {
                StartRequest(now);
            }
            return;
        }
        if (Link->GetState() == RelayLink::State::Failed) {
            Fail(Link->GetError());
            return;
        }

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
                u32 localAddress;
                u16 localPort;
                Link->GetLocalAddress(&localAddress, &localPort);
                writer.Write32(localAddress);
                writer.Write16(localPort);
                SendToRelay(packet, writer.GetSize());
                LastKeepAliveSent = now;
            }
            if (State == Status::InRoom) {
                Link->UpdateDirect(!IsDirectBlocked(now));
            }
        }
    }

    bool IsDirectBlocked(Clock::time_point now) const {
        return !DirectEnabled || (BlockDirectAfter.has_value() && (now - JoinedAt >= *BlockDirectAfter));
    }

    bool HasDirect(u8 slot, Clock::time_point now) const {
        return (Link != nullptr) && (slot < kMaxSlots) && !IsDirectBlocked(now) && Link->HasDirect(slot);
    }

    /** Takes the relay's description of the room's members and hands it to the link. */
    void ReadMembers(ByteReader& reader, u8 occupied) {
        RelayLink::Members members;
        u64 ownKey = 0;
        for (s32 slot = 0; slot < (s32) kMaxSlots; slot++) {
            if ((occupied & (1 << slot)) == 0) {
                continue;
            }
            RelayLink::Member& member = members[slot];
            member.Key = reader.Read64();
            member.PublicAddress = reader.Read32();
            member.PublicPort = reader.Read16();
            member.LocalAddress = reader.Read32();
            member.LocalPort = reader.Read16();
            if (reader.Failed()) {
                return;
            }
            if (slot == Slot) {
                ownKey = member.Key;
            }
        }
        Link->SetMembers(Slot, ownKey, members);
    }

    u32 GetMilliseconds(Clock::time_point now) const {
        return (u32) std::chrono::duration_cast<std::chrono::milliseconds>(now - Epoch).count();
    }

    void Receive() {
        if (Link != nullptr) {
            Link->Receive();
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
        } else if ((channel == kSignalChannel) && (Link != nullptr)) {
            Link->HandleSignal(source, reader.GetRemaining(), reader.GetRemainingSize());
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
        if (!HasDirect(destination, Clock::now())) {
            SendThroughRelay(destination, header, headerSize, data, size);
            return;
        }
        u8 payload[kMaxPayload];
        memcpy(payload, header, headerSize);
        memcpy(payload + headerSize, data, size);
        Link->SendDirect(destination, payload, headerSize + size);
    }

    void SendThroughRelay(u8 destination, const u8* header, size_t headerSize, const void* data, size_t size) {
        if ((State != Status::InRoom) || (headerSize + size > kMaxPayload)) {
            return;
        }
        u8 packet[kMaxPacketSize];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::Relay);
        writer.Write64(Token);
        writer.Write8(destination);
        writer.WriteBytes(header, headerSize);
        writer.WriteBytes(data, size);
        SendToRelay(packet, writer.GetSize());
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
    sGekkoClient->Receive();
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
    return IsDirect(slot) ? mImpl->Link->GetDirectPingMs(slot) : -1;
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
    std::unique_ptr<RelayLink> Link;
    std::string Address;
    BuildId Build = {};
    u64 Content = 0;
    u32 Request = 0;
    bool Busy = false;
    bool Sent = false; // Whether the pending request went out: not before the link is open
    Clock::time_point RequestStarted;
    Clock::time_point LastRequestSent;
    bool HasList = false;
    std::vector<Room> Rooms;
    u8 OtherBuilds = 0;
    u8 OtherContent = 0;
    std::string Error;
    std::mt19937 Random{ std::random_device{}() };

    void Open(const std::string& address) {
        Address = address;
        RelayLink::Callbacks callbacks;
        callbacks.FromRelay = [this](const u8* data, size_t size) { Handle(data, size); };
        callbacks.FromMember = [](u8, const u8*, size_t) {};
        callbacks.SendSignal = [](u8, const u8*, size_t) {};
        Link = RelayLink::Create(callbacks);
        Link->Open(address);
    }

    void SendRequest(Clock::time_point now) {
        u8 packet[kHeaderSize + 4 + kBuildIdLength + 8];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(MessageType::ListRooms);
        writer.Write32(Request);
        writer.WriteBytes(Build.data(), Build.size());
        writer.Write64(Content);
        Link->SendToRelay(packet, writer.GetSize());
        LastRequestSent = now;
        Sent = true;
    }

    void Fail(const std::string& error) {
        Error = error;
        Busy = false;
    }

    void Update() {
        if (Link == nullptr) {
            return;
        }
        const Clock::time_point now = Clock::now();
        Link->Receive();
        if (Link->GetState() == RelayLink::State::Failed) {
            if (Busy) {
                Fail(Link->GetError());
            }
            // Opened again by the next refresh.
            Link.reset();
            return;
        }
        if (!Busy) {
            return;
        }
        if (now - RequestStarted > kRequestTimeout) {
            Fail("No answer from the relay server at " + Address + " (is it running, and as new as the game?)");
        } else if ((Link->GetState() == RelayLink::State::Open) &&
                   (!Sent || (now - LastRequestSent > kRequestRetryInterval))) {
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
        // Relays before 2026-11-13 do not send these.
        const u8 otherBuilds = (reader.GetRemainingSize() > 0) ? reader.Read8() : 0;
        const u8 otherContent = (reader.GetRemainingSize() > 0) ? reader.Read8() : 0;
        if (reader.Failed()) {
            return;
        }
        Rooms = rooms;
        OtherBuilds = otherBuilds;
        OtherContent = otherContent;
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
    impl.Sent = false;
    impl.Error.clear();
    // The connection to the relay stays open between refreshes, which then ask it straight away.
    if ((relayAddress != impl.Address) || (impl.Link == nullptr)) {
        impl.Open(relayAddress);
    }
    if (impl.Link->GetState() == RelayLink::State::Open) {
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

u8 RelayRoomList::GetOtherBuildRooms() const {
    return mImpl->OtherBuilds;
}

u8 RelayRoomList::GetOtherContentRooms() const {
    return mImpl->OtherContent;
}

const std::string& RelayRoomList::GetError() const {
    return mImpl->Error;
}
