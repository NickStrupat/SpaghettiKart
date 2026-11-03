#include "SyntheticDriver.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "port/Game.h"

extern "C" {
#include "main.h"
#include "waypoints.h"
#include "racing/math_util.h"
#include "actor_types.h"
#include "code_80005FD0.h"
}

namespace {

// Steers toward the track point this far ahead of the nearest one.
constexpr u16 kPathLookAhead = 8;
// Full lock for this much heading error: about 30 degrees.
constexpr s32 kFullLockAngle = 0x1555;
constexpr s32 kMaxStick = 80;
// Below this speed for kStuckFrames, a racer backs out: it is against a wall.
constexpr f32 kStuckSpeed = 1.0f;
constexpr u32 kStuckFrames = 45;
constexpr u32 kReverseFrames = 40;
// A racer that moved less than this share of a lap along the track in kNoProgressFrames is wedged or circling
// without being slow: it backs out for longer.
constexpr f32 kMinProgress = 0.01f;
constexpr u32 kNoProgressFrames = 300;
// After the race, A is pressed once in this many frames: menus react to a press, not a held button.
constexpr u32 kConfirmInterval = 20;
// In Battle, an item is fired at an opponent this close and about ahead.
constexpr f32 kFireDistance = 1200.0f;
constexpr s32 kFireAngle = 0x1000;
// An item box can be collected in this state (update_actor_item_box()).
constexpr s16 kItemBoxReady = 2;
// In Battle, a racer that hit a wall heads for a random item box this long: arenas are mazes, and the nearest box
// is often behind the wall.
constexpr u32 kDetourFrames = 300;

s8 SteerToward(Player* player, Vec3f target) {
    const s16 heading = (s16) -get_angle_between_two_vectors(player->pos, target);
    const s32 error = (s16) (heading - player->rotation[1]);
    const s32 stick = std::clamp(error * kMaxStick / kFullLockAngle, -kMaxStick, kMaxStick);
    return (s8) stick;
}

/** @returns the point kPathLookAhead points ahead of the player on its path. */
void GetPathTarget(s32 playerIndex, Vec3f target) {
    const u16 path = gPathIndexByPlayerId[playerIndex];
    const u16 count = gPathCountByPathIndex[path];
    const TrackPathPoint* point = &gTrackPaths[path][(gNearestPathPointByPlayerId[playerIndex] + kPathLookAhead) % count];
    target[0] = point->x;
    target[1] = point->y;
    target[2] = point->z;
}

f32 GetDistanceSquared(const Player* a, const Player* b) {
    const f32 x = a->pos[0] - b->pos[0];
    const f32 z = a->pos[2] - b->pos[2];
    return (x * x) + (z * z);
}

/** @returns the nearest opponent still in the battle, or -1. */
s32 GetNearestOpponent(s32 playerIndex) {
    s32 nearest = -1;
    for (s32 i = 0; i < gPlayerCountSelection1; i++) {
        const Player* other = &gPlayers[i];
        if ((i == playerIndex) || !(other->type & PLAYER_EXISTS) || (other->type & PLAYER_CINEMATIC_MODE)) {
            continue;
        }
        if ((nearest < 0) || (GetDistanceSquared(&gPlayers[playerIndex], other) <
                              GetDistanceSquared(&gPlayers[playerIndex], &gPlayers[nearest]))) {
            nearest = i;
        }
    }
    return nearest;
}

bool IsItemBoxReady(size_t index) {
    if (index >= CM_GetActorSize()) {
        return false;
    }
    const struct Actor* actor = CM_GetActor(index);
    return (actor->type == ACTOR_ITEM_BOX) && (actor->state == kItemBoxReady);
}

/** @returns the index of the nearest item box that can be collected, or -1. */
s32 GetNearestItemBox(const Player* player) {
    s32 nearest = -1;
    f32 nearestDistance = 0.0f;
    for (size_t i = 0; i < CM_GetActorSize(); i++) {
        if (!IsItemBoxReady(i)) {
            continue;
        }
        const struct Actor* actor = CM_GetActor(i);
        const f32 x = actor->pos[0] - player->pos[0];
        const f32 z = actor->pos[2] - player->pos[2];
        const f32 distance = (x * x) + (z * z);
        if ((nearest < 0) || (distance < nearestDistance)) {
            nearest = (s32) i;
            nearestDistance = distance;
        }
    }
    return nearest;
}

/** @returns the index of the choice-th item box that can be collected, counting round, or -1 when none can. */
s32 GetItemBox(u32 choice) {
    std::vector<s32> boxes;
    for (size_t i = 0; i < CM_GetActorSize(); i++) {
        if (IsItemBoxReady(i)) {
            boxes.push_back((s32) i);
        }
    }
    return boxes.empty() ? -1 : boxes[choice % boxes.size()];
}

} // namespace

PadInput SyntheticDriver::Next(s32 playerIndex) {
    mFrame++;
    return (mStyle == Style::Racer) ? NextRacer(playerIndex) : NextRandom();
}

