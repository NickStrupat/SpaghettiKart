#include "RaceConfig.h"

#include <cstdlib>
#include <cstring>

#include "port/Game.h"
#include "engine/TrackBrowser.h"

extern "C" {
#include "main.h"
#include "buffers.h"
#include "menus.h"
#include "code_800029B0.h"
#include "defines.h"
}

namespace {

struct IntCVar {
    const char* Name;
    s32 Default;
    bool AlwaysOff = false; // Local-only tools that take over player input
};

struct FloatCVar {
    const char* Name;
    f32 Default;
};

// CVars read by simulation code. Render-only CVars (gNoCulling, gDisableLod, ...) stay under player control.
constexpr std::array<IntCVar, kNumGameplayIntCVars> kGameplayIntCVars = { {
    { "gHarderCPU", 0 },
    { "gShellsShootStraight", 0 },
    { "gNoWallColision", 0 },
    { "gMultiplayerNoFeatureCuts", 0 },
    { "gEnableCustomCC", 0 },
    { "gNumTrucks", 7 },
    { "gNumTrains", 2 },
    { "gNumTankerTrucks", 7 },
    { "gNumCars", 7 },
    { "gNumCarriages", 5 },
    { "gNumBuses", 7 },
    { "gHasTender", 1 },
    { "gEnableMoonJump", 0 },
    { "gGoFish", 0 },
    { "gLookBehind", 0 },
    { "gFreecam", 0, true },
    { "gIsEditorEnabled", 0, true },
} };

constexpr std::array<FloatCVar, kNumGameplayFloatCVars> kGameplayFloatCVars = { {
    { "gMinHeight", 0.0f },
    { "gCustomCC", 150.0f },
} };

const char* GetEnv(const std::string& name) {
    const char* value = getenv(name.c_str());
    return ((value != nullptr) && (value[0] != '\0')) ? value : nullptr;
}

s32 ParseMode(const char* mode) {
    if (mode == nullptr) {
        return GRAND_PRIX;
    }
    if (strcmp(mode, "vs") == 0) {
        return VERSUS;
    }
    if (strcmp(mode, "battle") == 0) {
        return BATTLE;
    }
    return GRAND_PRIX;
}

// libultraship declares CVarExists() but never defines it.
bool IsCVarSet(const char* name) {
    return Ship::Context::GetRawInstance()->GetConsoleVariables()->Get(name) != nullptr;
}

} // namespace

RaceConfig RaceConfig::MakeDefault() {
    RaceConfig config;
    for (size_t i = 0; i < kGameplayIntCVars.size(); i++) {
        config.IntCVars[i] = kGameplayIntCVars[i].Default;
    }
    for (size_t i = 0; i < kGameplayFloatCVars.size(); i++) {
        config.FloatCVars[i] = kGameplayFloatCVars[i].Default;
    }
    return config;
}

RaceConfig RaceConfig::FromEnvironment(const std::string& prefix, s8 playerCount) {
    RaceConfig config = MakeDefault();
    config.ModeSelection = ParseMode(GetEnv(prefix + "MODE"));
    config.PlayerCount = playerCount;
    if (config.ModeSelection == BATTLE) {
        config.TrackName = "mk:block_fort";
    }
    if (const char* track = GetEnv(prefix + "TRACK")) {
        config.TrackName = track;
    }
    const char* seed = GetEnv(prefix + "SEED");
    config.RandomSeed = (u16) ((seed != nullptr) ? atoi(seed) : 1);
    return config;
}

s32 RaceConfig::GetScreenMode() const {
    switch (PlayerCount) {
        case 1:
            return SCREEN_MODE_1P;
        case 2:
            return SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL;
        default:
            return SCREEN_MODE_3P_4P_SPLITSCREEN;
    }
}

void GameplayCVarOverride::Apply(const RaceConfig& config) {
    if (!mActive) {
        for (size_t i = 0; i < kGameplayIntCVars.size(); i++) {
            const char* name = kGameplayIntCVars[i].Name;
            mSavedInts[i] = { IsCVarSet(name), CVarGetInteger(name, kGameplayIntCVars[i].Default), 0.0f };
        }
        for (size_t i = 0; i < kGameplayFloatCVars.size(); i++) {
            const char* name = kGameplayFloatCVars[i].Name;
            mSavedFloats[i] = { IsCVarSet(name), 0, CVarGetFloat(name, kGameplayFloatCVars[i].Default) };
        }
        mSavedDebugMode = gEnableDebugMode;
        mActive = true;
    }

    for (size_t i = 0; i < kGameplayIntCVars.size(); i++) {
        const IntCVar& cvar = kGameplayIntCVars[i];
        CVarSetInteger(cvar.Name, cvar.AlwaysOff ? 0 : config.IntCVars[i]);
    }
    for (size_t i = 0; i < kGameplayFloatCVars.size(); i++) {
        CVarSetFloat(kGameplayFloatCVars[i].Name, config.FloatCVars[i]);
    }
    // Debug mode adds lap-skip and race-start shortcuts to the race logic.
    gEnableDebugMode = 0;
}

void GameplayCVarOverride::Restore() {
    if (!mActive) {
        return;
    }
    for (size_t i = 0; i < kGameplayIntCVars.size(); i++) {
        const char* name = kGameplayIntCVars[i].Name;
        if (mSavedInts[i].Existed) {
            CVarSetInteger(name, mSavedInts[i].IntValue);
        } else {
            CVarClear(name);
        }
    }
    for (size_t i = 0; i < kGameplayFloatCVars.size(); i++) {
        const char* name = kGameplayFloatCVars[i].Name;
        if (mSavedFloats[i].Existed) {
            CVarSetFloat(name, mSavedFloats[i].FloatValue);
        } else {
            CVarClear(name);
        }
    }
    gEnableDebugMode = mSavedDebugMode;
    mActive = false;
}

void RaceConfig_StartRace(const RaceConfig& config) {
    // Mirrors setup_game_mode_selected() in menus.c
    gModeSelection = config.ModeSelection;
    gCCSelection = config.CCSelection;
    set_mirror_mode(config.MirrorMode);
    gPlaceItemBoxes = 1;
    gPlayerCount = config.PlayerCount;
    gScreenModeSelection = config.GetScreenMode();
    for (size_t i = 0; i < config.Characters.size(); i++) {
        gCharacterSelections[i] = config.Characters[i];
    }

    if (config.ModeSelection == GRAND_PRIX) {
        CM_SetCupIndex(config.CupIndex);
        gCupSelection = (s8) config.CupIndex;
        gCourseIndexInCup = config.CourseIndexInCup;
        SetCupCursorPosition(config.CourseIndexInCup);
        // setup_race() skips this when gMenuSelection is START_MENU
        TrackBrowser_SetTrackFromCup();
    } else {
        TrackBrowser_SetTrack(config.TrackName.c_str());
    }

    gDemoMode = 0;
    gRandomSeed16 = config.RandomSeed;
    gCurrentlyLoadedTrackAddr = 0; // Force a fresh track load
    gGamestateNext = RACING;
}
