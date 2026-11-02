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
 * Seeded stand-in for a human driver: holds the throttle and randomly steers, hops and fires items,
 * so a run exercises physics, collisions, items and CPU reactions.
 */
class SyntheticDriver {
public:
    explicit SyntheticDriver(u32 seed) : mState(seed != 0 ? seed : 1) {
    }

    PadInput Next() {
        PadInput pad;
        pad.Buttons = A_BUTTON;

        if (mSteerFramesLeft == 0) {
            mStickX = (s8) ((s32) (NextRandom() % 161) - 80);
            mSteerFramesLeft = 15 + (NextRandom() % 30);
        }
        mSteerFramesLeft--;
        pad.StickX = mStickX;

        if ((mHopFramesLeft == 0) && ((NextRandom() % 120) == 0)) {
            mHopFramesLeft = 10 + (NextRandom() % 20);
        }
        if (mHopFramesLeft > 0) {
            mHopFramesLeft--;
            pad.Buttons |= R_TRIG;
        }

        if ((mBrakeFramesLeft == 0) && ((NextRandom() % 300) == 0)) {
            mBrakeFramesLeft = 20;
        }
        if (mBrakeFramesLeft > 0) {
            mBrakeFramesLeft--;
            pad.Buttons |= B_BUTTON;
        }

        if ((NextRandom() % 60) == 0) {
            pad.Buttons |= Z_TRIG;
        }
        return pad;
    }

private:
    // xorshift32
    u32 NextRandom() {
        mState ^= mState << 13;
        mState ^= mState >> 17;
        mState ^= mState << 5;
        return mState;
    }

    u32 mState;
    s8 mStickX = 0;
    u32 mSteerFramesLeft = 0;
    u32 mHopFramesLeft = 0;
    u32 mBrakeFramesLeft = 0;
};

#endif // NETPLAY_SYNTHETIC_DRIVER_H
