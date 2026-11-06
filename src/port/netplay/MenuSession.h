#ifndef NETPLAY_MENU_SESSION_H
#define NETPLAY_MENU_SESSION_H

#include <libultraship/libultraship.h>

#include <functional>
#include <string>
#include <vector>

#include <gekkonet.h>

/**
 * The menus of a netplay room, run in lockstep: every peer runs the same menu frames from the same inputs, as if all
 * the players sat at one console with a controller each. Player k's controller is controller k on every peer.
 *
 * A menu frame runs only once every player's input for it has arrived; until then the game shows the last frame
 * again. Nothing is predicted, so nothing is rolled back: menu loads and the draws that change menu state (item
 * animations, random noise) happen exactly once per frame on every peer, in the same order.
 *
 * Every peer starts the session from the same state: the simulation's globals as they were at boot, then a reset to
 * the Game Select screen, or the state the race before it ended on.
 *
 * When the menus load a race, the session freezes: frames run outside it, as the race's own session takes over, while
 * it keeps handing out this peer's input until every peer has run the last menu frame.
 */
struct MenuSessionSettings {
    s32 LocalPlayer = 0;
    /** Each player's peer address, in player order, as GekkoNet sees them: opaque bytes. The local entry is unused. */
    std::vector<std::vector<u8>> Addresses;
    /** Carries GekkoNet's packets. Owned by the caller; must outlive the session. */
    std::function<GekkoNetAdapter*()> GetAdapter;
    /** Frames between reading this player's input and running the frame it belongs to; hides that much latency. */
    u8 InputDelay = 1;
    /**
     * true: every peer starts from the globals as they were at boot, then resets to the Game Select screen (a new
     * console). false: the menus go on from the state a race left, the same on every peer.
     */
    bool ResetToGameSelect = true;
    /** Testing: write each menu frame's checksum to <LogPath>.menu, to compare peers. */
    std::string LogPath;
};

namespace MenuSession {

void Start(const MenuSessionSettings& settings);
void Stop(const std::string& reason);
bool IsActive();
/** @returns true once every player has connected and menu frames run. */
bool IsRunning();
/** Menu frames run so far. */
u32 GetFrames();
/** Why the last session ended; empty while it runs or after a clean stop. */
const std::string& GetStopReason();

/**
 * Polls the network, hands this player's input to the session and asks for the next menu frame. Call once per game
 * loop iteration, before the frame would run.
 * @param localPad This player's controller, with any test input already applied.
 */
void BeginFrame(const OSContPad& localPad);
/**
 * @returns true while the next frame's inputs have not all arrived, or a reset to Game Select has not happened yet:
 * this iteration must not run a frame.
 */
bool IsWaiting();
/**
 * Puts every player's pad for the frame about to run into pads[0..3].
 * @returns false when no menu frame runs this iteration.
 */
bool TakePads(OSContPad* pads);
/** Call after a menu frame ran. */
void EndFrame();
/** Stops running menu frames, but keeps exchanging input until Stop(), so that the other peers can finish theirs. */
void Freeze();
bool IsFrozen();

} // namespace MenuSession

#endif // NETPLAY_MENU_SESSION_H
