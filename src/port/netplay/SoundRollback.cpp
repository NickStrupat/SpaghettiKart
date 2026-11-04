#include "SoundRollback.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <tuple>
#include <vector>

#include "SimArena.h"
#include "SimState.h"

extern "C" {
#include "audio/external.h"
}

namespace {

// A re-simulated sound at most this many frames from one that played is the same sound, moved a little.
constexpr s32 kMatchFrames = 3;
// Sounds stay comparable for this many frames, well past GekkoNet's 8-frame prediction window.
constexpr s32 kKeptFrames = 32;

struct SoundRequest {
    u32 SoundBits;
    Vec3f* Position;
    u8 CameraId;
    f32* FreqScale;
    f32* Volume;
    s8* Reverb;
};

struct FrameSound {
    s32 Frame;
    SoundRequest Request;
};

struct State {
    bool Active = false;
    bool KeepHistory = false;
    s32 Frame = -1; // The frame being simulated, or -1
    bool RollingBack = false;
    bool LoadPending = false;
    s32 RollbackFrame = -1; // The first frame this update re-simulates
    // Sounds of the frames a rollback can still reach, as they play: a sound that kept playing through a rollback
    // keeps the request that started it.
    std::vector<FrameSound> Played;
    // Sounds of this update's frames after a rollback, until Settle() compares them
    std::vector<FrameSound> Captured;
    std::vector<FrameSound> History; // Final sounds of older frames, with KeepHistory
    u32 NumPlayed = 0;
    u32 NumLate = 0;
    u32 NumStopped = 0;
};

State sState;

bool IsSameSound(const SoundRequest& a, const SoundRequest& b) {
    return (a.SoundBits == b.SoundBits) && (a.CameraId == b.CameraId);
}

/** Moves the sounds of frames no rollback can reach any more to the history. */
void Prune(s32 newestFrame) {
    const auto isFinal = [newestFrame](const FrameSound& sound) { return sound.Frame < newestFrame - kKeptFrames; };
    if (sState.KeepHistory) {
        std::copy_if(sState.Played.begin(), sState.Played.end(), std::back_inserter(sState.History), isFinal);
    }
    sState.Played.erase(std::remove_if(sState.Played.begin(), sState.Played.end(), isFinal), sState.Played.end());
}

/** @returns the index of the nearest sound like this one that is not matched yet, or -1. */
s32 FindMatch(const std::vector<FrameSound>& sounds, const std::vector<bool>& matched, const FrameSound& sound) {
    s32 best = -1;
    s32 bestDistance = kMatchFrames + 1;
    for (size_t i = 0; i < sounds.size(); i++) {
        const s32 distance = std::abs(sounds[i].Frame - sound.Frame);
        if (!matched[i] && IsSameSound(sounds[i].Request, sound.Request) && (distance < bestDistance)) {
            best = (s32) i;
            bestDistance = distance;
        }
    }
    return best;
}

/** @returns true when a sound kept playing comes from the same source: stopping one would stop both. */
bool IsStillPlaying(const std::vector<FrameSound>& kept, const SoundRequest& request) {
    return std::any_of(kept.begin(), kept.end(), [&request](const FrameSound& sound) {
        return (sound.Request.Position == request.Position) && (sound.Request.SoundBits == request.SoundBits);
    });
}

void Reconcile(s32 newestFrame) {
    // Frames before the rollback stand; the rest played sounds the re-simulation replaces.
    std::vector<FrameSound> kept;
    std::vector<FrameSound> replaced;
    for (const FrameSound& sound : sState.Played) {
        ((sState.RollbackFrame < 0) || (sound.Frame < sState.RollbackFrame) ? kept : replaced).push_back(sound);
    }

    const size_t numStanding = kept.size();
    std::vector<bool> matched(replaced.size(), false);
    for (const FrameSound& sound : sState.Captured) {
        const s32 match = FindMatch(replaced, matched, sound);
        if (match >= 0) {
            matched[match] = true;
            kept.push_back({ sound.Frame, replaced[match].Request });
            release_sound_slot(sound.Request.Position);
            continue;
        }
        queue_sound(sound.Request.SoundBits, sound.Request.Position, sound.Request.CameraId, sound.Request.FreqScale,
                    sound.Request.Volume, sound.Request.Reverb);
        sState.NumPlayed++;
        sState.NumLate += (sound.Frame < newestFrame) ? 1 : 0;
        kept.push_back(sound);
    }

    const std::vector<FrameSound> reconciled(kept.begin() + (std::ptrdiff_t) numStanding, kept.end());
    for (size_t i = 0; i < replaced.size(); i++) {
        const SoundRequest& request = replaced[i].Request;
        if (!matched[i] && !IsStillPlaying(reconciled, request)) {
            func_800C5578(request.Position, request.SoundBits);
            sState.NumStopped++;
        }
    }
    sState.Played = std::move(kept);
    sState.Captured.clear();
}

} // namespace

