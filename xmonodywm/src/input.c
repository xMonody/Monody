/*
 * input.c - seat, keyboard and compositor shortcuts
 *
 * Keyboard focus is driven by the focused toplevel; key events are first
 * checked against the compositor's own shortcut table and forwarded to the
 * focused client otherwise.  Pointer/keyboard devices (including virtual
 * pointers used for testing) are attached to the shared cursor here.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libinput.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/backend/libinput.h>
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

static void client_cursor_shape_client_destroy(struct wl_listener *listener,
		void *data) {
	struct server *server = wl_container_of(listener, server,
		client_cursor_shape_client_destroy);
	wl_list_remove(&server->client_cursor_shape_client_destroy.link);
	server->client_cursor_shape = 0;
	server->client_cursor_shape_client = NULL;
}

void seat_request_set_cursor(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (event->seat_client == server->seat->pointer_state.focused_client) {
		/* a cursor surface supersedes any previously set cursor shape
		 * (clients mixing both protocols: the last request wins) */
		server->client_cursor_shape = 0;
		if (server->client_cursor_shape_client != NULL) {
			wl_list_remove(&server->client_cursor_shape_client_destroy.link);
			server->client_cursor_shape_client = NULL;
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

/* the toplevel whose surface (or whose client's surface) the pointer is
 * over, or NULL.  seat_request_set_shape() uses it to decide whether the
 * client's own window-resize shapes should be suppressed (clients that
 * never negotiate xdg-decoration, e.g. Firefox, draw their own CSD frame
 * but are treated as undecorated here, so the compositor owns the resize). */
static struct toplevel *toplevel_for_surface(struct server *server,
		struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->xdg_toplevel->base != NULL &&
				tl->xdg_toplevel->base->surface != NULL &&
				(tl->xdg_toplevel->base->surface == surface ||
				 tl->xdg_toplevel->base->surface->resource->client ==
					surface->resource->client)) {
			return tl;
		}
	}
	return NULL;
}

void seat_request_set_shape(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		cursor_shape_set_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (event->device_type !=
			WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
		return; /* no tablet support in this compositor */
	}
	if (event->seat_client != server->seat->pointer_state.focused_client) {
		return;
	}
	struct wlr_surface *focused =
		server->seat->pointer_state.focused_surface;
	/* A client that draws its own decorations but never negotiates
	 * xdg-decoration (Firefox, Chromium) is treated as an undecorated
	 * window here: the compositor owns its frame, so it provides the
	 * resize edges and the matching resize cursor.  The client's own
	 * directional window-resize shapes (e-resize, s-resize, ...) then
	 * fight the compositor's cursor just outside the compositor's edge
	 * zone - the "two resize cursors" seen around Firefox.  Those client
	 * shapes cannot resize the window anyway (the compositor grabs the
	 * presses at the edge), so ignore them: the compositor's own resize
	 * cursor stays the only one.  Web-content cursors (ew-resize
	 * splitters, text, pointer, ...) are unaffected. */
	struct toplevel *tl = toplevel_for_surface(server, focused);
	if (tl != NULL &&
			tl->decoration_mode !=
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
			event->shape >= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE &&
			event->shape <= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE) {
		wlr_log(WLR_DEBUG, "cursor: ignore %s resize shape from %s "
			"(compositor owns the frame)",
			wlr_cursor_shape_v1_name(event->shape),
			tl->app_id ? tl->app_id : "?");
		return;
	}
	/* remember the shape so pointer.c can restore it when the compositor
	 * cursor override ends; a shape supersedes an earlier cursor surface
	 * (mixing protocols: the last request wins).  Track the owning seat
	 * client so a stale shape is never shown for another client. */
	if (server->client_cursor_surface != NULL) {
		wl_list_remove(&server->client_cursor_destroy.link);
		server->client_cursor_surface = NULL;
	}
	if (server->client_cursor_shape_client != event->seat_client) {
		if (server->client_cursor_shape_client != NULL) {
			wl_list_remove(&server->client_cursor_shape_client_destroy.link);
		}
		server->client_cursor_shape_client_destroy.notify =
			client_cursor_shape_client_destroy;
		wl_signal_add(&event->seat_client->events.destroy,
			&server->client_cursor_shape_client_destroy);
		server->client_cursor_shape_client = event->seat_client;
	}
	server->client_cursor_shape = event->shape;
	wlr_log(WLR_DEBUG, "cursor: shape %d", event->shape);
	if (server->cursor_override != NULL) {
		/* we own the cursor (title zone / resize edge): keep the override,
		 * just remember the client's preference for later */
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			server->cursor_override);
		return;
	}
	/* render the shape ourselves: the image comes from the compositor's
	 * xcursor theme at the current output scale, so the cursor size always
	 * matches the compositor's own cursors - the client never has to guess */
	wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
		wlr_cursor_shape_v1_name(event->shape));
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
 * virtual keyboard) still reach the focused client.  The struct itself
 * lives in server.h: ime.c walks the same list to pick a real keyboard
 * for the IM grab. */
