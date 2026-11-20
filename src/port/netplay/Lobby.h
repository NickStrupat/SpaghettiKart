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
 * A netplay room: players gather through the relay, pick characters and get ready; the host picks the race. They get
 * in by hosting a public or private room, by joining one with its code, or by matchmaking, which can run while the
 * player plays offline: a member that is still in an offline race when matched is away, and the room waits for it.
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
/** The most players one computer brings to a room: one per controller port. */
constexpr s32 kMaxLocalPlayers = 4;
/**
 * Tests: this many players on this computer, on ports 1 to N, as test runs have no gamepads to count. Without it the
 * players are counted from the controllers (GetLocalPorts).
 */
constexpr const char* kTestLocalPlayersEnv = "SK_TEST_LOCAL_PLAYERS";
/** The relay server, "host[:port]". */
constexpr const char* kRelayCVar = "gNetplay.Relay";
constexpr const char* kDefaultRelay = "netkart64.duckdns.org:7777";

enum class State { Offline, Connecting, Matchmaking, InRoom };

enum class Visibility {
    Public,  // In the room list, and filled by matchmaking
    Private, // Joined only by its code
};

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
    s32 GekkoWaitMs = -1;    // How long GekkoNet packets wait for it to read them (RelayClient::GetGekkoWaitMs())
    bool Voice = false;      // Uses voice chat (VoiceChat.h): only such members are sent voice
    bool Away = false;       // Matched while playing offline: not in the menus yet, so no console can start
    u8 LocalPlayers = 1;     // Players on its computer: it holds this many places on a shared console
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
    std::vector<u8> Slots; // Relay slot of each player, in player (controller) order; a computer with several
                           // players appears once for each
    s32 LocalPlayer;       // This peer's first player
    std::vector<s32> LocalPlayers; // All of this peer's players, in order
    std::vector<s32> LocalPorts;   // The controller port (0-based) that drives each of LocalPlayers
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
void Host(const std::string& relayAddress, const std::string& name, bool sharedConsole, Visibility visibility);
void Join(const std::string& relayAddress, const std::string& code, const std::string& name);
/**
 * Looks for other players (State::Matchmaking) until the relay puts this peer in a room with a shared console: a public
 * room with a free slot, or a new one with another player looking, hosted by whoever waited longest. A room that goes
 * away before its first console started (its host left) sends this peer back to looking.
 */
void Matchmake(const std::string& relayAddress, const std::string& name);
/** Leaves the room, or stops matchmaking. */
void Leave();
/** Talks to the relay and the other members. Call once per frame. */
void Update();

State GetState();
/** Why the last attempt to connect, or the connection, failed. */
const std::string& GetError();
std::string GetRoomCode();
/** @returns true while in a private room, whose code is the only way in. */
bool IsRoomPrivate();
/** While matchmaking: how many players of this build are looking, this one included; 0 until the relay says. */
u32 GetWaitingPlayers();
/** @returns true once, after matchmaking put this peer in a room. */
bool TakeMatchFound();
s32 GetLocalSlot();

/**
 * @returns the controller ports (0-based) whose players play on this computer, in order: port 1 always, then each port
 * a gamepad is set up to play on (GamepadAssignment::GetExtraPlayers). A change counts once it has lasted a moment,
 * so a gamepad that drops out briefly doesn't start the room's console over.
 */
const std::vector<s32>& GetLocalPorts();
bool IsHost();
s32 GetPingMs();
/** @returns true while packets to this slot go to it directly, not through the relay. */
bool IsDirect(u8 slot);
/** Round trip straight to this slot, or -1 while not connected directly. */
s32 GetDirectPingMs(u8 slot);
/** Testing: direct connections stop working this long after joining a room, as if a router closed them. */
void BlockDirectAfter(u32 milliseconds);
/**
 * @returns how much of GekkoNet's round trip to this slot is packets waiting at either end for a frame to read them,
 *          in ms, rather than time on the network; -1 while not known.
 */
s32 GetGekkoWaitMs(u8 slot);
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
/** Tells the room whether this peer is away (Member::Away); the host starts no console while anyone is. */
void SetAway(bool away);
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

/** Voice from another member (VoiceChat.h). */
struct VoiceData {
    u8 Source; // Sender's relay slot
    std::vector<u8> Data;
};
/** Tells the room whether this peer uses voice chat, so that the others send it theirs. */
void SetVoice(bool voice);
/**
 * Sends voice to one slot, only while it is reached directly: voice never goes through the relay.
 * @returns whether it was sent.
 */
bool SendVoice(u8 slot, const void* data, size_t size);
/** The voice that arrived since the last call. */
std::vector<VoiceData> TakeVoice();

/** A message for a race's session from another member, such as part of a state for desync recovery (Resync.h). */
struct RaceData {
    u8 Source; // Sender's relay slot
    std::vector<u8> Data;
};
/**
 * Sends a message to the session of a race this peer runs, on one relay slot, or on every other member with
 * RelayProtocol::kBroadcast. At most RelayProtocol::kMaxPayload - 5 bytes; nothing is acknowledged or resent.
 */
void SendRaceData(u32 raceId, u8 destination, const std::vector<u8>& data);
/** The messages for this race's session that arrived since the last call. */
std::vector<RaceData> TakeRaceData(u32 raceId);
/** @returns the end of this peer's race reported by another member, or by losing the room, since the last call. */
std::optional<RaceEnd> TakeRaceEndedElsewhere();
/** How the last race ended, shown in the lobby. */
const std::string& GetLastRaceMessage();
void SetLastRaceMessage(const std::string& message);

} // namespace Lobby

#endif // NETPLAY_LOBBY_H
