/*
 * toplevel.c - xdg-shell toplevel windows
 *
 * Window lifecycle (map/unmap/commit/destroy), window state (maximize,
 * minimize, fullscreen, move, close), xdg-decoration mode negotiation and
 * the foreign-toplevel handle that mirrors each window for taskbars.
 */

#include "server.h"

#include "ipc.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>

/* effective window geometry box in layout coordinates.  Normally this is
 * the xdg window geometry (the window bounds per xdg-shell, excluding drop
 * shadows and CSD margins), which starts exactly at the scene node because
 * the content buffer is placed at -geometry.  Some clients (Chromium/
 * Electron frameless windows, e.g. QQ's login window) report a window
 * geometry smaller than the opaque surface they actually commit; the mask
 * pass detects that (mask.c: the pixels outside the geometry are opaque,
 * not a transparent shadow) and sets wrap_surface, in which case the box
 * wraps the committed surface instead. */
void toplevel_box(struct toplevel *tl, struct wlr_box *box) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	box->x = tl->scene_tree->node.x;
	box->y = tl->scene_tree->node.y;
	int gx = base != NULL ? base->geometry.x : 0;
	int gy = base != NULL ? base->geometry.y : 0;
	int gw = base != NULL ? base->geometry.width : 0;
	int gh = base != NULL ? base->geometry.height : 0;
	int sw = base != NULL && base->surface != NULL
		? base->surface->current.width : 0;
	int sh = base != NULL && base->surface != NULL
		? base->surface->current.height : 0;
	if (tl->wrap_surface && sw > 0 && sh > 0) {
		/* the surface really extends beyond the geometry with opaque
		 * pixels: the border and the interaction zones must wrap it */
		box->x -= gx;
		box->y -= gy;
		box->width = sw;
		box->height = sh;
	} else {
		box->width = gw;
		box->height = gh;
	}
}

/* a dialog / transient window: one that declared a parent toplevel via
 * xdg_toplevel.set_parent (GTK/Qt dialogs do this).  wlroots keeps
 * toplevel->parent updated (including clearing it when the parent
 * unmaps), so a live check is always current. */
bool toplevel_is_dialog(struct toplevel *tl) {
	return tl->xdg_toplevel != NULL && tl->xdg_toplevel->parent != NULL;
}

/* a window pinned to a fixed size via min == max constraints (e.g. QQ's
 * login window, splash screens): it cannot meaningfully be resized,
 * maximized or minimized.  A dimension is fixed only when min > 0 and
 * equals max (min=0/max=0 means "unconstrained"). */
bool toplevel_is_fixed_size(struct toplevel *tl) {
	if (tl->xdg_toplevel == NULL) {
		return false;
	}
	return tl->xdg_toplevel->current.min_width > 0 &&
		tl->xdg_toplevel->current.min_width ==
			tl->xdg_toplevel->current.max_width &&
		tl->xdg_toplevel->current.min_height > 0 &&
		tl->xdg_toplevel->current.min_height ==
			tl->xdg_toplevel->current.max_height;
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
	/* when a bar sits above the work area, the top limit is taken from the
	 * maximized geometry itself (maximized_box truncates CONFIG_BORDER_WIDTH
	 * and the bar gap to int exactly like maximize does), so restore always
	 * lands on the same pixel the maximized window occupied, shifted by
	 * CONFIG_BAR_TOP_OVERLAP: set it to -CONFIG_MAXIMIZED_GAP_BAR and
	 * restore is pixel-identical to maximize, whatever the values are */
	struct wlr_box out;
	wlr_output_layout_get_box(server->output_layout, output, &out);
	int top_limit = area.y;
	if (area.y > out.y) {           /* bar at the top */
		struct wlr_box mbox;
		maximized_box(server, output, &mbox);
		top_limit = mbox.y + CONFIG_BAR_TOP_OVERLAP
			+ CONFIG_MAXIMIZED_GAP_BAR;
	}
	if (*x < area.x) {
		*x = area.x;
	}
	if (*y < top_limit) {
		*y = top_limit;
	}
	/* right edge: a bar there clamps the window flush to the work area
	 * edge (never underneath it); without a bar at least 40 px stay
	 * visible when the window is bigger than the area */
	if (area.x + area.width < out.x + out.width) { /* bar at the right */
		if (*x + width > area.x + area.width) {
			*x = area.x + area.width - width;
		}
	} else if (*x + 40 > area.x + area.width) {
		*x = area.x + area.width - 40;
	}
	/* bottom edge: same rule - a bar there clamps the window's bottom
	 * to the work area edge, so a restored/mapped window can never slide
	 * underneath the bar */
	if (area.y + area.height < out.y + out.height) { /* bar at the bottom */
		if (*y + height > area.y + area.height) {
			*y = area.y + area.height - height;
		}
	} else if (*y + 40 > area.y + area.height) {
		*y = area.y + area.height - 40;
	}
}

