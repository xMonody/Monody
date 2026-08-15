/*
 * toplevel.c - xdg-shell toplevel windows
 *
 * Window lifecycle (map/unmap/commit/destroy), window state (maximize,
 * minimize, fullscreen, move, close), xdg-decoration mode negotiation and
 * the foreign-toplevel handle that mirrors each window for taskbars.
 *
 * Decorations (rounded corners, border ring, shadow) are provided by
 * scenefx: the whole window's rounded region (the window-geometry box with
 * the content corner radius) is computed once and applied as a unified
 * clip/mask to every surface that belongs to the xdg_toplevel - each
 * surface node keeps its own buffer and is drawn directly, the region is
 * expressed per node with wlr_scene_buffer_set_corner_radius.  The border
 * is a wlr_scene_rect with a rounded hole clipped out of it, and the
 * shadow is a wlr_scene_shadow node lowered below the border.  All
 * parameters come from config.h.
 */

#include "server.h"

#include "ipc.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>

#include <scenefx/types/wlr_scene.h>

/* effective window geometry box in layout coordinates: the xdg window
 * geometry (the window bounds per xdg-shell, excluding CSD margins/drop
 * shadows).  The xdg scene tree is anchored at this box's top-left corner. */
void toplevel_box(struct toplevel *tl, struct wlr_box *box) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	box->x = tl->scene_tree->node.x;
	box->y = tl->scene_tree->node.y;
	box->width = base != NULL ? base->geometry.width : 0;
	box->height = base != NULL ? base->geometry.height : 0;
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

/* visible border thickness in whole pixels (config is a float) */
static int border_thickness(void) {
	int t = (int)(CONFIG_BORDER_WIDTH + 0.5);
	return t > 0 ? t : 0;
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
	int extent = border_thickness();
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
 * the work area). */
static void fullscreen_box(struct server *server, struct wlr_output *output,
		struct wlr_box *box) {
	struct wlr_box out;
	wlr_output_layout_get_box(server->output_layout, output, &out);
	int gap = border_thickness() + CONFIG_FULLSCREEN_GAP;
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
	update_toplevel_decoration(tl);
}

/* raise a toplevel's scene node and, above it, every dialog that declared
 * it as its xdg parent (recursively): focusing / clicking the main window
 * must never bury its open dialogs underneath it.  Dialogs are raised
 * after their parent, so they always end up stacked above it. */
static void toplevel_raise(struct server *server, struct toplevel *tl) {
	if (tl->scene_tree == NULL) {
		return;
	}
	wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	struct toplevel *child;
	wl_list_for_each(child, &server->toplevels, link) {
		if (child != tl && child->xdg_toplevel != NULL &&
				child->xdg_toplevel->parent == tl->xdg_toplevel) {
			toplevel_raise(server, child);
		}
	}
}

void focus_toplevel(struct server *server, struct toplevel *tl) {
	if (tl->minimized || tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->base->surface->mapped) {
		return;
	}
	struct toplevel *prev = server->focused;
	if (prev == tl) {
		toplevel_raise(server, tl);
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
	toplevel_raise(server, tl);
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

static void toplevel_apply_corner_radius(struct toplevel *tl);

/* ------------------------------------------------------------------ */
/* subsurfaces: Firefox/Chromium cover the window edges with subsurfaces,
 * so their commits must re-apply the unified window rounded-region mask
 * (the region is the same for the main surface and every subsurface) */
/* ------------------------------------------------------------------ */

static void toplevel_subsurface_commit(struct wl_listener *listener,
		void *data) {
	struct toplevel_subsurface *ts = wl_container_of(listener, ts, commit);
	(void)data;
	toplevel_apply_corner_radius(ts->tl);
}

static void toplevel_subsurface_destroy(struct wl_listener *listener,
		void *data) {
	struct toplevel_subsurface *ts = wl_container_of(listener, ts, destroy);
	(void)data;
	wl_list_remove(&ts->commit.link);
	wl_list_remove(&ts->destroy.link);
	wl_list_remove(&ts->link);
	free(ts);
}

static void toplevel_subsurface_add(struct toplevel *tl,
		struct wlr_subsurface *subsurface) {
	struct toplevel_subsurface *ts = calloc(1, sizeof(*ts));
	if (ts == NULL) {
		return;
	}
	ts->tl = tl;
	ts->subsurface = subsurface;
	ts->commit.notify = toplevel_subsurface_commit;
	wl_signal_add(&subsurface->surface->events.commit, &ts->commit);
	ts->destroy.notify = toplevel_subsurface_destroy;
	wl_signal_add(&subsurface->events.destroy, &ts->destroy);
	wl_list_insert(tl->subsurfaces.prev, &ts->link);
	/* the subsurface may already carry a buffer: round it now */
	toplevel_apply_corner_radius(tl);
}

static void xdg_toplevel_new_subsurface(struct wl_listener *listener,
		void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, new_subsurface);
	toplevel_subsurface_add(tl, data);
}

/* remove all subsurface bookkeeping before the toplevel is freed */
static void toplevel_destroy_subsurfaces(struct toplevel *tl) {
	struct toplevel_subsurface *ts, *tmp;
	wl_list_for_each_safe(ts, tmp, &tl->subsurfaces, link) {
		wl_list_remove(&ts->commit.link);
		wl_list_remove(&ts->destroy.link);
		wl_list_remove(&ts->link);
		free(ts);
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
	update_toplevel_decoration(tl);
	update_cursor_style(tl->server);
}

static void toplevel_client_cursor_gone(struct server *server,
		struct wl_client *client);

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, toplevel_destroy);
	struct server *server = tl->server;

	toplevel_unfocus(server, tl);
	/* the window is going away: if its client owned the cursor (I-beam
	 * over a text field, etc.), revert to the default arrow.  This must run
	 * here, while xdg_toplevel is still alive - wlroots frees it right
	 * after this signal, so xdg_surface_destroy cannot touch it. */
	if (tl->xdg_toplevel->base != NULL) {
		toplevel_client_cursor_gone(server,
			tl->xdg_toplevel->base->surface->resource->client);
	}
	wl_list_remove(&tl->link);

	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_destroy(tl->fthandle);
	}

	/* remove all listeners attached to the toplevel and its surface; the
	 * xdg surface listeners (tl->destroy, tl->new_popup) stay linked until
	 * the xdg surface itself is destroyed.  Popups need no cleanup here:
	 * their scene trees are children of tl->scene_tree and each popup frees
	 * itself when its own tree is destroyed (which happens when the popup's
	 * xdg surface goes away or with tl->scene_tree). */
	toplevel_destroy_subsurfaces(tl);
	wl_list_remove(&tl->new_subsurface.link);
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

