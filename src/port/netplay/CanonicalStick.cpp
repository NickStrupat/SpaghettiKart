#include "CanonicalStick.h"

#include <array>
#include <climits>
#include <map>
#include <tuple>

#include "SimArena.h"

extern "C" {
#include "common_structs.h"
#include "actor_types.h"
#include "player_controller.h"
}

namespace {

constexpr s32 kStickMin = -128;
constexpr s32 kStickValues = 256;
// Stands for "not thrown with the stick's exact Y".
constexpr s32 kNoAimedThrow = INT_MIN;

/** Everything a race reads from a stick position: positions with the same meaning play exactly the same. */
using Meaning = std::tuple<s16, bool, bool, u16, s32, bool, bool>;

Meaning GetMeaning(s8 x, s8 y) {
    Controller controller = {};
    controller.rawStickX = x;
    controller.rawStickY = y;
    // Steering reads its value exactly; backing up compares the Y axis' version with two limits (player_controller.c).
    const s16 steer = func_80038534(&controller);
    const s16 steerY = func_800388B0(&controller);
    // The stick as a D-pad, which menus read (apply_pad_to_controller()).
    u16 direction = 0;
    direction |= (x < -50) ? L_JPAD : 0;
    direction |= (x > 50) ? R_JPAD : 0;
    direction |= (y < -50) ? D_JPAD : 0;
    direction |= (y > 50) ? U_JPAD : 0;
    // Pushed up, a banana bunch's banana is thrown forward as far as the exact Y says (actors_extended.c), and so is a
    // banana held behind (actors/banana), within a narrower X range. Pulled down, a green shell is thrown backwards.
    const bool bunchAimed = (y >= 0x1F) && (x < 0x28) && (x >= -0x27);
    const bool bananaAimed = (y > 30) && (x < 10) && (x >= -9);
    const bool shellBackwards = y < THROW_SHELL_BACKWARDS;
    return { steer, steerY < -0x31, steerY >= -0x1D, direction, bunchAimed ? y : kNoAimedThrow, bananaAimed,
             shellBackwards };
}

struct Position {
    s8 X;
    s8 Y;
};

using Table = std::array<std::array<Position, kStickValues>, kStickValues>;

s32 GetDistanceSquared(Position position) {
    return (s32) position.X * position.X + (s32) position.Y * position.Y;
}

/** For every position, indexed [x + 128][y + 128]: the position nearest the centre with the same meaning. */
Table BuildTable() {
    // Built once, on the first race frame; never the sim arena's.
    SimHeapPause pause;
    std::map<Meaning, Position> nearest;
    for (s32 x = kStickMin; x < kStickMin + kStickValues; x++) {
        for (s32 y = kStickMin; y < kStickMin + kStickValues; y++) {
            const Position position = { (s8) x, (s8) y };
            const auto [entry, added] = nearest.emplace(GetMeaning(position.X, position.Y), position);
            if (!added && (GetDistanceSquared(position) < GetDistanceSquared(entry->second))) {
                entry->second = position;
            }
        }
    }
    Table table;
    for (s32 x = kStickMin; x < kStickMin + kStickValues; x++) {
        for (s32 y = kStickMin; y < kStickMin + kStickValues; y++) {
            table[x - kStickMin][y - kStickMin] = nearest.at(GetMeaning((s8) x, (s8) y));
        }
    }
    return table;
}

} // namespace

namespace CanonicalStick {

void Apply(s8* x, s8* y) {
    static const Table sTable = BuildTable();
    const Position canonical = sTable[*x - kStickMin][*y - kStickMin];
    *x = canonical.X;
    *y = canonical.Y;
}

} // namespace CanonicalStick
