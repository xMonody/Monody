/*
 * input.c - seat, keyboard and compositor shortcuts
 *
 * Keyboard focus is driven by the focused toplevel; key events are first
 * checked against the compositor's own shortcut table and forwarded to the
 * focused client otherwise.  Pointer/keyboard devices (including virtual
 * pointers used for testing) are attached to the shared cursor here.
 */

#include "server.h"

#include <stdlib.h>

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* seat requests                                                      */
/* ------------------------------------------------------------------ */

static void client_cursor_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct server *server =
		wl_container_of(listener, server, client_cursor_destroy);
	wl_list_remove(&server->client_cursor_destroy.link);
	server->client_cursor_surface = NULL;
}

void seat_request_set_cursor(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (event->seat_client == server->seat->pointer_state.focused_client) {
		/* Over layer-shell surfaces (e.g. the Qt taskbar) keep the
		 * compositor's own xcursor instead of the client's: Qt's client
		 * cursor is rendered at a different size on scaled outputs
		 * (fractional scale), which makes the pointer look bigger over
		 * the bar. The bar only ever uses the default arrow, so nothing
		 * is lost. */
		struct wlr_surface *focused =
			server->seat->pointer_state.focused_surface;
		struct layer_surface *ls;
		bool over_layer = false;
		if (focused != NULL) {
			wl_list_for_each(ls, &server->layer_surfaces, link) {
				if (ls->scene_layer != NULL &&
						ls->scene_layer->layer_surface != NULL &&
						ls->scene_layer->layer_surface->surface == focused) {
					over_layer = true;
					break;
				}
			}
		}
		if (over_layer) {
			return;
		}
		/* remember the client's cursor so it can be restored when the
		 * compositor cursor override (title strip / resize edge) ends */
		if (server->client_cursor_surface != event->surface) {
			if (server->client_cursor_surface != NULL) {
				wl_list_remove(&server->client_cursor_destroy.link);
			}
			server->client_cursor_surface = event->surface;
			if (event->surface != NULL) {
				server->client_cursor_destroy.notify =
					client_cursor_surface_destroy;
				wl_signal_add(&event->surface->events.destroy,
					&server->client_cursor_destroy);
			}
		}
		server->client_cursor_hotspot_x = event->hotspot_x;
		server->client_cursor_hotspot_y = event->hotspot_y;
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
		if (server->cursor_override != NULL) {
			/* entering a surface makes the client re-request its cursor; when
			 * we own the cursor (title zone / resize edge), re-assert it right
			 * away so the style changes on enter, not on the next motion */
			wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
				server->cursor_override);
		}
	}
}

void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void seat_request_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

void seat_request_start_drag(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	wlr_seat_start_drag(server->seat, event->drag, event->serial);
}

void seat_start_drag(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, seat_start_drag);
	struct wlr_drag *drag = data;
	if (drag->icon == NULL) {
		return;
	}
	server->drag_tree = wlr_scene_drag_icon_create(
		server->layers[LAYER_OVERLAY], drag->icon);
	if (server->drag_tree != NULL) {
		wlr_scene_node_set_position(&server->drag_tree->node,
			server->cursor->x, server->cursor->y);
	}
}

/* ------------------------------------------------------------------ */
/* keyboard                                                           */
/* ------------------------------------------------------------------ */

/* one attached keyboard device (real or virtual); each gets its own
 * listeners so keys from re-injection devices (e.g. fcitx5's passthrough
 * virtual keyboard) still reach the focused client */
struct keyboard {
	struct server *server;
	struct wlr_keyboard *keyboard;
	struct wl_list link; /* server.keyboards */

	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener destroy;
};

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, modifiers);
	struct server *server = kb->server;
	struct wlr_keyboard *keyboard = data;
	wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->modifiers);
}

/* the next/previous mapped toplevel relative to the focused one, wrapping
 * around the list. Minimized windows are included (the caller restores
 * them); unmapped toplevels are skipped. */
static struct toplevel *cycle_toplevel(struct server *server, bool next) {
	if (server->focused == NULL) {
		struct toplevel *tl;
		wl_list_for_each(tl, &server->toplevels, link) {
			if (tl->xdg_toplevel->base != NULL &&
					tl->xdg_toplevel->base->surface->mapped) {
				return tl;
			}
		}
		return NULL;
	}
	return neighbor_toplevel(server, server->focused, next, true);
}

/* focus a toplevel, unminimizing it first (it reappears at its remembered
 * position because minimizing only hides the scene node) */
void focus_window(struct server *server, struct toplevel *tl) {
	if (tl == NULL) {
		return;
	}
	if (tl->minimized) {
		set_minimized(server, tl, false);
	}
	focus_toplevel(server, tl);
	update_cursor_style(server);
}

/* compositor keyboard shortcuts (modifier combos and keysyms are defined
 * in config.h). Returns true when the key was consumed. */