/* geometry of a maximized window: the window box is inset so that the
 * border ring's outer edge sits CONFIG_MAXIMIZED_GAP (1 px) away from the
 * work area on every side, except the side(s) facing a layer-shell status
 * bar's exclusive zone (bar at the top or bottom of the output), which get
 * CONFIG_MAXIMIZED_GAP_BAR (0 px by default) so the ring sits flush with
 * the bar.  The bar side is detected by comparing the work area with the
 * output box: an edge of the work area that lies inside the output box is
 * adjacent to a bar. */
void maximized_box(struct server *server, struct wlr_output *output,
		struct wlr_box *box) {
	struct wlr_box area;
	get_work_area(server, output, &area);
	struct wlr_box out;
	wlr_output_layout_get_box(server->output_layout, output, &out);

	/* the visible ring is drawn CONFIG_BORDER_WIDTH px outside the window
	 * box, so the box itself must be inset by that much plus the requested
	 * gap for the ring to land at the gap */
	int extent = CONFIG_BORDER_WIDTH;
	int gap = extent + CONFIG_MAXIMIZED_GAP;
	int gap_bar = extent + CONFIG_MAXIMIZED_GAP_BAR;

	box->x = area.x + gap;
	box->y = area.y + gap;
	box->width = area.width - 2 * gap;
	box->height = area.height - 2 * gap;

	if (area.x > out.x) {           /* bar at the left */
		box->x = area.x + gap_bar;
		box->width -= gap_bar - gap;
	}
	if (area.y > out.y) {           /* bar at the top */
		box->y = area.y + gap_bar;
		box->height -= gap_bar - gap;
	}
	if (area.x + area.width < out.x + out.width) { /* bar at the right */
		box->width -= gap_bar - gap;
	}
	if (area.y + area.height < out.y + out.height) { /* bar at the bottom */
		box->height -= gap_bar - gap;
	}
}

/* geometry of a fullscreen window: inset from the raw output box so the
 * border ring's outer edge sits CONFIG_FULLSCREEN_GAP px from the screen
 * edge (CONFIG_MAXIMIZED_GAP is the maximized equivalent; fullscreen
 * deliberately covers layer-shell bars, so this uses the output box, not
 * the work area).  The ring is drawn CONFIG_BORDER_WIDTH px outside the
 * window box, so the box itself is inset by that plus the requested gap,
 * exactly like maximized_box. */
