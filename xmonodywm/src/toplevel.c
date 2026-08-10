/*
 * toplevel.c - xdg-shell toplevel windows
 *
 * Window lifecycle (map/unmap/commit/destroy), window state (maximize,
 * minimize, fullscreen, move, close), xdg-decoration mode negotiation and
 * the foreign-toplevel handle that mirrors each window for taskbars.
 */

#include "server.h"

#include "ipc.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>

/* effective window geometry box in layout coordinates */
void toplevel_box(struct toplevel *tl, struct wlr_box *box) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	box->x = tl->scene_tree->node.x;
	box->y = tl->scene_tree->node.y;
	box->width = base != NULL ? base->geometry.width : 0;
	box->height = base != NULL ? base->geometry.height : 0;
}

/* output under the toplevel (its center), or the center output */
struct wlr_output *toplevel_output(struct server *server,
		struct toplevel *tl) {
	struct wlr_box box;
	toplevel_box(tl, &box);
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, box.x + box.width / 2.0,
		box.y + box.height / 2.0);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	return output;
}

/* clamp a position so the window never ends up underneath a layer-shell
 * bar's exclusive zone: the top-left corner goes into the work area (and
 * at least 40 px stay visible if the window is bigger than the area) */
static void clamp_to_work_area(struct server *server, int *x, int *y,
		int width, int height) {
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, *x + width / 2, *y + height / 2);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output == NULL) {
		return;
	}
	struct wlr_box area;
	get_work_area(server, output, &area);
	if (*x < area.x) {
		*x = area.x;
	}
	if (*y < area.y) {
		*y = area.y;
	}
	if (*x + 40 > area.x + area.width) {
		*x = area.x + area.width - 40;
	}
	if (*y + 40 > area.y + area.height) {
		*y = area.y + area.height - 40;
	}
}

/* after a layer-shell exclusive-zone change, existing windows that now sit
 * under the bar are moved back into the work area (the bar must never
 * cover them), and maximized windows are re-fitted to the new area */
void arrange_toplevels_work_area(struct server *server,
		struct wlr_output *output) {
	struct wlr_box area;
	get_work_area(server, output, &area);
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (base == NULL || !base->surface->mapped || tl->minimized ||
				tl->fullscreen) {
			/* fullscreen windows deliberately cover the whole output,
			 * bars included */
			continue;
		}
		struct wlr_box box;
		toplevel_box(tl, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}
		if (toplevel_output(server, tl) != output) {
			continue;
		}
		if (tl->xdg_toplevel->current.maximized) {
			/* re-fit the maximized window to the (possibly shrunk) area */
			if (box.x != area.x || box.y != area.y ||
					box.width != area.width || box.height != area.height) {
				wlr_xdg_toplevel_set_size(tl->xdg_toplevel, area.width,
					area.height);
				wlr_scene_node_set_position(&tl->scene_tree->node, area.x,
					area.y);
				update_toplevel_decoration(tl);
			}
			continue;
		}
		int x = box.x;
		int y = box.y;
		clamp_to_work_area(server, &x, &y, box.width, box.height);
		if (x != box.x || y != box.y) {
			wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
			update_toplevel_decoration(tl);
		}
	}
}


/* the next/previous mapped toplevel relative to tl, wrapping around the
 * list. include_minimized controls whether hidden windows are eligible.
 * Returns NULL when there is no such toplevel. */
