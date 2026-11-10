// The browser build's RelayLink (RelayLink.h, docs/WEB.md). A page cannot send UDP: it reaches the relay with a
// WebSocket, which carries the same messages, one per binary message, and reaches the other members with WebRTC data
// channels (WebRtcPeers.h). The desktop game uses RelayLinkUdp.cpp instead.
#ifdef __EMSCRIPTEN__

#include "RelayLink.h"

#include <deque>
#include <vector>

#include <emscripten/websocket.h>

#include "WebRtcPeers.h"

using namespace RelayProtocol;

// src/port/web/WebRtcLink.js
extern "C" int sk_page_is_secure(void);

namespace {

class RelayLinkWeb : public RelayLink {
  public:
    explicit RelayLinkWeb(const Callbacks& callbacks)
        : mCallbacks(callbacks), mPeers(WebRtcPeers::Role::Browser, { callbacks.FromMember, callbacks.SendSignal }) {
    }

    ~RelayLinkWeb() override {
        if (mSocket > 0) {
            emscripten_websocket_close(mSocket, 1000, "");
            emscripten_websocket_delete(mSocket);
        }
    }

    void Open(const std::string& relayAddress) override {
        mAddress = relayAddress;
        const std::string url = MakeUrl(relayAddress);
        EmscriptenWebSocketCreateAttributes attributes;
        emscripten_websocket_init_create_attributes(&attributes);
        attributes.url = url.c_str();
        attributes.protocols = nullptr;
        attributes.createOnMainThread = true;
        mSocket = emscripten_websocket_new(&attributes);
        if (mSocket <= 0) {
            Fail("Could not connect to the relay server at " + relayAddress);
            return;
        }
        mState = State::Connecting;
        emscripten_websocket_set_onopen_callback(mSocket, this, OnOpen);
        emscripten_websocket_set_onmessage_callback(mSocket, this, OnMessage);
        emscripten_websocket_set_onerror_callback(mSocket, this, OnError);
        emscripten_websocket_set_onclose_callback(mSocket, this, OnClose);
    }

    State GetState() const override {
        return mState;
    }

    const std::string& GetError() const override {
        return mError;
    }

    void SendToRelay(const u8* data, size_t size) override {
        if (mState == State::Open) {
            emscripten_websocket_send_binary(mSocket, const_cast<u8*>(data), (uint32_t) size);
        }
    }

    void Receive() override {
        // The WebSocket's callbacks queued what arrived between frames.
        while (!mIncoming.empty()) {
            const std::vector<u8> message = std::move(mIncoming.front());
            mIncoming.pop_front();
            mCallbacks.FromRelay(message.data(), message.size());
        }
        mPeers.Receive();
    }

    void GetLocalAddress(u32* address, u16* port) const override {
        // Members reach a browser with WebRTC, never at an address.
        *address = 0;
        *port = 0;
    }

    void SetMembers(s32 ownSlot, u64, const Members& members) override {
        mPeers.SetMembers(ownSlot, members);
    }

    void UpdateDirect(bool allowed) override {
        mPeers.Update(allowed);
    }

    bool HasDirect(u8 slot) const override {
        return mPeers.HasDirect(slot);
    }

    s32 GetDirectPingMs(u8 slot) const override {
        return mPeers.GetPingMs(slot);
    }

    void SendDirect(u8 slot, const u8* data, size_t size) override {
        mPeers.Send(slot, data, size);
    }

    void HandleSignal(u8 slot, const u8* data, size_t size) override {
        mPeers.HandleSignal(slot, data, size);
    }

  private:
    static std::string MakeUrl(const std::string& address) {
        if ((address.rfind("ws://", 0) == 0) || (address.rfind("wss://", 0) == 0)) {
            return address;
        }
        // A page served over HTTPS may only open secure WebSockets, which the relay leaves to a proxy that adds TLS on
        // the standard port of its host (tools/netplay-relay/README.md): host:port means wss://host/ there.
        if (sk_page_is_secure()) {
            return "wss://" + address.substr(0, address.find(':')) + "/";
        }
        const std::string hostAndPort =
            (address.find(':') == std::string::npos) ? address + ":" + std::to_string(kDefaultPort) : address;
        return "ws://" + hostAndPort + "/";
    }

    void Fail(const std::string& error) {
        mError = error;
        mState = State::Failed;
    }

    static EM_BOOL OnOpen(int, const EmscriptenWebSocketOpenEvent*, void* userData) {
        static_cast<RelayLinkWeb*>(userData)->mState = State::Open;
        return EM_TRUE;
    }

    static EM_BOOL OnMessage(int, const EmscriptenWebSocketMessageEvent* event, void* userData) {
        if (!event->isText) {
            static_cast<RelayLinkWeb*>(userData)->mIncoming.emplace_back(event->data, event->data + event->numBytes);
        }
        return EM_TRUE;
    }

    static EM_BOOL OnError(int, const EmscriptenWebSocketErrorEvent*, void* userData) {
        static_cast<RelayLinkWeb*>(userData)->Disconnected();
        return EM_TRUE;
    }

    static EM_BOOL OnClose(int, const EmscriptenWebSocketCloseEvent*, void* userData) {
        static_cast<RelayLinkWeb*>(userData)->Disconnected();
        return EM_TRUE;
    }

    void Disconnected() {
        if (mState == State::Failed) {
            return;
        }
        Fail((mState == State::Open) ? "Lost the connection to the relay server"
                                     : "Could not connect to the relay server at " + mAddress);
    }

    Callbacks mCallbacks;
    WebRtcPeers mPeers;
    State mState = State::Connecting;
    std::string mError;
    std::string mAddress;
    EMSCRIPTEN_WEBSOCKET_T mSocket = 0;
    std::deque<std::vector<u8>> mIncoming;
};

} // namespace

std::unique_ptr<RelayLink> RelayLink::Create(const Callbacks& callbacks) {
    return std::make_unique<RelayLinkWeb>(callbacks);
}

#endif // __EMSCRIPTEN__
