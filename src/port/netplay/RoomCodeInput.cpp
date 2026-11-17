#include "RoomCodeInput.h"

#include <cstdlib>

#include "tools/netplay-relay/RelayProtocol.h"

namespace RoomCodeInput {
namespace {

// The stick reads about 80 at full tilt. A flick counts past 70% of that along one axis, while the other axis stays
// under half as far; the next one counts once the stick is back under 30%.
constexpr s32 kFlickThreshold = 56;
constexpr s32 kCenterThreshold = 24;

Entry FromButtons(u16 pressed) {
    const bool up = (pressed & (U_JPAD | U_CBUTTONS)) != 0;
    const bool down = (pressed & (D_JPAD | D_CBUTTONS)) != 0;
    const bool left = (pressed & (L_JPAD | L_CBUTTONS)) != 0;
    const bool right = (pressed & (R_JPAD | R_CBUTTONS)) != 0;
    // Two at once is ambiguous: neither counts.
    if (up + down + left + right != 1) {
        return Entry::None;
    }
    return up ? Entry::Up : (down ? Entry::Down : (left ? Entry::Left : Entry::Right));
}

} // namespace

Entry Reader::Read(s8 stickX, s8 stickY, u16 pressed) {
    if (pressed & B_BUTTON) {
        return Entry::Erase;
    }
    const s32 x = stickX;
    const s32 y = stickY;
    if ((std::abs(x) < kCenterThreshold) && (std::abs(y) < kCenterThreshold)) {
        mStickArmed = true;
    }
    const Entry button = FromButtons(pressed);
    if (button != Entry::None) {
        return button;
    }
    if (!mStickArmed) {
        return Entry::None;
    }
    Entry stick = Entry::None;
    if ((std::abs(x) >= kFlickThreshold) && (std::abs(y) * 2 <= std::abs(x))) {
        stick = (x > 0) ? Entry::Right : Entry::Left;
    } else if ((std::abs(y) >= kFlickThreshold) && (std::abs(x) * 2 <= std::abs(y))) {
        stick = (y > 0) ? Entry::Up : Entry::Down;
    }
    if (stick != Entry::None) {
        mStickArmed = false;
    }
    return stick;
}

void Reader::Reset() {
    mStickArmed = false;
}

char ToLetter(Entry entry) {
    switch (entry) {
        case Entry::Up:
            return 'U';
        case Entry::Down:
            return 'D';
        case Entry::Left:
            return 'L';
        case Entry::Right:
            return 'R';
        default:
            return 0;
    }
}

bool Apply(Entry entry, std::string* code) {
    if (entry == Entry::Erase) {
        if (code->empty()) {
            return false;
        }
        code->pop_back();
        return true;
    }
    const char letter = ToLetter(entry);
    if ((letter == 0) || (code->size() >= RelayProtocol::kCodeLength)) {
        return false;
    }
    *code += letter;
    return true;
}

} // namespace RoomCodeInput