PadInput SyntheticDriver::NextRandom() {
    PadInput pad;
    pad.Buttons = A_BUTTON;

    if (mSteerFramesLeft == 0) {
        mStickX = (s8) ((s32) (NextRandomNumber() % 161) - 80);
        mSteerFramesLeft = 15 + (NextRandomNumber() % 30);
    }
    mSteerFramesLeft--;
    pad.StickX = mStickX;

    if ((mHopFramesLeft == 0) && ((NextRandomNumber() % 120) == 0)) {
        mHopFramesLeft = 10 + (NextRandomNumber() % 20);
    }
    if (mHopFramesLeft > 0) {
        mHopFramesLeft--;
        pad.Buttons |= R_TRIG;
    }

    if ((mBrakeFramesLeft == 0) && ((NextRandomNumber() % 300) == 0)) {
        mBrakeFramesLeft = 20;
    }
    if (mBrakeFramesLeft > 0) {
        mBrakeFramesLeft--;
        pad.Buttons |= B_BUTTON;
    }

    if ((NextRandomNumber() % 60) == 0) {
        pad.Buttons |= Z_TRIG;
    }
    return pad;
}

PadInput SyntheticDriver::NextRacer(s32 playerIndex) {
    PadInput pad;
    Player* player = &gPlayers[playerIndex];

    // Results, prompts and the podium: confirm, as a player would. A finished racer drives itself.
    if ((gRaceState >= RACE_FINISHED) || (player->type & PLAYER_CINEMATIC_MODE)) {
        if ((mFrame % kConfirmInterval) == 0) {
            pad.Buttons = A_BUTTON;
        }
        return pad;
    }

    pad.Buttons = A_BUTTON;
    if (Unstick(playerIndex, &pad)) {
        return pad;
    }

    Vec3f target;
    if (gModeSelection != BATTLE) {
        GetPathTarget(playerIndex, target);
        pad.StickX = SteerToward(player, target);
        UseItems(playerIndex, &pad);
        return pad;
    }

    // Battle: collect an item, then hunt the nearest opponent with it.
    if (mDetourFramesLeft > 0) {
        mDetourFramesLeft--;
        if (!IsItemBoxReady((size_t) mDetourBox)) {
            mDetourFramesLeft = 0;
        }
    }
    const s32 opponent = GetNearestOpponent(playerIndex);
    if (mDetourFramesLeft > 0) {
        const struct Actor* box = CM_GetActor((size_t) mDetourBox);
        target[0] = box->pos[0];
        target[1] = box->pos[1];
        target[2] = box->pos[2];
        pad.StickX = SteerToward(player, target);
    } else if ((player->currentItemCopy != 0) && (opponent >= 0)) {
        target[0] = gPlayers[opponent].pos[0];
        target[1] = gPlayers[opponent].pos[1];
        target[2] = gPlayers[opponent].pos[2];
        pad.StickX = SteerToward(player, target);
        const bool ahead = std::abs((s32) pad.StickX) * kFullLockAngle / kMaxStick < kFireAngle;
        if (ahead && (GetDistanceSquared(player, &gPlayers[opponent]) < kFireDistance * kFireDistance)) {
            mItemDelay = 1;
        }
    } else if (const s32 box = GetNearestItemBox(player); box >= 0) {
        const struct Actor* actor = CM_GetActor((size_t) box);
        target[0] = actor->pos[0];
        target[1] = actor->pos[1];
        target[2] = actor->pos[2];
        pad.StickX = SteerToward(player, target);
    } else {
        pad.StickX = (s8) ((mFrame / 60) % 2 == 0 ? 40 : -40);
    }
    UseItems(playerIndex, &pad);
    return pad;
}

bool SyntheticDriver::Unstick(s32 playerIndex, PadInput* pad) {
    const Player* player = &gPlayers[playerIndex];
    if (mReverseFramesLeft > 0) {
        mReverseFramesLeft--;
        pad->Buttons = B_BUTTON;
        pad->StickX = mReverseStickX;
        return true;
    }
    const bool racing = gRaceState == RACE_IN_PROGRESS;
    const bool slow = racing && (player->speed < kStuckSpeed);
    mSlowFrames = slow ? (mSlowFrames + 1) : 0;

    bool noProgress = false;
    if ((gModeSelection != BATTLE) && racing) {
        const f32 progress = gCourseCompletionPercentByPlayerId[playerIndex];
        if ((mNoProgressFrames == 0) || (progress > mProgressMark + kMinProgress)) {
            mProgressMark = progress;
            mNoProgressFrames = 0;
        }
        noProgress = ++mNoProgressFrames >= kNoProgressFrames;
    } else {
        mNoProgressFrames = 0;
    }

    if ((mSlowFrames >= kStuckFrames) || noProgress) {
        mSlowFrames = 0;
        mNoProgressFrames = 0;
        mReverseFramesLeft = noProgress ? (kReverseFrames * 2) : kReverseFrames;
        mReverseStickX = (NextRandomNumber() % 2 == 0) ? kMaxStick : -kMaxStick;
        if (gModeSelection == BATTLE) {
            mDetourBox = GetItemBox(NextRandomNumber());
            mDetourFramesLeft = (mDetourBox >= 0) ? kDetourFrames : 0;
        }
    }
    return false;
}

void SyntheticDriver::UseItems(s32 playerIndex, PadInput* pad) {
    if (gPlayers[playerIndex].currentItemCopy == 0) {
        mItemDelay = 0;
        return;
    }
    if (mItemDelay == 0) {
        mItemDelay = 30 + (NextRandomNumber() % 90);
    }
    // Z is pressed on a single frame, so a held item (a banana trailing behind) is dropped too.
    if (--mItemDelay == 0) {
        pad->Buttons |= Z_TRIG;
    }
}