struct toplevel *neighbor_toplevel(struct server *server,
		struct toplevel *tl, bool next, bool include_minimized) {
	struct wl_list *head = &server->toplevels;
	struct wl_list *cur = &tl->link;
	struct wl_list *iter = next ? cur->next : cur->prev;
	while (iter != cur) {
		if (iter == head) {
			/* wrap around the circular list */
			iter = next ? head->next : head->prev;
			continue;
		}
		struct toplevel *candidate = wl_container_of(iter, candidate, link);
		if (candidate->xdg_toplevel->base != NULL &&
				candidate->xdg_toplevel->base->surface->mapped &&
				(include_minimized || !candidate->minimized)) {
			return candidate;
		}
		iter = next ? iter->next : iter->prev;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* window state                                                       */
/* ------------------------------------------------------------------ */

void close_toplevel(struct toplevel *tl) {
	if (tl->xdg_toplevel->base != NULL) {
		wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
	}
}

void set_fullscreen(struct server *server, struct toplevel *tl,
		bool fullscreen) {
	if (tl->xdg_toplevel->base == NULL ||
			tl->xdg_toplevel->current.fullscreen == fullscreen) {
		return;
	}
	tl->fullscreen = fullscreen;
	if (fullscreen) {
		/* remember the floating geometry so it can be restored later */
		toplevel_box(tl, &tl->restore_box);
		tl->has_restore_box = true;

		struct wlr_output *output = toplevel_output(server, tl);
		if (output != NULL) {
			struct wlr_box area;
			wlr_output_layout_get_box(server->output_layout, output, &area);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, area.width,
				area.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, area.x,
				area.y);
		} else {
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
	} else {
		if (tl->has_restore_box && tl->restore_box.width > 0) {
			int x = tl->restore_box.x;
			int y = tl->restore_box.y;
			/* never restore underneath a layer-shell bar */
			clamp_to_work_area(server, &x, &y, tl->restore_box.width,
				tl->restore_box.height);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				tl->restore_box.width, tl->restore_box.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
		}
	}
	wlr_xdg_toplevel_set_fullscreen(tl->xdg_toplevel, fullscreen);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(tl->fthandle,
			fullscreen);
	}
	update_toplevel_decoration(tl);
	/* notify status bars */
	ipc_send_window_event(server, "window_full", tl);
}

void set_maximized(struct server *server, struct toplevel *tl,
		bool maximized) {
	if (tl->xdg_toplevel->base == NULL ||
			tl->xdg_toplevel->current.maximized == maximized) {
		return;
	}
	if (maximized) {
		/* remember the floating geometry so dragging the title bar of the
		 * maximized window can restore it (Windows behavior) */
		toplevel_box(tl, &tl->restore_box);
		tl->has_restore_box = true;

		struct wlr_output *output = toplevel_output(server, tl);
		struct wlr_box area;
		if (output != NULL) {
			get_work_area(server, output, &area);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, area.width,
				area.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, area.x, area.y);
		} else {
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
	} else {
		/* 0x0 lets the client pick its own size again */
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
	}
	wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, maximized);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_maximized(tl->fthandle, maximized);
	}
	update_toplevel_decoration(tl);
}

/* un-maximize back to the previously saved geometry (drag of a maximized
 * window's title bar) */
void restore_maximized_toplevel(struct toplevel *tl) {
	if (tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->current.maximized) {
		return;
	}
	if (tl->has_restore_box && tl->restore_box.width > 0) {
		int x = tl->restore_box.x;
		int y = tl->restore_box.y;
		/* the work area may have shrunk since the window was maximized (a
		 * bar appeared): clamp the restore into the work area so the window
		 * never lands back underneath the bar */
		clamp_to_work_area(tl->server, &x, &y, tl->restore_box.width,
			tl->restore_box.height);
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, tl->restore_box.width,
			tl->restore_box.height);
		wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
	}
	wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, false);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_maximized(tl->fthandle, false);
	}
	update_toplevel_decoration(tl);
}

void set_minimized(struct server *server, struct toplevel *tl,
		bool minimized) {
	if (tl->minimized == minimized) {
		return;
	}
	tl->minimized = minimized;
	/* minimizing only hides the scene node: the window keeps its position and
	 * size, so restoring it (focus cycle, foreign-toplevel activate) puts it
	 * back exactly where it was */
	wlr_scene_node_set_enabled(&tl->scene_tree->node, !minimized);
	if (tl->deco_tree != NULL) {
		wlr_scene_node_set_enabled(&tl->deco_tree->node, !minimized);
	}
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_minimized(tl->fthandle, minimized);
	}
	if (minimized && server->focused == tl) {
		/* hand keyboard/cursor focus to the previous visible window */
		struct toplevel *prev = neighbor_toplevel(server, tl, false, false);
		server->focused = NULL;
		if (tl->xdg_toplevel->base != NULL) {
			wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, false);
		}
		wlr_seat_keyboard_clear_focus(server->seat);
		if (prev != NULL) {
			focus_toplevel(server, prev);
		} else {
			ipc_send_window_event(server, "window_focus", NULL);
			ime_set_focus(server, NULL);
		}
	}
	/* hiding the window may expose a different surface under the cursor */
	update_cursor_style(server);
}

