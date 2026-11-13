#ifndef NETPLAY_PAGE_VERSION_H
#define NETPLAY_PAGE_VERSION_H

/**
 * The browser build's version (docs/WEB.md, "Offline"). The page keeps a copy of the game and downloads a new version
 * in the background, which runs only once the player restarts the page. Two players must run the same build to share
 * a room (NetplayIdentity.h), so the online menu asks the page to look for a new version and says what it found.
 * The desktop game is always the build it was made as: there the state is always Unknown.
 */
namespace PageVersion {

enum class State {
    Unknown,     // Not checked yet, no network, or not the browser build
    Downloading, // A new version is being downloaded
    Waiting,     // A new version is downloaded and runs once the page restarts
    Newest,      // The server has no newer version than this one
};

/** Asks the page to look for a new version now. It also does whenever it opens or the player returns to it. */
void Check();

State GetState();

} // namespace PageVersion

#endif // NETPLAY_PAGE_VERSION_H
