#include "ZipDirectory.h"

#include <algorithm>
#include <cstdio>

namespace {

constexpr u32 kZipEndOfDirectorySignature = 0x06054B50;
constexpr u32 kZipDirectoryEntrySignature = 0x02014B50;
constexpr size_t kZipEndOfDirectorySize = 22;
constexpr size_t kZipDirectoryEntrySize = 46;
constexpr size_t kZipMaxCommentSize = 0xFFFF;

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

} // namespace

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

