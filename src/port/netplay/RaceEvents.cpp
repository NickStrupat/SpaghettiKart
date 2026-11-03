#include "RaceEvents.h"

#include <array>

#include "NetLog.h"

extern "C" {
#include "defines.h"
#include "main.h"
#include "code_80057C60.h"
#include "code_80005FD0.h"
}

namespace {

struct PlayerState {
    s32 Lap = 0;
    bool Out = false; // Finished the race, or eliminated in Battle
};

struct State {
    std::array<PlayerState, NUM_PLAYERS> Players;
    s32 RaceState = -1;
};

State sState;

const char* GetPlaceSuffix(s32 place) {
    switch (place) {
        case 1:
            return "st";
        case 2:
            return "nd";
        case 3:
            return "rd";
        default:
            return "th";
    }
}

} // namespace

namespace RaceEvents {

void Reset() {
    sState = State();
}

void Update(u32 frame) {
    if (gGamestate != RACING) {
        return;
    }
    for (s32 i = 0; i < gPlayerCountSelection1; i++) {
        const Player& player = gPlayers[i];
        PlayerState& state = sState.Players[i];
        if ((gModeSelection != BATTLE) && (gLapCountByPlayerId[i] > state.Lap) && (gLapCountByPlayerId[i] < 3)) {
            NetLog("Race: player %d started lap %d at frame %u", i + 1, gLapCountByPlayerId[i] + 1, frame);
        }
        state.Lap = gLapCountByPlayerId[i];

        const bool out = (player.type & PLAYER_CINEMATIC_MODE) != 0;
        if (out && !state.Out) {
            if (gModeSelection == BATTLE) {
                NetLog("Race: player %d was eliminated at frame %u", i + 1, frame);
            } else {
                const s32 place = gGPCurrentRaceRankByPlayerId[i] + 1;
                NetLog("Race: player %d finished %d%s at frame %u", i + 1, place, GetPlaceSuffix(place), frame);
            }
        }
        state.Out = out;
    }
    if ((gRaceState != sState.RaceState) && (gRaceState == RACE_FINISHED)) {
        NetLog("Race: the race is over at frame %u", frame);
    }
    sState.RaceState = gRaceState;
}

} // namespace RaceEvents