extern "C" bool SoundRollback_Capture(u32 soundBits, Vec3f* position, u8 cameraId, f32* freqScale, f32* volume,
                                      s8* reverb) {
    // A frame being shown played its sounds when it was simulated. The determinism harness re-simulates frames with
    // the inputs they had, so their sounds come out the same again.
    if (SimState_IsDrawingShownFrame() || (!sState.Active && SimState_IsResimulating())) {
        release_sound_slot(position);
        return true;
    }
    if (!sState.Active) {
        return false;
    }
    if (sState.Frame < 0) {
        return false;
    }
    const FrameSound sound = { sState.Frame, { soundBits, position, cameraId, freqScale, volume, reverb } };
    // Sounds are asked for mid-frame, inside the sim heap, which a rollback would free from under these lists.
    SimHeapPause pause;
    if (sState.RollingBack) {
        sState.Captured.push_back(sound);
        return true;
    }
    sState.Played.push_back(sound);
    sState.NumPlayed++;
    return false;
}

namespace SoundRollback {

void Start(bool keepHistory) {
    sState = State();
    sState.Active = true;
    sState.KeepHistory = keepHistory;
}

void Stop() {
    for (const FrameSound& sound : sState.Captured) {
        release_sound_slot(sound.Request.Position);
    }
    sState.Active = false;
    sState.Frame = -1;
    sState.RollingBack = false;
    sState.LoadPending = false;
    sState.Captured.clear();
}

void BeginFrame(s32 frame) {
    sState.Frame = frame;
    if (sState.LoadPending) {
        // A second rollback in one update replaces what the first re-simulated from here on.
        sState.LoadPending = false;
        sState.RollbackFrame = (sState.RollbackFrame < 0) ? frame : std::min(sState.RollbackFrame, frame);
        const auto isReplaced = [frame](const FrameSound& sound) { return sound.Frame >= frame; };
        for (const FrameSound& sound : sState.Captured) {
            if (isReplaced(sound)) {
                release_sound_slot(sound.Request.Position);
            }
        }
        sState.Captured.erase(std::remove_if(sState.Captured.begin(), sState.Captured.end(), isReplaced),
                              sState.Captured.end());
    }
}

void EndFrame() {
    sState.Frame = -1;
}

void Rollback() {
    if (sState.Active) {
        sState.RollingBack = true;
        sState.LoadPending = true;
    }
}

void Settle(s32 newestFrame) {
    if (!sState.Active) {
        return;
    }
    if (sState.RollingBack) {
        Reconcile(newestFrame);
        sState.RollingBack = false;
        sState.LoadPending = false;
        sState.RollbackFrame = -1;
    }
    if (newestFrame >= 0) {
        Prune(newestFrame);
    }
}

void WriteLog(const std::string& path) {
    FILE* log = fopen(path.c_str(), "w");
    if (log == nullptr) {
        return;
    }
    std::vector<FrameSound> sounds = sState.History;
    sounds.insert(sounds.end(), sState.Played.begin(), sState.Played.end());
    std::sort(sounds.begin(), sounds.end(), [](const FrameSound& a, const FrameSound& b) {
        return std::make_tuple(a.Frame, a.Request.SoundBits, a.Request.CameraId) <
               std::make_tuple(b.Frame, b.Request.SoundBits, b.Request.CameraId);
    });
    for (const FrameSound& sound : sounds) {
        fprintf(log, "%d %08x %u\n", sound.Frame, sound.Request.SoundBits, sound.Request.CameraId);
    }
    fclose(log);
}

std::string GetSummary() {
    char summary[128];
    snprintf(summary, sizeof(summary), "%u sounds, %u played late, %u stopped", sState.NumPlayed, sState.NumLate,
             sState.NumStopped);
    return summary;
}

} // namespace SoundRollback
