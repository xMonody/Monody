/*
 * ime.c - input method relay (fcitx5 / ibus, Chinese input)
 *
 * Glues zwp_input_method_unstable_v2 (the input method, e.g. fcitx5) to
 * zwp_text_input_unstable_v3 (per-window text input).  When the keyboard
 * focuses a surface, its client's text input object is associated with it,
 * the input method is activated and state flows both ways:
 *
 *   app --(surrounding text / content type)--> compositor --(activate)--> IM
 *   IM  --(preedit / commit / delete)---------> compositor ------------> app
 *
 * While the IM holds the keyboard grab, raw key/modifier events are
 * forwarded to it and the focused client is bypassed (input.c skips its
 * normal key handling during a grab).  The IM's candidate window (an input
 * popup surface) is placed in the overlay layer and follows the cursor.
 */

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* the text input object owned by the client that owns the given surface */
static struct wlr_text_input_v3 *text_input_for_surface(
		struct server *server, struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		struct wlr_text_input_v3 *input = ti->text_input;
		if (wl_resource_get_client(input->resource) == client) {
			return input;
		}
	}
	return NULL;
}

/* forward the text input state (surrounding text, content type, cause) to
 * the input method and end the exchange with done */
static void ime_send_text_input_state(struct wlr_input_method_v2 *context,
		struct wlr_text_input_v3 *text_input) {
	wlr_input_method_v2_send_surrounding_text(context,
		text_input->current.surrounding.text,
		text_input->current.surrounding.cursor,
		text_input->current.surrounding.anchor);
	wlr_input_method_v2_send_content_type(context,
		text_input->current.content_type.hint,
		text_input->current.content_type.purpose);
	wlr_input_method_v2_send_text_change_cause(context,
		text_input->current.text_change_cause);
	wlr_input_method_v2_send_done(context);
}

/* (re)associate the focused text input with the given surface; called
 * whenever keyboard focus changes.  surface == NULL clears the
 * association (and deactivates the input method). */
void ime_set_focus(struct server *server, struct wlr_surface *surface) {
	struct wlr_input_method_v2 *context = server->input_method;
	struct wlr_text_input_v3 *text_input =
		text_input_for_surface(server, surface);

	server->focused_text_input = text_input;

	if (context == NULL) {
			return;
	}
	if (text_input == NULL) {
		wlr_input_method_v2_send_deactivate(context);
		wlr_input_method_v2_send_done(context);
		return;
	}
	wlr_input_method_v2_send_activate(context);
	ime_send_text_input_state(context, text_input);
}

/* keep the candidate window glued to the cursor */
void ime_update_popup(struct server *server) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->popup_scene_surface != NULL) {
			wlr_scene_node_set_position(
				&ime->popup_scene_surface->buffer->node,
				server->cursor->x, server->cursor->y);
		}
	}
}

/* ------------------------------------------------------------------ */
/* input method (fcitx5)                                              */
/* ------------------------------------------------------------------ */

/* forward key events to the IM while it holds the keyboard grab */
static void ime_keyboard_grab_key(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, keyboard_grab_key);
	struct wlr_keyboard_key_event *event = data;
	if (ime->input_method == NULL || ime->input_method->keyboard_grab == NULL) {
		return;
	}
	wlr_input_method_keyboard_grab_v2_send_key(
		ime->input_method->keyboard_grab, event->time_msec,
		event->keycode, event->state);
}

static void ime_keyboard_grab_modifiers(struct wl_listener *listener,
		void *data) {
	struct ime *ime = wl_container_of(listener, ime, keyboard_grab_modifiers);
	struct wlr_keyboard *keyboard = data;
	if (ime->input_method == NULL || ime->input_method->keyboard_grab == NULL) {
		return;
	}
	wlr_input_method_keyboard_grab_v2_send_modifiers(
		ime->input_method->keyboard_grab, &keyboard->modifiers);
}

static void ime_keyboard_grab_destroy(struct wl_listener *listener,
		void *data) {
	struct ime *ime = wl_container_of(listener, ime, keyboard_grab_destroy);
	if (ime->keyboard != NULL) {
		wl_list_remove(&ime->keyboard_grab_key.link);
		wl_list_remove(&ime->keyboard_grab_modifiers.link);
	}
	wl_list_remove(&ime->keyboard_grab_destroy.link);
	ime->keyboard_grab_destroy_added = false;
	ime->keyboard = NULL;
}

/* attach the given seat keyboard to the IM's grab: send it the current
 * keymap/repeat info and forward key/modifier events to the IM from now
 * on.  Idempotent: re-attaching the same keyboard is a no-op. */
