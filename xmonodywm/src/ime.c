/*
 * ime.c - input method relay (fcitx5 / ibus, Chinese input)
 *
 * Glues zwp_input_method_unstable_v2 (the input method, e.g. fcitx5) to
 * zwp_text_input_unstable_v3 (per-window text input), following the relay
 * design used by labwc / dwl (dwl PR #235, DreamMaoMao's patch):
 *
 *   - the compositor tracks one "focused surface" per seat.  When keyboard
 *     focus changes, every text input whose client owns the focused surface
 *     receives an `enter` (the others receive `leave`).
 *   - a text input only becomes "active" when it is focused AND its client
 *     sent `enable`.  The input method is activated exactly when such a
 *     text input exists and deactivated otherwise.
 *   - the IM's state (surrounding text / content type) is relayed to the
 *     input method only for the active text input, and the IM's
 *     preedit/commit/delete strings are forwarded only to the active text
 *     input.
 *   - while the IM holds the keyboard grab, key/modifier events from the
 *     grabbed keyboard are forwarded to it (input.c skips its normal key
 *     handling during a grab).  Keys from the IM's own virtual keyboard
 *     (its passthrough re-injection device) are never forwarded back to the
 *     grab, so events don't loop.
 *
 * The candidate window (an input popup surface) is placed in the overlay
 * layer near the text cursor (or the cursor when no rectangle is known).
 */

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static bool same_client(struct wlr_text_input_v3 *ti,
		struct wlr_surface *surface) {
	return wl_resource_get_client(ti->resource) ==
		wl_resource_get_client(surface->resource);
}

/* is this keyboard the input method's own passthrough virtual keyboard?
 * Keys from it are already the IM's own output and must never be sent back
 * to the IM grab (that would loop key events between compositor and IM). */
static bool is_keyboard_emulated_by_input_method(struct wlr_keyboard *keyboard,
		struct wlr_input_method_v2 *input_method) {
	if (keyboard == NULL || input_method == NULL) {
		return false;
	}
	struct wlr_virtual_keyboard_v1 *vk =
		wlr_input_device_get_virtual_keyboard(&keyboard->base);
	return vk != NULL &&
		wl_resource_get_client(vk->resource) ==
		wl_resource_get_client(input_method->resource);
}

/* ---- forwarded-key bookkeeping: only forward a release if the matching
 * press was forwarded (the IM may grab while a key is already down) ---- */

static bool ime_key_forwarded(struct ime *ime, uint32_t keycode) {
	for (size_t i = 0; i < ime->forwarded_key_count; i++) {
		if (ime->forwarded_keys[i] == keycode) {
			return true;
		}
	}
	return false;
}

static void ime_key_forwarded_add(struct ime *ime, uint32_t keycode) {
	if (ime->forwarded_key_count >=
			sizeof(ime->forwarded_keys) / sizeof(ime->forwarded_keys[0])) {
		return;
	}
	ime->forwarded_keys[ime->forwarded_key_count++] = keycode;
}

static void ime_key_forwarded_remove(struct ime *ime, uint32_t keycode) {
	for (size_t i = 0; i < ime->forwarded_key_count; i++) {
		if (ime->forwarded_keys[i] == keycode) {
			ime->forwarded_keys[i] =
				ime->forwarded_keys[--ime->forwarded_key_count];
			return;
		}
	}
}

/* forward the text input state (surrounding text, content type, cause) to
 * the input method and end the exchange with done */
static void ime_send_text_input_state(struct server *server,
		struct wlr_text_input_v3 *text_input) {
	struct wlr_input_method_v2 *context = server->input_method;
	if (context == NULL) {
		return;
	}
	if (text_input->active_features &
			WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
		wlr_input_method_v2_send_surrounding_text(context,
			text_input->current.surrounding.text,
			text_input->current.surrounding.cursor,
			text_input->current.surrounding.anchor);
	}
	wlr_input_method_v2_send_text_change_cause(context,
		text_input->current.text_change_cause);
	if (text_input->active_features &
			WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
		wlr_input_method_v2_send_content_type(context,
			text_input->current.content_type.hint,
			text_input->current.content_type.purpose);
	}
	wlr_input_method_v2_send_done(context);
}

