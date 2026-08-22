/*
 * keycast.h - keyboard echo feed (JSON over a Unix domain socket)
 *
 * A small, self-contained hook that publishes every physical key event to
 * interested local clients (e.g. the showkey overlay).  The rest of the
 * compositor only calls keycast_init() once and keycast_key() from the
 * keyboard handler; all socket/JSON/xkb translation stays in keycast.c.
 */

#ifndef XMONODYWM_KEYCAST_H
#define XMONODYWM_KEYCAST_H

#include "server.h"

/* Create the listening socket at $XDG_RUNTIME_DIR/monodywm-keycast.sock
 * (fallback /tmp).  Returns false on failure; the compositor keeps
 * running without the echo feed. */
bool keycast_init(struct server *server);

/* Broadcast one key event.  keycode is the evdev/linux-input code
 * (wlroots keycode - 8), state is 0 (released) or 1 (pressed). */
void keycast_key(struct server *server, struct wlr_keyboard *keyboard,
	uint32_t keycode, uint32_t state);

/* Broadcast a pointer button event.  button is a linux-input BTN_* code,
 * state is 0 (released) or 1 (pressed). */
void keycast_button(struct server *server, uint32_t button, uint32_t state);

/* Broadcast a discrete scroll step.  orientation is enum wl_pointer_axis
 * and delta_discrete is the signed number of steps (negative = up/left). */
void keycast_scroll(struct server *server, uint32_t orientation,
	int32_t delta_discrete);

/* Tear the socket down (called during compositor shutdown). */
void keycast_finish(struct server *server);

#endif /* XMONODYWM_KEYCAST_H */