void focus_toplevel(struct server *server, struct toplevel *tl) {
	if (tl->minimized || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->surface->mapped) {
		return;
	}
	struct toplevel *prev = server->focused;
	struct wlr_scene_node *raise = tl->deco_tree != NULL
		? &tl->deco_tree->node : &tl->scene_tree->node;
	if (prev == tl) {
		wlr_scene_node_raise_to_top(raise);
		return;
	}
	if (prev != NULL && prev->xdg_toplevel->base != NULL) {
		wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
		if (prev->fthandle != NULL) {
			wlr_foreign_toplevel_handle_v1_set_activated(prev->fthandle, false);
		}
	}
	server->focused = tl;
	wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, true);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_activated(tl->fthandle, true);
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat,
			tl->xdg_toplevel->base->surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat,
			tl->xdg_toplevel->base->surface, NULL, 0, NULL);
	}
	wlr_scene_node_raise_to_top(raise);
	/* repaint the borders: the new focused window switches to the focused
	 * color, the previously focused one to the unfocused color */
	if (prev != NULL) {
		update_toplevel_decoration(prev);
	}
	update_toplevel_decoration(tl);
	/* tell the input method which surface now owns text input */
	ime_set_focus(server, tl->xdg_toplevel->base->surface);
	/* notify status bars that the focus changed */
	ipc_send_window_event(server, "window_focus", tl);
}

void update_toplevel_output(struct server *server, struct toplevel *tl) {
	if (tl->fthandle == NULL || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->surface->mapped) {
		return;
	}
	struct wlr_output *output = toplevel_output(server, tl);
	if (output == tl->last_output) {
		return;
	}
	if (tl->last_output != NULL) {
		wlr_foreign_toplevel_handle_v1_output_leave(tl->fthandle,
			tl->last_output);
	}
	tl->last_output = output;
	if (output != NULL) {
		wlr_foreign_toplevel_handle_v1_output_enter(tl->fthandle, output);
	}
}

/* ------------------------------------------------------------------ */
/* xdg-shell toplevels                                                */
/* ------------------------------------------------------------------ */

