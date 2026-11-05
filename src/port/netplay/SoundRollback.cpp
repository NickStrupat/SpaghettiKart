#include "SoundRollback.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "SimArena.h"
#include "SimState.h"

extern "C" {
#include "audio/external.h"
#include "main.h"
#include "sounds.h"
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
    // A positional sound plays from a slot (func_800C1C88) that follows these and is freed when the sound stops:
    // starting it again takes a new slot like it.
    bool InSlot = false;
    f32* SlotPosition = nullptr;
    f32* SlotVelocity = nullptr;
    f32* SlotScale = nullptr;
    u8 SlotKind = 0;
    f32 SlotDistance = 0.0f;
    bool FreqFromSlot = false;
};

struct FrameSound {
    s32 Frame;
    u32 Order; // When the simulation asked for it, among the sounds and stops of a rollback
    SoundRequest Request;
};

/**
 * A stop the simulation made: one position's sound (func_800C5578), a sound at every position (func_800C56F0), or the
 * positional sounds that follow a position (stop_slot_sounds).
 */
struct FrameStop {
    s32 Frame;
    u32 Order;
    Vec3f* Position;
    const void* Source; // What the stopped sounds follow (see GetSource)
    u32 SoundBits;
    bool AnyPosition;
    u8 Slots; // For the sounds that follow Source: at most this many
    // What it silenced when it was carried out: a rollback that finds it never happened starts these again.
    std::vector<FrameSound> Silenced;
};

struct State {
    bool Active = false;
    bool KeepHistory = false;
    s32 Frame = -1; // The frame being simulated, or -1
    bool RollingBack = false;
    bool LoadPending = false;
    s32 RollbackFrame = -1; // The first frame this update re-simulates
    u32 NextOrder = 0;
    // Sounds and stops of the frames a rollback can still reach, as they were carried out: a sound that kept playing
    // through a rollback keeps the request that started it.
    std::vector<FrameSound> Played;
    std::vector<FrameStop> PlayedStops;
    // This update's sounds and stops after a rollback, until Settle() compares them
    std::vector<FrameSound> Captured;
    std::vector<FrameStop> CapturedStops;
    // Final sounds and stops of older frames, with KeepHistory
    std::vector<FrameSound> History;
    std::vector<FrameStop> StopHistory;
    // Every sound started that may still be playing, however long ago: what a stop silences
    std::vector<FrameSound> Started;
    u32 NumPlayed = 0;
    u32 NumLate = 0;
    u32 NumStopped = 0;
    u32 NumRestarted = 0;
};

State sState;

/** @returns what a sound follows: its slot's position for a positional sound, otherwise its own position. */
const void* GetSource(const SoundRequest& request) {
    return request.InSlot ? (const void*) request.SlotPosition : (const void*) request.Position;
}

SoundRequest MakeRequest(u32 soundBits, Vec3f* position, u8 cameraId, f32* freqScale, f32* volume, s8* reverb) {
    SoundRequest request = { soundBits, position, cameraId, freqScale, volume, reverb };
    if (const Unk_8018EFD8* slot = find_sound_slot(position)) {
        request.InSlot = true;
        request.SlotPosition = slot->posX;
        request.SlotVelocity = slot->velX;
        request.SlotScale = slot->unk24;
        request.SlotKind = slot->unk30;
        request.SlotDistance = slot->unk34;
        request.FreqFromSlot = (freqScale == &slot->unk2C);
    }
    return request;
}

bool IsSameSound(const SoundRequest& a, const SoundRequest& b) {
    return (a.SoundBits == b.SoundBits) && (a.CameraId == b.CameraId);
}

/** @returns whether carrying out this stop silences that sound. */
bool Stops(const FrameStop& stop, const SoundRequest& request) {
    if (stop.SoundBits != request.SoundBits) {
        return false;
    }
    if (stop.Slots > 0) {
        return request.InSlot && (request.SlotPosition == stop.Source);
    }
    return stop.AnyPosition || (stop.Position == request.Position);
}

/** @returns whether this stop concerns that sound, or another start of it. */
bool MayConcern(const FrameStop& stop, const SoundRequest& request) {
    return Stops(stop, request) || ((stop.SoundBits == request.SoundBits) && (stop.Source == GetSource(request)));
}

bool IsSameStop(const FrameStop& a, const FrameStop& b) {
    return (a.SoundBits == b.SoundBits) && (a.AnyPosition == b.AnyPosition) && (a.Slots == b.Slots) &&
           (a.Source == b.Source);
}