/* the closing window's client may own the current pointer cursor (a
 * cursor-shape or a cursor surface, e.g. the I-beam over a text field):
 * drop the stale state and revert to the default arrow when the
 * compositor isn't overriding the cursor (move / resize / title strip) */
static void toplevel_client_cursor_gone(struct server *server,
		struct wl_client *client) {
	bool owned = false;
	if (server->client_cursor_shape != 0 &&
			server->client_cursor_shape_client != NULL &&
			server->client_cursor_shape_client->client == client) {
		server->client_cursor_shape = 0;
		/* drop the destroy listener together with the pointer, or the
		 * orphaned listener is re-added to another seat client later and
		 * corrupts the destroy-listener list (wlroots asserts on it in
		 * seat_client_destroy when that client disconnects) */
		wl_list_remove(&server->client_cursor_shape_client_destroy.link);
		server->client_cursor_shape_client = NULL;
		owned = true;
	}
	if (server->client_cursor_surface != NULL &&
			server->client_cursor_surface->resource->client == client) {
		wl_list_remove(&server->client_cursor_destroy.link);
		server->client_cursor_surface = NULL;
		owned = true;
	}
	if (owned && server->cursor_override == NULL) {
		wlr_log(WLR_DEBUG, "cursor: default (window closed)");
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, destroy);
	wl_list_remove(&tl->destroy.link);
	/* notify status bars before the window disappears */
	if (tl->ipc_added) {
		ipc_send_window_event(tl->server, "window_removed", tl);
	}
	/* the xdg scene tree (and with it the shadow/border nodes) is destroyed
	 * by wlroots' own xdg-surface scene handler registered by
	 * wlr_scene_xdg_surface_create */
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
	/* geometry (and thus the border/shadow and the resize/title zones
	 * under the cursor) may have changed */
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
	struct server *server;
};