/* is this device a real typing keyboard?  libinput classifies special
 * buttons (Power Button, Lid Switch, Video Bus, ...) as keyboards because
 * they have key capabilities; such pseudo-keyboards must never become the
 * seat keyboard nor the input method's grab keyboard, or every keystroke
 * on the real keyboard would bypass the IM.  Virtual keyboards (the IM's
 * passthrough device, test tools) always count as typing keyboards. */
bool keyboard_is_typing(struct wlr_input_device *device) {
	if (!wlr_input_device_is_libinput(device)) {
		return true;
	}
	struct libinput_device *dev = wlr_libinput_get_device_handle(device);
	if (dev == NULL) {
		return true;
	}
	return libinput_device_keyboard_has_key(dev, KEY_Q) ||
		libinput_device_keyboard_has_key(dev, KEY_A) ||
		libinput_device_keyboard_has_key(dev, KEY_SPACE);
}

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

/* the Nth mapped toplevel in creation order (1-based index); NULL when
 * there is no such window.  Used by MOD+1..9 task switching. */
static struct toplevel *nth_toplevel(struct server *server, int index) {
	struct toplevel *tl;
	int i = 0;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->xdg_toplevel->base == NULL ||
				!tl->xdg_toplevel->base->surface->mapped) {
			continue;
		}
		if (i == index) {
			return tl;
		}
		i++;
	}
	return NULL;
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

/* launch an application from the config_app_shortcuts table */
static void spawn_app(const struct config_app_shortcut *app) {
	wlr_log(WLR_DEBUG, "input: spawn app '%s' args='%s'",
		app->app, app->args != NULL ? app->args : "");
	if (app->args != NULL && app->args[0] != '\0') {
		size_t len = strlen(app->app) + 1 + strlen(app->args) + 1;
		char *cmd = malloc(len);
		if (cmd == NULL) {
			return;
		}
		snprintf(cmd, len, "%s %s", app->app, app->args);
		spawn_command(cmd);
		free(cmd);
	} else {
		spawn_command(app->app);
	}
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
		if (main_mod && sym == CONFIG_KEY_CLOSE_OTHER) {
			/* close every window except the currently focused one;
			 * close_toplevel works for minimized/maximized/fullscreen
			 * toplevels as well */
			struct toplevel *tl, *tmp;
			wl_list_for_each_safe(tl, tmp, &server->toplevels, link) {
				if (tl != server->focused) {
					close_toplevel(tl);
				}
			}
			return true;
		}
		if ((mods & CONFIG_MOD_TASK) == CONFIG_MOD_TASK &&
				sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
			struct toplevel *tl = nth_toplevel(server,
				sym - XKB_KEY_1);
			if (tl != NULL) {
				focus_window(server, tl);
			}
			return true;
		}
		for (size_t i = 0;
				i < sizeof(config_app_shortcuts) /
					sizeof(config_app_shortcuts[0]); i++) {
			const struct config_app_shortcut *app =
				&config_app_shortcuts[i];
			if ((mods & app->mods) == app->mods &&
					sym == app->key) {
				spawn_app(app);
				return true;
			}
		}
	}
	return false;
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, key);
	struct server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;
	uint32_t keycode = event->keycode + 8;
	wlr_log(WLR_DEBUG, "input: KEY kb=%p code=%u grabbed=%d seat=%p",
		(void *)kb->keyboard, event->keycode,
		ime_keyboard_grabbed(server, kb->keyboard),
		(void *)wlr_seat_get_keyboard(server->seat));

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
		wlr_log(WLR_DEBUG, "input: key from grabbed keyboard, skipping client");
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
	wlr_log(WLR_DEBUG, "input: keyboard attach %p name='%s' type=%d",
		(void *)keyboard, device->name ? device->name : "(null)",
		device->type);

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

	/* give the input method's grab a chance to attach to this keyboard:
	 * every new device is offered, so a real keyboard that appears while
	 * the seat is held by the IM's own virtual keyboard still connects
	 * (fcitx5 with PersistentVirtualKeyboard starts its vk before any
	 * other keyboard exists).  ime_attach_keyboard() decides. */
	ime_attach_keyboard(server, keyboard);

	/* the first REAL typing keyboard becomes the seat keyboard (so an
	 * input method's auxiliary virtual keyboard and special-button
	 * pseudo-keyboards like the Power Button never hijack the seat);
	 * later keyboards keep their own listeners and still reach the
	 * focused client */
	bool first = wlr_seat_get_keyboard(server->seat) == NULL &&
		keyboard_is_typing(device);
	if (first) {
		wlr_seat_set_keyboard(server->seat, keyboard);
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