static bool keyboard_shortcut(struct server *server,
		struct wlr_keyboard *keyboard, uint32_t keycode) {
	if (keyboard->xkb_state == NULL) {
		return false; /* no keymap yet: nothing to match */
	}
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, keycode, &syms);
	uint32_t mods = keyboard->modifiers.depressed | keyboard->modifiers.latched;
	bool main_mod = (mods & CONFIG_MOD_MAIN) == CONFIG_MOD_MAIN;
	bool quit_mod = (mods & CONFIG_MOD_QUIT) == CONFIG_MOD_QUIT;

	for (int i = 0; i < nsyms; i++) {
		/* with Shift held xkb reports the shifted keysym (e.g. 'Q'), so
		 * compare case-insensitively for the letter bindings */
		xkb_keysym_t sym = xkb_keysym_to_lower(syms[i]);
		if (sym == CONFIG_KEY_QUIT && (main_mod || quit_mod)) {
			wl_display_terminate(server->display);
			return true;
		}
		if (main_mod && sym == CONFIG_KEY_MAXIMIZE) {
			if (server->focused != NULL) {
				if (server->focused->xdg_toplevel->current.maximized) {
					/* toggle: restore the size/position saved before maximizing */
					restore_maximized_toplevel(server->focused);
				} else {
					set_maximized(server, server->focused, true);
				}
			}
			return true;
		}
		if (main_mod && sym == CONFIG_KEY_MINIMIZE) {
			if (server->focused != NULL) {
				set_minimized(server, server->focused, true);
			}
			return true;
		}
		if (main_mod && sym == CONFIG_KEY_NEXT_WINDOW) {
			focus_window(server, cycle_toplevel(server, true));
			return true;
		}
		if (main_mod && sym == CONFIG_KEY_PREV_WINDOW) {
			focus_window(server, cycle_toplevel(server, false));
			return true;
		}
		if (main_mod && sym == CONFIG_KEY_CLOSE) {
			if (server->focused != NULL) {
				close_toplevel(server->focused);
			}
			return true;
		}
		if (main_mod && sym == CONFIG_KEY_TERMINAL) {
			spawn_command("foot");
			return true;
		}
	}
	return false;
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, key);
	struct server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;
	uint32_t keycode = event->keycode + 8;

	/* virtual keyboards (wlr_virtual_keyboard_v1 and the IM relay's
	 * passthrough device) deliver keys with update_state=false: wlroots
	 * never advances their xkb state, so without this the modifier mask
	 * stays empty and every Shift/Alt combo shortcut silently dies. The
	 * wlr_keyboard's modifiers field is recomputed right after this
	 * listener returns, so the next key already sees the new mask. */
	if (!event->update_state && kb->keyboard->xkb_state != NULL) {
		xkb_state_update_key(kb->keyboard->xkb_state, keycode,
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED
				? XKB_KEY_DOWN : XKB_KEY_UP);
	}

	/* keys from the keyboard the IM grabbed are forwarded to it by ime.c
	 * and must not reach shortcuts or the focused client.  Keys from other
	 * keyboards (e.g. fcitx5's passthrough re-injection device) pass
	 * through normally. */
	if (ime_keyboard_grabbed(server, kb->keyboard)) {
		return;
	}

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		handled = keyboard_shortcut(server, kb->keyboard, keycode);
	}
	if (!handled) {
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, destroy);
	struct server *server = kb->server;
	ime_detach_keyboard(server, kb->keyboard);
	/* unlink every listener so wlr's destroy-time assertions pass */
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

/* ------------------------------------------------------------------ */
/* device hotplug                                                     */
/* ------------------------------------------------------------------ */

void server_new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor,
		&event->new_pointer->pointer.base);
}

/* attach a (real or virtual) keyboard: give it its own key/modifiers
 * listeners so keys from every device are seen, and make it the seat
 * keyboard unless one is already present (the first keyboard wins, so an
 * input method's auxiliary virtual keyboard never hijacks the seat) */
static void keyboard_attach(struct server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(device);

	struct keyboard *kb = calloc(1, sizeof(*kb));
	if (kb == NULL) {
		return;
	}
	kb->server = server;
	kb->keyboard = keyboard;
	kb->key.notify = keyboard_key;
	wl_signal_add(&keyboard->events.key, &kb->key);
	kb->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&keyboard->events.modifiers, &kb->modifiers);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&device->events.destroy, &kb->destroy);
	wl_list_insert(server->keyboards.prev, &kb->link);

	/* every keyboard needs an xkb keymap: keyboard_shortcut() dereferences
	 * xkb_state on key presses, so a keyboard without one would crash */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (context != NULL) {
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context,
			NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap != NULL) {
			wlr_keyboard_set_keymap(keyboard, keymap);
			wlr_keyboard_set_repeat_info(keyboard, 25, 600);
			xkb_keymap_unref(keymap);
		}
		xkb_context_unref(context);
	}

	/* the first keyboard becomes the seat keyboard (so an input method's
	 * auxiliary virtual keyboard never hijacks the seat); later keyboards
	 * keep their own listeners and still reach the focused client */
	bool first = wlr_seat_get_keyboard(server->seat) == NULL;
	if (first) {
		wlr_seat_set_keyboard(server->seat, keyboard);
		/* if the input method grabbed before a keyboard existed, connect it */
		ime_attach_keyboard(server, keyboard);

		if (server->focused != NULL &&
				server->focused->xdg_toplevel->base != NULL &&
				server->focused->xdg_toplevel->base->surface->mapped) {
			wlr_seat_keyboard_notify_enter(server->seat,
				server->focused->xdg_toplevel->base->surface,
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
		}
	}
}

void server_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;
	keyboard_attach(server, &virtual_keyboard->keyboard.base);
}

void server_new_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		keyboard_attach(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	default:
		break;
	}
}