/** Keeps the start of a sound that may be silenced later, replacing an older one from the same position. */
void AddStarted(const FrameSound& sound) {
    std::vector<FrameSound>& started = sState.Started;
    started.erase(std::remove_if(started.begin(), started.end(),
                                 [&sound](const FrameSound& other) {
                                     return (other.Request.Position == sound.Request.Position) &&
                                            (other.Request.SoundBits == sound.Request.SoundBits);
                                 }),
                  started.end());
    started.push_back(sound);
}

void ForgetStarted(const SoundRequest& request) {
    std::vector<FrameSound>& started = sState.Started;
    started.erase(std::remove_if(started.begin(), started.end(),
                                 [&request](const FrameSound& other) {
                                     return (other.Request.Position == request.Position) &&
                                            (other.Request.SoundBits == request.SoundBits);
                                 }),
                  started.end());
}

/** @returns the sounds this stop silences if it is carried out now. */
std::vector<FrameSound> FindSilenced(const FrameStop& stop) {
    std::vector<FrameSound> silenced;
    for (const FrameSound& sound : sState.Started) {
        if (Stops(stop, sound.Request) && is_sound_active(sound.Request.Position, sound.Request.SoundBits)) {
            silenced.push_back(sound);
        }
    }
    return silenced;
}

void CarryOut(const FrameStop& stop) {
    // Outside a simulated frame, so none of these comes back here.
    if (stop.Slots > 0) {
        stop_slot_sounds((f32*) stop.Source, stop.SoundBits, stop.Slots);
    } else if (stop.AnyPosition) {
        func_800C56F0(stop.SoundBits);
    } else {
        func_800C5578(stop.Position, stop.SoundBits);
    }
}

