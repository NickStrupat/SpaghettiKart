#include "KartSmoothing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

extern "C" {
#include "defines.h"
#include "main.h"
#include "menus.h"
}

namespace {

// Each frame an offset keeps this share of itself, so a correction fades over about 150 ms (5 frames at 30 fps).
constexpr f32 kKeptPerFrame = 0.6f;
// Offsets shorter than this end.
constexpr f32 kMinDistance = 0.05f;
// Four frames at top speed (9 units a frame). Rollbacks between bots move karts 4.6 units at most, 0.34 in 99 cases of
// 100; an offset beyond this is a jump the race made, such as a kart put back on the track, and is shown as one.
constexpr f32 kMaxDistance = 36.0f;

struct State {
    // Where each kart was on the frame shown before the update
    std::array<std::array<f32, 3>, NUM_PLAYERS> ShownPos = {};
    std::array<s16, NUM_PLAYERS> ShownYaw = {};
    // How far each kart is drawn from where it is
    std::array<std::array<f32, 3>, NUM_PLAYERS> Offset = {};
    std::array<f32, NUM_PLAYERS> YawOffset = {};
    u32 NumSmoothed = 0;
    u32 NumTooFar = 0;
    f32 Largest = 0.0f;
};

State sState;

bool Exists(s32 kart) {
    return (gPlayers[kart].type & PLAYER_EXISTS) != 0;
}

f32 GetLength(const std::array<f32, 3>& v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void ClearOffset(s32 kart) {
    sState.Offset[kart] = {};
    sState.YawOffset[kart] = 0.0f;
}

/** @returns true for a kart that a camera on screen follows. */
bool IsFollowed(s32 kart, s32 viewPlayer) {
    return (viewPlayer >= 0) ? (kart == viewPlayer) : (kart < gPlayerCount);
}

} // namespace

namespace KartSmoothing {

void Reset() {
    sState = State{};
}

void BeginUpdate() {
    for (s32 kart = 0; kart < NUM_PLAYERS; kart++) {
        for (s32 axis = 0; axis < 3; axis++) {
            sState.ShownPos[kart][axis] = gPlayers[kart].pos[axis];
        }
        sState.ShownYaw[kart] = gPlayers[kart].rotation[1];
    }
}

void EndRollback() {
    for (s32 kart = 0; kart < NUM_PLAYERS; kart++) {
        if (!Exists(kart)) {
            continue;
        }
        std::array<f32, 3> moved;
        for (s32 axis = 0; axis < 3; axis++) {
            moved[axis] = sState.ShownPos[kart][axis] - gPlayers[kart].pos[axis];
        }
        const f32 distance = GetLength(moved);
        const s16 turned = (s16) (sState.ShownYaw[kart] - gPlayers[kart].rotation[1]);
        if ((distance < kMinDistance) && (turned == 0)) {
            continue;
        }
        for (s32 axis = 0; axis < 3; axis++) {
            sState.Offset[kart][axis] += moved[axis];
        }
        sState.YawOffset[kart] += turned;
        if (GetLength(sState.Offset[kart]) > kMaxDistance) {
            ClearOffset(kart);
            sState.NumTooFar++;
            continue;
        }
        sState.NumSmoothed++;
        sState.Largest = std::max(sState.Largest, distance);
    }
}

void Apply(s32 viewPlayer) {
    if (CVarGetInteger(kCVar, 1) == 0) {
        for (s32 kart = 0; kart < NUM_PLAYERS; kart++) {
            ClearOffset(kart);
        }
        return;
    }
    for (s32 kart = 0; kart < NUM_PLAYERS; kart++) {
        if (Exists(kart) && !IsFollowed(kart, viewPlayer)) {
            for (s32 axis = 0; axis < 3; axis++) {
                gPlayers[kart].pos[axis] += sState.Offset[kart][axis];
            }
            gPlayers[kart].rotation[1] += (s16) lroundf(sState.YawOffset[kart]);
        }
        for (s32 axis = 0; axis < 3; axis++) {
            sState.Offset[kart][axis] *= kKeptPerFrame;
        }
        sState.YawOffset[kart] *= kKeptPerFrame;
        if ((GetLength(sState.Offset[kart]) < kMinDistance) && (fabsf(sState.YawOffset[kart]) < 1.0f)) {
            ClearOffset(kart);
        }
    }
}

std::string GetSummary() {
    char summary[96];
    snprintf(summary, sizeof(summary), "%u kart corrections smoothed (largest %.1f), %u too far", sState.NumSmoothed,
             sState.Largest, sState.NumTooFar);
    return summary;
}

} // namespace KartSmoothing