static void fullscreen_box(struct server *server, struct wlr_output *output,
		struct wlr_box *box) {
	struct wlr_box out;
	wlr_output_layout_get_box(server->output_layout, output, &out);
	int gap = CONFIG_BORDER_WIDTH + CONFIG_FULLSCREEN_GAP;
	box->x = out.x + gap;
	box->y = out.y + gap;
	box->width = out.width - 2 * gap;
	box->height = out.height - 2 * gap;
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
			struct wlr_box mbox;
			maximized_box(server, output, &mbox);
			if (box.x != mbox.x || box.y != mbox.y ||
					box.width != mbox.width || box.height != mbox.height) {
				wlr_xdg_toplevel_set_size(tl->xdg_toplevel, mbox.width,
					mbox.height);
				wlr_scene_node_set_position(&tl->scene_tree->node, mbox.x,
					mbox.y);
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
	tl->user_moved = true; /* fullscreen state: stop auto-centering */
	tl->fullscreen = fullscreen;
	if (fullscreen) {
		/* remember the pre-fullscreen geometry so it can be restored
		 * later.  Keep this separate from restore_box: entering fullscreen
		 * from a maximized window must not overwrite the floating geometry
		 * that maximize saved. */
		toplevel_box(tl, &tl->fullscreen_restore_box);
		tl->has_fullscreen_restore_box = true;

		struct wlr_output *output = toplevel_output(server, tl);
		if (output != NULL) {
			struct wlr_box fbox;
			fullscreen_box(server, output, &fbox);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, fbox.width,
				fbox.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, fbox.x,
				fbox.y);
		} else {
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
	} else {
		if (tl->has_fullscreen_restore_box &&
				tl->fullscreen_restore_box.width > 0) {
			int x = tl->fullscreen_restore_box.x;
			int y = tl->fullscreen_restore_box.y;
			/* never restore underneath a layer-shell bar */
			clamp_to_work_area(server, &x, &y,
				tl->fullscreen_restore_box.width,
				tl->fullscreen_restore_box.height);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				tl->fullscreen_restore_box.width,
				tl->fullscreen_restore_box.height);
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
	if (tl->xdg_toplevel->base == NULL) {
		return;
	}
	if (tl->fullscreen) {
		/* while fullscreen, maximize/restore is not available; only
		 * leaving fullscreen returns the window to its previous state */
		return;
	}
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		/* dialogs and fixed-size windows (e.g. QQ's login) are never
		 * maximized (their top border is a close button, not a title bar) */
		return;
	}
	if (maximized && tl->xdg_toplevel->current.maximized) {
		/* already maximized: re-fit the window to the maximized geometry.
		 * A maximized window can drift off it (e.g. a client-drag of a
		 * maximized window that never got restored), and the client's own
		 * maximize button then sends a set_maximized that would otherwise
		 * be a silent no-op - the window stays stuck.  Re-asserting the
		 * box makes a maximize request always respond. */
		struct wlr_output *output = toplevel_output(server, tl);
		if (output != NULL) {
			struct wlr_box box;
			maximized_box(server, output, &box);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, box.width,
				box.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, box.x, box.y);
		}
		return;
	}
	if (tl->xdg_toplevel->current.maximized == maximized) {
		return;
	}
	tl->user_moved = true; /* maximize/restore state: stop auto-centering */
	if (maximized) {
		/* remember the floating geometry so dragging the title bar of the
		 * maximized window can restore it (Windows behavior).  Only capture
		 * it on the genuine floating -> maximized transition: clients like
		 * QQ re-assert set_maximized repeatedly (before the first request
		 * is even acked, when current.maximized is still false), and
		 * re-saving then would overwrite the floating geometry with the
		 * already-maximized box. */
		if (!tl->xdg_toplevel->current.maximized &&
				!tl->has_restore_box) {
			toplevel_box(tl, &tl->restore_box);
			tl->has_restore_box = true;
		}

		struct wlr_output *output = toplevel_output(server, tl);
		if (output != NULL) {
			struct wlr_box box;
			maximized_box(server, output, &box);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, box.width,
				box.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, box.x, box.y);
		} else {
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
	} else {
		/* restore to the geometry saved when the window was maximized
		 * (position + size), so the client's own restore button brings
		 * the window back exactly where it was.  Falling back to 0x0
		 * only when no floating geometry was ever saved. */
		if (tl->has_restore_box && tl->restore_box.width > 0) {
			int x = tl->restore_box.x;
			int y = tl->restore_box.y;
			/* the work area may have shrunk since the window was
			 * maximized (a bar appeared): clamp the restore into the work
			 * area so the window never lands back underneath the bar */
			clamp_to_work_area(server, &x, &y, tl->restore_box.width,
				tl->restore_box.height);
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				tl->restore_box.width, tl->restore_box.height);
			wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
		} else {
			/* 0x0 lets the client pick its own size again */
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		}
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
			!tl->xdg_toplevel->current.maximized || tl->fullscreen) {
		return;
	}
	tl->user_moved = true; /* drag-restore: stop auto-centering */
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
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		/* dialogs and fixed-size windows have no minimize button */
		return;
	}
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
	/* keep the border in sync with the minimized state: minimizing drops
	 * the border buffer (the window is hidden), restoring re-renders it.
	 * Without this a window that committed a frame while minimized would
	 * restore with its border buffer still NULL (the render cache in
	 * border.c thought it was up to date). */
	update_toplevel_decoration(tl);
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

/* xdg-activation-v1 request_activate: the surface's client asked for focus
 * with a valid activation token.  Focus (and restore) the matching toplevel. */
