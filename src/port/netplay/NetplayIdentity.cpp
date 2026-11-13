#include "NetplayIdentity.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <ship/Context.h>
#include <ship/resource/archive/ArchiveManager.h>

#include "NetplayBuildId.h"
#include "port/ZipDirectory.h"

namespace {

constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime = 0x100000001B3ull;
void Hash(u64* hash, const void* data, size_t size) {
    const u8* bytes = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; i++) {
        *hash = (*hash ^ bytes[i]) * kFnvPrime;
    }
}

void HashU32(u64* hash, u32 value) {
    const u8 bytes[] = { (u8) value, (u8) (value >> 8), (u8) (value >> 16), (u8) (value >> 24) };
    Hash(hash, bytes, sizeof(bytes));
}

void HashString(u64* hash, const std::string& text) {
    HashU32(hash, (u32) text.size());
    Hash(hash, text.data(), text.size());
}

void HashFileContents(u64* hash, const std::filesystem::path& path) {
    FILE* file = fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return;
    }
    u8 buffer[64 * 1024];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        Hash(hash, buffer, read);
    }
    fclose(file);
}

void HashFolder(u64* hash, const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        HashString(hash, std::filesystem::relative(file, root).generic_string());
        HashFileContents(hash, file);
    }
}

} // namespace

namespace NetplayIdentity {

std::string GetBuildId() {
    return NETPLAY_BUILD_ID;
}

u64 ComputeContentHash() {
    u64 hash = kFnvOffsetBasis;
    const auto archives = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->GetArchives();
    for (const auto& archive : *archives) {
        const std::string& path = archive->GetPath();
        std::vector<ZipEntry> entries;
        if (std::filesystem::is_directory(path)) {
            HashFolder(&hash, path);
        } else if (ReadZipEntries(path, &entries)) {
            std::sort(entries.begin(), entries.end(),
                      [](const ZipEntry& a, const ZipEntry& b) { return a.Name < b.Name; });
            for (const ZipEntry& entry : entries) {
                HashString(&hash, entry.Name);
                HashU32(&hash, entry.Crc);
                HashU32(&hash, entry.Size);
            }
        } else {
            HashFileContents(&hash, path);
        }
        // Separates archives, so that moving a file from one archive to the next changes the hash.
        HashU32(&hash, 0xFFFFFFFF);
    }
    return hash;
}

} // namespace NetplayIdentity