static void popup_unconstrain_handle_commit(struct wl_listener *listener,
		void *data) {
	struct popup_unconstrain *pu = wl_container_of(listener, pu, commit);
	struct server *server = pu->server;
	/* the popup follows the cursor, so constrain it into the output under
	 * the cursor (never touch the owning toplevel - it may be gone) */
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
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

/* ------------------------------------------------------------------ */
/* popups (scenefx)                                                   */
/* ------------------------------------------------------------------ */

static void xdg_popup_attach(struct toplevel *tl, struct wlr_xdg_popup *popup,
		struct wlr_scene_tree *parent_tree);
static void xdg_popup_new_popup(struct wl_listener *listener, void *data);
static void xdg_popup_decoration(struct toplevel_popup *pp);

/* the ring's inner arc matches the content arc: CONFIG_BORDER_RADIUS minus
 * the border thickness.  Fullscreen uses the same radius as maximized so
 * the corners stay visible; only the border color differs (see
 * toplevel_border_color). */
static int content_radius(struct toplevel *tl) {
	(void)tl;
	int r = CONFIG_BORDER_RADIUS - border_thickness();
	return r > 0 ? r : 0;
}

static int popup_content_radius(void) {
	int r = CONFIG_BORDER_RADIUS - border_thickness();
	return r > 0 ? r : 0;
}

/* The window's rounded region: the xdg window-geometry box (layout
 * coordinates) plus the content corner radius.  This single region is the
 * unified clip/mask for the whole window: every surface that belongs to
 * the xdg_toplevel (CSD windows have several - the main surface plus its
 * subsurfaces) is clipped against it.  Each surface node keeps its own
 * buffer and is drawn directly by the scene - nothing is merged into a
 * shared texture - and the region is expressed per node by rounding the
 * surface's own corners that overlap the region's corners.  Because the
 * region is the same for every node, all surfaces are cut along the exact
 * same arc and overlapping surfaces (e.g. a subsurface on top of the main
 * surface at a window corner) line up pixel-perfectly. */
struct window_rounded_region {
	struct wlr_box box;   /* window geometry, layout coordinates */
	int radius;
};

/* compute the window's rounded region; false when the window has no
 * usable geometry yet */
static bool window_rounded_region_get(struct toplevel *tl,
		struct window_rounded_region *region) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (tl->scene_tree == NULL || base == NULL) {
		return false;
	}
	region->box.x = tl->scene_tree->node.x;
	region->box.y = tl->scene_tree->node.y;
	region->box.width = base->geometry.width;
	region->box.height = base->geometry.height;
	region->radius = content_radius(tl);
	return region->box.width > 0 && region->box.height > 0;
}

/* the rounded-corner cap at one corner of the region: the radius x radius
 * square whose outer corner the rounding arc cuts */
static struct wlr_box region_corner_cap(
		const struct window_rounded_region *region,
		enum corner_location corner) {
	struct wlr_box cap = { 0, 0, region->radius, region->radius };
	switch (corner) {
	case CORNER_LOCATION_TOP_LEFT:
		cap.x = region->box.x;
		cap.y = region->box.y;
		break;
	case CORNER_LOCATION_TOP_RIGHT:
		cap.x = region->box.x + region->box.width - region->radius;
		cap.y = region->box.y;
		break;
	case CORNER_LOCATION_BOTTOM_RIGHT:
		cap.x = region->box.x + region->box.width - region->radius;
		cap.y = region->box.y + region->box.height - region->radius;
		break;
	case CORNER_LOCATION_BOTTOM_LEFT:
		cap.x = region->box.x;
		cap.y = region->box.y + region->box.height - region->radius;
		break;
	default:
		cap.width = 0;
		cap.height = 0;
		break;
	}
	return cap;
}

/* does `box` overlap the region's corner cap at `corner`?  A surface that
 * merely grazes the cap (without containing the exact corner pixel) still
 * gets its corner rounded, so no sharp edge survives next to the arc. */
static bool box_overlaps_corner(const struct window_rounded_region *region,
		const struct wlr_box *box, enum corner_location corner) {
	struct wlr_box cap = region_corner_cap(region, corner);
	if (cap.width <= 0 || cap.height <= 0) {
		return false;
	}
	struct wlr_box inter;
	return wlr_box_intersection(&inter, box, &cap);
}

/* round a scene surface's corners; used for both toplevel content and popups */
struct corner_radius_ctx {
	struct toplevel *tl;            /* toplevel mode: window-region mask */
	struct window_rounded_region region; /* the unified clip region */
	struct wlr_surface *surface;    /* popup mode: round this surface fully */
	int radius;
};

/* root surface of a surface's subsurface chain (itself if not a subsurface) */
static struct wlr_surface *surface_root(struct wlr_surface *surface) {
	struct wlr_subsurface *sub =
		wlr_subsurface_try_from_wlr_surface(surface);
	while (sub != NULL && sub->parent != NULL) {
		surface = sub->parent;
		sub = wlr_subsurface_try_from_wlr_surface(surface);
	}
	return surface;
}