void xdg_activation_request_activate(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		activation_request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->xdg_toplevel->base == NULL ||
				tl->xdg_toplevel->base->surface != event->surface) {
			continue;
		}
		if (tl->minimized) {
			set_minimized(server, tl, false);
		}
		focus_toplevel(server, tl);
		return;
	}
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
		/* size stays exactly what the client committed; the position is
		 * centered on the output both horizontally and vertically
		 * (place.c) */
		place_toplevel(server, tl);
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

	/* a top/left grab is positioned here, once the client has actually
	 * committed the new geometry: update_resize() only sends the size
	 * configure and never moves the node for these grabs, so the fixed
	 * (opposite) edge stays glued to where it was when the grab started
	 * and the old, still-larger buffer is never shown at a moved
	 * position - which is what made the bottom edge bounce while
	 * resizing from the top.  Re-asserting the position before the next
	 * frame is drawn keeps the box in sync with the committed buffer. */
	if (tl->server->resizing && tl->server->resize_toplevel == tl &&
			base != NULL) {
		int x = tl->scene_tree->node.x;
		int y = tl->scene_tree->node.y;
		if ((tl->server->resize_edges & WLR_EDGE_LEFT) != 0) {
			x = tl->server->resize_orig.x + tl->server->resize_orig.width
				- base->geometry.width;
		}
		if ((tl->server->resize_edges & WLR_EDGE_TOP) != 0) {
			y = tl->server->resize_orig.y
				+ tl->server->resize_orig.height - base->geometry.height;
		}
		wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
	}

	/* geometry (and thus the border and the resize/title zones under the
	 * cursor) may have changed */
	/* rounded-corner masked content tracks the latest committed buffer */
	mask_toplevel_content(tl);
	/* keep a fresh window centered until the user interacts with it:
	 * Electron windows (QQ) often map with a small placeholder surface
	 * and only commit their real size on a later frame, so re-center
	 * whenever the surface size changes (place.c).  User interactions
	 * (move / resize / maximize / fullscreen) set user_moved and stop
	 * this, and the window then stays where the user puts it. */
	if (!tl->user_moved && !tl->minimized &&
			!tl->xdg_toplevel->current.maximized && !tl->fullscreen &&
			base != NULL && base->surface != NULL) {
		struct wlr_surface *s = base->surface;
		if (!tl->placed || s->current.width != tl->placed_w ||
				s->current.height != tl->placed_h) {
			if (place_toplevel(tl->server, tl)) {
				tl->placed = true;
				tl->placed_w = s->current.width;
				tl->placed_h = s->current.height;
			}
		}
	}
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
	/* honor the client's requested value (wlroots stores it in
	 * requested.maximized): a client's own maximize button sends
	 * set_maximized(true) to maximize and set_maximized(false) to restore.
	 * Ignoring the value and always maximizing made the second press a
	 * silent no-op - the window could never be restored from its own
	 * button. */
	set_maximized(tl->server, tl, tl->xdg_toplevel->requested.maximized);
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
	/* a maximized window is NOT restored here: clients like QQ send
	 * xdg_toplevel.move on a plain title-bar click, which would restore
	 * the window without any drag.  move_toplevel_to() restores it on the
	 * first real motion instead, so only an actual drag un-maximizes
	 * (Windows behavior) while a mere click does nothing. */
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

/* defer clamping a popup into its output until the popup has committed
 * once.  wlr_xdg_popup_unconstrain_from_box() schedules a configure, and
 * wlroots asserts on surfaces that are not yet initialized (first commit)
 * — calling it from the new_popup handler crashed the compositor when a
 * Qt app (fcitx5-config-qt theme page, tooltips, combo boxes) opened a
 * popup.  A one-shot commit listener runs right after the role commit set
 * initialized, so the clamp happens before the popup is shown. */
struct popup_unconstrain {
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wlr_xdg_popup *popup;
	struct toplevel *tl;
};

static void popup_unconstrain_handle_commit(struct wl_listener *listener,
		void *data) {
	struct popup_unconstrain *pu = wl_container_of(listener, pu, commit);
	struct server *server = pu->tl->server;
	struct wlr_output *output = toplevel_output(server, pu->tl);
	if (output != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output, &box);
		wlr_xdg_popup_unconstrain_from_box(pu->popup, &box);
	}
	wl_list_remove(&pu->commit.link);
	wl_list_remove(&pu->destroy.link);
	free(pu);
}

static void popup_unconstrain_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct popup_unconstrain *pu = wl_container_of(listener, pu, destroy);
	wl_list_remove(&pu->commit.link);
	wl_list_remove(&pu->destroy.link);
	free(pu);
}

static void mask_popup_new_popup(struct wl_listener *listener, void *data);
static void mask_subsurface_destroy(struct wl_listener *listener, void *data);
static void mask_popup_decoration(struct mask_popup *mp);

/* rounded-corner popups (mask.c) - the popup surface's own scene node is
 * replaced by a masked re-render buffer, so its corners get
 * CONFIG_BORDER_RADIUS exactly like the windows.  The popup is still hit-
 * tested through the buffer: its tree carries a TAG_POPUP tag that pointer
 * input resolves back to the popup surface (see pointer.c). */

/* the masked buffer replaces the popup's scene node, so output enter/
 * leave, presentation feedback and frame done must be forwarded to the
 * popup surface from mp->masked */
static void mask_popup_output_enter(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, mask_enter);
	struct wlr_scene_output *scene_output = data;
	if (mp->popup != NULL && mp->popup->base != NULL) {
		wlr_surface_send_enter(mp->popup->base->surface,
			scene_output->output);
	}
}

static void mask_popup_output_leave(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, mask_leave);
	struct wlr_scene_output *scene_output = data;
	if (mp->popup != NULL && mp->popup->base != NULL) {
		wlr_surface_send_leave(mp->popup->base->surface,
			scene_output->output);
	}
}

static void mask_popup_output_sample(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, mask_sample);
	struct wlr_scene_output_sample_event *event = data;
	if (!event->direct_scanout && mp->popup != NULL &&
			mp->popup->base != NULL) {
		wlr_presentation_surface_textured_on_output(
			mp->popup->base->surface, event->output->output);
	}
}

