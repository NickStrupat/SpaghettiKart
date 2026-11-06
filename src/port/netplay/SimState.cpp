#include "SimState.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "SimArena.h"
#include "port/Game.h"
#include "engine/World.h"
#include "engine/TrackBrowser.h"
#include "port/interpolation/FrameInterpolation.h"

extern "C" {
#include "main.h"
#include "code_800029B0.h"
#include "menus.h"
#include "save.h"
#include "save_data.h"
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
bool sDrawingShownFrame = false;
bool sSimFrameScopeOpen = false;
bool sHostControlsMenus = false;
u32 sRaceSetupCount = 0;
// The state before SimState_DrawFrame(), put back once the frame has been displayed
SimSnapshot sBeforeDisplay;
bool sDisplayPending = false;
// The sim section as it was before the first race was set up
std::vector<u8> sBaseline;
std::vector<u8> sBootState;

struct KeptRange {
    void* Data;
    size_t Size;
};

// State in the sim section that must survive SimState_RestoreBaseline()
const std::array<KeptRange, 9> kKeptRanges = { {
    { &gGamestate, sizeof(gGamestate) },
    { &gGamestateNext, sizeof(gGamestateNext) },
    { &gSaveData, sizeof(gSaveData) },
    { D_8018EE10, sizeof(struct_8018EE10_entry) * 2 },
    { &gControllerPak1State, sizeof(gControllerPak1State) },
    { &sControllerPak2State, sizeof(sControllerPak2State) },
    { &gControllerPak1FileHandle, sizeof(gControllerPak1FileHandle) },
    { &gControllerPak2FileHandle, sizeof(gControllerPak2FileHandle) },
    { &gSoundMode, sizeof(gSoundMode) },
} };

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

// The section names must match SimSection.h. C files' globals come first: SimState_RestoreBaseline() restores only
// those, because C++ statics own heap memory that a stale copy would leave dangling.
constexpr size_t kNumSections = 4;
constexpr size_t kNumCSections = 2;
constexpr std::array<const char*, kNumSections> kSectionNames = { "sim_data", "sim_bss", "sim_cxx_data",
                                                                  "sim_cxx_bss" };
using SectionRanges = std::array<Range, kNumSections>;

#if defined(__clang__) && defined(__APPLE__)

SectionRanges FindSectionRanges() {
    SectionRanges ranges;
    for (size_t i = 0; i < kNumSections; i++) {
        const std::string name = std::string("__") + kSectionNames[i];
        unsigned long size = 0;
        u8* data = getsectiondata(&_mh_execute_header, "__DATA", name.c_str(), &size);
        ranges[i] = { data, (size_t) size };
    }
    return ranges;
}

#elif defined(__clang__) && defined(__ELF__)

// Defined by the linker for sections whose names are valid C identifiers.
extern "C" {
extern u8 __start_sim_data[] __attribute__((weak));
extern u8 __stop_sim_data[] __attribute__((weak));
extern u8 __start_sim_bss[] __attribute__((weak));
extern u8 __stop_sim_bss[] __attribute__((weak));
extern u8 __start_sim_cxx_data[] __attribute__((weak));
extern u8 __stop_sim_cxx_data[] __attribute__((weak));
extern u8 __start_sim_cxx_bss[] __attribute__((weak));
extern u8 __stop_sim_cxx_bss[] __attribute__((weak));
}

SectionRanges FindSectionRanges() {
    return { Range{ __start_sim_data, (size_t) (__stop_sim_data - __start_sim_data) },
             Range{ __start_sim_bss, (size_t) (__stop_sim_bss - __start_sim_bss) },
             Range{ __start_sim_cxx_data, (size_t) (__stop_sim_cxx_data - __start_sim_cxx_data) },
             Range{ __start_sim_cxx_bss, (size_t) (__stop_sim_cxx_bss - __start_sim_cxx_bss) } };
}

#else

SectionRanges FindSectionRanges() {
    return {};
}

#endif

const SectionRanges& GetSectionRanges() {
    static const SectionRanges sRanges = FindSectionRanges();
    return sRanges;
}

size_t GetSectionsSize(size_t count) {
    size_t size = 0;
    for (size_t i = 0; i < count; i++) {
        size += GetSectionRanges()[i].Size;
    }
    return size;
}

/** Copies the first `count` sections, back to back, into `out`. */
void CopySections(std::vector<u8>& out, size_t count) {
    out.resize(GetSectionsSize(count));
    u8* data = out.data();
    for (size_t i = 0; i < count; i++) {
        const Range& range = GetSectionRanges()[i];
        memcpy(data, range.Data, range.Size);
        data += range.Size;
    }
}

void RestoreSections(const std::vector<u8>& in, size_t count) {
    const u8* data = in.data();
    for (size_t i = 0; i < count; i++) {
        const Range& range = GetSectionRanges()[i];
        memcpy(range.Data, data, range.Size);
        data += range.Size;
    }
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

/** Restores the C files' globals from a copy, except the kept ranges. */
void RestoreCGlobalsKeeping(const std::vector<u8>& copy) {
    std::vector<std::vector<u8>> kept;
    for (const KeptRange& range : kKeptRanges) {
        const u8* data = static_cast<const u8*>(range.Data);
        kept.emplace_back(data, data + range.Size);
    }
    RestoreSections(copy, kNumCSections);
    for (size_t i = 0; i < kKeptRanges.size(); i++) {
        memcpy(kKeptRanges[i].Data, kept[i].data(), kKeptRanges[i].Size);
    }
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
    if ((sRaceSetupCount == 0) && SimState_IsSupported()) {
        CopySections(sBaseline, kNumCSections);
    }
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

extern "C" void SimState_RestoreBaseline(void) {
    if (sBaseline.empty()) {
        return; // No race has run yet
    }
    RestoreCGlobalsKeeping(sBaseline);
}

extern "C" void SimState_CaptureBootState(void) {
    if (SimState_IsSupported()) {
        CopySections(sBootState, kNumCSections);
    }
}

extern "C" void SimState_RestoreBootState(void) {
    if (!sBootState.empty()) {
        RestoreCGlobalsKeeping(sBootState);
    }
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
        // Draw code writes state the simulation reads (init_render_player(), for one), so every frame is drawn the
        // same way on every peer: split screen, into the gfx pool that is not on screen, then discarded.
        // SimState_DrawFrame() draws what is shown.
        FrameInterpolation_SetRecordingPaused(true);
        struct GfxPool* shownPool = gGfxPool;
        Gfx* shownHead = gDisplayListHead;
        gGfxPool = (gGfxPool == &gGfxPools[0]) ? &gGfxPools[1] : &gGfxPools[0];
        gDisplayListHead = gGfxPool->gfxPool;
        race_render_frame();
        gGfxPool = shownPool;
        gDisplayListHead = shownHead;
    }
    CM_RunGarbageCollector();
    FrameInterpolation_SetRecordingPaused(false);
    sResimulating = false;
}

extern "C" void SimState_BeginDisplay(void) {
    if (!sDisplayPending) {
        SimState::Save(sBeforeDisplay);
        sDisplayPending = true;
    }
}

extern "C" void SimState_DrawFrame(s32 viewPlayer) {
    SimState_BeginDisplay();
    sDrawingShownFrame = true;
    if (viewPlayer >= 0) {
        race_render_own_view(viewPlayer);
    } else {
        race_render_frame();
    }
    sDrawingShownFrame = false;
}

extern "C" void SimState_EndDisplay(void) {
    if (sDisplayPending) {
        SimState::Load(sBeforeDisplay);
        sDisplayPending = false;
    }
}

extern "C" bool SimState_IsResimulating(void) {
    return sResimulating;
}

extern "C" bool SimState_IsDrawingShownFrame(void) {
    return sDrawingShownFrame;
}

namespace SimState {

void Save(SimSnapshot& snapshot) {
    // Snapshot buffers are long-lived and must never come from the arena they copy.
    SimHeapPause pause;

    CopySections(snapshot.Section, kNumSections);

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
    RestoreSections(snapshot.Section, kNumSections);

    SimArena::Restore(snapshot.Arena.data(), snapshot.Arena.size());

    const u8* in = snapshot.World.data();
    GetWorld()->ForEachSimStateRange([&](void* data, size_t size) {
        memcpy(data, in, size);
        in += size;
    });
}

void WriteDifferences(const SimSnapshot& a, const SimSnapshot& b, FILE* out) {
    const uintptr_t slide = GetImageSlide();
    size_t offset = 0;
    for (size_t i = 0; i < kNumSections; i++) {
        const Range& range = GetSectionRanges()[i];
        WriteDifferingRuns(kSectionNames[i], a.Section.data() + offset, b.Section.data() + offset, range.Size,
                           (uintptr_t) range.Data - slide, out);
        offset += range.Size;
    }

    if (a.Arena.size() != b.Arena.size()) {
        fprintf(out, "arena size %zu vs %zu\n", a.Arena.size(), b.Arena.size());
    }
    WriteDifferingRuns("arena", a.Arena.data(), b.Arena.data(), std::min(a.Arena.size(), b.Arena.size()), 0, out);
    WriteDifferingRuns("world", a.World.data(), b.World.data(), std::min(a.World.size(), b.World.size()), 0, out);
}

void WriteSnapshotFile(const SimSnapshot& snapshot, FILE* out) {
    const uintptr_t slide = GetImageSlide();
    fprintf(out, "SKSNAP %zu", kNumSections);
    for (size_t i = 0; i < kNumSections; i++) {
        const Range& range = GetSectionRanges()[i];
        fprintf(out, " %s %llx %zu", kSectionNames[i], (unsigned long long) ((uintptr_t) range.Data - slide),
                range.Size);
    }
    fprintf(out, " %zu %zu\n", snapshot.Arena.size(), snapshot.World.size());
    fwrite(snapshot.Section.data(), 1, snapshot.Section.size(), out);
    fwrite(snapshot.Arena.data(), 1, snapshot.Arena.size(), out);
    fwrite(snapshot.World.data(), 1, snapshot.World.size(), out);
}

} // namespace SimState