static void ime_keyboard_connect(struct ime *ime,
		struct wlr_keyboard *keyboard) {
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab =
		ime->input_method->keyboard_grab;
	if (keyboard_grab == NULL) {
		return;
	}
	if (ime->keyboard == keyboard) {
		return;
	}
	if (ime->keyboard != NULL) {
		wl_list_remove(&ime->keyboard_grab_key.link);
		wl_list_remove(&ime->keyboard_grab_modifiers.link);
	}
	ime->keyboard = keyboard;
	/* sends the current keymap + repeat info to the IM and keeps them in
	 * sync from here on */
	wlr_input_method_keyboard_grab_v2_set_keyboard(keyboard_grab, keyboard);

	ime->keyboard_grab_key.notify = ime_keyboard_grab_key;
	wl_signal_add(&keyboard->events.key, &ime->keyboard_grab_key);
	ime->keyboard_grab_modifiers.notify = ime_keyboard_grab_modifiers;
	wl_signal_add(&keyboard->events.modifiers, &ime->keyboard_grab_modifiers);
	if (!ime->keyboard_grab_destroy_added) {
		ime->keyboard_grab_destroy.notify = ime_keyboard_grab_destroy;
		wl_signal_add(&keyboard_grab->events.destroy,
			&ime->keyboard_grab_destroy);
		ime->keyboard_grab_destroy_added = true;
	}
}

static void ime_grab_keyboard(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab = data;
	struct server *server = ime->server;
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard == NULL) {
		/* no seat keyboard yet (e.g. grabbed before device hotplug):
		 * keyboard_attach() connects it when one appears */
		return;
	}
	ime_keyboard_connect(ime, keyboard);
}

/* called from input.c when the seat keyboard is destroyed: drop the
 * grab listeners so wlr_keyboard_finish() sees an empty listener list */
void ime_detach_keyboard(struct server *server,
		struct wlr_keyboard *keyboard) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->keyboard == keyboard) {
			wl_list_remove(&ime->keyboard_grab_key.link);
			wl_list_remove(&ime->keyboard_grab_modifiers.link);
			ime->keyboard = NULL;
			/* the grab destroy listener stays attached; it is removed when the
			 * grab or the input method itself is destroyed */
		}
	}
}

/* called from input.c whenever a keyboard device appears; connects the IM
 * grab to it only if the IM grabbed before any keyboard existed */
void ime_attach_keyboard(struct server *server,
		struct wlr_keyboard *keyboard) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->input_method != NULL &&
				ime->input_method->keyboard_grab != NULL &&
				ime->keyboard == NULL) {
			ime_keyboard_connect(ime, keyboard);
		}
	}
}

/* is this keyboard the one whose keys the IM grab receives? */
bool ime_keyboard_grabbed(struct server *server,
		struct wlr_keyboard *keyboard) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->keyboard == keyboard) {
			return true;
		}
	}
	return false;
}

/* the IM committed a state change: forward preedit/commit/delete to the
 * focused text input */
static void ime_commit(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, commit);
	struct wlr_input_method_v2 *context = data;
	struct server *server = ime->server;
	struct wlr_text_input_v3 *text_input = server->focused_text_input;
	if (text_input == NULL) {
		return;
	}

	if (context->active) {
		if (text_input->focused_surface == NULL) {
			struct wlr_surface *surface =
				server->seat->keyboard_state.focused_surface;
			if (surface != NULL) {
				wlr_text_input_v3_send_enter(text_input, surface);
			}
		}
	} else {
		if (text_input->focused_surface != NULL) {
			wlr_text_input_v3_send_leave(text_input);
		}
	}

	struct wlr_input_method_v2_state *state = &context->current;
	wlr_text_input_v3_send_preedit_string(text_input,
		state->preedit.text, state->preedit.cursor_begin,
		state->preedit.cursor_end);
	wlr_text_input_v3_send_commit_string(text_input, state->commit_text);
	wlr_text_input_v3_send_delete_surrounding_text(text_input,
		state->delete.before_length, state->delete.after_length);
	wlr_text_input_v3_send_done(text_input);
}

/* candidate window: place the popup surface in the overlay layer near the
 * cursor so fcitx5's candidate list is visible */
static void ime_popup_destroy(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, popup_destroy);
	wl_list_remove(&ime->popup_destroy.link);
	ime->popup_scene_surface = NULL;
}

static void ime_new_popup_surface(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, new_popup_surface);
	struct wlr_input_popup_surface_v2 *popup = data;
	struct server *server = ime->server;

	struct wlr_scene_surface *scene_surface = wlr_scene_surface_create(
		server->layers[LAYER_OVERLAY], popup->surface);
	if (scene_surface == NULL) {
		return;
	}
	wlr_scene_node_set_position(&scene_surface->buffer->node,
		server->cursor->x, server->cursor->y);

	ime->popup_scene_surface = scene_surface;
	ime->popup_destroy.notify = ime_popup_destroy;
	wl_signal_add(&popup->events.destroy, &ime->popup_destroy);
}

