#ifndef NETPLAY_RELAY_WEBSOCKET_H
#define NETPLAY_RELAY_WEBSOCKET_H

/**
 * The parts of the WebSocket protocol (RFC 6455) the relay needs to serve browsers, which cannot send UDP: the opening
 * handshake and binary frames. Each relay datagram travels as one binary message, so a browser speaks the same
 * protocol as the desktop game (RelayProtocol.h), over TCP. Depends only on the standard library; no TLS (a proxy in
 * front of the relay can add it for pages served over HTTPS).
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace WebSocket {

// A request longer than this is not a browser's handshake.
constexpr size_t kMaxHandshakeSize = 8192;
// Relay datagrams are at most RelayProtocol::kMaxPacketSize; anything much larger is not one.
constexpr size_t kMaxMessageSize = 4096;

enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

/** SHA-1 of the data, which the handshake's accept key is made from. */
inline std::array<uint8_t, 20> Sha1(const std::string& data) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::vector<uint8_t> message(data.begin(), data.end());
    const uint64_t bitLength = (uint64_t) data.size() * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    for (int i = 7; i >= 0; i--) {
        message.push_back((uint8_t) (bitLength >> (i * 8)));
    }
    const auto rotate = [](uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); };
    for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const uint8_t* bytes = &message[chunk + i * 4];
            w[i] = ((uint32_t) bytes[0] << 24) | ((uint32_t) bytes[1] << 16) | ((uint32_t) bytes[2] << 8) | bytes[3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t next = rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotate(b, 30);
            b = a;
            a = next;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<uint8_t, 20> digest;
    for (int i = 0; i < 20; i++) {
        digest[i] = (uint8_t) (h[i / 4] >> (24 - (i % 4) * 8));
    }
    return digest;
}

inline std::string Base64(const uint8_t* data, size_t size) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t chunk = ((uint32_t) data[i] << 16) | ((i + 1 < size) ? (uint32_t) data[i + 1] << 8 : 0) |
                               ((i + 2 < size) ? data[i + 2] : 0);
        out += kAlphabet[(chunk >> 18) & 63];
        out += kAlphabet[(chunk >> 12) & 63];
        out += (i + 1 < size) ? kAlphabet[(chunk >> 6) & 63] : '=';
        out += (i + 2 < size) ? kAlphabet[chunk & 63] : '=';
    }
    return out;
}

/** The Sec-WebSocket-Accept value answering a client's Sec-WebSocket-Key. */
inline std::string AcceptKey(const std::string& clientKey) {
    const std::array<uint8_t, 20> digest = Sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return Base64(digest.data(), digest.size());
}

/** The value of a header in an HTTP request, matched without regard to case, or nothing. */
inline std::optional<std::string> FindHeader(const std::string& request, const std::string& name) {
    size_t lineStart = request.find("\r\n");
    while ((lineStart != std::string::npos) && (lineStart + 2 < request.size())) {
        lineStart += 2;
        const size_t lineEnd = request.find("\r\n", lineStart);
        const std::string line = request.substr(lineStart, lineEnd - lineStart);
        const size_t colon = line.find(':');
        if ((colon == name.size()) &&
            std::equal(name.begin(), name.end(), line.begin(), [](char a, char b) { return tolower(a) == tolower(b); })) {
            size_t valueStart = colon + 1;
            while ((valueStart < line.size()) && (line[valueStart] == ' ')) {
                valueStart++;
            }
            return line.substr(valueStart);
        }
        lineStart = lineEnd;
    }
    return std::nullopt;
}

/**
 * The server's answer to a complete handshake request (everything up to the blank line): the switch to WebSocket, or
 * an error for anything else, such as a browser opening the relay's address as a page.
 * @param accepted set to whether the connection now speaks WebSocket
 */
inline std::string AnswerHandshake(const std::string& request, bool* accepted) {
    const std::optional<std::string> key = FindHeader(request, "Sec-WebSocket-Key");
    const std::optional<std::string> upgrade = FindHeader(request, "Upgrade");
    const bool isUpgrade = upgrade.has_value() && (upgrade->size() == 9) &&
                           std::equal(upgrade->begin(), upgrade->end(), "websocket",
                                      [](char a, char b) { return tolower(a) == b; });
    *accepted = (request.compare(0, 4, "GET ") == 0) && isUpgrade && key.has_value() && !key->empty();
    if (!*accepted) {
        return "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
               "SpaghettiKart netplay relay: connect with a WebSocket.\n";
    }
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " +
           AcceptKey(*key) + "\r\n\r\n";
}

struct Frame {
    Opcode Op = Opcode::Binary;
    bool Final = true;
    std::vector<uint8_t> Payload;
};

enum class ParseResult {
    Frame,      // One frame was read and removed from the buffer
    NeedMore,   // The buffer holds part of a frame
    Malformed,  // Not a frame a browser would send: close the connection
};

/** Reads one frame a client sent from the start of the buffer. Clients must mask their frames. */
inline ParseResult ParseClientFrame(std::vector<uint8_t>& buffer, Frame* frame) {
    if (buffer.size() < 2) {
        return ParseResult::NeedMore;
    }
    const bool final = (buffer[0] & 0x80) != 0;
    const uint8_t opcode = buffer[0] & 0x0F;
    const bool masked = (buffer[1] & 0x80) != 0;
    if (((buffer[0] & 0x70) != 0) || !masked) {
        return ParseResult::Malformed; // Extension bits, or an unmasked client frame
    }
    uint64_t length = buffer[1] & 0x7F;
    size_t offset = 2;
    if (length == 126) {
        if (buffer.size() < 4) {
            return ParseResult::NeedMore;
        }
        length = ((uint64_t) buffer[2] << 8) | buffer[3];
        offset = 4;
    } else if (length == 127) {
        if (buffer.size() < 10) {
            return ParseResult::NeedMore;
        }
        length = 0;
        for (int i = 0; i < 8; i++) {
            length = (length << 8) | buffer[2 + i];
        }
        offset = 10;
    }
    if (length > kMaxMessageSize) {
        return ParseResult::Malformed;
    }
    if (buffer.size() < offset + 4 + length) {
        return ParseResult::NeedMore;
    }
    const uint8_t* mask = &buffer[offset];
    frame->Op = (Opcode) opcode;
    frame->Final = final;
    frame->Payload.resize((size_t) length);
    for (size_t i = 0; i < length; i++) {
        frame->Payload[i] = buffer[offset + 4 + i] ^ mask[i % 4];
    }
    buffer.erase(buffer.begin(), buffer.begin() + (std::ptrdiff_t) (offset + 4 + length));
    return ParseResult::Frame;
}

/** A frame from the server, which sends them unmasked. At most 65535 bytes, far more than a relay datagram. */
inline std::vector<uint8_t> MakeServerFrame(Opcode op, const uint8_t* data, size_t size) {
    std::vector<uint8_t> frame;
    frame.push_back((uint8_t) (0x80 | (uint8_t) op));
    if (size < 126) {
        frame.push_back((uint8_t) size);
    } else {
        frame.push_back(126);
        frame.push_back((uint8_t) (size >> 8));
        frame.push_back((uint8_t) size);
    }
    frame.insert(frame.end(), data, data + size);
    return frame;
}

} // namespace WebSocket

#endif // NETPLAY_RELAY_WEBSOCKET_H
