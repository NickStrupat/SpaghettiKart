#ifndef NETPLAY_ONLINE_MENU_H
#define NETPLAY_ONLINE_MENU_H

#include <libultraship/libultraship.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "menu_items.h"
#ifdef __cplusplus
}
#endif

/**
 * The ONLINE screen, reached with Z on Game Select: the relay's open rooms, hosting one, and this player's name.
 * Joining or hosting a room puts every member on the room's shared console (Netplay.h), which starts its menus on
 * Game Select; leaving it comes back here.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct Controller;

/** Call when the screen is entered. */
void online_menu_enter(void);
/** Navigation, from controller 1. */
void online_menu_act(struct Controller* controller);
void render_online_menu(MenuItem* item);
/**
 * Game Select's "Z ONLINE" button, beside "L OPTION" and "R DATA". On a shared console it reads "Z LEAVE", with
 * the players' names along the bottom of the screen.
 */
void render_online_button(MenuItem* item);

#ifdef __cplusplus
}
#endif

#endif // NETPLAY_ONLINE_MENU_H
