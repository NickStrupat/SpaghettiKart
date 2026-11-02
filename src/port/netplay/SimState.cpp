#include "SimState.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "SimArena.h"
#include "port/Game.h"
#include "engine/World.h"
#include "engine/TrackBrowser.h"
#include "port/interpolation/FrameInterpolation.h"

extern "C" {
#include "main.h"
#include "code_800029B0.h"
}

#if defined(__clang__) && defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

namespace {

struct Range {
    u8* Data = nullptr;
    size_t Size = 0;
};

bool sRollbackMode = false;
bool sResimulating = false;
bool sSimFrameScopeOpen = false;
bool sHostControlsMenus = false;
u32 sRaceSetupCount = 0;

void UpdateMenuController() {
    if (!sHostControlsMenus) {
        combine_controllers();
        return;
    }
    gControllerFive->button = gControllerOne->button;
    gControllerFive->buttonPressed = gControllerOne->buttonPressed;
    gControllerFive->buttonDepressed = gControllerOne->buttonDepressed;
    gControllerFive->stickDirection = gControllerOne->stickDirection;
    gControllerFive->stickPressed = gControllerOne->stickPressed;
    gControllerFive->stickDepressed = gControllerOne->stickDepressed;
}

// The section names must match SimSection.h.
#if defined(__clang__) && defined(__APPLE__)

Range GetMachOSection(const char* name) {
    unsigned long size = 0;
    u8* data = getsectiondata(&_mh_execute_header, "__DATA", name, &size);
    return { data, (size_t) size };
}

std::array<Range, 2> FindSectionRanges() {
    return { GetMachOSection("__sim_data"), GetMachOSection("__sim_bss") };
}

#elif defined(__clang__) && defined(__ELF__)

// Defined by the linker for sections whose names are valid C identifiers.
extern "C" {
extern u8 __start_sim_data[] __attribute__((weak));
extern u8 __stop_sim_data[] __attribute__((weak));
extern u8 __start_sim_bss[] __attribute__((weak));
extern u8 __stop_sim_bss[] __attribute__((weak));
}

std::array<Range, 2> FindSectionRanges() {
    return { Range{ __start_sim_data, (size_t) (__stop_sim_data - __start_sim_data) },
             Range{ __start_sim_bss, (size_t) (__stop_sim_bss - __start_sim_bss) } };
}

#else

std::array<Range, 2> FindSectionRanges() {
    return {};
}

#endif

const std::array<Range, 2>& GetSectionRanges() {
    static const std::array<Range, 2> sRanges = FindSectionRanges();
    return sRanges;
}

// Runtime address minus link-time address of the executable.
uintptr_t GetImageSlide() {
#if defined(__APPLE__)
    return (uintptr_t) _dyld_get_image_vmaddr_slide(0);
#else
    return 0;
#endif
}

// Writes each run of differing bytes as "label start-address length".
void WriteDifferingRuns(const char* label, const u8* a, const u8* b, size_t size, uintptr_t baseAddress, FILE* out) {
    size_t i = 0;
    while (i < size) {
        if (a[i] == b[i]) {
            i++;
            continue;
        }
        size_t end = i;
        // Merge runs separated by fewer than 8 equal bytes.
        while ((end < size) && ((a[end] != b[end]) || ((end + 8 < size) && (memcmp(a + end, b + end, 8) != 0)))) {
            end++;
        }
        fprintf(out, "%s 0x%llx %zu\n", label, (unsigned long long) (baseAddress + i), end - i);
        i = end;
    }
}

size_t GetWorldStateSize() {
    size_t size = 0;
    GetWorld()->ForEachSimStateRange([&](void*, size_t rangeSize) { size += rangeSize; });
    return size;
}

} // namespace

extern "C" bool SimState_IsSupported(void) {
    return GetSectionRanges()[1].Size > 0;
}

extern "C" void SimState_SetRollbackMode(bool enabled) {
    sRollbackMode = enabled;
}

extern "C" bool SimState_IsRollbackMode(void) {
    return sRollbackMode;
}

extern "C" void SimState_BeginRaceSetup(void) {
    sRaceSetupCount++;
    if (!sRollbackMode) {
        return;
    }
    // Rebuild everything the race mutates inside the arena, so that snapshots capture all of it:
    // the World's objects and containers, and the track object itself.
    Track* track = GetWorld()->GetTrack();
    const std::string trackName = (track != nullptr) ? track->ResourceName : "";

    SimHeap_BeginScope();
    CM_CleanWorld();
    GetWorld()->ResetSimContainers();
    if (!trackName.empty()) {
        TrackBrowser_SetTrack(trackName.c_str());
    }
    // setup_race() skips loading a course whose Track object sits at the address of the last one it loaded. The
    // arena hands freed addresses out again, so the next course's object can land there; always load.
    gCurrentlyLoadedTrackAddr = 0;
}

extern "C" void SimState_EndRaceSetup(void) {
    if (sRollbackMode) {
        SimHeap_EndScope();
    }
}

extern "C" u32 SimState_GetRaceSetupCount(void) {
    return sRaceSetupCount;
}

extern "C" void SimState_SetHostControlsMenus(bool enabled) {
    sHostControlsMenus = enabled;
}

extern "C" void SimState_ResetControllers(void) {
    // Applying a released pad twice clears both the held state and the edges computed from it.
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < 4; i++) {
            OSContPad released = {};
            apply_pad_to_controller(&gControllers[i], &released);
        }
    }
    UpdateMenuController();
}