static void mask_popup_frame_done(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, mask_frame);
	struct timespec *now = data;
	if (mp->popup != NULL && mp->popup->base != NULL) {
		wlr_surface_send_frame_done(mp->popup->base->surface, now);
	}
}

/* keep every popup subsurface glued to its surface position */
static void mask_popup_subsurfaces_reposition(struct mask_popup *mp) {
	struct mask_subsurface *ms;
	wl_list_for_each(ms, &mp->subsurfaces, link) {
		wlr_scene_node_set_position(&ms->tree->node,
			-mp->popup->base->geometry.x + ms->subsurface->current.x,
			-mp->popup->base->geometry.y + ms->subsurface->current.y);
	}
}

static void mask_popup_subsurface_add(struct mask_popup *mp,
		struct wlr_subsurface *subsurface) {
	struct mask_subsurface *ms = calloc(1, sizeof(*ms));
	if (ms == NULL) {
		return;
	}
	ms->tl = mp->tl;
	ms->subsurface = subsurface;
	ms->tree = wlr_scene_subsurface_tree_create(mp->tree,
		subsurface->surface);
	if (ms->tree == NULL) {
		free(ms);
		return;
	}
	ms->destroy.notify = mask_subsurface_destroy;
	wl_signal_add(&subsurface->events.destroy, &ms->destroy);
	wl_list_insert(mp->subsurfaces.prev, &ms->link);
}

static void mask_popup_new_subsurface(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, new_subsurface);
	mask_popup_subsurface_add(mp, data);
}

/* For explicit-sync surfaces, tie the client's release point to the
 * compositor-owned masked buffer.  The client buffer is sampled while
 * producing `buf`; once the scene releases `buf`, the compositor is
 * guaranteed to be done with every buffer that contributed to it, so the
 * release point can be signalled safely. */
static void mask_signal_syncobj_release(struct wlr_surface *surface,
		struct wlr_buffer *buf) {
	struct wlr_linux_drm_syncobj_surface_v1_state *state =
		wlr_linux_drm_syncobj_v1_get_surface_state(surface);
	if (state == NULL) {
		return;
	}
	if (!wlr_linux_drm_syncobj_v1_state_signal_release_with_buffer(state,
			buf)) {
		wlr_log(WLR_ERROR, "mask: failed to schedule syncobj release "
			"point signal");
	}
}

/* re-render the popup's rounded content from its current buffer; called on
 * every commit that carries a buffer (same contract as mask_toplevel_content) */
static void mask_popup_content(struct mask_popup *mp) {
	struct wlr_xdg_surface *base = mp->popup->base;
	if (mp->masked == NULL || base == NULL || !base->initialized ||
			!base->surface->mapped || base->surface->buffer == NULL) {
		return;
	}
	struct wlr_surface *surface = base->surface;
	int w = surface->current.width;
	int h = surface->current.height;
	if (w <= 0 || h <= 0) {
		return;
	}
	wlr_log(WLR_DEBUG, "mask: popup surface=%dx%d scale=%d buf=%dx%d "
		"geo=%d,%d %dx%d radius=%d",
		w, h, surface->current.scale,
		surface->current.buffer ? surface->current.buffer->width : 0,
		surface->current.buffer ? surface->current.buffer->height : 0,
		base->geometry.x, base->geometry.y, base->geometry.width,
		base->geometry.height, CONFIG_BORDER_RADIUS - CONFIG_BORDER_WIDTH);
	struct wlr_buffer *buf = content_mask_buffer(mp->tl->server, w, h);
	if (buf == NULL) {
		return;
	}
	/* the popup content is rounded to the ring's inner arc, exactly like
	 * the toplevel content (the ring itself - mask_popup_decoration -
	 * spans CONFIG_BORDER_RADIUS).  No geometry probe / margin handling:
	 * popups (Qt menus, tooltips, combo boxes) have no drop shadow */
	if (!content_mask_render(mp->tl->server, surface, buf, w, h,
			(float)(CONFIG_BORDER_RADIUS - CONFIG_BORDER_WIDTH),
			NULL, false, NULL)) {
		mask_signal_syncobj_release(surface, buf);
		wlr_buffer_drop(buf);
		return;
	}
	wlr_scene_buffer_set_buffer(mp->masked, buf);
	mask_signal_syncobj_release(surface, buf);
	wlr_buffer_drop(buf);
	wlr_scene_node_set_position(&mp->masked->node,
		-base->geometry.x, -base->geometry.y);
	mask_popup_subsurfaces_reposition(mp);
}

/* the popup moved / resized / repainted: glue the tree to its geometry and
 * re-render the rounded content */
static void mask_popup_commit(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, commit);
	(void)data;
	wlr_scene_node_set_position(&mp->tree->node,
		mp->popup->current.geometry.x, mp->popup->current.geometry.y);
	mask_popup_content(mp);
	mask_popup_decoration(mp);
}

