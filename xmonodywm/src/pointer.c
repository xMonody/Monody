/*
 * pointer.c - compositor cursor interaction
 *
 * Undecorated windows get an invisible frame owned by the compositor:
 *   - top 20 px          : move / maximize (wheel up) / minimize (wheel
 *                          down) / close (double click) while pressed
 *   - edges / corners    : resize
 * A press in the frame is swallowed by the compositor and never reaches
 * the client; client-side decorated windows keep their native controls.
 */

#include "server.h"

#include <limits.h>
#include <time.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

/* is the cursor over the invisible title strip of an undecorated window? */
static bool is_in_titlebar_zone(struct server *server, struct toplevel *tl) {
	if (tl->decoration_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
		return false; /* client draws its own title bar, use it natively */
	}
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || !base->surface->mapped) {
		return false;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0) {
		return false;
	}
	double lx = server->cursor->x;
	double ly = server->cursor->y;
	return lx >= box.x && lx < box.x + box.width &&
		ly >= box.y && ly < box.y + CONFIG_TITLEBAR_HEIGHT;
}

/* ------------------------------------------------------------------ */
/* window move                                                        */
/* ------------------------------------------------------------------ */

static bool is_double_click(struct server *server, uint32_t button) {
	if (!server->last_was_click || button != server->last_click_button) {
		return false;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t dt = (now.tv_sec - server->last_release_time.tv_sec) * 1000000000L
		+ (now.tv_nsec - server->last_release_time.tv_nsec);
	return dt >= 0 && dt <= CONFIG_DOUBLE_CLICK_NS;
}

void begin_move(struct server *server, struct toplevel *tl,
		double ref_x, double ref_y) {
	if (server->moving) {
		return;
	}
	server->moving = true;
	server->move_toplevel = tl;
	server->grab_x = ref_x - tl->scene_tree->node.x;
	server->grab_y = ref_y - tl->scene_tree->node.y;
}

void end_move(struct server *server) {
	server->moving = false;
	server->move_toplevel = NULL;
	server->zone_toplevel = NULL;
	server->zone_press = false;
	server->dragged = false;
	server->close_pending = false;
}

static void move_toplevel_to(struct server *server, double lx, double ly) {
	if (server->move_toplevel != NULL) {
		wlr_scene_node_set_position(&server->move_toplevel->scene_tree->node,
			lx - server->grab_x, ly - server->grab_y);
		update_toplevel_decoration(server->move_toplevel);
	}
}

/* ------------------------------------------------------------------ */
/* edge resize                                                        */
/* ------------------------------------------------------------------ */

/* which edges of the toplevel the cursor is over (top edge excluded: the
 * top 10 px are the title strip). Only compositor-owned windows (no
 * client-side decoration) resize here; CSD windows handle their own edges.
 * The grab zones live *outside* the window box: the moment the pointer
 * crosses into the window the normal cursor style applies again. */
static uint32_t toplevel_resize_edges(struct server *server,
		struct toplevel *tl) {
	if (tl->minimized || tl->xdg_toplevel->base == NULL ||
			tl->decoration_mode ==
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ||
			tl->xdg_toplevel->current.maximized) {
		return 0;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return 0;
	}
	double lx = server->cursor->x;
	double ly = server->cursor->y;
	uint32_t edges = 0;
	/* left edge (and bottom-left corner, diagonally): 20 px outside the
	 * box, spanning the window height plus the corner strip */
	if (lx >= box.x - CONFIG_EDGE_THICKNESS && lx < box.x &&
			ly >= box.y && ly <= box.y + box.height + CONFIG_EDGE_THICKNESS) {
		edges |= WLR_EDGE_LEFT;
	}
	if (lx > box.x + box.width &&
			lx <= box.x + box.width + CONFIG_EDGE_THICKNESS &&
			ly >= box.y && ly <= box.y + box.height + CONFIG_EDGE_THICKNESS) {
		edges |= WLR_EDGE_RIGHT;
	}
	if (ly > box.y + box.height &&
			ly <= box.y + box.height + CONFIG_EDGE_THICKNESS &&
			lx >= box.x - CONFIG_EDGE_THICKNESS &&
			lx <= box.x + box.width + CONFIG_EDGE_THICKNESS) {
		edges |= WLR_EDGE_BOTTOM;
	}
	return edges;
}

static const char *resize_cursor_name(uint32_t edges) {
	bool left = (edges & WLR_EDGE_LEFT) != 0;
	bool right = (edges & WLR_EDGE_RIGHT) != 0;
	bool bottom = (edges & WLR_EDGE_BOTTOM) != 0;
	if (left && bottom) {
		return "nesw-resize";
	}
	if (right && bottom) {
		return "nwse-resize";
	}
	if (left || right) {
		return "ew-resize";
	}
	return "ns-resize";
}

static void set_cursor_override(struct server *server, const char *name) {
	if (server->cursor_override == name) {
		return;
	}
	server->cursor_override = name;
	wlr_log(WLR_DEBUG, "cursor: %s", name);
	wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, name);
}

