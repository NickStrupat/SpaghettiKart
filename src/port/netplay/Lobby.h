#ifndef NETPLAY_LOBBY_H
#define NETPLAY_LOBBY_H

#include <libultraship/libultraship.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <gekkonet.h>

#include "NetSession.h"
#include "RaceConfig.h"

extern "C" {
#include "defines.h"
}

/**
 * A netplay room: players gather through the relay, pick characters and get ready; the host picks the race.
 *
 * The host is authoritative. Every member broadcasts its own state (name, character, ready) a few times a second,
 * and the host broadcasts the room's settings plus, once started, the race to run. Because each message carries
 * the full state, a lost packet is simply replaced by the next one; nothing needs acknowledging.
 */
namespace Lobby {

constexpr size_t kMaxMembers = RelayProtocol::kMaxSlots;
constexpr size_t kMaxNameLength = 16;
/**
 * 1 (the default): connect to the other players directly when their routers allow it, which shares this player's
 * address with them. 0: everything goes through the relay. Read when connecting to a room.
 */
constexpr const char* kDirectConnectCVar = "gNetplay.DirectConnect";
/** This player's name, shown to the other players and, for a host, in the room list. */
constexpr const char* kNameCVar = "gNetplay.Name";
/** The relay server, "host[:port]". */
constexpr const char* kRelayCVar = "gNetplay.Relay";
constexpr const char* kDefaultRelay = "127.0.0.1:7777";

enum class State { Offline, Connecting, InRoom };

struct Member {
    bool Present = false;
    bool Known = false; // Its state has arrived
    std::string Name;
    s8 Character = 0;
    bool Ready = false;
    bool InRace = false;     // Runs a race, or a shared console (its RaceId)
    bool Racing = false;     // Is in a race right now, rather than in the menus
    u32 RaceId = 0;          // The race it runs, or ran last
    u32 BarrierSegment = 0;  // The last segment of that race it confirmed the end of
    s32 BarrierFrame = 0;    // The frame it confirmed that segment ended on
    s32 PingMs = -1;         // Round trip to the relay; -1 until measured
};

/** What the host picks. Guests see the host's choice. */
struct Settings {
    s32 Mode = VERSUS;
    s32 CC = CC_150;                            // CC_EXTRA races mirrored; Battle ignores it
    s32 CupIndex = 0;                           // Grand Prix
    std::string TrackName = "mk:luigi_raceway"; // Versus and Battle
};

/**
 * A shared console the host started: every member listed plays on one console, as if each had a controller plugged
 * into it (player k on controller k). The menus run in lockstep and races with rollback (Netplay.cpp).
 */
struct ConsoleStart {
    u32 Id;
    std::vector<u8> Slots; // Relay slot of each player, in player (controller) order
    s32 LocalPlayer;
    RaceConfig Rules;      // The host's gameplay settings, which apply to everyone
    bool ExtraMode;        // Whether the host unlocked Extra (mirrored 150cc), which the menus then offer
};

/** A race this peer has to run, handed to the session code. */
struct RaceStart {
    u32 Id;
    RaceConfig Config;
    s32 LocalPlayer;
    std::vector<u8> Slots; // Relay slot of each player, in player order
};

/**
 * Opens a room on the relay and hosts it.
 * @param sharedConsole true: the room plays on a shared console (ConsoleStart). false: the host picks races, which
 *        start from RaceConfig (StartRace(); the netplay window's old flow and the simulator's bots).
 */
void Host(const std::string& relayAddress, const std::string& name, bool sharedConsole);
void Join(const std::string& relayAddress, const std::string& code, const std::string& name);
void Leave();
/** Talks to the relay and the other members. Call once per frame. */
void Update();

State GetState();
/** Why the last attempt to connect, or the connection, failed. */
const std::string& GetError();
std::string GetRoomCode();
s32 GetLocalSlot();
bool IsHost();
s32 GetPingMs();
/** @returns true while packets to this slot go to it directly, not through the relay. */
bool IsDirect(u8 slot);
/** Round trip straight to this slot, or -1 while not connected directly. */
s32 GetDirectPingMs(u8 slot);
/** Testing: direct connections stop working this long after joining a room, as if a router closed them. */
void BlockDirectAfter(u32 milliseconds);
/**
 * @returns the one-way latency in ms from this peer to the slowest other player of a race, through the relay, or -1
 *          while a ping is not known yet.
 * @param slots The relay slot of each player.
 */
s32 GetLatencyMs(const std::vector<u8>& slots);
const std::array<Member, kMaxMembers>& GetMembers();

const Settings& GetSettings();
/** Host only. */
void SetSettings(const Settings& settings);
void SetName(const std::string& name);
void SetCharacter(s8 character);
void SetReady(bool ready);

/** @returns why the host cannot start a race now, or an empty string when it can. */
std::string GetStartBlocker();
/** Host only: starts a race with everyone in the room. */
void StartRace();
/** The race this peer has been asked to run, if any; taken once. */
std::optional<RaceStart> TakeRaceStart();
/** The newest shared console that includes this peer, once; the host starts a new one when the members change. */
std::optional<ConsoleStart> TakeConsoleStart();
/** @returns true while this peer is in a room that plays on a shared console. */
bool IsSharedConsole();
/** Tells the room whether this peer is racing; the host waits for nobody to be before changing the console. */
void SetRacing(bool racing);
/** @returns the GekkoNet adapter for one segment of a race, carrying packets between the room's members. */
GekkoNetAdapter* GetGekkoAdapter(u32 raceId, u32 segment);
/** Tells the room that this peer confirmed a segment of its race ended on this frame. */
void ReachBarrier(u32 raceId, u32 segment, s32 frame);
/** @returns Passed once every player in the race confirmed the same end, Failed if one left or disagrees. */
BarrierStatus GetBarrierStatus(u32 raceId, u32 segment, s32 frame);
/** Tells the room that this peer's race ended, so that the others end theirs. */
void ReportRaceEnded(u32 raceId, SessionResult result, const std::string& message);

struct RaceEnd {
    u32 Id;
    SessionResult Result;
    std::string Message;
};
/** @returns the end of this peer's race reported by another member, or by losing the room, since the last call. */
std::optional<RaceEnd> TakeRaceEndedElsewhere();
/** How the last race ended, shown in the lobby. */
const std::string& GetLastRaceMessage();
void SetLastRaceMessage(const std::string& message);

} // namespace Lobby

#endif // NETPLAY_LOBBY_H
