#include "AssetsVersion.h"

#include <algorithm>
#include <cstring>

#include <ship/Context.h>

#include "AssetsVersionDefine.h"
#include "Engine.h"
#include "ZipDirectory.h"

namespace {

/** The zip format's CRC-32, of the stamp only: a few bytes, so without a table. */
u32 Crc32(const char* data, size_t size) {
    u32 crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc ^= (u8) data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

} // namespace

namespace AssetsVersion {

const char* GetCurrent() {
    return SK_ASSETS_VERSION;
}

std::vector<char> GetStamp() {
    const std::string version = SK_ASSETS_VERSION;
    return std::vector<char>(version.begin(), version.end());
}

bool IsCurrent(const std::string& archivePath) {
    std::vector<ZipEntry> entries;
    if (!ReadZipEntries(archivePath, &entries)) {
        return false;
    }
    const std::vector<char> stamp = GetStamp();
    const u32 crc = Crc32(stamp.data(), stamp.size());
    return std::any_of(entries.begin(), entries.end(), [&](const ZipEntry& entry) {
        return (entry.Name == kArchiveFile) && (entry.Size == stamp.size()) && (entry.Crc == crc);
    });
}

bool IsGameArchiveCurrent() {
    static const bool sCurrent = IsCurrent(Ship::Context::GetPathRelativeToAppDirectory(game_asset_file));
    return sCurrent;
}

} // namespace AssetsVersion
