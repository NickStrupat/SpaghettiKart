// asio first: on Windows it must include winsock2.h before anything includes windows.h.
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif
#define ASIO_STANDALONE
#include <asio.hpp>

#include "RelayClient.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <random>

using namespace RelayProtocol;
using asio::ip::udp;
using Clock = std::chrono::steady_clock;

namespace {

constexpr u8 kLobbyChannel = 1;
constexpr u8 kGekkoChannel = 2;
constexpr size_t kGekkoHeaderSize = 3; // Channel, session tag
constexpr auto kRequestRetryInterval = std::chrono::milliseconds(500);
constexpr auto kRequestTimeout = std::chrono::seconds(10);
constexpr auto kConnectionTimeout = std::chrono::seconds(10);
constexpr int kLeaveRepeats = 3; // Leave is never answered, so send it a few times in case of loss

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
    u32 Request = 0;
    Clock::time_point RequestStarted;
    Clock::time_point LastRequestSent;

    u64 Token = 0;
    s32 Slot = -1;
    u8 Occupied = 0;
    s32 PingMs = -1;
    Clock::time_point LastKeepAliveSent;
    Clock::time_point LastHeardFromRelay;
    Clock::time_point Epoch = Clock::now();

    std::vector<LobbyPacket> LobbyPackets;
    u16 GekkoTag = 0;
    std::vector<GekkoNetResult*> GekkoPackets;
    std::vector<GekkoNetResult*> GekkoHandedOut;

    std::mt19937 Random{ std::random_device{}() };

    ~Impl() {
        FreeGekkoPackets();
    }

    void Start(const std::string& address, bool creating, const RoomCode& code, const BuildId& build, u64 content) {
        Close();
        Address = address;
        Creating = creating;
        Code = code;
        Build = build;
        Content = content;
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
        LobbyPackets.clear();
        FreeGekkoPackets();
        State = Status::Idle;
    }

    void FreeGekkoPackets() {
        for (GekkoNetResult* packet : GekkoPackets) {
            FreeGekkoPacket(packet);
        }
        GekkoPackets.clear();
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
        u8 packet[kHeaderSize + 4 + kCodeLength + kBuildIdLength + 8];
        ByteWriter writer(packet, sizeof(packet));
        writer.WriteHeader(Creating ? MessageType::Create : MessageType::Join);
        writer.Write32(Request);
        if (!Creating) {
            writer.WriteBytes(Code.data(), Code.size());
        }
        writer.WriteBytes(Build.data(), Build.size());
        writer.Write64(Content);
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
        Request = Random() | 1;
        RequestStarted = now;
        State = Status::Joining;
        SendRequest(now);
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
                Fail("No answer from the relay server at " + Address);
            } else if (now - LastRequestSent > kRequestRetryInterval) {
                SendRequest(now);
            }
        } else if (State == Status::InRoom) {
            if (now - LastHeardFromRelay > kConnectionTimeout) {
                Fail("Lost the connection to the relay server");
            } else if (now - LastKeepAliveSent > std::chrono::milliseconds(kKeepAliveIntervalMs)) {
                u8 packet[kHeaderSize + 8 + 4];
                ByteWriter writer(packet, sizeof(packet));
                writer.WriteHeader(MessageType::KeepAlive);
                writer.Write64(Token);
                // 0 means "not an answer to a keep-alive" in RoomState, so never send it.
                writer.Write32(GetMilliseconds(now) | 1);
                SendToRelay(packet, writer.GetSize());
                LastKeepAliveSent = now;
            }
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
            if (!error && (sender == Relay)) {
                Handle(buffer, size, now);
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
                    LastHeardFromRelay = now;
                    if (echo != 0) {
                        PingMs = (s32) (GetMilliseconds(now) - echo);
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
                const u8 channel = reader.Read8();
                if (reader.Failed() || (State != Status::InRoom)) {
                    break;
                }
                LastHeardFromRelay = now;
                if (channel == kLobbyChannel) {
                    const u8* payload = reader.GetRemaining();
                    LobbyPackets.push_back({ source, std::vector<u8>(payload, payload + reader.GetRemainingSize()) });
                } else if (channel == kGekkoChannel) {
                    const u16 tag = reader.Read16();
                    if (!reader.Failed() && (tag == GekkoTag)) {
                        QueueGekkoPacket(source, reader.GetRemaining(), reader.GetRemainingSize());
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    void QueueGekkoPacket(u8 source, const u8* data, size_t size) {
        GekkoNetResult* packet = static_cast<GekkoNetResult*>(malloc(sizeof(GekkoNetResult)));
        packet->addr.size = 1;
        packet->addr.data = malloc(1);
        *static_cast<u8*>(packet->addr.data) = source;
        packet->data_len = (unsigned int) size;
        packet->data = malloc(size > 0 ? size : 1);
        memcpy(packet->data, data, size);
        GekkoPackets.push_back(packet);
    }

    void SendPayload(u8 destination, const u8* header, size_t headerSize, const void* data, size_t size) {
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

// GekkoNet's adapter is a table of plain functions, so they reach the client through this.
RelayClient::Impl* sGekkoClient = nullptr;

void GekkoSend(GekkoNetAddress* address, const char* data, int length) {
    if ((sGekkoClient == nullptr) || (address->size != 1)) {
        return;
    }
    const u8 header[kGekkoHeaderSize] = { kGekkoChannel, (u8) sGekkoClient->GekkoTag,
                                          (u8) (sGekkoClient->GekkoTag >> 8) };
    sGekkoClient->SendPayload(*static_cast<const u8*>(address->data), header, sizeof(header), data, (size_t) length);
}

GekkoNetResult** GekkoReceive(int* length) {
    if (sGekkoClient == nullptr) {
        *length = 0;
        return nullptr;
    }
    sGekkoClient->Receive(Clock::now());
    // GekkoNet frees each packet with GekkoFree; the array stays ours until the next call.
    sGekkoClient->GekkoHandedOut.swap(sGekkoClient->GekkoPackets);
    sGekkoClient->GekkoPackets.clear();
    *length = (int) sGekkoClient->GekkoHandedOut.size();
    return sGekkoClient->GekkoHandedOut.data();
}

void GekkoFree(void* data) {
    free(data);
}

GekkoNetAdapter sGekkoAdapter = { GekkoSend, GekkoReceive, GekkoFree };

} // namespace

RelayClient::RelayClient() : mImpl(std::make_unique<Impl>()) {
}

RelayClient::~RelayClient() {
    mImpl->Close();
    if (sGekkoClient == mImpl.get()) {
        sGekkoClient = nullptr;
    }
}

void RelayClient::Create(const std::string& relayAddress, const BuildId& build, u64 content) {
    mImpl->Start(relayAddress, true, {}, build, content);
}

void RelayClient::Join(const std::string& relayAddress, const std::string& code, const BuildId& build, u64 content) {
    RoomCode roomCode = {};
    for (size_t i = 0; (i < roomCode.size()) && (i < code.size()); i++) {
        roomCode[i] = (char) toupper((unsigned char) code[i]);
    }
    mImpl->Start(relayAddress, false, roomCode, build, content);
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
    if (mImpl->GekkoTag != sessionTag) {
        mImpl->FreeGekkoPackets();
        mImpl->GekkoTag = sessionTag;
    }
    sGekkoClient = mImpl.get();
    return &sGekkoAdapter;
}
