#ifndef NETPLAY_RACE_CONFIG_H
#define NETPLAY_RACE_CONFIG_H

#include <libultraship/libultraship.h>

#ifdef __cplusplus

#include <array>
#include <string>

#include "tools/netplay-relay/RelayProtocol.h"

constexpr size_t kNumGameplayIntCVars = 20;
constexpr size_t kNumGameplayFloatCVars = 2;

/**
 * Everything that must be identical on every peer for a race to simulate identically.
 * Netplay sends this from the host; the determinism harness stores it in its log file.
 */
struct RaceConfig {
    std::string TrackName = "mk:luigi_raceway"; // TrackInfo resource name. Ignored for Grand Prix, which uses the cup.
    s32 ModeSelection = 0;                      // GRAND_PRIX, VERSUS or BATTLE
    s32 CCSelection = 2;                        // CC_50 .. CC_EXTRA
    s32 MirrorMode = 0;
    s8 PlayerCount = 1;                         // Human players
    std::array<s8, 4> Characters = { 0, 1, 2, 3 };
    s32 CupIndex = 0;                           // Grand Prix only
    s8 CourseIndexInCup = 0;                    // Grand Prix only
    u16 RandomSeed = 0;

    // Gameplay CVars that change simulation results, in the order listed in RaceConfig.cpp.
    std::array<s32, kNumGameplayIntCVars> IntCVars = {};
    std::array<f32, kNumGameplayFloatCVars> FloatCVars = {};

    /** A config with every gameplay CVar at its default value. */
    static RaceConfig MakeDefault();

    /** A default config with the player's own gameplay CVar values, which a netplay host shares with its guests. */
    static RaceConfig FromLocalCVars();

    /**
     * A default config adjusted by environment variables, for automated test runs:
     * <prefix>MODE=gp|vs|battle, <prefix>TRACK=<resource name>, <prefix>SEED=<number>.
     */
    static RaceConfig FromEnvironment(const std::string& prefix, s8 playerCount);

    /** Screen mode the simulation must run in for this many human players. */
    s32 GetScreenMode() const;

    void Write(RelayProtocol::ByteWriter& writer) const;
    /** @returns false if the data is cut short or names a mode, track or character this game does not have. */
    bool Read(RelayProtocol::ByteReader& reader);
};

/**
 * Forces the race's gameplay CVars for the duration of a session and restores the player's own values afterwards.
 * Overridden values are never written to the config file as long as Restore() runs before the engine shuts down.
 */
class GameplayCVarOverride {
public:
    void Apply(const RaceConfig& config);
    void Restore();
    bool IsActive() const { return mActive; }

private:
    struct SavedCVar {
        bool Existed = false;
        s32 IntValue = 0;
        f32 FloatValue = 0.0f;
    };

    bool mActive = false;
    u16 mSavedDebugMode = 0;
    std::array<SavedCVar, kNumGameplayIntCVars> mSavedInts = {};
    std::array<SavedCVar, kNumGameplayFloatCVars> mSavedFloats = {};
};

/**
 * Applies the config to the game's race globals and starts loading the race on the next frame.
 * The track is always reloaded so no state carries over from a previous race.
 * Call after the frame's game logic: the menus overwrite the track selection while they run.
 */
void RaceConfig_StartRace(const RaceConfig& config);

#endif // __cplusplus

#endif // NETPLAY_RACE_CONFIG_H
