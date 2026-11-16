#ifndef NETPLAY_VOICE_CHAT_H
#define NETPLAY_VOICE_CHAT_H

#include <libultraship/libultraship.h>

/**
 * Voice chat among the members of a netplay room: an open microphone, heard in the menus and in races alike.
 *
 * Voice only ever travels straight between two members (RelayClient::SendVoice()), never through the relay, which is
 * a small server: two members whose routers do not let them connect directly do not hear each other. The microphone
 * opens once another member who uses voice chat is reached directly, and closes again when none is left.
 *
 * Each game frame, on the game thread: what the microphone recorded (VoiceDevice.h) is packed by the Opus codec into
 * 20 ms frames and sent to every such member; what arrived from each member is unpacked, mixed and queued to play.
 * Silence is not sent. Packets are never resent: a lost frame is patched over from the next one.
 *
 * None of this is simulation state, and nothing here may feed back into the simulation: what one player hears is
 * theirs alone.
 */
namespace VoiceChat {

/** 1 (the default): use voice chat in rooms. 0: no microphone, and nobody's voice is played. */
constexpr const char* kEnabledCVar = "gNetplay.Voice";
/** 1: keep hearing the others, send nothing. */
constexpr const char* kMicMutedCVar = "gNetplay.VoiceMicMuted";
/** How loud the others' voices play, as a factor: 1 as they come. */
constexpr const char* kVolumeCVar = "gNetplay.VoiceVolume";

enum class MicStatus {
    Off,         // Voice chat is turned off, or this game is not in a room
    Unavailable, // This platform has no voice chat
    Idle,        // Nobody to talk to: no other member who uses voice chat is reached directly
    Starting,    // Waiting for the microphone, or for the player to allow it
    Live,
    Muted,       // By kMicMutedCVar
    Denied,      // The player or the system refused the microphone: no voice chat at all
    Failed,
};

/** How this game stands with another member of the room. */
enum class MemberStatus {
    None,      // Nobody in that slot, or this game's own
    VoiceOff,  // The member does not use voice chat
    NotDirect, // No direct connection to the member: no voice either way
    Quiet,
    Talking,
};

/**
 * Moves voice between the microphone, the room's members and the speakers. Call once per game frame, after
 * Lobby::Update().
 * @param real Whether this is a player's game, which may use the microphone; false for test runs
 *        (VoiceDevice::Create()).
 */
void Update(bool real);
/** Closes the microphone and the speakers. */
void Shutdown();

MicStatus GetMicStatus();
/** @returns true while this player's own voice is being sent. */
bool IsTalking();
MemberStatus GetMemberStatus(u8 slot);
/** Whether this player chose not to hear the member in this slot; forgotten when the slot's member leaves. */
bool IsMuted(u8 slot);
void SetMuted(u8 slot, bool muted);

} // namespace VoiceChat

#endif // NETPLAY_VOICE_CHAT_H
