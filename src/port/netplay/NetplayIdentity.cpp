#include "NetplayIdentity.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <ship/Context.h>
#include <ship/resource/archive/ArchiveManager.h>

#include "NetplayBuildId.h"

namespace {

constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime = 0x100000001B3ull;
constexpr u32 kZipEndOfDirectorySignature = 0x06054B50;
constexpr u32 kZipDirectoryEntrySignature = 0x02014B50;
constexpr size_t kZipEndOfDirectorySize = 22;
constexpr size_t kZipDirectoryEntrySize = 46;
constexpr size_t kZipMaxCommentSize = 0xFFFF;

struct ZipEntry {
    std::string Name;
    u32 Crc;
    u32 Size;
};

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

u16 ReadU16(const u8* p) {
    return (u16) (p[0] | (p[1] << 8));
}

u32 ReadU32(const u8* p) {
    return (u32) ReadU16(p) | ((u32) ReadU16(p + 2) << 16);
}

std::vector<u8> ReadFileRange(FILE* file, long offset, size_t size) {
    std::vector<u8> data(size);
    if ((fseek(file, offset, SEEK_SET) != 0) || (fread(data.data(), 1, size, file) != size)) {
        data.clear();
    }
    return data;
}

/** Reads a zip file's central directory: one record per file with its name, CRC and uncompressed size. */
bool ReadZipEntries(const std::string& path, std::vector<ZipEntry>* entries) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long fileSize = ftell(file);
    const size_t tailSize = (size_t) std::min<long>(fileSize, kZipEndOfDirectorySize + kZipMaxCommentSize);
    const std::vector<u8> tail = ReadFileRange(file, fileSize - (long) tailSize, tailSize);

    bool found = false;
    u16 count = 0;
    u32 directorySize = 0;
    u32 directoryOffset = 0;
    for (size_t i = tail.size() >= kZipEndOfDirectorySize ? tail.size() - kZipEndOfDirectorySize + 1 : 0; i-- > 0;) {
        if (ReadU32(&tail[i]) == kZipEndOfDirectorySignature) {
            count = ReadU16(&tail[i + 10]);
            directorySize = ReadU32(&tail[i + 12]);
            directoryOffset = ReadU32(&tail[i + 16]);
            found = true;
            break;
        }
    }
    if (!found || ((long) directoryOffset + (long) directorySize > fileSize)) {
        fclose(file);
        return false;
    }

    const std::vector<u8> directory = ReadFileRange(file, (long) directoryOffset, directorySize);
    fclose(file);
    size_t offset = 0;
    for (u16 i = 0; i < count; i++) {
        if ((offset + kZipDirectoryEntrySize > directory.size()) ||
            (ReadU32(&directory[offset]) != kZipDirectoryEntrySignature)) {
            return false;
        }
        const u8* entry = &directory[offset];
        const size_t nameSize = ReadU16(entry + 28);
        const size_t entrySize = kZipDirectoryEntrySize + nameSize + ReadU16(entry + 30) + ReadU16(entry + 32);
        if (offset + entrySize > directory.size()) {
            return false;
        }
        entries->push_back({ std::string((const char*) entry + kZipDirectoryEntrySize, nameSize), ReadU32(entry + 16),
                             ReadU32(entry + 24) });
        offset += entrySize;
    }
    return true;
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