/* ------------------------------------------------------------------ */
/* focused surface / active text input                                */
/* ------------------------------------------------------------------ */

/* send enter/leave to every text input so that exactly the text inputs of
 * the focused surface's client are "focused".  Sending enter lets the
 * client know it may enable text input (GTK/Qt only enable after enter). */
static void update_text_inputs_focused_surface(struct server *server) {
	struct wlr_surface *surface = server->ime_focused_surface;
	struct text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		struct wlr_text_input_v3 *input = ti->text_input;
		struct wlr_surface *new_focused = NULL;
		if (server->input_method != NULL && surface != NULL &&
				same_client(input, surface)) {
			new_focused = surface;
		}
		if (input->focused_surface == new_focused) {
			continue;
		}
		if (input->focused_surface != NULL) {
			wlr_text_input_v3_send_leave(input);
		}
		if (new_focused != NULL) {
			wlr_text_input_v3_send_enter(input, new_focused);
		}
	}
}

/* the text input the IM should talk to: focused AND enabled by its client */
static struct text_input *get_active_text_input(struct server *server) {
	if (server->input_method == NULL) {
		return NULL;
	}
	struct text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		if (ti->text_input->focused_surface != NULL &&
				ti->text_input->current_enabled) {
			return ti;
		}
	}
	return NULL;
}

/* (re)compute the active text input and activate/deactivate the input
 * method when it changed */
static void update_active_text_input(struct server *server) {
	struct text_input *active = get_active_text_input(server);
	struct wlr_text_input_v3 *new_active =
		active != NULL ? active->text_input : NULL;
	if (server->input_method != NULL &&
			server->focused_text_input != new_active) {
		if (new_active != NULL) {
			wlr_log(WLR_DEBUG, "ime: sending activate (serial %u -> %u)",
				server->input_method->current_serial,
				server->input_method->current_serial + 1);
			wlr_input_method_v2_send_activate(server->input_method);
		} else {
			wlr_log(WLR_DEBUG, "ime: sending deactivate");
			wlr_input_method_v2_send_deactivate(server->input_method);
		}
		wlr_input_method_v2_send_done(server->input_method);
	}
	server->focused_text_input = new_active;
}

static void ime_focused_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct server *server =
		wl_container_of(listener, server, ime_focused_surface_destroy);
	ime_set_focus(server, NULL);
}

/* (re)associate the focused surface with the seat's text inputs; called
 * whenever keyboard focus changes.  surface == NULL clears the association
 * and deactivates the input method. */
void ime_set_focus(struct server *server, struct wlr_surface *surface) {
	if (server->ime_focused_surface == surface) {
		return;
	}
	if (server->ime_focused_surface != NULL) {
		wl_list_remove(&server->ime_focused_surface_destroy.link);
	}
	server->ime_focused_surface = surface;
	if (surface != NULL) {
		server->ime_focused_surface_destroy.notify =
			ime_focused_surface_destroy;
		wl_signal_add(&surface->events.destroy,
			&server->ime_focused_surface_destroy);
	}
	update_text_inputs_focused_surface(server);
	update_active_text_input(server);
	ime_update_popup(server);
}

/* ------------------------------------------------------------------ */
/* candidate window (input popup surface)                             */
/* ------------------------------------------------------------------ */

/* keep the candidate window glued to the text cursor (or the pointer when
 * the app didn't report a cursor rectangle) */
