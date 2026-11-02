#ifndef NETPLAY_SIM_SECTION_H
#define NETPLAY_SIM_SECTION_H

/**
 * Force-included (see CMakeLists.txt) into every translation unit that holds simulation state.
 * Their mutable globals and static locals are placed in one dedicated section, so a netplay
 * rollback snapshot can save and restore them all with a single copy (see SimState.h).
 *
 * Wrap anything in those files that is not simulation state, such as render buffers, OS objects
 * or huge memory pools, in NOT_SIM_STATE_BEGIN / NOT_SIM_STATE_END.
 *
 * Only clang supports per-file section placement; other compilers build without rollback support.
 */

#if defined(__clang__)
#define NETPLAY_SIM_SECTION_SUPPORTED 1
#if defined(__APPLE__)
// "zerofill" keeps zero-initialized state out of the executable file, as with __bss.
#define SIM_STATE_BEGIN _Pragma("clang section bss=\"__DATA,__sim_bss,zerofill\" data=\"__DATA,__sim_data\"")
#else
#define SIM_STATE_BEGIN _Pragma("clang section bss=\"sim_bss\" data=\"sim_data\"")
#endif
#define NOT_SIM_STATE_BEGIN _Pragma("clang section bss=\"\" data=\"\"")
#define NOT_SIM_STATE_END SIM_STATE_BEGIN
#else
#define NETPLAY_SIM_SECTION_SUPPORTED 0
#define SIM_STATE_BEGIN
#define NOT_SIM_STATE_BEGIN
#define NOT_SIM_STATE_END
#endif

SIM_STATE_BEGIN

#endif // NETPLAY_SIM_SECTION_H
