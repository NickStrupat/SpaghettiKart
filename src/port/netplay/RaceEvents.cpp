#include "RaceEvents.h"

#include <array>

#include "NetLog.h"

extern "C" {
#include "defines.h"
#include "main.h"
#include "code_80057C60.h"
#include "code_80005FD0.h"
#include "code_800029B0.h"
}

namespace {

struct PlayerState {
    s32 Lap = 0;
    bool Out = false; // Finished the race, or eliminated in Battle
};

struct State {
    std::array<PlayerState, NUM_PLAYERS> Players;
    s32 RaceState = -1;
    u16 Paused = 0; // gIsGamePaused: the player who paused, from 1
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

/**
 * In Grand Prix, how many CPU karts have completed a lap by the end of the race. Every CPU should have: a CPU that
 * never moved (karts 5-8 of a 3 or 4 player race once did) still counts as in the race.
 */
void LogCpusRacing() {
    if (gModeSelection != GRAND_PRIX) {
        return;
    }
    s32 cpus = 0;
    s32 racing = 0;
    for (s32 i = gPlayerCountSelection1; i < NUM_PLAYERS; i++) {
        if ((gPlayers[i].type & PLAYER_EXISTS) && (gPlayers[i].type & PLAYER_CPU)) {
            cpus++;
            if (gLapCountByPlayerId[i] >= 1) {
                racing++;
            } else {
                const Player& cpu = gPlayers[i];
                NetLog("Race: CPU in kart %d (character %d) never completed a lap: rank %d, at x %.0f z %.0f, "
                       "speed %.2f, effects 0x%x",
                       (int) i + 1, (int) cpu.characterId, (int) cpu.currentRank + 1, cpu.pos[0], cpu.pos[2],
                       cpu.speed, (unsigned) cpu.effects);
            }
        }
    }
    NetLog("Race: %d of %d CPUs completed a lap", racing, cpus);
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
    if ((gIsGamePaused != 0) && (sState.Paused == 0)) {
        NetLog("Race: player %d paused at frame %u", gIsGamePaused, frame);
    } else if ((gIsGamePaused == 0) && (sState.Paused != 0)) {
        NetLog("Race: player %d went on at frame %u", sState.Paused, frame);
    }
    sState.Paused = gIsGamePaused;
    if ((gRaceState != sState.RaceState) && (gRaceState == RACE_FINISHED)) {
        NetLog("Race: the race is over at frame %u", frame);
        LogCpusRacing();
    }
    sState.RaceState = gRaceState;
}

} // namespace RaceEvents
