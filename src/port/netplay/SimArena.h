#ifndef NETPLAY_SIM_ARENA_H
#define NETPLAY_SIM_ARENA_H

#include <libultraship/libultraship.h>

/**
 * Sim arena: a fixed-address heap for the C++ objects the simulation creates and destroys, such as actors,
 * objects, particle emitters and the containers that hold them.
 *
 * While a sim heap scope is open on a thread, every global operator new on that thread allocates from the arena.
 * The allocator keeps all of its bookkeeping inside the arena, so copying the arena's used bytes is a complete
 * snapshot of the simulation's heap, and copying them back restores it (see SimState.h).
 * operator delete frees into whichever heap a pointer came from, so arena and regular allocations can mix freely.
 *
 * Only open a scope around simulation code. Anything long-lived that is not simulation state must not be allocated
 * in a scope, or a rollback would free it from under its owner; wrap such code in SimHeapPause.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Starts routing this thread's C++ allocations into the sim arena. Scopes nest. */
void SimHeap_BeginScope(void);

/** Ends the scope started by the matching SimHeap_BeginScope(). */
void SimHeap_EndScope(void);

#ifdef __cplusplus
}

#include <cstddef>

namespace SimArena {

/** @returns true when ptr was allocated from the arena. */
bool Contains(const void* ptr);

/** @returns the start of the arena, or nullptr before the first arena allocation. */
const u8* GetBase();

/** @returns the number of bytes at the start of the arena that hold allocations or bookkeeping. */
size_t GetUsedBytes();

/** Copies a snapshot taken from GetBase() / GetUsedBytes() back into the arena. */
void Restore(const u8* data, size_t size);

} // namespace SimArena

/** Opens a sim heap scope for the lifetime of the object. */
class SimHeapScope {
public:
    SimHeapScope() {
        SimHeap_BeginScope();
    }
    ~SimHeapScope() {
        SimHeap_EndScope();
    }
    SimHeapScope(const SimHeapScope&) = delete;
    SimHeapScope& operator=(const SimHeapScope&) = delete;
};

/** Suspends any open sim heap scopes for the lifetime of the object, for long-lived non-simulation allocations. */
class SimHeapPause {
public:
    SimHeapPause();
    ~SimHeapPause();
    SimHeapPause(const SimHeapPause&) = delete;
    SimHeapPause& operator=(const SimHeapPause&) = delete;

private:
    int mSavedDepth;
};

#endif // __cplusplus

#endif // NETPLAY_SIM_ARENA_H