static void toplevel_unfocus(struct server *server, struct toplevel *tl) {
	if (server->focused == tl) {
		/* the focused window is going away: hand focus to the previous
		 * visible window */
		struct toplevel *prev = neighbor_toplevel(server, tl, false, false);
		server->focused = NULL;
		wlr_seat_keyboard_clear_focus(server->seat);
		if (prev != NULL) {
			focus_toplevel(server, prev);
		} else {
			ipc_send_window_event(server, "window_focus", NULL);
			ime_set_focus(server, NULL);
		}
	}
	if (server->zone_toplevel == tl || server->move_toplevel == tl) {
		end_move(server);
	}
	if (server->resize_toplevel == tl) {
		end_resize(server);
	}
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, map);
	struct server *server = tl->server;

	if (!tl->positioned) {
		tl->positioned = true;
		struct wlr_box box;
		toplevel_box(tl, &box);
		if (box.width > 0 && box.height > 0) {
			int x = server->cursor->x - box.width / 2;
			int y = server->cursor->y - box.height / 2;
			/* clamp into the work area, not the full output box: a
			 * top-anchored bar (layer-shell exclusive zone) must never
			 * cover a freshly mapped window, even when the cursor is
			 * over the bar and the window is centered on it */
			clamp_to_work_area(server, &x, &y, box.width, box.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
		}
	}
	/* notify status bars first, then the state/focus events */
	tl->ipc_added = true;
	ipc_send_window_event(server, "window_added", tl);
	/* honor fullscreen/maximize requests that arrived before the first
	 * commit (the surface is initialized and mapped by now) */
	if (tl->xdg_toplevel->requested.fullscreen) {
		set_fullscreen(server, tl, true);
	} else if (tl->xdg_toplevel->requested.maximized) {
		set_maximized(server, tl, true);
	}
	focus_toplevel(server, tl);
	update_toplevel_output(server, tl);
	/* a window mapped under a stationary cursor must immediately show the
	 * right cursor (title zone / resize edge), without waiting for motion */
	update_toplevel_decoration(tl);
	update_cursor_style(server);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, unmap);
	toplevel_unfocus(tl->server, tl);
	if (tl->masked != NULL) {
		wlr_scene_buffer_set_buffer(tl->masked, NULL);
	}
	update_toplevel_decoration(tl);
	update_cursor_style(tl->server);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, toplevel_destroy);
	struct server *server = tl->server;

	toplevel_unfocus(server, tl);
	wl_list_remove(&tl->link);

	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_destroy(tl->fthandle);
	}

	/* remove all listeners attached to the toplevel and its surface; the
	 * xdg surface listeners (tl->destroy, tl->new_popup) stay linked until
	 * the xdg surface itself is destroyed */
	mask_toplevel_destroy(tl);
	wl_list_remove(&tl->toplevel_destroy.link);
	wl_list_remove(&tl->map.link);
	wl_list_remove(&tl->unmap.link);
	wl_list_remove(&tl->commit.link);
	wl_list_remove(&tl->request_maximize.link);
	wl_list_remove(&tl->request_minimize.link);
	wl_list_remove(&tl->request_fullscreen.link);
	wl_list_remove(&tl->request_move.link);
	wl_list_remove(&tl->set_title.link);
	wl_list_remove(&tl->set_app_id.link);
	wl_list_remove(&tl->new_popup.link);
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, destroy);
	wl_list_remove(&tl->destroy.link);
	/* notify status bars before the window disappears */
	if (tl->ipc_added) {
		ipc_send_window_event(tl->server, "window_removed", tl);
	}
	/* the content tree is destroyed by wlroots' own xdg-surface destroy
	 * handler (registered before ours), so only the border tree remains */
	if (tl->deco_tree != NULL) {
		wlr_scene_node_destroy(&tl->deco_tree->node);
	}
	free(tl->app_id);
	free(tl);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, commit);
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;

	/* per xdg-shell the compositor must reply to the initial commit with
	 * the first configure */
	if (base != NULL && base->initial_commit) {
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
	}

	update_toplevel_output(tl->server, tl);

	/* the first configure must only be sent once the xdg surface is
	 * initialized; respond to the client's decoration mode (or our
	 * default) on its first commit */
	if (tl->decoration != NULL && !tl->decoration_configured &&
			base != NULL && base->initialized) {
		tl->decoration_configured = true;
		if (tl->decoration_mode !=
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
			wlr_xdg_toplevel_decoration_v1_set_mode(tl->decoration,
				tl->decoration_mode);
		}
	}

	/* geometry (and thus the border and the resize/title zones under the
	 * cursor) may have changed */
	blur_toplevel_commit(tl);
	/* rounded-corner masked content tracks the latest committed buffer */
	mask_toplevel_content(tl);
	update_toplevel_decoration(tl);
	update_cursor_style(tl->server);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_maximize);
	/* early requests (before the first commit) are applied at map time */
	if (tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->initialized) {
		return;
	}
	set_maximized(tl->server, tl, true);
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_minimize);
	set_minimized(tl->server, tl, true);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_fullscreen);
	(void)data;
	/* early requests (before the first commit) are applied at map time */
	if (tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->initialized) {
		return;
	}
	set_fullscreen(tl->server, tl, tl->xdg_toplevel->requested.fullscreen);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, request_move);
	struct server *server = tl->server;
	if (tl->minimized || server->moving || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->surface->mapped) {
		return;
	}
	begin_move(server, tl, server->cursor->x, server->cursor->y);
	focus_toplevel(server, tl);
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, set_title);
	if (tl->fthandle != NULL && tl->xdg_toplevel->title != NULL) {
		wlr_foreign_toplevel_handle_v1_set_title(tl->fthandle,
			tl->xdg_toplevel->title);
	}
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, set_app_id);
	if (tl->xdg_toplevel->app_id != NULL) {
		free(tl->app_id);
		tl->app_id = strdup(tl->xdg_toplevel->app_id);
		if (tl->fthandle != NULL) {
			wlr_foreign_toplevel_handle_v1_set_app_id(tl->fthandle,
				tl->xdg_toplevel->app_id);
		}
	}
}

static void xdg_toplevel_new_popup(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, new_popup);
	struct wlr_xdg_popup *popup = data;
	struct wlr_output *output = toplevel_output(tl->server, tl);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(tl->server->output_layout, output, &box);
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

static void foreign_toplevel_request_maximize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_request_maximize);
	struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
	set_maximized(tl->server, tl, event->maximized);
}

static void foreign_toplevel_request_minimize(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_request_minimize);
	struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
	set_minimized(tl->server, tl, event->minimized);
}

