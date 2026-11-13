#ifndef PORT_ASSETS_VERSION_H
#define PORT_ASSETS_VERSION_H

#include <string>
#include <vector>

/**
 * Which version of the extractor made mk64.o2r. The build hashes everything that shapes the archive Torch makes from the
 * ROM (cmake/AssetsVersion.cmake); the extractor writes that hash into the archive, and the game compares it with its
 * own. Files made by an older version still play, but netplay hides the rooms of players whose files differ, so the
 * game offers to make them again (ModManager.cpp on the desktop, the page in the browser) and the ONLINE screen says so.
 */
namespace AssetsVersion {

/** The file at the archive's root that holds the version. */
constexpr const char* kArchiveFile = "assetsVersion";

/** This build's version: what its extractor writes. */
const char* GetCurrent();

/** The stamp's contents, for Torch (Companion::RegisterArchiveFile). */
std::vector<char> GetStamp();

/** Whether the archive at this path carries this build's version. Reads only the zip's directory. */
bool IsCurrent(const std::string& archivePath);

/** Whether the game's own mk64.o2r carries this build's version. Checked once. */
bool IsGameArchiveCurrent();

} // namespace AssetsVersion

#endif // PORT_ASSETS_VERSION_H