void ime_update_popup(struct server *server) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->popup_scene_surface == NULL) {
			continue;
		}
		struct wlr_scene_surface *scene_surface = ime->popup_scene_surface;

		/* global rectangle of the text caret, if the app provides one */
		struct wlr_text_input_v3 *ti = server->focused_text_input;
		struct wlr_box caret = {0};
		bool have_caret = false;
		if (ti != NULL &&
				(ti->current.features &
					WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE)) {
			caret = ti->current.cursor_rectangle;
			have_caret = true;
		}

		int lx = server->cursor->x;
		int ly = server->cursor->y;
		if (have_caret && server->focused != NULL &&
				server->focused->xdg_toplevel != NULL &&
				server->focused->xdg_toplevel->base != NULL &&
				server->focused->xdg_toplevel->base->surface ==
					server->ime_focused_surface) {
			/* surface coordinates -> scene coordinates: the masked
			 * content sits at (-geometry) inside the toplevel's tree */
			struct wlr_xdg_surface *xdg =
				server->focused->xdg_toplevel->base;
			lx = server->focused->scene_tree->node.x -
				xdg->geometry.x + caret.x;
			ly = server->focused->scene_tree->node.y -
				xdg->geometry.y + caret.y;
			/* place the candidate list below the text line */
			ly += caret.height;
		}

		int popup_x = lx;
		int popup_y = ly;
		/* clamp into the output the cursor is on */
		struct wlr_output *output = wlr_output_layout_output_at(
			server->output_layout, lx, ly);
		if (output != NULL) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, output, &box);
			int pw = scene_surface->surface->current.width;
			int ph = scene_surface->surface->current.height;
			if (popup_x + pw > box.x + box.width) {
				popup_x = box.x + box.width - pw;
			}
			if (popup_y + ph > box.y + box.height) {
				popup_y = box.y + box.height - ph;
			}
			if (popup_x < box.x) {
				popup_x = box.x;
			}
			if (popup_y < box.y) {
				popup_y = box.y;
			}
		}

		wlr_scene_node_set_position(&scene_surface->buffer->node,
			popup_x, popup_y);
		/* make sure the candidate list sits above layer-shell surfaces */
		wlr_scene_node_raise_to_top(&scene_surface->buffer->node);

		/* tell the IM where the text caret is (relative to the popup) */
		if (ime->popup_surface != NULL) {
			struct wlr_box sbox = {
				.x = lx - popup_x,
				.y = ly - popup_y,
				.width = caret.width,
				.height = caret.height,
			};
			wlr_input_popup_surface_v2_send_text_input_rectangle(
				ime->popup_surface, &sbox);
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
	if (ime->input_method == NULL ||
			ime->input_method->keyboard_grab == NULL) {
		return;
	}
	/* never forward a release without the matching forwarded press */
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED &&
			!ime_key_forwarded(ime, event->keycode)) {
		return;
	}
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		ime_key_forwarded_add(ime, event->keycode);
	} else {
		ime_key_forwarded_remove(ime, event->keycode);
	}
	wlr_input_method_keyboard_grab_v2_send_key(
		ime->input_method->keyboard_grab, event->time_msec,
		event->keycode, event->state);
}