static void foreign_toplevel_request_activate(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_request_activate);
	struct wlr_foreign_toplevel_handle_v1_activated_event *event = data;
	(void)event;
	if (tl->minimized) {
		set_minimized(tl->server, tl, false);
	}
	focus_toplevel(tl->server, tl);
}

static void foreign_toplevel_request_close(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_request_close);
	close_toplevel(tl);
}

static void foreign_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, ft_destroy);
	wl_list_remove(&tl->ft_request_maximize.link);
	wl_list_remove(&tl->ft_request_minimize.link);
	wl_list_remove(&tl->ft_request_activate.link);
	wl_list_remove(&tl->ft_request_close.link);
	wl_list_remove(&tl->ft_destroy.link);
	tl->fthandle = NULL;
}

/* ------------------------------------------------------------------ */
/* masked content (rounded corners)                                   */
/* ------------------------------------------------------------------ */

/* the xdg surface's own scene node is replaced by the masked buffer, so
 * output enter/leave, presentation feedback and frame done must be
 * forwarded to the client surface from tl->masked */
static void mask_output_enter(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, mask_enter);
	struct wlr_scene_output *scene_output = data;
	if (tl->xdg_toplevel->base != NULL) {
		wlr_surface_send_enter(tl->xdg_toplevel->base->surface,
			scene_output->output);
	}
}

static void mask_output_leave(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, mask_leave);
	struct wlr_scene_output *scene_output = data;
	if (tl->xdg_toplevel->base != NULL) {
		wlr_surface_send_leave(tl->xdg_toplevel->base->surface,
			scene_output->output);
	}
}

static void mask_output_sample(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, mask_sample);
	struct wlr_scene_output_sample_event *event = data;
	if (!event->direct_scanout && tl->xdg_toplevel->base != NULL) {
		wlr_presentation_surface_textured_on_output(
			tl->xdg_toplevel->base->surface, event->output->output);
	}
}

static void mask_frame_done(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, mask_frame);
	struct timespec *now = data;
	if (tl->xdg_toplevel->base != NULL) {
		wlr_surface_send_frame_done(tl->xdg_toplevel->base->surface, now);
	}
}

static void mask_subsurface_destroy(struct wl_listener *listener, void *data) {
	struct mask_subsurface *ms = wl_container_of(listener, ms, destroy);
	(void)data;
	wl_list_remove(&ms->destroy.link);
	wl_list_remove(&ms->link);
	free(ms);
	/* the scene tree is destroyed by wlr_scene_subsurface_tree's own
	 * surface-destroy handler */
}

static void mask_subsurface_add(struct toplevel *tl,
		struct wlr_subsurface *subsurface) {
	struct mask_subsurface *ms = calloc(1, sizeof(*ms));
	if (ms == NULL) {
		return;
	}
	ms->tl = tl;
	ms->subsurface = subsurface;
	ms->tree = wlr_scene_subsurface_tree_create(tl->scene_tree,
		subsurface->surface);
	if (ms->tree == NULL) {
		free(ms);
		return;
	}
	ms->destroy.notify = mask_subsurface_destroy;
	wl_signal_add(&subsurface->events.destroy, &ms->destroy);
	wl_list_insert(tl->subsurfaces.prev, &ms->link);
}

static void xdg_toplevel_new_subsurface(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, new_subsurface);
	mask_subsurface_add(tl, data);
}

/* keep every subsurface glued to its surface position (subsurface
 * positions live in the parent surface's state and change on commit) */
static void mask_subsurfaces_reposition(struct toplevel *tl) {
	struct mask_subsurface *ms;
	wl_list_for_each(ms, &tl->subsurfaces, link) {
		wlr_scene_node_set_position(&ms->tree->node,
			-tl->xdg_toplevel->base->geometry.x + ms->subsurface->current.x,
			-tl->xdg_toplevel->base->geometry.y + ms->subsurface->current.y);
	}
}

/* re-render the rounded-corner masked content from the surface's current
 * buffer; called on every commit that carries a buffer so animations and
 * damaged partial redraws stay current */