static void ime_destroy(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, destroy);
	struct server *server = ime->server;

	if (ime->keyboard != NULL) {
		wl_list_remove(&ime->keyboard_grab_key.link);
		wl_list_remove(&ime->keyboard_grab_modifiers.link);
	}
	if (ime->keyboard_grab_destroy_added) {
		wl_list_remove(&ime->keyboard_grab_destroy.link);
	}
	wl_list_remove(&ime->destroy.link);
	wl_list_remove(&ime->grab_keyboard.link);
	wl_list_remove(&ime->commit.link);
	wl_list_remove(&ime->new_popup_surface.link);
	wl_list_remove(&ime->link);
	if (server->input_method == ime->input_method) {
		server->input_method = NULL;
	}
	free(ime);
}

void ime_new_input_method(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_ime);
	struct wlr_input_method_v2 *input_method = data;

	if (server->input_method != NULL) {
		wlr_log(WLR_INFO, "ignoring additional input method");
		return;
	}
	server->input_method = input_method;

	struct ime *ime = calloc(1, sizeof(*ime));
	if (ime == NULL) {
		server->input_method = NULL;
		return;
	}
	ime->server = server;
	ime->input_method = input_method;

	ime->destroy.notify = ime_destroy;
	wl_signal_add(&input_method->events.destroy, &ime->destroy);
	ime->grab_keyboard.notify = ime_grab_keyboard;
	wl_signal_add(&input_method->events.grab_keyboard, &ime->grab_keyboard);
	ime->commit.notify = ime_commit;
	wl_signal_add(&input_method->events.commit, &ime->commit);
	ime->new_popup_surface.notify = ime_new_popup_surface;
	wl_signal_add(&input_method->events.new_popup_surface,
		&ime->new_popup_surface);

	wl_list_insert(server->imes.prev, &ime->link);

	/* if a surface is already focused, activate the IM right away */
	if (server->seat->keyboard_state.focused_surface != NULL) {
		ime_set_focus(server, server->seat->keyboard_state.focused_surface);
	}
}

/* ------------------------------------------------------------------ */
/* text input (the focused app)                                       */
/* ------------------------------------------------------------------ */

static void text_input_commit(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, commit);
	struct server *server = ti->server;
	if (server->focused_text_input != ti->text_input ||
			server->input_method == NULL) {
		return;
	}
	/* the app updated its text (surrounding text, cursor, ...): relay */
	ime_send_text_input_state(server->input_method, ti->text_input);
}

static void text_input_enable(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, enable);
	struct server *server = ti->server;
	if (server->focused_text_input != ti->text_input ||
			server->input_method == NULL) {
		return;
	}
	/* the app enabled text input on the focused surface: (re)activate the
	 * IM so it knows the client is ready */
	wlr_input_method_v2_send_activate(server->input_method);
	ime_send_text_input_state(server->input_method, ti->text_input);
}

static void text_input_disable(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, disable);
	struct server *server = ti->server;
	if (server->focused_text_input != ti->text_input ||
			server->input_method == NULL) {
		return;
	}
	wlr_input_method_v2_send_deactivate(server->input_method);
	wlr_input_method_v2_send_done(server->input_method);
}

static void text_input_destroy(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, destroy);
	struct server *server = ti->server;
	wl_list_remove(&ti->destroy.link);
	wl_list_remove(&ti->enable.link);
	wl_list_remove(&ti->disable.link);
	wl_list_remove(&ti->commit.link);
	wl_list_remove(&ti->link);
	if (server->focused_text_input == ti->text_input) {
		/* the focused text input went away: re-resolve for the surface */
		server->focused_text_input = NULL;
		ime_set_focus(server, server->seat->keyboard_state.focused_surface);
	}
	free(ti);
}

void ime_new_text_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_text_input);
	struct wlr_text_input_v3 *input = data;

	struct text_input *ti = calloc(1, sizeof(*ti));
	if (ti == NULL) {
		return;
	}
	ti->server = server;
	ti->text_input = input;

	ti->destroy.notify = text_input_destroy;
	wl_signal_add(&input->events.destroy, &ti->destroy);
	ti->enable.notify = text_input_enable;
	wl_signal_add(&input->events.enable, &ti->enable);
	ti->disable.notify = text_input_disable;
	wl_signal_add(&input->events.disable, &ti->disable);
	ti->commit.notify = text_input_commit;
	wl_signal_add(&input->events.commit, &ti->commit);

	wl_list_insert(server->text_inputs.prev, &ti->link);

	/* the text input may have been created after its surface was focused
	 * (e.g. GTK creates it on map) */
	struct wlr_surface *surface = server->seat->keyboard_state.focused_surface;
	if (surface != NULL &&
			wl_resource_get_client(surface->resource) ==
			wl_resource_get_client(input->resource)) {
		ime_set_focus(server, surface);
	}
}
