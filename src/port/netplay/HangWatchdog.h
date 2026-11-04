#ifndef NETPLAY_HANG_WATCHDOG_H
#define NETPLAY_HANG_WATCHDOG_H

/**
 * Records where the game is stuck when it stops responding during netplay. A freeze seen between two computers is
 * hard to reproduce on one, so after kStallTime without a frame, a background thread runs macOS's sample tool on the
 * game and keeps every thread's call stack in netplay/<date-time>.hang.txt in the game's folder. Once per freeze.
 */
namespace HangWatchdog {

/** Starts the background thread. Call once at startup, from the game thread. */
void Start();
void Stop();
/**
 * Call once per frame from the game thread.
 * @param watching Whether a freeze from now on should be recorded: in a netplay room or race.
 */
void Beat(bool watching);

} // namespace HangWatchdog

#endif // NETPLAY_HANG_WATCHDOG_H