/* the popup's thin rounded ring (border.c): without it the transparent
 * corners of a popup blend into whatever is behind it, so an all-white
 * menu over a white window looks square - gray popups only LOOK rounded
 * by luck of the background contrast.  The ring outlines the corners,
 * same as the window ring (no title strip, no glow). */
static void mask_popup_decoration(struct mask_popup *mp) {
	struct wlr_xdg_surface *base = mp->popup->base;
	if (mp->deco_border == NULL || base == NULL) {
		return;
	}
	if (!base->surface->mapped || base->surface->buffer == NULL ||
			base->geometry.width <= 0 || base->geometry.height <= 0) {
		wlr_scene_buffer_set_buffer(mp->deco_border, NULL);
		return;
	}
	/* width 0 disables the popup border entirely */
	if (CONFIG_POPUP_BORDER_WIDTH <= 0) {
		wlr_scene_buffer_set_buffer(mp->deco_border, NULL);
		return;
	}
	int extent = (int)(CONFIG_POPUP_BORDER_WIDTH + 0.5);
	int bw = base->geometry.width + 2 * extent;
	int bh = base->geometry.height + 2 * extent;
	uint32_t color = CONFIG_POPUP_BORDER_COLOR;
	wlr_log(WLR_DEBUG, "border: popup box=%d,%d %dx%d buf=%dx%d width=%.1f",
		base->geometry.x, base->geometry.y, base->geometry.width,
		base->geometry.height, bw, bh, CONFIG_POPUP_BORDER_WIDTH);
	if (mp->deco_w != bw || mp->deco_h != bh || mp->deco_color != color) {
		struct wlr_buffer *buffer = border_alloc_buffer(mp->tl->server,
			bw, bh);
		if (buffer == NULL) {
			wlr_log(WLR_ERROR, "border: popup failed to allocate %dx%d",
				bw, bh);
			return;
		}
		if (!border_render_ring(mp->tl->server, buffer, bw, bh, color,
				CONFIG_POPUP_BORDER_WIDTH)) {
			wlr_buffer_drop(buffer);
			return;
		}
		wlr_scene_buffer_set_buffer(mp->deco_border, buffer);
		wlr_buffer_drop(buffer);
		mp->deco_w = bw;
		mp->deco_h = bh;
		mp->deco_color = color;
	}
	wlr_scene_node_set_position(&mp->deco_border->node, -extent, -extent);
	/* the ring must outline the popup even when a subsurface or a nested
	 * popup (submenu) overlaps the edge - same as the window ring */
	wlr_scene_node_raise_to_top(&mp->deco_border->node);
}

/* the wlr_xdg_popup object is gone (menu closed / grab dismissed): drop
 * every listener and stop rendering.  The scene tree and this struct live
 * until the popup's xdg surface itself is destroyed (mask_popup_xdg_destroy), so
 * the tree can safely outlive the popup object. */
static void mask_popup_destroy(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, destroy);
	(void)data;
	wl_list_remove(&mp->commit.link);
	wl_list_remove(&mp->new_subsurface.link);
	wl_list_remove(&mp->mask_enter.link);
	wl_list_remove(&mp->mask_leave.link);
	wl_list_remove(&mp->mask_sample.link);
	wl_list_remove(&mp->mask_frame.link);
	wl_list_remove(&mp->new_popup.link);
	wl_list_remove(&mp->destroy.link);
	wl_list_remove(&mp->link);
	if (mp->masked != NULL) {
		wlr_scene_buffer_set_buffer(mp->masked, NULL);
	}
	if (mp->deco_border != NULL) {
		wlr_scene_buffer_set_buffer(mp->deco_border, NULL);
	}
	struct mask_subsurface *ms, *tmp;
	wl_list_for_each_safe(ms, tmp, &mp->subsurfaces, link) {
		wl_list_remove(&ms->destroy.link);
		wl_list_remove(&ms->link);
		free(ms);
	}
}

/* the popup's xdg surface is destroyed: release the scene tree (and with it
 * the masked buffer and every subsurface / nested popup tree) and the struct */
static void mask_popup_xdg_destroy(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, xdg_destroy);
	(void)data;
	wl_list_remove(&mp->xdg_destroy.link);
	if (mp->tree != NULL) {
		wlr_scene_node_destroy(&mp->tree->node);
	}
	free(mp);
}