static void apply_corner_radius_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct corner_radius_ctx *ctx = data;
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL) {
		return;
	}
	int w = buffer->dst_width > 0 ? buffer->dst_width
		: (buffer->buffer ? buffer->buffer->width : 0);
	int h = buffer->dst_height > 0 ? buffer->dst_height
		: (buffer->buffer ? buffer->buffer->height : 0);
	if (w <= 0 || h <= 0) {
		return;
	}

	if (ctx->tl != NULL) {
		/* iterate the surfaces under the xdg_toplevel: the main surface
		 * and every subsurface share the base surface as their root
		 * (Firefox/Chromium cover the window edges with subsurfaces, so
		 * the main surface alone never reaches all four corners).  Popups
		 * and decoration buffers stay unrounded. */
		struct wlr_xdg_surface *base = ctx->tl->xdg_toplevel->base;
		if (base == NULL ||
				surface_root(scene_surface->surface) != base->surface) {
			return;
		}
		/* clip this node against the window's rounded region: round the
		 * node's own corners that overlap the region's corner caps.  The
		 * region is the same for every node (that is what makes the mask
		 * unified), so interior subsurfaces (scrollbars, toolbars) touch
		 * no cap and stay unclipped. */
		struct wlr_box buf = { sx, sy, w, h };
		enum corner_location corners = CORNER_LOCATION_NONE;
		if (box_overlaps_corner(&ctx->region, &buf,
				CORNER_LOCATION_TOP_LEFT)) {
			corners |= CORNER_LOCATION_TOP_LEFT;
		}
		if (box_overlaps_corner(&ctx->region, &buf,
				CORNER_LOCATION_TOP_RIGHT)) {
			corners |= CORNER_LOCATION_TOP_RIGHT;
		}
		if (box_overlaps_corner(&ctx->region, &buf,
				CORNER_LOCATION_BOTTOM_LEFT)) {
			corners |= CORNER_LOCATION_BOTTOM_LEFT;
		}
		if (box_overlaps_corner(&ctx->region, &buf,
				CORNER_LOCATION_BOTTOM_RIGHT)) {
			corners |= CORNER_LOCATION_BOTTOM_RIGHT;
		}
		wlr_scene_buffer_set_corner_radius(buffer, ctx->radius, corners);
	} else if (ctx->surface != NULL &&
			scene_surface->surface == ctx->surface) {
		/* popup: round the popup's own surface fully */
		wlr_scene_buffer_set_corner_radius(buffer, ctx->radius,
			CORNER_LOCATION_ALL);
	}
}

static void toplevel_apply_corner_radius(struct toplevel *tl) {
	struct window_rounded_region region;
	if (!window_rounded_region_get(tl, &region)) {
		return;
	}
	struct corner_radius_ctx ctx = {
		.tl = tl,
		.region = region,
		.radius = region.radius,
	};
	wlr_scene_node_for_each_buffer(&tl->scene_tree->node,
		apply_corner_radius_cb, &ctx);
}

static void popup_apply_corner_radius(struct toplevel_popup *pp) {
	if (pp->tree == NULL || pp->popup == NULL || pp->popup->base == NULL) {
		return;
	}
	struct corner_radius_ctx ctx = {
		.surface = pp->popup->base->surface,
		.radius = popup_content_radius(),
	};
	wlr_scene_node_for_each_buffer(&pp->tree->node,
		apply_corner_radius_cb, &ctx);
}

/* 0xAARRGGBB -> premultiplied float rgba (scene rects blend premultiplied) */
static void argb_to_premul(float out[4], uint32_t color) {
	float a = (float)((color >> 24) & 0xFF) / 255.0f;
	out[0] = (float)((color >> 16) & 0xFF) / 255.0f * a;
	out[1] = (float)((color >> 8) & 0xFF) / 255.0f * a;
	out[2] = (float)((color >> 0) & 0xFF) / 255.0f * a;
	out[3] = a;
}

/* 0xAARRGGBB -> straight float rgba (scene shadows blend straight alpha) */
static void argb_to_straight(float out[4], uint32_t color) {
	out[0] = (float)((color >> 16) & 0xFF) / 255.0f;
	out[1] = (float)((color >> 8) & 0xFF) / 255.0f;
	out[2] = (float)((color >> 0) & 0xFF) / 255.0f;
	out[3] = (float)((color >> 24) & 0xFF) / 255.0f;
}

/* the border color for a toplevel: brighter for the focused window, muted
 * for everything else; only fullscreen uses the dedicated color */
static uint32_t toplevel_border_color(struct toplevel *tl) {
	if (tl->fullscreen) {
		return CONFIG_FULLSCREEN_BORDER_COLOR;
	}
	if (tl->server->focused == tl) {
		return CONFIG_BORDER_COLOR;
	}
	return CONFIG_BORDER_COLOR_UNFOCUSED;
}

/* the three top-band segment colors.  Normal windows get the three
 * minimize/maximize/close colors; dialogs and fixed-size windows (whose top
 * band is one close button) get the close color everywhere; fullscreen
 * windows get a single unified strip in the ring color. */
static void toplevel_segment_colors(struct toplevel *tl, uint32_t out[3]) {
	bool dialog = toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl);
	if (tl->fullscreen) {
		uint32_t ring = toplevel_border_color(tl);
		out[0] = out[1] = out[2] = ring;
	} else if (dialog) {
		out[0] = out[1] = out[2] = CONFIG_BORDER_COLOR_CLOSE;
	} else {
		out[0] = CONFIG_BORDER_COLOR_MIN;
		out[1] = CONFIG_BORDER_COLOR_MAX;
		out[2] = CONFIG_BORDER_COLOR_CLOSE;
	}
}

/* ------------------------------------------------------------------ */
/* top-band gradient                                                  */
/* ------------------------------------------------------------------ */

/* the ring's top edge is drawn as a smooth three-color gradient.  scenefx's
 * scene API has no gradient node, so the ramp is sampled per-pixel into a
 * tiny CPU buffer (a data-ptr wlr_buffer) which is stretched over the top
 * band with a wlr_scene_buffer.  The ramp matches the original shader:
 * three equal thirds with smoothstep-blended seams over
 * CONFIG_BORDER_BAND_BLEND px. */

