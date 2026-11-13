#ifndef PORT_ZIP_DIRECTORY_H
#define PORT_ZIP_DIRECTORY_H

#include <libultraship/libultraship.h>

#include <string>
#include <vector>

/** One file listed in a zip file's central directory. */
struct ZipEntry {
    std::string Name;
    u32 Crc;
    u32 Size; // Uncompressed
};

/**
 * Reads a zip file's central directory: one record per file with its name, CRC and uncompressed size, without
 * unpacking anything. Netplay's content hash (NetplayIdentity.h) and the archive's assets version (AssetsVersion.h)
 * go by these. @returns false when the file is no zip file.
 */
bool ReadZipEntries(const std::string& path, std::vector<ZipEntry>* entries);

#endif // PORT_ZIP_DIRECTORY_H
