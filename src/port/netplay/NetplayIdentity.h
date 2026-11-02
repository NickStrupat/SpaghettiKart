#ifndef NETPLAY_IDENTITY_H
#define NETPLAY_IDENTITY_H

#include <libultraship/libultraship.h>

#include <string>

/**
 * What must match for two players to share a room: the same build of the game, and the same game files and mods.
 * The relay compares both when a player joins.
 */
namespace NetplayIdentity {

/** The commit the game was built from, plus a hash of uncommitted changes to tracked files. */
std::string GetBuildId();

/**
 * Hashes the contents of every mounted archive (game data, engine assets, mods), in load order.
 * Zip archives are hashed by each file's name, size and CRC, which do not depend on when or where they were packed.
 */
u64 ComputeContentHash();

} // namespace NetplayIdentity

#endif // NETPLAY_IDENTITY_H