/** Moves the sounds of frames no rollback can reach any more to the history. */
void Prune(s32 newestFrame) {
    const s32 oldest = newestFrame - kKeptFrames;
    const auto isFinal = [oldest](const auto& event) { return event.Frame < oldest; };
    if (sState.KeepHistory) {
        std::copy_if(sState.Played.begin(), sState.Played.end(), std::back_inserter(sState.History), isFinal);
        for (const FrameStop& stop : sState.PlayedStops) {
            if (isFinal(stop)) {
                sState.StopHistory.push_back({ stop.Frame, stop.Order, stop.Position, stop.Source, stop.SoundBits,
                                               stop.AnyPosition, stop.Slots, {} });
            }
        }
    }
    sState.Played.erase(std::remove_if(sState.Played.begin(), sState.Played.end(), isFinal), sState.Played.end());
    sState.PlayedStops.erase(std::remove_if(sState.PlayedStops.begin(), sState.PlayedStops.end(), isFinal),
                             sState.PlayedStops.end());
    // A sound that ended by itself has nothing left to silence.
    sState.Started.erase(std::remove_if(sState.Started.begin(), sState.Started.end(),
                                        [oldest](const FrameSound& sound) {
                                            return (sound.Frame < oldest) &&
                                                   !is_sound_active(sound.Request.Position, sound.Request.SoundBits);
                                        }),
                         sState.Started.end());
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

/** @returns the index of the nearest stop like this one that is not matched yet, or -1. */
s32 FindStopMatch(const std::vector<FrameStop>& stops, const std::vector<bool>& matched, const FrameStop& stop) {
    s32 best = -1;
    s32 bestDistance = kMatchFrames + 1;
    for (size_t i = 0; i < stops.size(); i++) {
        const s32 distance = std::abs(stops[i].Frame - stop.Frame);
        if (!matched[i] && IsSameStop(stops[i], stop) && (distance < bestDistance)) {
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

/** @returns whether any of the request's pointers lead into the sim arena, where an object can come and go. */
bool PointsIntoArena(const SoundRequest& request) {
    return SimArena::Contains(request.Position) || SimArena::Contains(request.FreqScale) ||
           SimArena::Contains(request.Volume) || SimArena::Contains(request.Reverb) ||
           SimArena::Contains(request.SlotPosition) || SimArena::Contains(request.SlotVelocity) ||
           SimArena::Contains(request.SlotScale);
}

/**
 * @returns whether a sound that a stop which no longer happens silenced should play again: the final frames still
 * start it, do not stop it some other way, and do not start it anew.
 */
bool ShouldRestart(const FrameSound& silenced, const std::vector<FrameSound>& reconciled,
                   const std::vector<FrameStop>& stops) {
    const SoundRequest& request = silenced.Request;
    const bool standing = silenced.Frame < sState.RollbackFrame;
    const bool started = standing || IsStillPlaying(reconciled, request);
    const bool stoppedOtherwise = std::any_of(stops.begin(), stops.end(), [&silenced](const FrameStop& stop) {
        return (stop.Frame >= silenced.Frame) && MayConcern(stop, silenced.Request);
    });
    const bool startedAnew = std::any_of(reconciled.begin(), reconciled.end(), [&silenced](const FrameSound& sound) {
        return (sound.Frame > silenced.Frame) && (sound.Request.SoundBits == silenced.Request.SoundBits) &&
               (sound.Request.CameraId == silenced.Request.CameraId) &&
               (GetSource(sound.Request) == GetSource(silenced.Request));
    });
    // A start that was itself re-simulated may follow an object that the re-simulation placed elsewhere.
    const bool safe = standing || !PointsIntoArena(request);
    return started && !stoppedOtherwise && !startedAnew && safe &&
           !is_sound_active(request.Position, request.SoundBits);
}

/** Starts a silenced sound again, in a new slot if it is positional. @returns false when no slot is free. */
bool Restart(SoundRequest& request) {
    if (request.InSlot) {
        Unk_8018EFD8* slot = func_800C1C88(request.SlotKind, request.SlotPosition, request.SlotVelocity,
                                           request.SlotScale, request.CameraId, request.SoundBits);
        if (slot == nullptr) {
            return false;
        }
        slot->unk34 = request.SlotDistance;
        request.Position = &slot->unk18;
        if (request.FreqFromSlot) {
            request.FreqScale = &slot->unk2C;
        }
    }
    queue_sound(request.SoundBits, request.Position, request.CameraId, request.FreqScale, request.Volume,
                request.Reverb);
    return true;
}

/** @returns whether a sound kept from before the rollback, and started after this stop, would be cut by it. */
bool CutsKeptSound(const FrameStop& stop, const std::vector<s32>& soundMatches, const std::vector<FrameSound>& replaced) {
    for (size_t i = 0; i < sState.Captured.size(); i++) {
        if ((sState.Captured[i].Order > stop.Order) && (soundMatches[i] >= 0) &&
            Stops(stop, replaced[soundMatches[i]].Request)) {
            return true;
        }
    }
    return false;
}

void Reconcile(s32 newestFrame) {
    // Frames before the rollback stand; the rest made sounds and stops that the re-simulation replaces.
    const auto isReplaced = [](s32 frame) { return (sState.RollbackFrame >= 0) && (frame >= sState.RollbackFrame); };
    std::vector<FrameSound> kept;
    std::vector<FrameSound> replaced;
    for (const FrameSound& sound : sState.Played) {
        (isReplaced(sound.Frame) ? replaced : kept).push_back(sound);
    }
    std::vector<FrameStop> keptStops;
    std::vector<FrameStop> replacedStops;
    for (FrameStop& stop : sState.PlayedStops) {
        (isReplaced(stop.Frame) ? replacedStops : keptStops).push_back(std::move(stop));
    }

    std::vector<bool> matched(replaced.size(), false);
    std::vector<s32> soundMatches;
    for (const FrameSound& sound : sState.Captured) {
        soundMatches.push_back(FindMatch(replaced, matched, sound));
        if (soundMatches.back() >= 0) {
            matched[soundMatches.back()] = true;
        }
    }
    std::vector<bool> matchedStops(replacedStops.size(), false);
    std::vector<s32> stopMatches;
    for (const FrameStop& stop : sState.CapturedStops) {
        stopMatches.push_back(FindStopMatch(replacedStops, matchedStops, stop));
        if (stopMatches.back() >= 0) {
            matchedStops[stopMatches.back()] = true;
        }
    }

    // In the order the simulation made them, so that a stop cancels the sounds started before it.
    const size_t numStanding = kept.size();
    size_t nextSound = 0;
    size_t nextStop = 0;
    while ((nextSound < sState.Captured.size()) || (nextStop < sState.CapturedStops.size())) {
        const bool takeStop = (nextStop < sState.CapturedStops.size()) &&
                              ((nextSound == sState.Captured.size()) ||
                               (sState.CapturedStops[nextStop].Order < sState.Captured[nextSound].Order));
        if (takeStop) {
            FrameStop& stop = sState.CapturedStops[nextStop];
            const s32 match = stopMatches[nextStop];
            nextStop++;
            if (match >= 0) {
                FrameStop same = std::move(replacedStops[match]);
                same.Frame = stop.Frame;
                same.Order = stop.Order;
                keptStops.push_back(std::move(same));
                continue;
            }
            if (!CutsKeptSound(stop, soundMatches, replaced)) {
                stop.Silenced = FindSilenced(stop);
                CarryOut(stop);
            }
            keptStops.push_back(std::move(stop));
            continue;
        }
        const FrameSound& sound = sState.Captured[nextSound];
        const s32 match = soundMatches[nextSound];
        nextSound++;
        if (match >= 0) {
            kept.push_back({ sound.Frame, sound.Order, replaced[match].Request });
            release_sound_slot(sound.Request.Position);
            continue;
        }
        queue_sound(sound.Request.SoundBits, sound.Request.Position, sound.Request.CameraId, sound.Request.FreqScale,
                    sound.Request.Volume, sound.Request.Reverb);
        sState.NumPlayed++;
        sState.NumLate += (sound.Frame < newestFrame) ? 1 : 0;
        kept.push_back(sound);
        AddStarted(sound);
    }

    const std::vector<FrameSound> reconciled(kept.begin() + (std::ptrdiff_t) numStanding, kept.end());
    for (size_t i = 0; i < replaced.size(); i++) {
        const SoundRequest& request = replaced[i].Request;
        if (!matched[i] && !IsStillPlaying(reconciled, request)) {
            func_800C5578(request.Position, request.SoundBits);
            ForgetStarted(request);
            sState.NumStopped++;
        }
    }
    for (size_t i = 0; i < replacedStops.size(); i++) {
        if (matchedStops[i]) {
            continue;
        }
        for (FrameSound& silenced : replacedStops[i].Silenced) {
            if (ShouldRestart(silenced, reconciled, keptStops) && Restart(silenced.Request)) {
                AddStarted(silenced);
                sState.NumRestarted++;
            }
        }
    }
    sState.Played = std::move(kept);
    sState.PlayedStops = std::move(keptStops);
    sState.Captured.clear();
    sState.CapturedStops.clear();
}

/** @returns whether sounds and stops asked for now are a frame's that was simulated before. */
bool IsRepeat() {
    // A frame being shown made its sounds when it was simulated. The determinism harness re-simulates frames with the
    // inputs they had, so their sounds come out the same again.
    return SimState_IsDrawingShownFrame() || (!sState.Active && SimState_IsResimulating());
}

} // namespace

extern "C" bool SoundRollback_Capture(u32 soundBits, Vec3f* position, u8 cameraId, f32* freqScale, f32* volume,
                                      s8* reverb) {
    if (IsRepeat()) {
        release_sound_slot(position);
        return true;
    }
    if (!sState.Active || (sState.Frame < 0)) {
        return false;
    }
    const FrameSound sound = { sState.Frame, sState.NextOrder++,
                               MakeRequest(soundBits, position, cameraId, freqScale, volume, reverb) };
    // Sounds are asked for mid-frame, inside the sim heap, which a rollback would free from under these lists.
    SimHeapPause pause;
    if (sState.RollingBack) {
        sState.Captured.push_back(sound);
        return true;
    }
    sState.Played.push_back(sound);
    AddStarted(sound);
    sState.NumPlayed++;
    return false;
}

namespace {

/** @returns true when the stop is carried out later or not at all. */
bool CaptureStop(FrameStop stop) {
    if (IsRepeat()) {
        return true;
    }
    if (!sState.Active || (sState.Frame < 0)) {
        return false;
    }
    stop.Frame = sState.Frame;
    stop.Order = sState.NextOrder++;
    SimHeapPause pause;
    if (sState.RollingBack) {
        sState.CapturedStops.push_back(std::move(stop));
        return true;
    }
    stop.Silenced = FindSilenced(stop);
    sState.PlayedStops.push_back(std::move(stop));
    return false;
}

} // namespace

extern "C" bool SoundRollback_CaptureStop(Vec3f* position, u32 soundBits, bool anyPosition) {
    return CaptureStop({ -1, 0, position, position, soundBits, anyPosition, 0, {} });
}

extern "C" bool SoundRollback_CaptureSlotStop(const f32* source, u32 soundBits, u8 count) {
    return CaptureStop({ -1, 0, nullptr, source, soundBits, false, count, {} });
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
    sState.CapturedStops.clear();
    sState.Started.clear();
}

void BeginFrame(s32 frame) {
    sState.Frame = frame;
    if (sState.LoadPending) {
        // A second rollback in one update replaces what the first re-simulated from here on.
        sState.LoadPending = false;
        sState.RollbackFrame = (sState.RollbackFrame < 0) ? frame : std::min(sState.RollbackFrame, frame);
        const auto isReplaced = [frame](const auto& event) { return event.Frame >= frame; };
        for (const FrameSound& sound : sState.Captured) {
            if (isReplaced(sound)) {
                release_sound_slot(sound.Request.Position);
            }
        }
        sState.Captured.erase(std::remove_if(sState.Captured.begin(), sState.Captured.end(), isReplaced),
                              sState.Captured.end());
        sState.CapturedStops.erase(
            std::remove_if(sState.CapturedStops.begin(), sState.CapturedStops.end(), isReplaced),
            sState.CapturedStops.end());
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
    std::vector<std::pair<s32, std::string>> lines;
    char line[64];
    const auto addSound = [&lines, &line](const FrameSound& sound) {
        snprintf(line, sizeof(line), "%d %08x %u", sound.Frame, sound.Request.SoundBits, sound.Request.CameraId);
        lines.emplace_back(sound.Frame, line);
    };
    const auto addStop = [&lines, &line](const FrameStop& stop) {
        snprintf(line, sizeof(line), "%d stop %08x%s", stop.Frame, stop.SoundBits,
                 stop.AnyPosition ? " everywhere" : ((stop.Slots > 0) ? " following" : ""));
        lines.emplace_back(stop.Frame, line);
    };
    std::for_each(sState.History.begin(), sState.History.end(), addSound);
    std::for_each(sState.Played.begin(), sState.Played.end(), addSound);
    std::for_each(sState.StopHistory.begin(), sState.StopHistory.end(), addStop);
    std::for_each(sState.PlayedStops.begin(), sState.PlayedStops.end(), addStop);
    std::sort(lines.begin(), lines.end());
    for (const auto& entry : lines) {
        fprintf(log, "%s\n", entry.second.c_str());
    }
    fclose(log);
}

std::string SelfTest() {
    // A sound one frame starts and the next stops, at a player's own position, and the star, which follows a player in
    // a slot. Requests stay queued meanwhile (nothing processes them), which is_sound_active() sees.
    constexpr u32 kSound = SOUND_ARG_LOAD(0x01, 0x00, 0xFA, 0x28);
    Vec3f* position = &D_800E9F7C[0].pos;
    const auto play = [position] { play_sound(kSound, position, 0, &D_800EA1D4, &D_800EA1D4, (s8*) &D_800EA1DC); };
    const auto stop = [] { func_800C9018(0, kSound); };
    const auto frame = [](s32 number, bool rollback, const auto& simulate) {
        if (rollback) {
            Rollback();
        }
        BeginFrame(number);
        simulate();
        EndFrame();
        Settle(number);
    };
    const auto nothing = [] {};

    State saved = std::move(sState);
    Start(false);
    std::string failure;
    const auto check = [&failure](bool condition, const char* what) {
        if (!condition && failure.empty()) {
            failure = what;
        }
    };
    frame(0, false, play);
    frame(1, false, stop);
    check(!is_sound_active(position, kSound), "the stop did not stop the sound");
    frame(1, true, nothing);
    check(is_sound_active(position, kSound) && (sState.NumRestarted == 1),
          "a stop that a rollback found never happened did not start the sound again");
    frame(2, false, stop);
    frame(2, true, stop);
    check(!is_sound_active(position, kSound) && (sState.NumRestarted == 1),
          "a stop made again after a rollback did not keep the sound stopped");
    frame(3, false, play);
    frame(4, false, nothing);
    frame(4, true, stop);
    check(!is_sound_active(position, kSound), "a stop that only the re-simulation made was not carried out");
    frame(5, false, [] { func_800CA984(0); });
    check(func_800C21E8(gPlayers[0].pos, SOUND_ITEM_STAR) != nullptr, "the star did not start");
    frame(6, false, [] { func_800CAACC(0); });
    frame(6, true, nothing);
    check((sState.NumRestarted == 2) && (func_800C21E8(gPlayers[0].pos, SOUND_ITEM_STAR) != nullptr),
          "the star, stopped by a stop that a rollback found never happened, did not start again");
    Stop();
    sState = std::move(saved);

    func_800C5578(position, kSound);
    stop_slot_sounds(gPlayers[0].pos, SOUND_ITEM_STAR, NUM_PLAYERS);
    return failure;
}

std::string GetSummary() {
    char summary[128];
    snprintf(summary, sizeof(summary), "%u sounds, %u played late, %u stopped, %u restarted", sState.NumPlayed,
             sState.NumLate, sState.NumStopped, sState.NumRestarted);
    return summary;
}

} // namespace SoundRollback