void mask_toplevel_content(struct toplevel *tl) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (tl->masked == NULL || base == NULL || !base->initialized ||
			!base->surface->mapped || base->surface->buffer == NULL) {
		return;
	}
	struct wlr_surface *surface = base->surface;
	int w = surface->current.width;
	int h = surface->current.height;
	if (w <= 0 || h <= 0) {
		return;
	}
	/* fullscreen windows are square: no rounding */
	float radius = tl->fullscreen ? 0.0f
		: (float)(CONFIG_BORDER_RADIUS - CONFIG_BORDER_WIDTH);
	struct wlr_buffer *buf = content_mask_buffer(tl->server, w, h);
	if (buf == NULL) {
		return;
	}
	if (!content_mask_render(tl->server, surface, buf, w, h, radius)) {
		wlr_buffer_drop(buf);
		return;
	}
	/* the scene locks the buffer; drop our reference */
	wlr_scene_buffer_set_buffer(tl->masked, buf);
	wlr_buffer_drop(buf);
	/* keep the content aligned with the xdg geometry */
	wlr_scene_node_set_position(&tl->masked->node,
		-base->geometry.x, -base->geometry.y);
	mask_subsurfaces_reposition(tl);
}

/* remove all listeners owned by the masked-content bookkeeping; must run
 * before the deco tree (and thus tl->masked) is destroyed */
void mask_toplevel_destroy(struct toplevel *tl) {
	if (tl->masked != NULL) {
		wl_list_remove(&tl->mask_enter.link);
		wl_list_remove(&tl->mask_leave.link);
		wl_list_remove(&tl->mask_sample.link);
		wl_list_remove(&tl->mask_frame.link);
	}
	wl_list_remove(&tl->new_subsurface.link);
	struct mask_subsurface *ms, *tmp;
	wl_list_for_each_safe(ms, tmp, &tl->subsurfaces, link) {
		wl_list_remove(&ms->destroy.link);
		wl_list_remove(&ms->link);
		free(ms);
	}
}

