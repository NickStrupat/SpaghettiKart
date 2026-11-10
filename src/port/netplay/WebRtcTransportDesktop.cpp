// The desktop game's WebRTC data channels (WebRtcTransport.h), from libdatachannel, for direct connections to
// browsers. The browser build gets the same functions from src/port/web/WebRtcLink.js.
#ifndef __EMSCRIPTEN__

#include "WebRtcTransport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include "NetLog.h"
#include "tools/netplay-relay/RelayProtocol.h"

namespace {

struct Event {
    int Slot;
    int Kind;
    std::vector<u8> Data;
};

struct Connection {
    std::shared_ptr<rtc::PeerConnection> Peer;
    std::shared_ptr<rtc::DataChannel> Channel;
    u32 Generation = 0; // Which opening of this slot it is: events from an earlier one are dropped
    bool Lost = false;
};

// libdatachannel calls back on its own threads, so everything here is behind this lock. It is never held while
// calling into libdatachannel, whose callbacks take it.
std::mutex sMutex;
std::array<Connection, RelayProtocol::kMaxSlots> sConnections;
std::array<u32, RelayProtocol::kMaxSlots> sGenerations = {};
std::deque<Event> sEvents;

bool IsValidSlot(int slot) {
    return (slot >= 0) && (slot < (int) RelayProtocol::kMaxSlots);
}

/** Queues an event if it comes from the slot's current connection. */
void Push(int slot, u32 generation, int kind, std::vector<u8> data = {}) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sGenerations[slot] != generation) {
        return;
    }
    if (kind == SK_RTC_EVENT_CLOSED) {
        if (sConnections[slot].Lost) {
            return;
        }
        sConnections[slot].Lost = true;
    }
    sEvents.push_back({ slot, kind, std::move(data) });
}

void PushSignal(int slot, u32 generation, const nlohmann::json& message) {
    const std::string text = message.dump();
    Push(slot, generation, SK_RTC_EVENT_SIGNAL, std::vector<u8>(text.begin(), text.end()));
}

void AttachChannel(int slot, u32 generation, const std::shared_ptr<rtc::DataChannel>& channel) {
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sGenerations[slot] != generation) {
            return;
        }
        sConnections[slot].Channel = channel;
    }
    channel->onOpen([slot, generation]() { Push(slot, generation, SK_RTC_EVENT_OPEN); });
    channel->onClosed([slot, generation]() { Push(slot, generation, SK_RTC_EVENT_CLOSED); });
    channel->onMessage(
        [slot, generation](rtc::binary data) {
            const u8* bytes = reinterpret_cast<const u8*>(data.data());
            Push(slot, generation, SK_RTC_EVENT_DATA, std::vector<u8>(bytes, bytes + data.size()));
        },
        [](rtc::string) {});
}

rtc::Configuration MakeConfiguration(const char* iceServersJson) {
    rtc::Configuration configuration;
    const nlohmann::json servers = nlohmann::json::parse(iceServersJson, nullptr, false);
    if (servers.is_array()) {
        for (const nlohmann::json& server : servers) {
            const nlohmann::json urls = server.value("urls", nlohmann::json());
            for (const nlohmann::json& url : urls.is_array() ? urls : nlohmann::json::array({ urls })) {
                if (url.is_string()) {
                    configuration.iceServers.emplace_back(url.get<std::string>());
                }
            }
        }
    }
    return configuration;
}

void InitLogging() {
    static std::once_flag sOnce;
    std::call_once(sOnce, []() {
        rtc::InitLogger(rtc::LogLevel::Warning,
                        [](rtc::LogLevel, std::string message) { NetLog("libdatachannel: %s", message.c_str()); });
    });
}

} // namespace

extern "C" void sk_rtc_open(int slot, int initiator, const char* iceServersJson) {
    if (!IsValidSlot(slot)) {
        return;
    }
    sk_rtc_close(slot);
    InitLogging();
    auto peer = std::make_shared<rtc::PeerConnection>(MakeConfiguration(iceServersJson));
    u32 generation;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        generation = ++sGenerations[slot];
        sConnections[slot] = Connection{ peer, nullptr, generation, false };
    }
    peer->onLocalDescription([slot, generation](rtc::Description description) {
        PushSignal(slot, generation, { { "type", description.typeString() }, { "sdp", std::string(description) } });
    });
    peer->onLocalCandidate([slot, generation](rtc::Candidate candidate) {
        PushSignal(slot, generation,
                   { { "type", "candidate" },
                     { "candidate", { { "candidate", candidate.candidate() }, { "sdpMid", candidate.mid() } } } });
    });
    peer->onStateChange([slot, generation](rtc::PeerConnection::State state) {
        if ((state == rtc::PeerConnection::State::Failed) || (state == rtc::PeerConnection::State::Closed)) {
            Push(slot, generation, SK_RTC_EVENT_CLOSED);
        }
    });
    if (initiator) {
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;
        AttachChannel(slot, generation, peer->createDataChannel("sk", init));
    } else {
        peer->onDataChannel([slot, generation](std::shared_ptr<rtc::DataChannel> channel) {
            AttachChannel(slot, generation, channel);
        });
    }
}

extern "C" void sk_rtc_close(int slot) {
    if (!IsValidSlot(slot)) {
        return;
    }
    Connection connection;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        connection = sConnections[slot];
        sConnections[slot] = Connection();
        sGenerations[slot]++;
        std::erase_if(sEvents, [slot](const Event& event) { return event.Slot == slot; });
    }
    if (connection.Peer != nullptr) {
        connection.Peer->close();
    }
}

extern "C" void sk_rtc_signal(int slot, const u8* data, int size) {
    if (!IsValidSlot(slot)) {
        return;
    }
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        peer = sConnections[slot].Peer;
    }
    const nlohmann::json message = nlohmann::json::parse(data, data + size, nullptr, false);
    if ((peer == nullptr) || !message.is_object()) {
        return;
    }
    try {
        const std::string type = message.value("type", "");
        if ((type == "offer") || (type == "answer")) {
            peer->setRemoteDescription(rtc::Description(message.value("sdp", ""), type));
        } else if (type == "candidate") {
            const nlohmann::json candidate = message.value("candidate", nlohmann::json::object());
            const std::string text = candidate.value("candidate", "");
            // An empty candidate marks the end of the other side's candidates.
            if (!text.empty()) {
                peer->addRemoteCandidate(rtc::Candidate(text, candidate.value("sdpMid", "")));
            }
        }
    } catch (const std::exception& e) {
        NetLog("WebRTC: slot %d sent a setup message that does not work: %s", slot, e.what());
    }
}

extern "C" int sk_rtc_send(int slot, const u8* data, int size) {
    if (!IsValidSlot(slot)) {
        return 0;
    }
    std::shared_ptr<rtc::DataChannel> channel;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        channel = sConnections[slot].Channel;
    }
    if ((channel == nullptr) || !channel->isOpen()) {
        return 0;
    }
    try {
        channel->send(reinterpret_cast<const std::byte*>(data), (size_t) size);
        return 1;
    } catch (const std::exception&) {
        return 0;
    }
}

extern "C" int sk_rtc_poll(int* slot, int* kind, u8* buffer, int capacity) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sEvents.empty()) {
        return -1;
    }
    const Event event = std::move(sEvents.front());
    sEvents.pop_front();
    *slot = event.Slot;
    *kind = event.Kind;
    const int size = std::min((int) event.Data.size(), capacity);
    memcpy(buffer, event.Data.data(), (size_t) size);
    return size;
}

#endif // __EMSCRIPTEN__