extern "C" void SimState_BeginSimFrame(void) {
    if (sRollbackMode && !sSimFrameScopeOpen) {
        SimHeap_BeginScope();
        sSimFrameScopeOpen = true;
    }
}

extern "C" void SimState_EndSimFrame(void) {
    if (sSimFrameScopeOpen) {
        SimHeap_EndScope();
        sSimFrameScopeOpen = false;
    }
}

extern "C" void SimState_AdvanceFrame(OSContPad* pads, bool shown) {
    sResimulating = !shown;
    FrameInterpolation_SetRecordingPaused(!shown);
    for (size_t i = 0; i < 4; i++) {
        apply_pad_to_controller(&gControllers[i], &pads[i]);
    }
    UpdateMenuController();

    // Same order as race_logic_loop().
    SimState_BeginSimFrame();
    const bool simulated = race_sim_frame();
    SimState_EndSimFrame();
    if (simulated) {
        if (shown) {
            race_render_frame();
        } else {
            // Draw code writes state the simulation reads (init_render_player(), for one), so hidden frames are
            // drawn too: into the gfx pool that is not on screen, then discarded.
            struct GfxPool* shownPool = gGfxPool;
            Gfx* shownHead = gDisplayListHead;
            gGfxPool = (gGfxPool == &gGfxPools[0]) ? &gGfxPools[1] : &gGfxPools[0];
            gDisplayListHead = gGfxPool->gfxPool;
            race_render_frame();
            gGfxPool = shownPool;
            gDisplayListHead = shownHead;
        }
    }
    CM_RunGarbageCollector();
    FrameInterpolation_SetRecordingPaused(false);
    sResimulating = false;
}

extern "C" void SimState_RedrawFrame(void) {
    static SimSnapshot sBeforeDraw;
    SimState::Save(sBeforeDraw);
    race_render_frame();
    SimState::Load(sBeforeDraw);
}

extern "C" bool SimState_IsResimulating(void) {
    return sResimulating;
}

namespace SimState {

void Save(SimSnapshot& snapshot) {
    // Snapshot buffers are long-lived and must never come from the arena they copy.
    SimHeapPause pause;

    const auto& sections = GetSectionRanges();
    snapshot.Section.resize(sections[0].Size + sections[1].Size);
    memcpy(snapshot.Section.data(), sections[0].Data, sections[0].Size);
    memcpy(snapshot.Section.data() + sections[0].Size, sections[1].Data, sections[1].Size);

    snapshot.Arena.resize(SimArena::GetUsedBytes());
    if (!snapshot.Arena.empty()) {
        memcpy(snapshot.Arena.data(), SimArena::GetBase(), snapshot.Arena.size());
    }

    snapshot.World.resize(GetWorldStateSize());
    u8* out = snapshot.World.data();
    GetWorld()->ForEachSimStateRange([&](void* data, size_t size) {
        memcpy(out, data, size);
        out += size;
    });
}

void Load(const SimSnapshot& snapshot) {
    const auto& sections = GetSectionRanges();
    memcpy(sections[0].Data, snapshot.Section.data(), sections[0].Size);
    memcpy(sections[1].Data, snapshot.Section.data() + sections[0].Size, sections[1].Size);

    SimArena::Restore(snapshot.Arena.data(), snapshot.Arena.size());

    const u8* in = snapshot.World.data();
    GetWorld()->ForEachSimStateRange([&](void* data, size_t size) {
        memcpy(data, in, size);
        in += size;
    });
}

void WriteDifferences(const SimSnapshot& a, const SimSnapshot& b, FILE* out) {
    const auto& sections = GetSectionRanges();
    const uintptr_t slide = GetImageSlide();
    WriteDifferingRuns("sim_data", a.Section.data(), b.Section.data(), sections[0].Size,
                       (uintptr_t) sections[0].Data - slide, out);
    WriteDifferingRuns("sim_bss", a.Section.data() + sections[0].Size, b.Section.data() + sections[0].Size,
                       sections[1].Size, (uintptr_t) sections[1].Data - slide, out);

    if (a.Arena.size() != b.Arena.size()) {
        fprintf(out, "arena size %zu vs %zu\n", a.Arena.size(), b.Arena.size());
    }
    WriteDifferingRuns("arena", a.Arena.data(), b.Arena.data(), std::min(a.Arena.size(), b.Arena.size()), 0, out);
    WriteDifferingRuns("world", a.World.data(), b.World.data(), std::min(a.World.size(), b.World.size()), 0, out);
}

void WriteSnapshotFile(const SimSnapshot& snapshot, FILE* out) {
    const auto& sections = GetSectionRanges();
    const uintptr_t slide = GetImageSlide();
    fprintf(out, "SKSNAP %llx %zu %llx %zu %zu %zu\n", (unsigned long long) ((uintptr_t) sections[0].Data - slide),
            sections[0].Size, (unsigned long long) ((uintptr_t) sections[1].Data - slide), sections[1].Size,
            snapshot.Arena.size(), snapshot.World.size());
    fwrite(snapshot.Section.data(), 1, snapshot.Section.size(), out);
    fwrite(snapshot.Arena.data(), 1, snapshot.Arena.size(), out);
    fwrite(snapshot.World.data(), 1, snapshot.World.size(), out);
}

} // namespace SimState