#define BAND_BUF_W 256
#define BAND_BUF_H 1

/* the band is purely visual: never let it intercept pointer input */
static bool band_buffer_no_input(struct wlr_scene_buffer *buffer,
		double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

/* a CPU-backed wlr_buffer holding the gradient pixels */
struct band_gradient {
	struct wlr_buffer base;
	uint32_t *pixels;
	int width, height;
};

static void band_gradient_destroy(struct wlr_buffer *wlr_buffer) {
	struct band_gradient *bg = wl_container_of(wlr_buffer, bg, base);
	free(bg->pixels);
	free(bg);
}

static bool band_gradient_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *attribs) {
	return false;
}

static bool band_gradient_get_shm(struct wlr_buffer *wlr_buffer,
		struct wlr_shm_attributes *attribs) {
	return false;
}

static bool band_gradient_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct band_gradient *bg = wl_container_of(wlr_buffer, bg, base);
	*data = bg->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)bg->width * 4;
	return true;
}

static void band_gradient_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
}

static const struct wlr_buffer_impl band_gradient_impl = {
	.destroy = band_gradient_destroy,
	.get_dmabuf = band_gradient_get_dmabuf,
	.get_shm = band_gradient_get_shm,
	.begin_data_ptr_access = band_gradient_begin_data_ptr_access,
	.end_data_ptr_access = band_gradient_end_data_ptr_access,
};

/* local smoothstep (avoids libc feature-macro quirks with _POSIX_C_SOURCE) */
static float smoothstepf(float edge0, float edge1, float x) {
	float t = fminf(fmaxf((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

/* sample the three-thirds ramp at a normalized position t in [0,1] across
 * the band: solid thirds with smoothstep-blended seams, and the band's two
 * ends fade into the ring color so the rounded corners blend in seamlessly */
static void band_ramp_sample(float t, float blend_frac, float end_fade,
		const float min[4], const float max[4], const float close[4],
		const float base[4], float out[4]) {
	float s1 = smoothstepf(1.0f / 3.0f - blend_frac, 1.0f / 3.0f + blend_frac,
		t);
	float s2 = smoothstepf(2.0f / 3.0f - blend_frac, 2.0f / 3.0f + blend_frac,
		t);
	float lf = smoothstepf(0.0f, end_fade, t);            /* left corner */
	float rf = 1.0f - smoothstepf(1.0f - end_fade, 1.0f, t); /* right corner */
	float ends = lf * rf; /* 0 at both ends (ring color), 1 in the middle */
	for (int i = 0; i < 4; i++) {
		float c = min[i] + (max[i] - min[i]) * s1;
		c = c + (close[i] - c) * s2;
		out[i] = base[i] + (c - base[i]) * ends;
	}
}

/* fill a fresh gradient buffer and publish it on the band scene buffer;
 * the scene locks the buffer, our reference is dropped */
static bool band_render_gradient(struct toplevel *tl, uint32_t colors[3],
		uint32_t base_color, int band_w) {
	struct band_gradient *bg = calloc(1, sizeof(*bg));
	if (bg == NULL) {
		return false;
	}
	bg->width = BAND_BUF_W;
	bg->height = BAND_BUF_H;
	bg->pixels = calloc((size_t)bg->width * bg->height, sizeof(uint32_t));
	if (bg->pixels == NULL) {
		free(bg);
		return false;
	}
	wlr_buffer_init(&bg->base, &band_gradient_impl, bg->width, bg->height);

	float min[4], max[4], close[4], base[4];
	argb_to_premul(min, colors[0]);
	argb_to_premul(max, colors[1]);
	argb_to_premul(close, colors[2]);
	argb_to_premul(base, base_color);

	float blend_frac = (float)CONFIG_BORDER_BAND_BLEND / (float)band_w;
	float end_fade = (float)CONFIG_BORDER_BAND_BLEND / (float)band_w;
	for (int x = 0; x < bg->width; x++) {
		float t = (float)x / (float)(bg->width - 1);
		float c[4];
		band_ramp_sample(t, blend_frac, end_fade, min, max, close, base, c);
		uint32_t a = (uint32_t)lrintf(c[3] * 255.0f);
		uint32_t r = (uint32_t)lrintf(c[0] * 255.0f);
		uint32_t g = (uint32_t)lrintf(c[1] * 255.0f);
		uint32_t b = (uint32_t)lrintf(c[2] * 255.0f);
		bg->pixels[x] = (a << 24) | (r << 16) | (g << 8) | b;
	}

	/* the scene locks the buffer; drop our reference */
	wlr_scene_buffer_set_buffer(tl->band, &bg->base);
	wlr_buffer_drop(&bg->base);
	return true;
}

/* update the popup's rounded ring and content corners */
static void xdg_popup_decoration(struct toplevel_popup *pp) {
	struct wlr_xdg_surface *base = pp->popup->base;
	if (base == NULL) {
		return;
	}

	if (pp->border != NULL) {
		if (CONFIG_POPUP_BORDER_ENABLED && CONFIG_POPUP_BORDER_WIDTH > 0 &&
				base->surface->mapped && base->surface->buffer != NULL &&
				base->geometry.width > 0 && base->geometry.height > 0) {
			int bw = (int)(CONFIG_POPUP_BORDER_WIDTH + 0.5);
			int radius = popup_content_radius();
			float premul[4];
			argb_to_premul(premul, CONFIG_POPUP_BORDER_COLOR);
			wlr_scene_rect_set_color(pp->border, premul);
			wlr_scene_rect_set_size(pp->border,
				base->geometry.width + 2 * bw,
				base->geometry.height + 2 * bw);
			wlr_scene_rect_set_corner_radius(pp->border,
				CONFIG_BORDER_RADIUS, CORNER_LOCATION_ALL);
			wlr_scene_rect_set_clipped_region(pp->border,
				(struct clipped_region) {
					.area = { bw, bw, base->geometry.width,
						base->geometry.height },
					.corner_radius = radius,
					.corners = CORNER_LOCATION_ALL,
				});
			wlr_scene_node_set_position(&pp->border->node,
				-base->geometry.x - bw, -base->geometry.y - bw);
			wlr_scene_node_set_enabled(&pp->border->node, true);
			wlr_scene_node_raise_to_top(&pp->border->node);
		} else {
			wlr_scene_node_set_enabled(&pp->border->node, false);
		}
	}

	popup_apply_corner_radius(pp);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, commit);
	(void)data;
	xdg_popup_decoration(pp);
}

/* wayland's wl_list_remove nulls the link (prev/next become NULL), so a
 * second wl_list_remove on the same listener dereferences NULL.  Remove a
 * listener only while it is still attached to a signal. */
static void listener_remove_if_attached(struct wl_listener *listener) {
	if (listener->link.prev != NULL) {
		wl_list_remove(&listener->link);
	}
}

/* the wlr_xdg_popup role object is gone (menu closed / grab dismissed):
 * stop reacting to its commits and hide the border.  The struct itself is
 * freed by xdg_popup_tree_destroy once the scene tree goes away. */
static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, destroy);
	(void)data;
	listener_remove_if_attached(&pp->commit);
	listener_remove_if_attached(&pp->new_popup);
	listener_remove_if_attached(&pp->destroy);
	if (pp->border != NULL) {
		wlr_scene_node_set_enabled(&pp->border->node, false);
	}
}

/* the popup's scene tree is destroyed (wlroots destroys it when the popup's
 * xdg surface is destroyed, or together with the toplevel's tree): release
 * every listener and the struct.  This is the popup's only owner - it never
 * outlives the tree, so no dangling listeners can be left behind.  The
 * per-signal removals are guarded: xdg_popup_destroy may already have
 * detached commit/new_popup/destroy when the popup role died first. */
static void xdg_popup_tree_destroy(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, tree_destroy);
	(void)data;
	listener_remove_if_attached(&pp->tree_destroy);
	listener_remove_if_attached(&pp->commit);
	listener_remove_if_attached(&pp->new_popup);
	listener_remove_if_attached(&pp->destroy);
	free(pp);
}