static void ime_keyboard_grab_modifiers(struct wl_listener *listener,
		void *data) {
	struct ime *ime = wl_container_of(listener, ime, keyboard_grab_modifiers);
	struct wlr_keyboard *keyboard = data;
	if (ime->input_method == NULL ||
			ime->input_method->keyboard_grab == NULL) {
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
	ime->forwarded_key_count = 0;
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
	wlr_log(WLR_DEBUG, "ime: connecting grab to keyboard %p (seat=%p vk=%d)",
		(void *)keyboard, (void *)wlr_seat_get_keyboard(ime->server->seat),
		is_keyboard_emulated_by_input_method(keyboard, ime->input_method));
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

static struct wlr_keyboard *ime_find_grab_keyboard_except(
		struct server *server, struct ime *ime,
		struct wlr_keyboard *except) {
	struct wlr_keyboard *seat_keyboard = wlr_seat_get_keyboard(server->seat);
	if (seat_keyboard != NULL && seat_keyboard != except &&
			keyboard_is_typing(&seat_keyboard->base) &&
			!is_keyboard_emulated_by_input_method(seat_keyboard,
				ime->input_method)) {
		return seat_keyboard;
	}
	struct keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		if (kb->keyboard != except &&
				keyboard_is_typing(&kb->keyboard->base) &&
				!is_keyboard_emulated_by_input_method(kb->keyboard,
					ime->input_method)) {
			return kb->keyboard;
		}
	}
	return NULL;
}

/* the keyboard the IM grab should listen to: the seat keyboard when it is
 * a real one, otherwise the first real keyboard.  The IM's own auxiliary
 * virtual keyboard is never used (its keys are the IM's own output and
 * would loop back into the grab).  Returns NULL when no real keyboard
 * exists yet; ime_attach_keyboard() connects the grab as soon as one
 * appears. */
static struct wlr_keyboard *ime_find_grab_keyboard(struct server *server,
		struct ime *ime) {
	return ime_find_grab_keyboard_except(server, ime, NULL);
}

static void ime_grab_keyboard(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab = data;
	struct server *server = ime->server;
	struct wlr_keyboard *keyboard = ime_find_grab_keyboard(server, ime);
	wlr_log(WLR_DEBUG, "ime: grab_keyboard event, grab keyboard=%p "
		"(seat=%p)", (void *)keyboard,
		(void *)wlr_seat_get_keyboard(server->seat));
	if (keyboard == NULL) {
		/* no real keyboard yet (e.g. the seat is held by the IM's own
		 * virtual keyboard, or grabbed before device hotplug):
		 * ime_attach_keyboard() connects the grab when one appears */
		return;
	}
	ime_keyboard_connect(ime, keyboard);
}

/* called from input.c when the grabbed keyboard is destroyed: drop the
 * grab listeners so wlr_keyboard_finish() sees an empty listener list,
 * then move the grab to another real keyboard if one exists */
void ime_detach_keyboard(struct server *server,
		struct wlr_keyboard *keyboard) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->keyboard == keyboard) {
			wl_list_remove(&ime->keyboard_grab_key.link);
			wl_list_remove(&ime->keyboard_grab_modifiers.link);
			ime->keyboard = NULL;
			ime->forwarded_key_count = 0;
			/* the grab destroy listener stays attached; it is removed when the
			 * grab or the input method itself is destroyed */
			/* move the grab to another real keyboard if one exists (never
			 * back to the keyboard being destroyed: it is still in the
			 * server->keyboards list at this point) */
			struct wlr_keyboard *next =
				ime_find_grab_keyboard_except(server, ime, keyboard);
			if (next != NULL) {
				ime_keyboard_connect(ime, next);
			}
		}
	}
}

/* called from input.c whenever a keyboard device appears; connects the IM
 * grab to it if the grab exists, the new keyboard is a real one and the
 * grab currently has no real keyboard (the IM may have grabbed while the
 * seat was held by the IM's own virtual keyboard, e.g. fcitx5 with
 * PersistentVirtualKeyboard).  A keyboard that is the IM's own re-injection
 * device never becomes the grab's keyboard. */
