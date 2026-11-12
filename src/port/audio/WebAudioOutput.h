#ifndef PORT_AUDIO_WEB_AUDIO_OUTPUT_H
#define PORT_AUDIO_WEB_AUDIO_OUTPUT_H

#include <cstddef>
#include <cstdint>

/**
 * The browser build's sound output (docs/WEB.md). SDL plays sound from the page's main thread in 11 ms pieces, so it
 * crackles whenever the game, which runs on that thread too, takes longer than that. Here the game's sound thread
 * writes its frames into a ring in the game's memory, and an AudioWorklet (src/port/web/AudioOutput.js), which runs on
 * the browser's own audio thread, plays them from there: a busy page no longer interrupts the sound.
 *
 * Frames are 16-bit stereo at the game's rate; the worklet converts them to the output's rate.
 */
namespace WebAudioOutput {

/** @returns whether this browser can play sound this way: it has AudioWorklet, and the page shares its memory. */
bool IsSupported();

/**
 * Starts the output, from the page's main thread. The browser keeps it silent until the player first touches the page
 * or presses a key (shell.html resumes it).
 * @param sampleRate The game's rate, in frames per second.
 * @param buffered The frames to keep waiting: what covers the gaps between the game's frames.
 * @returns whether it started; the worklet itself loads in the background.
 */
bool Start(int32_t sampleRate, int32_t buffered);

/** @returns whether Start() succeeded: the game's sound goes to Play() instead of libultraship's player. */
bool IsActive();

/** @returns the frames written and not yet played. Safe from any thread. */
int32_t Buffered();

/**
 * Queues frames to play. Frames that do not fit in the ring are dropped. From one thread at a time (the sound thread).
 * @param frames Interleaved left and right samples.
 * @param count The number of frames (half the number of samples).
 */
void Play(const int16_t* frames, size_t count);

} // namespace WebAudioOutput

#endif // PORT_AUDIO_WEB_AUDIO_OUTPUT_H