void server_new_toplevel(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;
	struct wlr_xdg_surface *base = xdg_toplevel->base;

	struct toplevel *tl = calloc(1, sizeof(*tl));
	if (tl == NULL) {
		return;
	}
	tl->server = server;
	tl->xdg_toplevel = xdg_toplevel;
	tl->decoration_mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE;
	base->data = tl;
	tl->id = ++server->next_window_id;
	tl->app_id = strdup(xdg_toplevel->app_id != NULL
		? xdg_toplevel->app_id : "");

	/* the border lives in its own tree behind the window; the xdg content
	 * tree is its child so raising the window raises the border with it */
	tl->deco_tree = wlr_scene_tree_create(server->layers[LAYER_TOPLEVELS]);
	if (tl->deco_tree == NULL) {
		free(tl);
		return;
	}
	/* blur node goes at the bottom of the decoration tree, below the
	 * content, so the blurred backdrop shines through transparent
	 * backgrounds */
	blur_toplevel_init(tl);
	tl->scene_tree = wlr_scene_tree_create(tl->deco_tree);
	if (tl->scene_tree == NULL) {
		wlr_scene_node_destroy(&tl->deco_tree->node);
		free(tl);
		return;
	}
	/* content is re-rendered through a rounded-corner alpha mask (mask.c):
	 * wlr_scene cannot clip an xdg surface to a rounded rectangle */
	tl->masked = wlr_scene_buffer_create(tl->scene_tree, NULL);
	if (tl->masked == NULL) {
		wlr_scene_node_destroy(&tl->deco_tree->node);
		free(tl);
		return;
	}
	xdg_surface_tag(tl->scene_tree, TAG_TOPLEVEL, tl);
	wl_list_init(&tl->subsurfaces);
	/* the xdg surface's own scene node is gone: forward output/frame
	 * events to the client surface from the masked buffer */
	tl->mask_enter.notify = mask_output_enter;
	wl_signal_add(&tl->masked->events.output_enter, &tl->mask_enter);
	tl->mask_leave.notify = mask_output_leave;
	wl_signal_add(&tl->masked->events.output_leave, &tl->mask_leave);
	tl->mask_sample.notify = mask_output_sample;
	wl_signal_add(&tl->masked->events.output_sample, &tl->mask_sample);
	tl->mask_frame.notify = mask_frame_done;
	wl_signal_add(&tl->masked->events.frame_done, &tl->mask_frame);
	tl->new_subsurface.notify = xdg_toplevel_new_subsurface;
	wl_signal_add(&base->surface->events.new_subsurface, &tl->new_subsurface);
	/* subsurfaces that already existed before this listener was added */
	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &base->surface->current.subsurfaces_below,
			current.link) {
		mask_subsurface_add(tl, subsurface);
	}
	wl_list_for_each(subsurface, &base->surface->current.subsurfaces_above,
			current.link) {
		mask_subsurface_add(tl, subsurface);
	}

	tl->deco_border = wlr_scene_buffer_create(tl->deco_tree, NULL);
	if (tl->deco_border != NULL) {
		tl->deco_border->point_accepts_input = border_buffer_no_input;
	}

	tl->fthandle = wlr_foreign_toplevel_handle_v1_create(
		server->foreign_toplevel_manager);
	if (tl->fthandle != NULL) {
		tl->fthandle->data = tl;
		if (xdg_toplevel->title != NULL) {
			wlr_foreign_toplevel_handle_v1_set_title(tl->fthandle,
				xdg_toplevel->title);
		}
		if (xdg_toplevel->app_id != NULL) {
			wlr_foreign_toplevel_handle_v1_set_app_id(tl->fthandle,
				xdg_toplevel->app_id);
		}

		tl->ft_request_maximize.notify = foreign_toplevel_request_maximize;
		wl_signal_add(&tl->fthandle->events.request_maximize,
			&tl->ft_request_maximize);
		tl->ft_request_minimize.notify = foreign_toplevel_request_minimize;
		wl_signal_add(&tl->fthandle->events.request_minimize,
			&tl->ft_request_minimize);
		tl->ft_request_activate.notify = foreign_toplevel_request_activate;
		wl_signal_add(&tl->fthandle->events.request_activate,
			&tl->ft_request_activate);
		tl->ft_request_close.notify = foreign_toplevel_request_close;
		wl_signal_add(&tl->fthandle->events.request_close,
			&tl->ft_request_close);
		tl->ft_destroy.notify = foreign_toplevel_destroy;
		wl_signal_add(&tl->fthandle->events.destroy, &tl->ft_destroy);
	}

	tl->map.notify = xdg_toplevel_map;
	wl_signal_add(&base->surface->events.map, &tl->map);
	tl->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&base->surface->events.unmap, &tl->unmap);
	tl->destroy.notify = xdg_surface_destroy;
	wl_signal_add(&base->events.destroy, &tl->destroy);
	tl->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&base->surface->events.commit, &tl->commit);
	tl->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &tl->request_maximize);
	tl->request_minimize.notify = xdg_toplevel_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &tl->request_minimize);
	tl->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen,
		&tl->request_fullscreen);
	tl->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &tl->request_move);
	tl->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &tl->set_title);
	tl->set_app_id.notify = xdg_toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &tl->set_app_id);
	tl->new_popup.notify = xdg_toplevel_new_popup;
	wl_signal_add(&base->events.new_popup, &tl->new_popup);

	/* the toplevel destroy handler (frees the foreign toplevel handle and
	 * unlinks the listeners above) must run before wlroots asserts that the
	 * toplevel signals are empty, i.e. on xdg_toplevel->events.destroy */
	tl->toplevel_destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &tl->toplevel_destroy);

	wl_list_insert(server->toplevels.prev, &tl->link);
}

/* ------------------------------------------------------------------ */
/* xdg-decoration                                                     */
/* ------------------------------------------------------------------ */

static void decoration_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, deco_destroy);
	wl_list_remove(&tl->deco_request_mode.link);
	wl_list_remove(&tl->deco_destroy.link);
	tl->decoration = NULL;
}

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct toplevel *tl = decoration->toplevel->base->data;
	if (tl == NULL) {
		return;
	}
	tl->decoration_mode = decoration->requested_mode;
	if (tl->xdg_toplevel->base != NULL && tl->xdg_toplevel->base->initialized &&
			tl->decoration_mode !=
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			tl->decoration_mode);
	}
}

void server_new_decoration(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct toplevel *tl = decoration->toplevel->base->data;
	if (tl == NULL) {
		return;
	}
	tl->decoration = decoration;
	decoration->data = tl;

	tl->deco_request_mode.notify = decoration_request_mode;
	wl_signal_add(&decoration->events.request_mode, &tl->deco_request_mode);
	tl->deco_destroy.notify = decoration_destroy;
	wl_signal_add(&decoration->events.destroy, &tl->deco_destroy);

	/* default: let clients draw their own decorations unless they explicitly
	 * ask for server-side (we never draw any, the top-10px zone takes over
	 * instead). The actual configure is sent on the first commit. */
	tl->decoration_mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}