static void xdg_popup_attach(struct toplevel *tl, struct wlr_xdg_popup *popup,
		struct wlr_scene_tree *parent_tree) {
	struct mask_popup *mp = calloc(1, sizeof(*mp));
	if (mp == NULL) {
		return;
	}
	mp->tl = tl;
	mp->popup = popup;
	/* the popup's own surface is re-rendered through the rounded-corner
	 * alpha mask (mask.c) like the toplevel content: a plain scene surface
	 * cannot round corners.  The tree is tagged so pointer input resolves
	 * back to the popup surface. */
	mp->tree = wlr_scene_tree_create(parent_tree);
	if (mp->tree == NULL) {
		free(mp);
		return;
	}
	mp->masked = wlr_scene_buffer_create(mp->tree, NULL);
	if (mp->masked == NULL) {
		wlr_scene_node_destroy(&mp->tree->node);
		free(mp);
		return;
	}
	/* the rounded ring behind/around the content; purely visual, never
	 * intercepts pointer input */
	mp->deco_border = wlr_scene_buffer_create(mp->tree, NULL);
	if (mp->deco_border != NULL) {
		mp->deco_border->point_accepts_input = border_buffer_no_input;
	}
	xdg_surface_tag(mp->tree, TAG_POPUP, popup);
	wl_list_init(&mp->subsurfaces);
	/* forward output/frame events from the masked buffer to the popup */
	mp->mask_enter.notify = mask_popup_output_enter;
	wl_signal_add(&mp->masked->events.output_enter, &mp->mask_enter);
	mp->mask_leave.notify = mask_popup_output_leave;
	wl_signal_add(&mp->masked->events.output_leave, &mp->mask_leave);
	mp->mask_sample.notify = mask_popup_output_sample;
	wl_signal_add(&mp->masked->events.output_sample, &mp->mask_sample);
	mp->mask_frame.notify = mask_popup_frame_done;
	wl_signal_add(&mp->masked->events.frame_done, &mp->mask_frame);
	mp->new_subsurface.notify = mask_popup_new_subsurface;
	wl_signal_add(&popup->base->surface->events.new_subsurface,
		&mp->new_subsurface);
	/* subsurfaces that already existed before this listener was added */
	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface,
			&popup->base->surface->current.subsurfaces_below, current.link) {
		mask_popup_subsurface_add(mp, subsurface);
	}
	wl_list_for_each(subsurface,
			&popup->base->surface->current.subsurfaces_above, current.link) {
		mask_popup_subsurface_add(mp, subsurface);
	}
	/* position the tree at the popup geometry (relative to the parent)
	 * and re-render on every commit; nested popups (submenus) attach here */
	mp->commit.notify = mask_popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &mp->commit);
	mp->new_popup.notify = mask_popup_new_popup;
	wl_signal_add(&popup->base->events.new_popup, &mp->new_popup);
	mp->destroy.notify = mask_popup_destroy;
	wl_signal_add(&popup->events.destroy, &mp->destroy);
	mp->xdg_destroy.notify = mask_popup_xdg_destroy;
	wl_signal_add(&popup->base->events.destroy, &mp->xdg_destroy);
	wl_list_insert(tl->popups.prev, &mp->link);

	wlr_scene_node_set_position(&mp->tree->node,
		popup->current.geometry.x, popup->current.geometry.y);
}

static void mask_popup_new_popup(struct wl_listener *listener, void *data) {
	struct mask_popup *mp = wl_container_of(listener, mp, new_popup);
	/* a nested popup (Qt submenu): its parent scene node is this popup's
	 * own scene tree */
	xdg_popup_attach(mp->tl, data, mp->tree);
}

static void xdg_toplevel_new_popup(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, new_popup);
	struct wlr_xdg_popup *popup = data;

	/* unconstrain the popup into its output box */
	struct popup_unconstrain *pu = calloc(1, sizeof(*pu));
	if (pu == NULL) {
		return;
	}
	pu->popup = popup;
	pu->tl = tl;
	pu->commit.notify = popup_unconstrain_handle_commit;
	wl_signal_add(&popup->base->surface->events.commit, &pu->commit);
	pu->destroy.notify = popup_unconstrain_handle_destroy;
	wl_signal_add(&popup->base->events.destroy, &pu->destroy);

	/* the popup's parent is the toplevel surface, so its scene tree lives
	 * inside the toplevel's content tree (whose origin is the window
	 * geometry top-left, exactly what the popup geometry is relative to) */
	xdg_popup_attach(tl, popup, tl->scene_tree);
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
 * forwarded to the client surface from tl->masked.  The masked buffer is a
 * plain wlr_scene_buffer (not a wlr_scene_surface), so the fractional-scale
 * notification that wlr_scene_surface would do automatically is done here
 * from the buffer's primary output. */
