#ifndef NETPLAY_CHECKSUM_H
#define NETPLAY_CHECKSUM_H

#include <stdio.h>
#include <libultraship/libultraship.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hashes the gameplay-relevant simulation state: RNG, race timer, players, actors, objects and cameras.
 * Pointers and struct padding are skipped, so equal state produces equal hashes on every machine and platform.
 * Netplay compares this between peers to detect desyncs.
 */
u64 Sim_GameplayChecksum(void);

/**
 * Writes one line per checksum component (RNG, each player, actor, object and camera) so two runs
 * can be diffed to find which part of the state diverged.
 */
void Sim_WriteChecksumBreakdown(FILE* out);

#ifdef __cplusplus
}
#endif

#endif // NETPLAY_CHECKSUM_H
