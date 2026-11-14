#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
bool GameEngine_OTRSigCheck(const char* imgData);
/**
 * libultraship's ResourceGetDataByName() and ResourceGetSizeByName(), kept out of the netplay sim arena: a lookup that
 * misses the resource cache allocates, and whether it misses depends on what this machine loaded before (each player
 * draws their own view), so inside a simulated frame the arenas of two peers would drift apart and a rollback would
 * free what the cache still holds (src/port/netplay/SimArena.h). Game code looks resources up through these.
 */
void* GameEngine_GetResourceDataByName(const char* name);
size_t GameEngine_GetResourceSizeByName(const char* name);
void* GameEngine_GetResourceDataByCrc(uint64_t crc);
/** libultraship's ResourceGetTexSizeByName(), ResourceGetTexWidthByName() and ResourceGetTexHeightByName(), likewise. */
size_t GameEngine_GetTexSizeByName(const char* name);
uint16_t GameEngine_GetTexWidthByName(const char* name);
uint16_t GameEngine_GetTexHeightByName(const char* name);
#ifdef __cplusplus
}
#endif

#if defined(_WIN32)
#define ALIGN_ASSET(x) __declspec(align(x))
#else
#define ALIGN_ASSET(x) __attribute__((aligned(x)))
#endif

#define LOAD_ASSET(path) \
    (path == NULL ? NULL \
                  : (GameEngine_OTRSigCheck((const char*) path) ? GameEngine_GetResourceDataByName((const char*) path) : path))
#define LOAD_ASSET_RAW(path) GameEngine_GetResourceDataByName((const char*) path)