static void mask_outputs_update(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, mask_outputs_update);
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL || tl->masked == NULL) {
		return;
	}
	if (tl->masked->primary_output == NULL) {
		return;
	}
	double scale = tl->masked->primary_output->output->scale;
	wlr_fractional_scale_v1_notify_scale(base->surface, scale);
	wlr_surface_set_preferred_buffer_scale(base->surface, ceil(scale));
}

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
	wlr_log(WLR_DEBUG, "mask: %s surface=%dx%d scale=%d buf=%dx%d geo=%d,%d %dx%d",
		tl->app_id ? tl->app_id : "?", w, h, surface->current.scale,
		surface->current.buffer ? surface->current.buffer->width : 0,
		surface->current.buffer ? surface->current.buffer->height : 0,
		base->geometry.x, base->geometry.y, base->geometry.width,
		base->geometry.height);
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
	/* the geometry probe decides whether the border wraps the xdg window
	 * geometry or the committed surface (see mask_margin_has_content) */
	bool margin_opaque = false;
	/* first pass without the geometry-corner clip: the margin probe must
	 * read the surface's own alpha, and the clip is only valid once we
	 * know the margin is a transparent shadow (not real content) */
	if (!content_mask_render(tl->server, surface, buf, w, h, radius,
			&base->geometry, false, &margin_opaque)) {
		mask_signal_syncobj_release(surface, buf);
		wlr_buffer_drop(buf);
		return;
	}
	tl->wrap_surface = margin_opaque;
	/* second pass, only for windows whose border trusts the geometry but
	 * whose surface is larger (GTK CSD shadows, e.g. Firefox): round the
	 * window-geometry corners too, so the content's sharp corners are cut
	 * to the border ring's arc (the shadow margin stays intact) */
	if (!tl->wrap_surface &&
			(base->geometry.x > 0 || base->geometry.y > 0 ||
			base->geometry.width < w || base->geometry.height < h)) {
		if (!content_mask_render(tl->server, surface, buf, w, h, radius,
				&base->geometry, true, NULL)) {
			mask_signal_syncobj_release(surface, buf);
			wlr_buffer_drop(buf);
			return;
		}
	}
	/* the scene locks the buffer; drop our reference */
	wlr_scene_buffer_set_buffer(tl->masked, buf);
	mask_signal_syncobj_release(surface, buf);
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
		wl_list_remove(&tl->mask_outputs_update.link);
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
	struct mask_popup *mp, *mp_tmp;
	wl_list_for_each_safe(mp, mp_tmp, &tl->popups, link) {
		wl_list_remove(&mp->commit.link);
		wl_list_remove(&mp->new_subsurface.link);
		wl_list_remove(&mp->mask_enter.link);
		wl_list_remove(&mp->mask_leave.link);
		wl_list_remove(&mp->mask_sample.link);
		wl_list_remove(&mp->mask_frame.link);
		wl_list_remove(&mp->new_popup.link);
		wl_list_remove(&mp->destroy.link);
		wl_list_remove(&mp->xdg_destroy.link);
		wl_list_remove(&mp->link);
		if (mp->masked != NULL) {
			wlr_scene_buffer_set_buffer(mp->masked, NULL);
		}
		if (mp->deco_border != NULL) {
			wlr_scene_buffer_set_buffer(mp->deco_border, NULL);
		}
		struct mask_subsurface *ms, *tmp2;
		wl_list_for_each_safe(ms, tmp2, &mp->subsurfaces, link) {
			wl_list_remove(&ms->destroy.link);
			wl_list_remove(&ms->link);
			free(ms);
		}
		free(mp);
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
	/* the window content tree is a child of the decoration tree, so it
	 * stays above the border */
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
	wl_list_init(&tl->popups);
	/* the xdg surface's own scene node is gone: forward output/frame
	 * events to the client surface from the masked buffer */
	tl->mask_outputs_update.notify = mask_outputs_update;
	wl_signal_add(&tl->masked->events.outputs_update, &tl->mask_outputs_update);
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

/* clients that draw their own decorations but whose own window resize is
 * broken (frameless Electron windows, e.g. QQ): the compositor owns their
 * frame (resize edges, top strip, resize cursor) exactly like an
 * undecorated window, because the client's own edge resize never works.
 * QQ explicitly requests client-side decorations but cannot resize
 * itself, so its request is overridden to NONE below. */
static bool toplevel_force_undecorated(struct toplevel *tl) {
	if (tl->app_id == NULL) {
		return false;
	}
	/* QQ (linuxqq) and variants: frameless, requests CSD, but its own
	 * resize handles never resize the window */
	return strncasecmp(tl->app_id, "qq", 2) == 0;
}

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
	tl->decoration_mode = toplevel_force_undecorated(tl)
		? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE
		: decoration->requested_mode;
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
	 * instead). The actual configure is sent on the first commit.  Clients
	 * whose own resize is known to be broken (frameless Electron, e.g.
	 * QQ) get NONE instead so the compositor owns their frame. */
	tl->decoration_mode = toplevel_force_undecorated(tl)
		? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE
		: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}
