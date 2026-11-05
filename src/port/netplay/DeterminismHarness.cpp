#include "DeterminismHarness.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <utility>
#include <string>
#include <vector>

#include "Checksum.h"
#include "RaceConfig.h"
#include "SimState.h"
#include "SyntheticDriver.h"
#include "RaceEvents.h"
#include "engine/World.h"
#include "port/Game.h"

extern "C" {
#include "main.h"
#include "defines.h"
}

static_assert(std::endian::native == std::endian::little, "Harness log files are written in host byte order");

namespace {

constexpr char kMagic[4] = { 'S', 'K', 'D', 'T' };
constexpr u32 kFormatVersion = 1;
constexpr u32 kDefaultFrames = 1800;
// Frames to wait after boot before starting the race, so the engine and menus finish initializing.
constexpr u32 kBootFrames = 30;
constexpr size_t kNumPads = 4;


struct FrameRecord {
    std::array<PadInput, kNumPads> Pads = {};
    u64 Checksum = 0;
};

enum class HarnessMode { Off, Record, Verify };
enum class HarnessState { WaitingForBoot, WaitingForRace, Running, Finished };


struct Harness {
    HarnessMode Mode = HarnessMode::Off;
    HarnessState State = HarnessState::WaitingForBoot;
    std::string Path;
    RaceConfig Config;
    u32 TotalFrames = kDefaultFrames;
    u32 BootFramesLeft = kBootFrames;
    u32 Frame = 0;
    std::vector<FrameRecord> Frames;
    std::vector<SyntheticDriver> Drivers;
    GameplayCVarOverride CVars;
    // Rollback stress test: after every frame, roll back this many frames and re-simulate them.
    u32 RollbackFrames = 0;
    std::vector<SimSnapshot> Snapshots;
    FILE* Trace = nullptr;
    s32 ExitCode = EXIT_SUCCESS;
};

// Frames written to the SK_DETERMINISM_TRACE file; the first divergence is almost always within these.
constexpr u32 kTraceFrames = 5;

Harness sHarness;

template <typename T> void Write(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T> bool Read(std::ifstream& in, T& value) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

void WriteConfig(std::ofstream& out, const RaceConfig& config) {
    const u16 nameLength = (u16) config.TrackName.size();
    Write(out, nameLength);
    out.write(config.TrackName.data(), nameLength);
    Write(out, config.ModeSelection);
    Write(out, config.CCSelection);
    Write(out, config.MirrorMode);
    Write(out, config.PlayerCount);
    Write(out, config.Characters);
    Write(out, config.CupIndex);
    Write(out, config.CourseIndexInCup);
    Write(out, config.RandomSeed);
    Write(out, config.IntCVars);
    Write(out, config.FloatCVars);
}

bool ReadConfig(std::ifstream& in, RaceConfig& config) {
    u16 nameLength = 0;
    if (!Read(in, nameLength)) {
        return false;
    }
    config.TrackName.resize(nameLength);
    in.read(config.TrackName.data(), nameLength);
    return Read(in, config.ModeSelection) && Read(in, config.CCSelection) && Read(in, config.MirrorMode) &&
           Read(in, config.PlayerCount) && Read(in, config.Characters) && Read(in, config.CupIndex) &&
           Read(in, config.CourseIndexInCup) && Read(in, config.RandomSeed) && Read(in, config.IntCVars) &&
           Read(in, config.FloatCVars);
}

bool WriteLog(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(kMagic, sizeof(kMagic));
    Write(out, kFormatVersion);
    WriteConfig(out, sHarness.Config);
    Write(out, (u32) sHarness.Frames.size());
    for (const FrameRecord& frame : sHarness.Frames) {
        Write(out, frame.Pads);
        Write(out, frame.Checksum);
    }
    return static_cast<bool>(out);
}

bool ReadLog(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[sizeof(kMagic)] = {};
    u32 version = 0;
    u32 frameCount = 0;
    if (!in.read(magic, sizeof(magic)) || (memcmp(magic, kMagic, sizeof(kMagic)) != 0) || !Read(in, version) ||
        (version != kFormatVersion) || !ReadConfig(in, sHarness.Config) || !Read(in, frameCount)) {
        return false;
    }
    sHarness.Frames.resize(frameCount);
    for (FrameRecord& frame : sHarness.Frames) {
        if (!Read(in, frame.Pads) || !Read(in, frame.Checksum)) {
            return false;
        }
    }
    sHarness.TotalFrames = frameCount;
    return true;
}

const char* GetEnv(const char* name) {
    const char* value = getenv(name);
    return ((value != nullptr) && (value[0] != '\0')) ? value : nullptr;
}

s32 GetEnvInt(const char* name, s32 fallback) {
    const char* value = GetEnv(name);
    return (value != nullptr) ? atoi(value) : fallback;
}

RaceConfig MakeRecordConfig() {
    const bool grandPrix = (GetEnv("SK_DETERMINISM_MODE") == nullptr) || (strcmp(GetEnv("SK_DETERMINISM_MODE"), "gp") == 0);
    s32 players = GetEnvInt("SK_DETERMINISM_PLAYERS", grandPrix ? 1 : 2);
    if (players < 1) {
        players = 1;
    } else if (players > (s32) kNumPads) {
        players = (s32) kNumPads;
    }
    return RaceConfig::FromEnvironment("SK_DETERMINISM_", (s8) players);
}

void Finish(s32 exitCode) {
    SimState_SetRollbackMode(false);
    if (sHarness.Trace != nullptr) {
        fclose(sHarness.Trace);
        sHarness.Trace = nullptr;
    }
    // The game exits with _Exit(), which skips flushing stdio.
    fflush(stdout);
    sHarness.CVars.Restore();
    sHarness.ExitCode = exitCode;
    sHarness.State = HarnessState::Finished;
}

void FillPads(const std::array<PadInput, kNumPads>& inputs, OSContPad* pads) {
    for (size_t i = 0; i < kNumPads; i++) {
        memset(&pads[i], 0, sizeof(OSContPad));
        pads[i].stick_x = inputs[i].StickX;
        pads[i].stick_y = inputs[i].StickY;
        pads[i].button = inputs[i].Buttons;
    }
}

/**
 * Saves the frame that just ran, rolls back RollbackFrames frames and re-simulates up to the current frame,
 * checking every re-simulated frame against its recorded checksum.
 * @returns false on a mismatch, after finishing the run.
 */
void EndRun();

bool RunRollbackCheck() {
    const u32 frame = sHarness.Frame;
    const u32 depth = sHarness.RollbackFrames;
    std::vector<SimSnapshot>& ring = sHarness.Snapshots;
    SimState::Save(ring[frame % ring.size()]);
    if (frame < depth) {
        return true;
    }

    // This frame's display list, not run yet, points into the objects' matrix pool, which every re-simulated frame
    // clears; those frames get a pool of their own.
    std::deque<Mtx> shownMatrices;
    std::swap(shownMatrices, GetWorld()->world_mtx.Objects);
    struct RestoreMatrices {
        std::deque<Mtx>& Shown;
        ~RestoreMatrices() { std::swap(Shown, GetWorld()->world_mtx.Objects); }
    } restoreMatrices{ shownMatrices };

    SimState::Load(ring[(frame - depth) % ring.size()]);
    for (u32 resimFrame = frame - depth + 1; resimFrame <= frame; resimFrame++) {
        std::array<OSContPad, kNumPads> pads;
        FillPads(sHarness.Frames[resimFrame].Pads, pads.data());
        SimState_AdvanceFrame(pads.data(), false);

        const u64 checksum = Sim_GameplayChecksum();
        if (checksum == sHarness.Frames[resimFrame].Checksum) {
            continue;
        }
        printf("[Determinism] FAIL: frame %u diverged after rolling back from frame %u\n", resimFrame, frame);
        if (sHarness.Trace != nullptr) {
            SimSnapshot resimulated;
            SimState::Save(resimulated);
            fprintf(sHarness.Trace, "rollback resimulated frame %u\n", resimFrame);
            Sim_WriteChecksumBreakdown(sHarness.Trace);
            fprintf(sHarness.Trace, "rollback differences frame %u\n", resimFrame);
            SimState::WriteDifferences(ring[resimFrame % ring.size()], resimulated, sHarness.Trace);
            SimState::Load(ring[resimFrame % ring.size()]);
            fprintf(sHarness.Trace, "rollback original frame %u\n", resimFrame);
            Sim_WriteChecksumBreakdown(sHarness.Trace);
        }
        Finish(EXIT_FAILURE);
        return false;
    }
    return true;
}

} // namespace

extern "C" void Harness_Init(void) {
    if (const char* tracePath = GetEnv("SK_DETERMINISM_TRACE")) {
        sHarness.Trace = fopen(tracePath, "w");
    }
    sHarness.RollbackFrames = (u32) GetEnvInt("SK_DETERMINISM_ROLLBACK", 0);
    sHarness.Snapshots.resize(sHarness.RollbackFrames + 1);
    if ((sHarness.RollbackFrames > 0) && !SimState_IsSupported()) {
        printf("[Determinism] FAIL: this build cannot save simulation state (needs clang)\n");
        Finish(EXIT_FAILURE);
        return;
    }
    if (const char* path = GetEnv("SK_DETERMINISM_RECORD")) {
        sHarness.Mode = HarnessMode::Record;
        sHarness.Path = path;
        sHarness.Config = MakeRecordConfig();
        sHarness.TotalFrames = (u32) GetEnvInt("SK_DETERMINISM_FRAMES", kDefaultFrames);
        const char* driver = GetEnv("SK_DETERMINISM_DRIVER");
        const SyntheticDriver::Style style = ((driver != nullptr) && (strcmp(driver, "random") == 0))
                                                 ? SyntheticDriver::Style::Random
                                                 : SyntheticDriver::Style::Racer;
        for (size_t i = 0; i < kNumPads; i++) {
            sHarness.Drivers.emplace_back((sHarness.Config.RandomSeed + 1) * 0x9E3779B9u * (u32) (i + 1), style);
        }
    } else if (const char* path = GetEnv("SK_DETERMINISM_VERIFY")) {
        sHarness.Mode = HarnessMode::Verify;
        sHarness.Path = path;
        if (!ReadLog(path) || sHarness.Frames.empty()) {
            printf("[Determinism] FAIL: could not read log %s\n", path);
            Finish(EXIT_FAILURE);
        }
    }
}

extern "C" void Harness_BeginFrame(void) {
    // RaceConfig_StartRace() takes effect on the next frame's gamestate update.
    if ((sHarness.State == HarnessState::WaitingForRace) && (gGamestate == RACING)) {
        sHarness.State = HarnessState::Running;
        RaceEvents::Reset();
    }
}

extern "C" bool Harness_OverridePads(OSContPad* pads) {
    if (sHarness.State != HarnessState::Running) {
        return false;
    }

    std::array<PadInput, kNumPads> inputs = {};
    if (sHarness.Mode == HarnessMode::Record) {
        for (size_t i = 0; i < (size_t) sHarness.Config.PlayerCount; i++) {
            inputs[i] = sHarness.Drivers[i].Next((s32) i);
        }
        sHarness.Frames.push_back({ inputs, 0 });
    } else {
        inputs = sHarness.Frames[sHarness.Frame].Pads;
    }

    FillPads(inputs, pads);
    return true;
}

extern "C" void Harness_EndFrame(void) {
    if ((sHarness.State == HarnessState::WaitingForBoot) && (sHarness.Mode != HarnessMode::Off)) {
        // Start the race after this frame's menu update, which would otherwise overwrite the track selection.
        if (--sHarness.BootFramesLeft == 0) {
            sHarness.CVars.Apply(sHarness.Config);
            SimState_SetRollbackMode(sHarness.RollbackFrames > 0);
            RaceConfig_StartRace(sHarness.Config);
            sHarness.State = HarnessState::WaitingForRace;
        }
        return;
    }
    if (sHarness.State != HarnessState::Running) {
        return;
    }
    if (gGamestate != RACING) {
        if (sHarness.Mode == HarnessMode::Record) {
            // A racer driver finished the race and confirmed its results: the run ends with the race.
            EndRun();
            return;
        }
        printf("[Determinism] FAIL: race ended early at frame %u\n", sHarness.Frame);
        Finish(EXIT_FAILURE);
        return;
    }
    RaceEvents::Update(sHarness.Frame);

    const u64 checksum = Sim_GameplayChecksum();
    if ((sHarness.Trace != nullptr) && (sHarness.Frame < kTraceFrames)) {
        fprintf(sHarness.Trace, "frame %u %016llx\n", sHarness.Frame, (unsigned long long) checksum);
        Sim_WriteChecksumBreakdown(sHarness.Trace);
    }
    if (sHarness.Mode == HarnessMode::Record) {
        sHarness.Frames[sHarness.Frame].Checksum = checksum;
    } else if (checksum != sHarness.Frames[sHarness.Frame].Checksum) {
        printf("[Determinism] FAIL: frame %u diverged (expected %016llx, got %016llx)\n", sHarness.Frame,
               (unsigned long long) sHarness.Frames[sHarness.Frame].Checksum, (unsigned long long) checksum);
        Finish(EXIT_FAILURE);
        return;
    }
    if ((sHarness.RollbackFrames > 0) && !RunRollbackCheck()) {
        return;
    }

    if (++sHarness.Frame < sHarness.TotalFrames) {
        return;
    }
    EndRun();
}

namespace {

/** Writes or checks the log and exits. */
void EndRun() {
    if (sHarness.Mode == HarnessMode::Record) {
        // The race may have ended before the planned frame count.
        sHarness.Frames.resize(sHarness.Frame);
        if (!WriteLog(sHarness.Path)) {
            printf("[Determinism] FAIL: could not write log %s\n", sHarness.Path.c_str());
            Finish(EXIT_FAILURE);
            return;
        }
        printf("[Determinism] Recorded %u frames to %s\n", sHarness.Frame, sHarness.Path.c_str());
    } else {
        printf("[Determinism] PASS: %u frames match %s\n", sHarness.Frame, sHarness.Path.c_str());
    }
    if (sHarness.RollbackFrames > 0) {
        const SimSnapshot& snapshot = sHarness.Snapshots[0];
        printf("[Determinism] Snapshot size: section %zu, arena %zu, world %zu bytes\n", snapshot.Section.size(),
               snapshot.Arena.size(), snapshot.World.size());
    }
    Finish(EXIT_SUCCESS);
}

} // namespace

extern "C" bool Harness_IsFinished(void) {
    return sHarness.State == HarnessState::Finished;
}

extern "C" s32 Harness_GetExitCode(void) {
    return sHarness.ExitCode;
}
