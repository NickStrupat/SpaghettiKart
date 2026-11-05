#include "TestScreenshots.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "NetLog.h"

extern "C" {
#include "main.h"
#include "racing/framebuffer_effects.h"
}

namespace {

constexpr size_t kWidth = SCREEN_WIDTH;
constexpr size_t kHeight = SCREEN_HEIGHT;
// Stored (uncompressed) deflate blocks hold at most this many bytes.
constexpr size_t kMaxStoredBlock = 0xFFFF;

struct Shot {
    std::string Name;
    u32 Frame;
};

std::vector<Shot> sShots;
std::string sDirectory = ".";
std::array<u16, kWidth * kHeight> sPixels;
std::string sPendingPath;
// Metal fills a framebuffer copy in a frame later, so each picture asks twice and keeps the second answer.
constexpr u32 kReadsPerShot = 2;
u32 sReadsLeft = 0;

u32 Crc32(const u8* data, size_t size, u32 crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

void AppendU32(std::vector<u8>& out, u32 value) {
    out.insert(out.end(), { (u8) (value >> 24), (u8) (value >> 16), (u8) (value >> 8), (u8) value });
}

void AppendChunk(std::vector<u8>& out, const char* type, const std::vector<u8>& body) {
    AppendU32(out, (u32) body.size());
    std::vector<u8> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    out.insert(out.end(), typed.begin(), typed.end());
    AppendU32(out, Crc32(typed.data(), typed.size()));
}

/** An RGB PNG of the picture, with uncompressed deflate blocks: no compression library needed. */
std::vector<u8> EncodePng() {
    std::vector<u8> raw;
    for (size_t y = 0; y < kHeight; y++) {
        raw.push_back(0); // No filter
        for (size_t x = 0; x < kWidth; x++) {
            const u16 pixel = sPixels[y * kWidth + x];
            raw.push_back((u8) (((pixel >> 11) & 0x1F) * 255 / 31));
            raw.push_back((u8) (((pixel >> 6) & 0x1F) * 255 / 31));
            raw.push_back((u8) (((pixel >> 1) & 0x1F) * 255 / 31));
        }
    }
    std::vector<u8> zlib = { 0x78, 0x01 };
    for (size_t offset = 0; offset < raw.size(); offset += kMaxStoredBlock) {
        const size_t size = std::min(kMaxStoredBlock, raw.size() - offset);
        const bool last = offset + size == raw.size();
        zlib.insert(zlib.end(), { (u8) (last ? 1 : 0), (u8) size, (u8) (size >> 8), (u8) ~size, (u8) (~size >> 8) });
        zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + size);
    }
    u32 a = 1;
    u32 b = 0;
    for (u8 byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    AppendU32(zlib, (b << 16) | a);

    std::vector<u8> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    std::vector<u8> header;
    AppendU32(header, kWidth);
    AppendU32(header, kHeight);
    header.insert(header.end(), { 8, 2, 0, 0, 0 }); // 8-bit RGB
    AppendChunk(png, "IHDR", header);
    AppendChunk(png, "IDAT", zlib);
    AppendChunk(png, "IEND", {});
    return png;
}

} // namespace

namespace TestScreenshots {

void Init() {
    if (const char* directory = getenv("SK_TEST_SCREENSHOT_DIR")) {
        sDirectory = directory;
    }
    const char* list = getenv("SK_TEST_SCREENSHOTS");
    if (list == nullptr) {
        return;
    }
    const std::string text = list;
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = std::min(text.find(',', start), text.size());
        const std::string item = text.substr(start, comma - start);
        const size_t at = item.find('@');
        if (at != std::string::npos) {
            sShots.push_back({ item.substr(0, at), (u32) strtoul(item.c_str() + at + 1, nullptr, 10) });
        }
        start = comma + 1;
    }
}

void EndFrame(u32 frame) {
    for (const Shot& shot : sShots) {
        if ((shot.Frame == frame) && sPendingPath.empty()) {
            sPendingPath = sDirectory + "/" + shot.Name + ".png";
            sReadsLeft = kReadsPerShot;
        }
    }
    if (sReadsLeft > 0) {
        FB_WriteFramebufferSliceToCPU(&gDisplayListHead, sPixels.data(), false);
        sReadsLeft--;
    }
}

void BeginFrame() {
    if (sPendingPath.empty() || (sReadsLeft > 0)) {
        return;
    }
    const std::vector<u8> png = EncodePng();
    if (FILE* file = fopen(sPendingPath.c_str(), "wb")) {
        fwrite(png.data(), 1, png.size(), file);
        fclose(file);
        NetLog("Test: saved %s", sPendingPath.c_str());
    }
    sPendingPath.clear();
}

} // namespace TestScreenshots