static void clear_cursor_override(struct server *server) {
	if (server->cursor_override == NULL) {
		return;
	}
	server->cursor_override = NULL;
	/* only hand the pointer back to the client when the pointer is still
	 * over that client's surface (e.g. leaving the resize strip back into
	 * the window). If the pointer moved out to empty desktop, the stored
	 * client cursor (e.g. the terminal's text caret) would be stale, so
	 * fall back to the default arrow. The client draws its cursor on a
	 * separate surface, so compare the owning clients rather than the
	 * surfaces themselves. */
	struct wlr_surface *focused =
		server->seat->pointer_state.focused_surface;
	bool over_client_surface = focused != NULL &&
		server->client_cursor_surface != NULL &&
		focused->resource->client ==
			server->client_cursor_surface->resource->client;
	wlr_log(WLR_DEBUG, "cursor: %s", over_client_surface ?
		"restore-client" : "left_ptr");
	if (over_client_surface) {
		wlr_cursor_set_surface(server->cursor, server->client_cursor_surface,
			server->client_cursor_hotspot_x,
			server->client_cursor_hotspot_y);
	} else {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
}

/* the toplevel the cursor interacts with: the window under the cursor, or
 * - when the cursor is outside every window - the one whose resize grab
 * zone (CONFIG_EDGE_THICKNESS px around the box) contains the cursor, so
 * the resize handles stay reachable even though they live outside the box */
static struct toplevel *toplevel_nearby(struct server *server) {
	struct toplevel *tl = toplevel_at(server);
	if (tl != NULL && !tl->minimized) {
		return tl;
	}
	struct toplevel *candidate;
	wl_list_for_each(candidate, &server->toplevels, link) {
		if (toplevel_resize_edges(server, candidate) != 0) {
			return candidate;
		}
	}
	return NULL;
}

/* decide which compositor-owned cursor to show at the current position */
void update_cursor_style(struct server *server) {
	const char *name = NULL;

	if (server->moving && server->move_toplevel != NULL) {
		name = "grabbing";
	} else if (server->resizing && server->resize_toplevel != NULL) {
		name = resize_cursor_name(server->resize_edges);
	} else {
		struct toplevel *tl = toplevel_nearby(server);
		if (tl != NULL) {
			if (is_in_titlebar_zone(server, tl)) {
				name = "move";
			} else {
				uint32_t edges = toplevel_resize_edges(server, tl);
				if (edges != 0) {
					name = resize_cursor_name(edges);
				}
			}
		}
	}

	if (name != NULL) {
		set_cursor_override(server, name);
	} else if (server->cursor_override != NULL) {
		/* leave the pointer over the client's own cursor (or the default
		 * one, which the motion handler resets when the surface changes) */
		clear_cursor_override(server);
	}
}

static void begin_resize(struct server *server, struct toplevel *tl,
		uint32_t edges) {
	if (server->resizing) {
		return;
	}
	server->resizing = true;
	server->resize_toplevel = tl;
	server->resize_edges = edges;
	server->press_x = server->cursor->x;
	server->press_y = server->cursor->y;
	toplevel_box(tl, &server->resize_orig);
	wlr_xdg_toplevel_set_resizing(tl->xdg_toplevel, true);
	focus_toplevel(server, tl);
}

static void update_resize(struct server *server) {
	struct toplevel *tl = server->resize_toplevel;
	if (tl == NULL || tl->xdg_toplevel->base == NULL) {
		return;
	}
	double dx = server->cursor->x - server->press_x;
	double dy = server->cursor->y - server->press_y;
	struct wlr_box orig = server->resize_orig;

	int nw = orig.width;
	int nh = orig.height;
	int nx = orig.x;
	int ny = orig.y;
	if ((server->resize_edges & WLR_EDGE_RIGHT) != 0) {
		nw = orig.width + (int)dx;
	}
	if ((server->resize_edges & WLR_EDGE_LEFT) != 0) {
		nw = orig.width - (int)dx;
		nx = orig.x + (int)dx;
	}
	if ((server->resize_edges & WLR_EDGE_BOTTOM) != 0) {
		nh = orig.height + (int)dy;
	}

	/* clamp to the client's constraints (or a sane fallback) */
	int min_w = tl->xdg_toplevel->current.min_width > 0
		? tl->xdg_toplevel->current.min_width : 40;
	int min_h = tl->xdg_toplevel->current.min_height > 0
		? tl->xdg_toplevel->current.min_height : 30;
	int max_w = tl->xdg_toplevel->current.max_width > 0
		? tl->xdg_toplevel->current.max_width : INT_MAX;
	int max_h = tl->xdg_toplevel->current.max_height > 0
		? tl->xdg_toplevel->current.max_height : INT_MAX;
	if (nw < min_w) {
		nw = min_w;
		nx = orig.x + orig.width - nw; /* keep the right edge fixed */
	}
	if (nw > max_w) {
		nw = max_w;
		nx = orig.x + orig.width - nw;
	}
	if (nh < min_h) {
		nh = min_h;
	}
	if (nh > max_h) {
		nh = max_h;
	}

	wlr_scene_node_set_position(&tl->scene_tree->node, nx, ny);
	wlr_xdg_toplevel_set_size(tl->xdg_toplevel, nw, nh);
	update_toplevel_decoration(tl);
}

void end_resize(struct server *server) {
	if (!server->resizing) {
		return;
	}
	if (server->resize_toplevel != NULL &&
			server->resize_toplevel->xdg_toplevel->base != NULL) {
		wlr_xdg_toplevel_set_resizing(server->resize_toplevel->xdg_toplevel,
			false);
	}
	server->resizing = false;
	server->resize_toplevel = NULL;
	server->resize_edges = 0;
}

/* ------------------------------------------------------------------ */
/* cursor event processing                                            */
/* ------------------------------------------------------------------ */

static void process_cursor_motion(struct server *server, uint32_t time_msec) {
	if (server->drag_tree != NULL) {
		wlr_scene_node_set_position(&server->drag_tree->node,
			server->cursor->x, server->cursor->y);
	}

	/* keep the input method's candidate window glued to the cursor */
	ime_update_popup(server);

	if (server->moving && server->move_toplevel != NULL) {
		move_toplevel_to(server, server->cursor->x, server->cursor->y);
		update_cursor_style(server);
		return;
	}

	if (server->resizing && server->resize_toplevel != NULL) {
		update_resize(server);
		update_cursor_style(server);
		return;
	}

	if (server->zone_press && server->zone_toplevel != NULL) {
		if (!server->dragged) {
			double dx = server->cursor->x - server->press_x;
			double dy = server->cursor->y - server->press_y;
			if (dx * dx + dy * dy > CONFIG_DRAG_THRESHOLD * CONFIG_DRAG_THRESHOLD) {
				server->dragged = true;
				server->close_pending = false;
				struct toplevel *tl = server->zone_toplevel;
				/* anchor the grab at the original press position */
				double ref_x = server->press_x;
				double ref_y = server->press_y;
				if (tl->xdg_toplevel->current.maximized) {
					/* drag of a maximized window's title bar: restore it to
					 * its previous geometry and clamp the grab point into the
					 * restored window, so the cursor grips its title bar and
					 * the window follows (Windows behavior) */
					restore_maximized_toplevel(tl);
					struct wlr_box rb = tl->restore_box;
					if (tl->has_restore_box && rb.width > 0) {
						if (ref_x < rb.x) {
							ref_x = rb.x;
						}
						if (ref_x > rb.x + rb.width - 1) {
							ref_x = rb.x + rb.width - 1;
						}
						if (ref_y < rb.y) {
							ref_y = rb.y;
						}
						if (ref_y > rb.y + rb.height - 1) {
							ref_y = rb.y + rb.height - 1;
						}
					}
				}
				begin_move(server, tl, ref_x, ref_y);
				move_toplevel_to(server, server->cursor->x,
					server->cursor->y);
			}
		} else if (server->moving) {
			move_toplevel_to(server, server->cursor->x, server->cursor->y);
		}
		return;
	}

	/* normal path: forward pointer motion to the surface under the cursor */
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, server->cursor->x, server->cursor->y,
		&sx, &sy);
	struct wlr_surface *surface = NULL;
	if (node != NULL && node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(buffer);
		if (scene_surface != NULL) {
			surface = scene_surface->surface;
		}
	}

	if (server->seat->pointer_state.focused_surface != surface) {
		clear_cursor_override(server);
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager,
			"left_ptr");
	}
	update_cursor_style(server);
	if (surface != NULL) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