static void xdg_popup_attach(struct toplevel *tl, struct wlr_xdg_popup *popup,
		struct wlr_scene_tree *parent_tree) {
	struct toplevel_popup *pp = calloc(1, sizeof(*pp));
	if (pp == NULL) {
		return;
	}
	pp->tl = tl;
	pp->popup = popup;

	/* unconstrain the popup into its output box (and thereby send its
	 * first configure) on its first commit.  Registered here so nested
	 * popups (Qt submenus) get it too, not just toplevel-level popups. */
	struct popup_unconstrain *pu = calloc(1, sizeof(*pu));
	if (pu != NULL) {
		pu->popup = popup;
		pu->server = tl->server;
		pu->commit.notify = popup_unconstrain_handle_commit;
		wl_signal_add(&popup->base->surface->events.commit, &pu->commit);
		pu->destroy.notify = popup_unconstrain_handle_destroy;
		wl_signal_add(&popup->base->events.destroy, &pu->destroy);
	}

	/* wlr_scene_xdg_surface_create handles the popup surface and its
	 * subsurfaces, and positions the tree at popup->current.geometry on
	 * every commit.  It also registers its own listener on the popup's
	 * xdg-surface destroy that destroys this tree, so we never destroy it
	 * ourselves (that would free wlroots' listener mid-signal-emit). */
	pp->tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
	if (pp->tree == NULL) {
		free(pp);
		return;
	}
	/* tagged so hit-testing can still resolve the owning window under a
	 * popup (scene.c) */
	xdg_surface_tag(pp->tree, TAG_POPUP, popup);

	/* a thin rounded ring behind the popup content, same as the window ring */
	if (CONFIG_POPUP_BORDER_ENABLED && CONFIG_POPUP_BORDER_WIDTH > 0) {
		pp->border = wlr_scene_rect_create(pp->tree, 0, 0,
			(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
		if (pp->border != NULL) {
			pp->border->accepts_input = false;
			wlr_scene_node_lower_to_bottom(&pp->border->node);
		}
	}

	/* pp lives exactly as long as the tree: when the tree is destroyed
	 * (popup xdg surface gone, or the toplevel's tree gone) the listener
	 * below releases everything.  Register it before the commit / popup
	 * listeners so it is the last one to run on destroy. */
	pp->tree_destroy.notify = xdg_popup_tree_destroy;
	wl_signal_add(&pp->tree->node.events.destroy, &pp->tree_destroy);

	/* nested popups (Qt submenus) attach under this popup's tree */
	pp->new_popup.notify = xdg_popup_new_popup;
	wl_signal_add(&popup->base->events.new_popup, &pp->new_popup);
	pp->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&popup->events.destroy, &pp->destroy);
	pp->commit.notify = xdg_popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &pp->commit);

	xdg_popup_decoration(pp);
}

