#ifndef NETPLAY_SOUND_ROLLBACK_H
#define NETPLAY_SOUND_ROLLBACK_H

#include <libultraship/libultraship.h>
#include <common_structs.h>

/**
 * Netplay sound effects across rollbacks.
 *
 * A frame's sounds play when it is first simulated, often from predicted inputs. A rollback simulates the frame
 * again with the real inputs, and its sounds can come out differently. Instead of muting re-simulated frames, their
 * sounds are compared with the ones that already played:
 *   - the same sound from the same camera, at most a few frames apart, is left playing;
 *   - a sound that is new plays now, a little late;
 *   - a sound that no longer happens is stopped.
 * The simulation's stops are compared the same way: a stop that no longer happens starts what it silenced again (a
 * shell's hum, the star), and a new one is carried out late.
 * Music and the continuous sounds (engines, tyres) follow the game's state and need none of this.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Takes a sound that play_sound() was asked for.
 * @returns true when it plays later or not at all; false to queue it now.
 */
bool SoundRollback_Capture(u32 soundBits, Vec3f* position, u8 cameraId, f32* freqScale, f32* volume, s8* reverb);
/**
 * Takes a stop the simulation made: func_800C5578(), or func_800C56F0() for every position.
 * @returns true when it is carried out later or not at all; false to carry it out now.
 */
bool SoundRollback_CaptureStop(Vec3f* position, u32 soundBits, bool anyPosition);
/** Takes a stop of the positional sounds that follow `source` (stop_slot_sounds()), like SoundRollback_CaptureStop(). */
bool SoundRollback_CaptureSlotStop(const f32* source, u32 soundBits, u8 count);

#ifdef __cplusplus
}

#include <string>

namespace SoundRollback {

/**
 * Starts tracking sounds, for one GekkoNet session.
 * @param keepHistory Keep every frame's final sounds, for WriteLog().
 */
void Start(bool keepHistory);
/** Stops tracking; sounds play as they come again. */
void Stop();

/** The simulation is about to run this frame. */
void BeginFrame(s32 frame);
void EndFrame();
/** A rollback loaded an earlier state; the frames simulated next have played their sounds before. */
void Rollback();
/** After a session update: compares the re-simulated frames' sounds with the ones that played. */
void Settle(s32 newestFrame);

/**
 * Writes the sounds and stops of every frame as they finally were: "<frame> <sound> <camera>" and
 * "<frame> stop <sound>" lines. Peers that hear every camera write the same file however their rollbacks went.
 */
void WriteLog(const std::string& path);
/**
 * Plays sounds and stops through a few made-up rollbacks, outside the session's own tracking, during a race (when the
 * sound banks are on), and every camera is heard.
 * @returns what went wrong, or an empty string.
 */
std::string SelfTest();
/** @returns a one-line summary: sounds played, played late, stopped and restarted. */
std::string GetSummary();

} // namespace SoundRollback
#endif

#endif // NETPLAY_SOUND_ROLLBACK_H