void ime_attach_keyboard(struct server *server,
		struct wlr_keyboard *keyboard) {
	struct ime *ime;
	wl_list_for_each(ime, &server->imes, link) {
		if (ime->input_method != NULL &&
				ime->input_method->keyboard_grab != NULL &&
				!is_keyboard_emulated_by_input_method(keyboard,
					ime->input_method) &&
				(ime->keyboard == NULL ||
					is_keyboard_emulated_by_input_method(ime->keyboard,
						ime->input_method))) {
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
 * active text input */
static void ime_commit(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, commit);
	struct wlr_input_method_v2 *context = data;
	struct server *server = ime->server;
	struct wlr_text_input_v3 *text_input = server->focused_text_input;
	wlr_log(WLR_DEBUG, "ime: IM commit active=%d text_input=%p serial=%u commit='%s' preedit='%s'",
		context->active, (void *)text_input, context->current_serial,
		context->current.commit_text ? context->current.commit_text : "",
		context->current.preedit.text ? context->current.preedit.text : "");
	if (text_input == NULL) {
		return;
	}
	struct wlr_input_method_v2_state *state = &context->current;
	if (state->preedit.text != NULL) {
		wlr_text_input_v3_send_preedit_string(text_input,
			state->preedit.text, state->preedit.cursor_begin,
			state->preedit.cursor_end);
	}
	if (state->commit_text != NULL) {
		wlr_text_input_v3_send_commit_string(text_input,
			state->commit_text);
	}
	if (state->delete.before_length != 0 ||
			state->delete.after_length != 0) {
		wlr_text_input_v3_send_delete_surrounding_text(text_input,
			state->delete.before_length, state->delete.after_length);
	}
	wlr_text_input_v3_send_done(text_input);
}

/* candidate window: place the popup surface in the overlay layer so
 * fcitx5's candidate list is visible */
static void ime_popup_destroy(struct wl_listener *listener, void *data) {
	struct ime *ime = wl_container_of(listener, ime, popup_destroy);
	wl_list_remove(&ime->popup_destroy.link);
	ime->popup_scene_surface = NULL;
	ime->popup_surface = NULL;
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
	wlr_scene_node_raise_to_top(&scene_surface->buffer->node);

	ime->popup_scene_surface = scene_surface;
	ime->popup_surface = popup;
	ime->popup_destroy.notify = ime_popup_destroy;
	wl_signal_add(&popup->events.destroy, &ime->popup_destroy);
	ime_update_popup(server);
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
	server->focused_text_input = NULL;
	/* with no IM, no text input may stay "focused" (enter was only sent
	 * because of the IM): send leave to all of them */
	update_text_inputs_focused_surface(server);
	free(ime);
}

void ime_new_input_method(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_ime);
	struct wlr_input_method_v2 *input_method = data;

	if (server->input_method != NULL) {
		wlr_log(WLR_INFO, "ignoring additional input method");
		wlr_input_method_v2_send_unavailable(input_method);
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

	/* the IM appeared after the surface was focused: now that there is an
	 * input method, text inputs of the focused client can receive enter
	 * and (once the client enables) activate the IM */
	update_text_inputs_focused_surface(server);
	update_active_text_input(server);
	ime_update_popup(server);
}

/* ------------------------------------------------------------------ */
/* text input (the focused app)                                       */
/* ------------------------------------------------------------------ */

static void text_input_commit(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, commit);
	struct server *server = ti->server;
	if (server->focused_text_input != ti->text_input) {
		return;
	}
	/* the app updated its text (surrounding text, cursor, ...): relay */
	ime_send_text_input_state(server, ti->text_input);
	ime_update_popup(server);
}

static void text_input_enable(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, enable);
	struct server *server = ti->server;
	update_active_text_input(server);
	if (server->focused_text_input == ti->text_input) {
		/* the app enabled text input on the focused surface: make sure the
		 * IM knows the client is ready and give it the current state */
		ime_send_text_input_state(server, ti->text_input);
		ime_update_popup(server);
	}
}

static void text_input_disable(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, disable);
	/* when the app disables its text input, it stops being active; the
	 * IM is deactivated if nothing else is active (this also covers the
	 * disable arriving after focus already moved to another client) */
	update_active_text_input(ti->server);
}

static void text_input_destroy(struct wl_listener *listener, void *data) {
	struct text_input *ti = wl_container_of(listener, ti, destroy);
	struct server *server = ti->server;
	wl_list_remove(&ti->destroy.link);
	wl_list_remove(&ti->enable.link);
	wl_list_remove(&ti->disable.link);
	wl_list_remove(&ti->commit.link);
	wl_list_remove(&ti->link);
	update_active_text_input(server);
	free(ti);
}

void ime_new_text_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_text_input);
	struct wlr_text_input_v3 *input = data;
	wlr_log(WLR_DEBUG, "ime: new text input %p (client pid?)",
		(void *)input);

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
	 * (e.g. GTK creates it on map): send enter now if appropriate and
	 * (re)compute the active text input */
	update_text_inputs_focused_surface(server);
	update_active_text_input(server);
}