static void xdg_popup_new_popup(struct wl_listener *listener, void *data) {
	struct toplevel_popup *pp = wl_container_of(listener, pp, new_popup);
	/* a nested popup (Qt submenu): its parent scene node is this popup's
	 * own scene tree */
	xdg_popup_attach(pp->tl, data, pp->tree);
}

static void xdg_toplevel_new_popup(struct wl_listener *listener, void *data) {
	struct toplevel *tl = wl_container_of(listener, tl, new_popup);
	struct wlr_xdg_popup *popup = data;

	/* the popup's parent is the toplevel surface, so its scene tree lives
	 * inside the toplevel's content tree (whose origin is the window
	 * geometry top-left, exactly what the popup geometry is relative to) */
	xdg_popup_attach(tl, popup, tl->scene_tree);
}

/* ------------------------------------------------------------------ */
/* foreign-toplevel management                                        */
/* ------------------------------------------------------------------ */

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
/* scenefx decoration                                                 */
/* ------------------------------------------------------------------ */

/* (re)build the rounded border ring and shadow around the toplevel and
 * round the content corners.  All geometry is relative to the xdg scene
 * tree, which is anchored at the window box's top-left corner. */
void update_toplevel_decoration(struct toplevel *tl) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL) {
		return;
	}
	bool maximized = tl->xdg_toplevel->current.maximized;
	bool show_border = base->surface->mapped && !tl->minimized &&
		!(maximized && !CONFIG_MAXIMIZED_BORDER_ENABLED) &&
		CONFIG_BORDER_WIDTH > 0;
	/* only the focused window casts a shadow; non-focused windows stay flat
	 * (focus changes re-render both windows through update_toplevel_decoration) */
	bool show_shadow = CONFIG_SHADOW_ENABLED && CONFIG_SHADOW_BLUR_SIGMA > 0 &&
		base->surface->mapped && !tl->minimized && !maximized && !tl->fullscreen &&
		tl->server->focused == tl;

	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		show_border = false;
		show_shadow = false;
	}

	int bw = border_thickness();
	int content_r = content_radius(tl);

	/* rounded border ring: a filled rounded rect with a rounded hole
	 * clipped out where the content sits */
	if (tl->border != NULL) {
		if (show_border) {
			uint32_t color = toplevel_border_color(tl);
			float premul[4];
			argb_to_premul(premul, color);
			if (tl->border_color != color) {
				wlr_scene_rect_set_color(tl->border, premul);
				tl->border_color = color;
			}
			wlr_scene_rect_set_size(tl->border, box.width + 2 * bw,
				box.height + 2 * bw);
			wlr_scene_rect_set_corner_radius(tl->border, CONFIG_BORDER_RADIUS,
				CORNER_LOCATION_ALL);
			wlr_scene_rect_set_clipped_region(tl->border,
				(struct clipped_region) {
					.area = { bw, bw, box.width, box.height },
					.corner_radius = content_r,
					.corners = CORNER_LOCATION_ALL,
				});
			wlr_scene_node_set_position(&tl->border->node, -bw, -bw);
			wlr_scene_node_set_enabled(&tl->border->node, true);
		} else {
			wlr_scene_node_set_enabled(&tl->border->node, false);
		}
	}

	/* the ring's top edge: a smooth three-color gradient band (minimize /
	 * maximize-restore / close) spanning the full top band, with its top
	 * corners rounded to match the ring (the trimmed corners show the ring
	 * color behind).  The pointer.c title-strip zones use the same thirds. */
	if (tl->band != NULL) {
		if (show_border) {
			int band_w = box.width + 2 * bw;
			uint32_t seg_colors[3];
			toplevel_segment_colors(tl, seg_colors);
			uint32_t base = toplevel_border_color(tl);
			if (!tl->band_rendered || tl->band_width != band_w ||
					tl->band_colors[0] != seg_colors[0] ||
					tl->band_colors[1] != seg_colors[1] ||
					tl->band_colors[2] != seg_colors[2] ||
					tl->band_base != base) {
				if (band_render_gradient(tl, seg_colors, base, band_w)) {
					tl->band_rendered = true;
					tl->band_width = band_w;
					tl->band_colors[0] = seg_colors[0];
					tl->band_colors[1] = seg_colors[1];
					tl->band_colors[2] = seg_colors[2];
					tl->band_base = base;
				}
			}
			wlr_scene_buffer_set_dest_size(tl->band, band_w, bw);
			wlr_scene_buffer_set_corner_radius(tl->band, CONFIG_BORDER_RADIUS,
				CORNER_LOCATION_TOP);
			wlr_scene_node_set_position(&tl->band->node, -bw, -bw);
			wlr_scene_node_set_enabled(&tl->band->node, true);
		} else {
			wlr_scene_node_set_enabled(&tl->band->node, false);
		}
	}

	/* soft shadow behind the window (scenefx box shadow) */
	if (tl->shadow != NULL) {
		if (show_shadow) {
			int blur = (int)(CONFIG_SHADOW_BLUR_SIGMA + 0.5);
			float straight[4];
			argb_to_straight(straight, CONFIG_SHADOW_COLOR);
			wlr_scene_shadow_set_color(tl->shadow, straight);
			wlr_scene_shadow_set_blur_sigma(tl->shadow,
				CONFIG_SHADOW_BLUR_SIGMA);
			wlr_scene_shadow_set_corner_radius(tl->shadow,
				CONFIG_BORDER_RADIUS);
			int border_w = box.width + 2 * bw;
			int border_h = box.height + 2 * bw;
			wlr_scene_shadow_set_size(tl->shadow, border_w + 2 * blur,
				border_h + 2 * blur);
			wlr_scene_shadow_set_clipped_region(tl->shadow,
				(struct clipped_region) {
					.area = { blur - CONFIG_SHADOW_OFFSET_X,
						blur - CONFIG_SHADOW_OFFSET_Y,
						border_w, border_h },
					.corner_radius = CONFIG_BORDER_RADIUS,
					.corners = CORNER_LOCATION_ALL,
				});
			wlr_scene_node_set_position(&tl->shadow->node,
				-bw - blur + CONFIG_SHADOW_OFFSET_X,
				-bw - blur + CONFIG_SHADOW_OFFSET_Y);
			wlr_scene_node_set_enabled(&tl->shadow->node, true);
		} else {
			wlr_scene_node_set_enabled(&tl->shadow->node, false);
		}
	}

	/* round the content corners (fullscreen windows are rounded like
	 * maximized ones) */
	toplevel_apply_corner_radius(tl);
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

	/* the xdg surface tree handles the surface, subsurfaces and their
	 * positioning; the border/shadow live as lowered siblings inside it */
	tl->scene_tree = wlr_scene_xdg_surface_create(
		server->layers[LAYER_TOPLEVELS], base);
	if (tl->scene_tree == NULL) {
		free(tl->app_id);
		free(tl);
		return;
	}
	xdg_surface_tag(tl->scene_tree, TAG_TOPLEVEL, tl);
	wl_list_init(&tl->subsurfaces);

	/* rounded border ring (purely visual, never intercepts pointer input) */
	tl->border = wlr_scene_rect_create(tl->scene_tree, 0, 0,
		(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
	if (tl->border != NULL) {
		tl->border->accepts_input = false;
		wlr_scene_node_lower_to_bottom(&tl->border->node);
	}

	/* the ring's top edge: a smooth three-color gradient band (minimize /
	 * maximize-restore / close); kept just above the ring and below the
	 * content */
	tl->band = wlr_scene_buffer_create(tl->scene_tree, NULL);
	if (tl->band != NULL) {
		tl->band->point_accepts_input = band_buffer_no_input;
		if (tl->border != NULL) {
			wlr_scene_node_place_above(&tl->band->node, &tl->border->node);
		}
	}

	/* soft window shadow */
	tl->shadow = wlr_scene_shadow_create(tl->scene_tree, 0, 0,
		CONFIG_BORDER_RADIUS, CONFIG_SHADOW_BLUR_SIGMA,
		(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
	if (tl->shadow != NULL) {
		wlr_scene_node_lower_to_bottom(&tl->shadow->node);
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
	tl->new_subsurface.notify = xdg_toplevel_new_subsurface;
	wl_signal_add(&base->surface->events.new_subsurface, &tl->new_subsurface);
	/* subsurfaces that already exist before this listener was added */
	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &base->surface->current.subsurfaces_below,
			current.link) {
		toplevel_subsurface_add(tl, subsurface);
	}
	wl_list_for_each(subsurface, &base->surface->current.subsurfaces_above,
			current.link) {
		toplevel_subsurface_add(tl, subsurface);
	}

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
