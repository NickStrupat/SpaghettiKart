#include "SimArena.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

// Address space reserved for the arena. Pages are only backed by memory once used.
constexpr size_t kArenaCapacity = 256 * 1024 * 1024;
// Blocks are powers of two, from 1 << kMinBlockShift bytes up to the arena capacity.
constexpr size_t kMinBlockShift = 5;
constexpr size_t kNumSizeClasses = 24;
// Keeps payloads aligned like malloc's (max_align_t).
constexpr size_t kBlockHeaderSize = 16;
constexpr u32 kBlockMagic = 0x51A4A11C;

struct BlockHeader {
    u32 SizeClass;
    u32 Magic;
    u64 Unused;
};
static_assert(sizeof(BlockHeader) == kBlockHeaderSize);

struct FreeBlock {
    FreeBlock* Next;
};

// Lives at the start of the arena so that arena snapshots include the allocator's own state.
struct ArenaHeader {
    size_t Top; // Offset of the first byte never handed out
    FreeBlock* FreeLists[kNumSizeClasses];
};

u8* sBase = nullptr;
bool sReportedFull = false;
// Arena blocks are allocated on the game thread, but any thread may free one (a resource loaded during race
// setup, for example). Only arena allocations and frees take this lock.
std::mutex sMutex;
thread_local int tScopeDepth = 0;

ArenaHeader* GetHeader() {
    return reinterpret_cast<ArenaHeader*>(sBase);
}

bool Reserve() {
#ifdef _WIN32
    void* memory = VirtualAlloc(nullptr, kArenaCapacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* memory = mmap(nullptr, kArenaCapacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        memory = nullptr;
    }
#endif
    if (memory == nullptr) {
        return false;
    }
    sBase = static_cast<u8*>(memory);
    // Fresh pages are zeroed, so only the bump offset needs setting.
    GetHeader()->Top = (sizeof(ArenaHeader) + kBlockHeaderSize - 1) & ~(kBlockHeaderSize - 1);
    return true;
}

size_t GetSizeClass(size_t blockSize) {
    size_t sizeClass = 0;
    while (((size_t) 1 << (sizeClass + kMinBlockShift)) < blockSize) {
        sizeClass++;
    }
    return sizeClass;
}

void* Allocate(size_t size) {
    if ((sBase == nullptr) && !Reserve()) {
        return nullptr;
    }
    const size_t sizeClass = GetSizeClass(size + kBlockHeaderSize);
    if (sizeClass >= kNumSizeClasses) {
        return nullptr;
    }

    ArenaHeader* header = GetHeader();
    u8* block;
    if (header->FreeLists[sizeClass] != nullptr) {
        FreeBlock* freeBlock = header->FreeLists[sizeClass];
        header->FreeLists[sizeClass] = freeBlock->Next;
        block = reinterpret_cast<u8*>(freeBlock) - kBlockHeaderSize;
    } else {
        const size_t blockSize = (size_t) 1 << (sizeClass + kMinBlockShift);
        if (header->Top + blockSize > kArenaCapacity) {
            if (!sReportedFull) {
                // Logging here could allocate and recurse.
                fprintf(stderr, "[SimArena] Full; falling back to malloc. Netplay rollback will desync.\n");
                sReportedFull = true;
            }
            return nullptr;
        }
        block = sBase + header->Top;
        header->Top += blockSize;
    }

    BlockHeader* blockHeader = reinterpret_cast<BlockHeader*>(block);
    blockHeader->SizeClass = (u32) sizeClass;
    blockHeader->Magic = kBlockMagic;
    return block + kBlockHeaderSize;
}

void Free(void* ptr) {
    BlockHeader* blockHeader = reinterpret_cast<BlockHeader*>(static_cast<u8*>(ptr) - kBlockHeaderSize);
    if (blockHeader->Magic != kBlockMagic) {
        fprintf(stderr, "[SimArena] Freeing a corrupt or already freed block %p\n", ptr);
        abort();
    }
    blockHeader->Magic = 0;

    ArenaHeader* header = GetHeader();
    FreeBlock* freeBlock = static_cast<FreeBlock*>(ptr);
    freeBlock->Next = header->FreeLists[blockHeader->SizeClass];
    header->FreeLists[blockHeader->SizeClass] = freeBlock;
}

void* NewImpl(size_t size) {
    if (tScopeDepth > 0) {
        std::lock_guard<std::mutex> lock(sMutex);
        if (void* ptr = Allocate(size)) {
            return ptr;
        }
    }
    return malloc(size != 0 ? size : 1);
}

void DeleteImpl(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    if (SimArena::Contains(ptr)) {
        std::lock_guard<std::mutex> lock(sMutex);
        Free(ptr);
    } else {
        free(ptr);
    }
}

} // namespace

extern "C" void SimHeap_BeginScope(void) {
    tScopeDepth++;
}

extern "C" void SimHeap_EndScope(void) {
    tScopeDepth--;
}

extern "C" int SimHeap_Pause(void) {
    const int depth = tScopeDepth;
    tScopeDepth = 0;
    return depth;
}

extern "C" void SimHeap_Resume(int pause) {
    tScopeDepth = pause;
}

SimHeapPause::SimHeapPause() : mSavedDepth(tScopeDepth) {
    tScopeDepth = 0;
}

SimHeapPause::~SimHeapPause() {
    tScopeDepth = mSavedDepth;
}

namespace SimArena {

bool Contains(const void* ptr) {
    const u8* bytes = static_cast<const u8*>(ptr);
    return (sBase != nullptr) && (bytes >= sBase) && (bytes < sBase + kArenaCapacity);
}

const u8* GetBase() {
    return sBase;
}

size_t GetUsedBytes() {
    return (sBase != nullptr) ? GetHeader()->Top : 0;
}

void Restore(const u8* data, size_t size) {
    std::lock_guard<std::mutex> lock(sMutex);
    if ((sBase != nullptr) && (size <= kArenaCapacity)) {
        memcpy(sBase, data, size);
    }
}

} // namespace SimArena

// Replacing the global allocation functions routes every C++ allocation through the arena check.
void* operator new(size_t size) {
    void* ptr = NewImpl(size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* operator new[](size_t size) {
    return operator new(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return NewImpl(size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    return NewImpl(size);
}

void operator delete(void* ptr) noexcept {
    DeleteImpl(ptr);
}

void operator delete[](void* ptr) noexcept {
    DeleteImpl(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    DeleteImpl(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    DeleteImpl(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    DeleteImpl(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    DeleteImpl(ptr);
}