static void process_cursor_button(struct server *server, uint32_t time_msec,
		uint32_t button, enum wl_pointer_button_state state) {
	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->resizing) {
			end_resize(server);
			update_cursor_style(server);
			return;
		}
		if (server->moving) {
			/* end of a move (zone drag or xdg_toplevel.move) */
			bool client_initiated = !server->zone_press;
			end_move(server);
			if (client_initiated) {
				/* the client started the move from its own button press,
				 * forward the release so it doesn't stay stuck */
				wlr_seat_pointer_notify_button(server->seat, time_msec,
					button, state);
				wlr_seat_pointer_notify_frame(server->seat);
			}
			/* don't leave the cursor stuck on "grabbing" after the release */
			update_cursor_style(server);
			return;
		}
		if (server->zone_press) {
			/* release of a swallowed zone press */
			struct toplevel *tl = server->zone_toplevel;
			server->zone_press = false;
			server->zone_toplevel = NULL;
			if (!server->dragged) {
				if (server->close_pending && tl != NULL) {
					close_toplevel(tl);
				} else {
					clock_gettime(CLOCK_MONOTONIC,
						&server->last_release_time);
					server->last_was_click = true;
					server->last_click_button = button;
				}
			} else {
				server->last_was_click = false;
			}
			server->dragged = false;
			server->close_pending = false;
			update_cursor_style(server);
			return;
		}
		wlr_seat_pointer_notify_button(server->seat, time_msec, button, state);
		return;
	}

	/* WLR_BUTTON_PRESSED */
	if (server->moving || server->zone_press || server->resizing) {
		return; /* already grabbed */
	}

	struct toplevel *tl = toplevel_nearby(server);
	if (tl != NULL && !tl->minimized && is_in_titlebar_zone(server, tl)) {
		/* take over: the top 10 px are our invisible title bar */
		focus_toplevel(server, tl);
		server->zone_press = true;
		server->zone_toplevel = tl;
		server->press_x = server->cursor->x;
		server->press_y = server->cursor->y;
		server->dragged = false;
		server->close_pending = is_double_click(server, button);
		if (server->close_pending) {
			server->last_was_click = false; /* consumed by the double click */
		}
		return;
	}

	if (tl != NULL && !tl->minimized) {
		uint32_t edges = toplevel_resize_edges(server, tl);
		if (edges != 0) {
			/* take over: the left/right/bottom edge is a resize handle */
			begin_resize(server, tl, edges);
			return;
		}
		focus_toplevel(server, tl);
	}
	wlr_seat_pointer_notify_button(server->seat, time_msec, button, state);
}

static void process_cursor_axis(struct server *server, uint32_t time_msec,
		struct wlr_pointer_axis_event *event) {
	struct toplevel *tl = toplevel_at(server);
	if (tl != NULL && !tl->minimized && server->zone_press &&
			event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
			event->delta_discrete != 0 &&
			is_in_titlebar_zone(server, tl)) {
		/* while holding a button over the top 10 px:
		 * wheel up = maximize, wheel down = minimize */
		if (event->delta_discrete > 0) {
			set_maximized(server, tl, true);
		} else {
			set_minimized(server, tl, true);
		}
		return;
	}
	wlr_seat_pointer_notify_axis(server->seat, time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source,
		event->relative_direction);
}

void cursor_motion(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

void cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

void cursor_button(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	process_cursor_button(server, event->time_msec, event->button,
		event->state);
}

void cursor_axis(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	process_cursor_axis(server, event->time_msec, event);
}

void cursor_frame(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}
