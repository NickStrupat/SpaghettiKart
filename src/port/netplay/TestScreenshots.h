#ifndef NETPLAY_TEST_SCREENSHOTS_H
#define NETPLAY_TEST_SCREENSHOTS_H

#include <libultraship/libultraship.h>

#include <string>

/**
 * Testing: saves what the game draws, as PNG files, on chosen frames. The game reads its own framebuffer, so this
 * needs no screen-recording permission and captures nothing but the game.
 *
 *   SK_TEST_SCREENSHOTS=menu@120,room@400   File name and game frame (counted from boot) of each picture.
 *   SK_TEST_SCREENSHOT_DIR=<folder>         Where they go; the working folder by default.
 *
 * Pictures are the 4:3 middle of the window, scaled to the N64's 320x240.
 */
namespace TestScreenshots {

void Init();
/** Before the master display list closes: asks for this frame's picture if one is due. */
void EndFrame(u32 frame);
/** Writes the picture asked for last frame, which drawing it has filled in by now. */
void BeginFrame();

} // namespace TestScreenshots

#endif // NETPLAY_TEST_SCREENSHOTS_H
