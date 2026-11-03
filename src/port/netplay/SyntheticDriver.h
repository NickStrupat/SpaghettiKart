#ifndef NETPLAY_SYNTHETIC_DRIVER_H
#define NETPLAY_SYNTHETIC_DRIVER_H

#include <libultraship/libultraship.h>

extern "C" {
#include "defines.h"
}

/** One frame of a player's controller input, as stored in harness logs and sent between netplay peers. */
struct PadInput {
    s8 StickX = 0;
    s8 StickY = 0;
    u16 Buttons = 0;
};

/**
 * Seeded stand-in for a human driver, for unattended tests. Its inputs depend only on the seed and the race state, so
 * a recorded run replays exactly.
 */
class SyntheticDriver {
public:
    enum class Style {
        /** Races: follows the track like the CPU drivers, fires items, chases opponents in Battle and confirms
         *  the screens after the race, so races, cups and battles reach their end. */
        Racer,
        /** Holds the throttle and steers, hops and fires at random: exercises walls, collisions and CPU reactions. */
        Random,
    };

    explicit SyntheticDriver(u32 seed, Style style = Style::Racer) : mState(seed != 0 ? seed : 1), mStyle(style) {
    }

    /** @param playerIndex The player this driver controls; a racer steers from its position. */
    PadInput Next(s32 playerIndex);

private:
    PadInput NextRandom();
    PadInput NextRacer(s32 playerIndex);
    /** Reverses out of a wall the player has been stuck against. @returns true while doing so. */
    bool Unstick(s32 playerIndex, PadInput* pad);
    /** Presses Z a short, random while after an item arrives. */
    void UseItems(s32 playerIndex, PadInput* pad);

    // xorshift32
    u32 NextRandomNumber() {
        mState ^= mState << 13;
        mState ^= mState >> 17;
        mState ^= mState << 5;
        return mState;
    }

    u32 mState;
    Style mStyle;
    u32 mFrame = 0;

    // Random style
    s8 mStickX = 0;
    u32 mSteerFramesLeft = 0;
    u32 mHopFramesLeft = 0;
    u32 mBrakeFramesLeft = 0;

    // Racer style
    u32 mSlowFrames = 0;
    u32 mReverseFramesLeft = 0;
    s8 mReverseStickX = 0;
    u32 mItemDelay = 0;
    s32 mDetourBox = -1;
    u32 mDetourFramesLeft = 0;
    f32 mProgressMark = 0.0f;
    u32 mNoProgressFrames = 0;
};

#endif // NETPLAY_SYNTHETIC_DRIVER_H
