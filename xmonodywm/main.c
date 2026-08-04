/*
 * xmonodywm - a minimal floating Wayland compositor built on wlroots 0.19
 *
 * Features:
 *  - protocols: wl_compositor, wl_surface, wl_seat, wl_shm, linux-dmabuf,
 *    xdg-shell, viewporter, presentation-time, wlr-layer-shell,
 *    xdg-decoration, wlr-output-management, wlr-foreign-toplevel-management
 *  - fully floating windows; undecorated windows get a rounded border
 *    (color #7C73B0) drawn by the compositor
 *  - client-side decorated windows keep their native title bars and their
 *    native move/close/maximize controls (xdg_toplevel.move is honored)
 *  - for windows without client-side decoration, the top 20 px of the window
 *    acts as an invisible title bar:
 *      * drag with the left/right button held  -> move the window
 *      * scroll wheel up   (while holding)     -> maximize
 *      * scroll wheel down (while holding)     -> minimize
 *      * double click                          -> close
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <cjson/cJSON.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <wlr/backend.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include <xkbcommon/xkbcommon.h>

#define TITLEBAR_HEIGHT 20             /* invisible title strip at the window top */
#define EDGE_THICKNESS 20              /* resize grab zone on window edges/corners */
#define BORDER_WIDTH 2                 /* server-side border stroke width */
#define BORDER_RADIUS 12               /* rounded-corner radius of the border */
#define BORDER_COLOR 0xFF7C73B0u       /* ARGB border color (#7C73B0) */
#define DOUBLE_CLICK_NS (400 * 1000000L) /* 400 ms */
#define DRAG_THRESHOLD 8.0               /* px before a press becomes a drag */

/* ordering of scene-graph trees (bottom to top) */
enum scene_layer {
	LAYER_BACKGROUND = 0, /* wlr-layer-shell background   */
	LAYER_BOTTOM,         /* wlr-layer-shell bottom       */
	LAYER_TOPLEVELS,      /* normal windows               */
	LAYER_TOP,            /* wlr-layer-shell top          */
	LAYER_OVERLAY,        /* wlr-layer-shell overlay + drag icon */
	LAYER_COUNT,
};

static int scene_layer_index(enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return LAYER_BACKGROUND;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:      return LAYER_BOTTOM;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:         return LAYER_TOP;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:     return LAYER_OVERLAY;
	}
	return LAYER_OVERLAY;
}

struct server;
struct toplevel;
struct layer_surface;

/* tag stored in wlr_scene_node.data so we can find the owning object from
 * an arbitrary hit-tested scene node (walking up the parents). */
enum scene_tag_type {
	TAG_TOPLEVEL,
	TAG_LAYER,
};

struct scene_tag {
	enum scene_tag_type type;
	void *ptr;

	struct wl_listener destroy; /* frees the tag when the node is destroyed */
};

struct monitor {
	struct server *server;
	struct wlr_output *output;
	struct wlr_scene_output *scene_output;

	struct wl_listener frame;
	struct wl_listener destroy;
};

struct toplevel {
	struct server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wlr_foreign_toplevel_handle_v1 *fthandle;
	struct wlr_output *last_output;

	/* server-side decoration (rounded border); the content tree lives inside
	 * deco_tree so the border stays glued to the window when raised */
	struct wlr_scene_tree *deco_tree;
	struct wlr_scene_buffer *deco_border;
	struct border_buffer *deco_buffer; /* current border pixels */
	int deco_w, deco_h;               /* border buffer size */

	/* xdg-decoration */
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	enum wlr_xdg_toplevel_decoration_v1_mode decoration_mode;
	bool decoration_configured;

	bool minimized;
	bool positioned; /* initial position has been assigned */

	/* geometry to restore when un-maximizing by dragging (Windows style) */
	struct wlr_box restore_box;
	bool has_restore_box;

	/* fullscreen state (tracks current.fullscreen which only updates on ack) */
	bool fullscreen;

	/* id exposed to status bars over the IPC socket */
	int id;
	bool ipc_added; /* window_added was emitted */
	char *app_id;   /* cached app_id (survives teardown for window_removed) */

	struct wl_list link; /* server.toplevels */

	struct wl_listener destroy;
	struct wl_listener toplevel_destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener request_maximize;
	struct wl_listener request_minimize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_move;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener new_popup;

	struct wl_listener ft_request_maximize;
	struct wl_listener ft_request_minimize;
	struct wl_listener ft_request_activate;
	struct wl_listener ft_request_close;
	struct wl_listener ft_destroy;

	struct wl_listener deco_request_mode;
	struct wl_listener deco_destroy;
};

struct layer_surface {
	struct server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;

	struct wl_list link; /* server.layer_surfaces */

	struct wl_listener destroy;
	struct wl_listener commit;
};

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layers[LAYER_COUNT];
	struct wlr_output_layout *output_layout;
	struct wlr_output_manager_v1 *output_manager;
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wlr_input_device *keyboard_device;
	struct wl_listener keyboard_modifiers;
	struct wl_listener keyboard_key;
	struct wl_listener keyboard_destroy;

	struct wl_list toplevels;      /* struct toplevel.link */
	struct wl_list layer_surfaces; /* struct layer_surface.link */
	struct toplevel *focused;

	/* IPC socket for status bars (JSON events) */
	int ipc_fd;
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients; /* ipc_client.link */
	uint32_t next_window_id;

	struct wlr_scene_tree *drag_tree;

	/* pointer interaction state */
	struct toplevel *zone_toplevel; /* toplevel under an active zone press */
	bool zone_press;                /* press in the top-10px zone, swallowed */
	bool moving;                    /* a window move is in progress */
	struct toplevel *move_toplevel;
	double grab_x, grab_y; /* cursor offset from the window origin */
	double press_x, press_y;
	bool dragged;       /* moved beyond DRAG_THRESHOLD during a press */
	bool close_pending; /* second click of a double click is armed */
	struct timespec last_release_time;
	bool last_was_click;
	uint32_t last_click_button;

	/* edge resize state */
	bool resizing;
	struct toplevel *resize_toplevel;
	uint32_t resize_edges;     /* enum wlr_edges */
	struct wlr_box resize_orig; /* window box at grab start */

	/* current compositor-driven cursor name, NULL when the client's cursor
	 * is shown (used to avoid redundant updates) */
	const char *cursor_override;

	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener new_virtual_pointer;
	struct wl_listener layout_change;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_layer_surface;
	struct wl_listener new_decoration;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener seat_request_set_cursor;
	struct wl_listener seat_request_set_selection;
	struct wl_listener seat_request_set_primary_selection;
	struct wl_listener seat_request_start_drag;
	struct wl_listener seat_start_drag;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void focus_toplevel(struct server *server, struct toplevel *tl);
static void set_maximized(struct server *server, struct toplevel *tl,
	bool maximized);
static void set_minimized(struct server *server, struct toplevel *tl,
	bool minimized);
static void update_toplevel_decoration(struct toplevel *tl);
static void update_cursor_style(struct server *server);
static void close_toplevel(struct toplevel *tl);
static void focus_window(struct server *server, struct toplevel *tl);

static void scene_tag_destroy(struct wl_listener *listener, void *data) {
	struct scene_tag *tag = wl_container_of(listener, tag, destroy);
	wl_list_remove(&tag->destroy.link);
	free(tag);
}

static void xdg_surface_tag(struct wlr_scene_tree *tree,
		enum scene_tag_type type, void *ptr) {
	struct scene_tag *tag = calloc(1, sizeof(*tag));
	if (tag == NULL) {
		return;
	}
	tag->type = type;
	tag->ptr = ptr;
	tag->destroy.notify = scene_tag_destroy;
	wl_signal_add(&tree->node.events.destroy, &tag->destroy);
	tree->node.data = tag;
}

/* find the tagged object under the given layout coordinates */
static void *scene_tag_at(struct server *server, enum scene_tag_type type,
		double lx, double ly) {
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, &sx, &sy);
	if (node == NULL) {
		return NULL;
	}
	struct wlr_scene_node *n = node;
	while (n != NULL) {
		if (n->data != NULL) {
			struct scene_tag *tag = n->data;
			if (tag->type == type) {
				return tag->ptr;
			}
			return NULL; /* a closer tagged object won the hit test */
		}
		n = n->parent != NULL ? &n->parent->node : NULL;
	}
	return NULL;
}
static struct toplevel *toplevel_at(struct server *server) {
	return scene_tag_at(server, TAG_TOPLEVEL, server->cursor->x,
		server->cursor->y);
}

/* effective window geometry box in layout coordinates */
static void toplevel_box(struct toplevel *tl, struct wlr_box *box) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	box->x = tl->scene_tree->node.x;
	box->y = tl->scene_tree->node.y;
	box->width = base != NULL ? base->geometry.width : 0;
	box->height = base != NULL ? base->geometry.height : 0;
}

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
		ly >= box.y && ly < box.y + TITLEBAR_HEIGHT;
}

/* output under the toplevel (its center), or the center output */
static struct wlr_output *toplevel_output(struct server *server,
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

/* output work area: output box shrunk by layer-shell exclusive zones */
static void layer_surface_exclusive_zone(struct wlr_layer_surface_v1_state *state,
		struct wlr_box *area) {
	uint32_t top = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	uint32_t bottom = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	uint32_t left = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	uint32_t right = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	uint32_t anchor = state->anchor;
	int32_t zone = state->exclusive_zone;
	if (zone <= 0) {
		return;
	}
	if (anchor == top || anchor == (top | left | right)) {
		area->y += zone + state->margin.top;
		area->height -= zone + state->margin.top;
	} else if (anchor == bottom || anchor == (bottom | left | right)) {
		area->height -= zone + state->margin.bottom;
	} else if (anchor == left || anchor == (top | bottom | left)) {
		area->x += zone + state->margin.left;
		area->width -= zone + state->margin.left;
	} else if (anchor == right || anchor == (top | bottom | right)) {
		area->width -= zone + state->margin.right;
	}
	if (area->width < 0) {
		area->width = 0;
	}
	if (area->height < 0) {
		area->height = 0;
	}
}

static void get_work_area(struct server *server, struct wlr_output *output,
		struct wlr_box *area) {
	wlr_output_layout_get_box(server->output_layout, output, area);
	struct layer_surface *ls;
	wl_list_for_each(ls, &server->layer_surfaces, link) {
		struct wlr_layer_surface_v1 *layer = ls->layer_surface;
		if (layer->output != output) {
			continue;
		}
		if (layer->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
			continue; /* transient overlays don't shrink the work area */
		}
		if (layer->surface->mapped) {
			layer_surface_exclusive_zone(&layer->current, area);
		}
	}
}

/* the next/previous mapped toplevel relative to tl, wrapping around the
 * list. include_minimized controls whether hidden windows are eligible.
 * Returns NULL when there is no such toplevel. */
static struct toplevel *neighbor_toplevel(struct server *server,
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
/* window decoration (rounded border)                                 */
/* ------------------------------------------------------------------ */

/* a wlr_buffer whose pixels we own; the scene graph imports them into a
 * texture the first time the node is displayed and caches it, so the CPU
 * data only needs to be regenerated when the window size changes */
struct border_buffer {
	struct wlr_buffer base;
	uint32_t *data;   /* DRM_FORMAT_ARGB8888 pixels */
	size_t stride;    /* bytes per row */
	uint32_t format;
};

/* the border is purely visual: never let it intercept pointer input */
static bool border_buffer_no_input(struct wlr_scene_buffer *buffer,
		double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

static bool border_buffer_begin_data_ptr_access(struct wlr_buffer *buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	if ((flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) != 0) {
		return false;
	}
	struct border_buffer *bb = wl_container_of(buffer, bb, base);
	*data = bb->data;
	*format = bb->format;
	*stride = bb->stride;
	return true;
}

static void border_buffer_end_data_ptr_access(struct wlr_buffer *buffer) {
	(void)buffer;
}

static void border_buffer_destroy(struct wlr_buffer *buffer) {
	struct border_buffer *bb = wl_container_of(buffer, bb, base);
	wlr_buffer_finish(&bb->base);
	free(bb->data);
	free(bb);
}

static const struct wlr_buffer_impl border_buffer_impl = {
	.destroy = border_buffer_destroy,
	.begin_data_ptr_access = border_buffer_begin_data_ptr_access,
	.end_data_ptr_access = border_buffer_end_data_ptr_access,
};

/* is (x, y) inside [rx, rx+rw) x [ry, ry+rh) with corners of radius r?
 * (r = 0 means a plain rectangle) */
static bool inside_rounded_rect(int x, int y, int rx, int ry, int rw, int rh,
		int r) {
	if (x < rx || x >= rx + rw || y < ry || y >= ry + rh) {
		return false;
	}
	if (r <= 0) {
		return true;
	}
	int cx, cy;
	if (x < rx + r) {
		cx = rx + r;
	} else if (x >= rx + rw - r) {
		cx = rx + rw - r - 1;
	} else {
		return true;
	}
	if (y < ry + r) {
		cy = ry + r;
	} else if (y >= ry + rh - r) {
		cy = ry + rh - r - 1;
	} else {
		return true;
	}
	int dx = x - cx;
	int dy = y - cy;
	return dx * dx + dy * dy <= r * r;
}

/* render a rounded-rect outline (stroke = BORDER_WIDTH, color #7C73B0) into
 * a new ARGB8888 wlr_buffer of the given size */
static struct border_buffer *border_buffer_create(int width, int height) {
	struct border_buffer *bb = calloc(1, sizeof(*bb));
	if (bb == NULL) {
		return NULL;
	}
	bb->data = calloc((size_t)width * height, sizeof(uint32_t));
	if (bb->data == NULL) {
		free(bb);
		return NULL;
	}
	bb->format = DRM_FORMAT_ARGB8888;
	bb->stride = (size_t)width * 4;
	wlr_buffer_init(&bb->base, &border_buffer_impl, width, height);

	int r = BORDER_RADIUS;
	if (r > width / 2) {
		r = width / 2;
	}
	if (r > height / 2) {
		r = height / 2;
	}
	int ri = r - BORDER_WIDTH;
	if (ri < 0) {
		ri = 0;
	}
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			bool outer = inside_rounded_rect(x, y, 0, 0, width, height, r);
			bool inner = inside_rounded_rect(x, y, BORDER_WIDTH, BORDER_WIDTH,
				width - 2 * BORDER_WIDTH, height - 2 * BORDER_WIDTH, ri);
			if (outer && !inner) {
				bb->data[(size_t)y * width + x] = BORDER_COLOR;
			}
		}
	}
	return bb;
}

/* (re)build the rounded border around the toplevel and position it */
static void update_toplevel_decoration(struct toplevel *tl) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (tl->deco_border == NULL || base == NULL) {
		return;
	}
	if (!base->surface->mapped || tl->minimized || tl->fullscreen) {
		/* the scene node unlocks (and destroys) the old border buffer;
		 * fullscreen windows don't show the rounded border either */
		tl->deco_buffer = NULL;
		wlr_scene_buffer_set_buffer(tl->deco_border, NULL);
		return;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		tl->deco_buffer = NULL;
		wlr_scene_buffer_set_buffer(tl->deco_border, NULL);
		return;
	}
	int bw = box.width + 2 * BORDER_WIDTH;
	int bh = box.height + 2 * BORDER_WIDTH;
	if (tl->deco_buffer == NULL || tl->deco_w != bw || tl->deco_h != bh) {
		struct border_buffer *bb = border_buffer_create(bw, bh);
		if (bb == NULL) {
			return;
		}
		/* installing the new buffer makes the scene node unlock the old one
		 * (already dropped by us when it was installed), which destroys it */
		tl->deco_buffer = bb;
		tl->deco_w = bw;
		tl->deco_h = bh;
		wlr_scene_buffer_set_buffer(tl->deco_border, &bb->base);
		wlr_buffer_drop(&bb->base);
	}
	wlr_scene_node_set_position(&tl->deco_border->node, box.x - BORDER_WIDTH,
		box.y - BORDER_WIDTH);
}

/* ------------------------------------------------------------------ */
/* IPC socket (status bar communication)                              */
/* ------------------------------------------------------------------ */

/* one connected status bar client; JSON messages are newline-delimited */
struct ipc_client {
	struct wl_list link; /* server.ipc_clients */
	struct server *server;
	int fd;
	struct wl_event_source *source;

	char *out;        /* pending outgoing bytes */
	size_t out_len;   /* bytes queued */
	size_t out_off;   /* bytes already written */
	size_t out_cap;

	char in[4096];    /* partial incoming line */
	size_t in_len;
};

static void ipc_send_window_event(struct server *server, const char *event,
		struct toplevel *tl);
static void ipc_send_window_list(struct server *server,
		struct ipc_client *target);

static void ipc_client_destroy(struct ipc_client *client) {
	wl_event_source_remove(client->source);
	close(client->fd);
	free(client->out);
	wl_list_remove(&client->link);
	free(client);
}

/* try to write all queued output; on success the buffer is emptied */
static void ipc_client_flush(struct ipc_client *client) {
	while (client->out_off < client->out_len) {
		ssize_t n = write(client->fd, client->out + client->out_off,
			client->out_len - client->out_off);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return; /* wait for the writable event */
			}
			ipc_client_destroy(client);
			return;
		}
		client->out_off += n;
	}
	client->out_len = 0;
	client->out_off = 0;
}

static void ipc_client_queue(struct ipc_client *client, const char *json) {
	size_t len = strlen(json);
	if (client->out_len + len + 1 > client->out_cap) {
		size_t cap = client->out_cap ? client->out_cap * 2 : 256;
		while (cap < client->out_len + len + 1) {
			cap *= 2;
		}
		char *buf = realloc(client->out, cap);
		if (buf == NULL) {
			return;
		}
		client->out = buf;
		client->out_cap = cap;
	}
	memcpy(client->out + client->out_len, json, len);
	client->out_len += len;
	client->out[client->out_len++] = '\n';
	ipc_client_flush(client);
	/* keep the writable event enabled while there is pending output */
	uint32_t mask = WL_EVENT_READABLE;
	if (client->out_off < client->out_len) {
		mask |= WL_EVENT_WRITABLE;
	}
	wl_event_source_fd_update(client->source, mask);
}

/* find a live toplevel by its IPC id */
static struct toplevel *toplevel_by_id(struct server *server, int id) {
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->id == id) {
			return tl;
		}
	}
	return NULL;
}

/* handle one complete JSON message received from a client */
static void ipc_handle_line(struct server *server, struct ipc_client *client,
		const char *line) {
	cJSON *root = cJSON_Parse(line);
	if (root == NULL) {
		return;
	}
	cJSON *action = cJSON_GetObjectItem(root, "action");
	if (action != NULL && cJSON_IsString(action)) {
		if (strcmp(action->valuestring, "list_windows") == 0) {
			ipc_send_window_list(server, client);
		} else if (strcmp(action->valuestring, "focus_window") == 0) {
			/* a taskbar clicked a window icon: switch focus to it */
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					focus_window(server, tl);
				}
			}
		} else if (strcmp(action->valuestring, "close_window") == 0) {
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					close_toplevel(tl);
				}
			}
		}
	}
	cJSON_Delete(root);
}

static void ipc_client_handle_input(struct server *server,
		struct ipc_client *client, const char *data, size_t len) {
	size_t i = 0;
	while (i < len) {
		while (i < len && data[i] != '\n' &&
				client->in_len < sizeof(client->in) - 1) {
			client->in[client->in_len++] = data[i++];
		}
		if (i < len && data[i] == '\n') {
			client->in[client->in_len] = '\0';
			if (client->in_len > 0) {
				ipc_handle_line(server, client, client->in);
			}
			client->in_len = 0;
			i++;
		}
	}
}

static int ipc_client_handle_data(int fd, uint32_t mask, void *data) {
	struct ipc_client *client = data;
	struct server *server = client->server;
	if ((mask & WL_EVENT_WRITABLE) != 0) {
		ipc_client_flush(client);
		uint32_t new_mask = WL_EVENT_READABLE;
		if (client->out_off < client->out_len) {
			new_mask |= WL_EVENT_WRITABLE;
		}
		wl_event_source_fd_update(client->source, new_mask);
	}
	if ((mask & WL_EVENT_READABLE) != 0) {
		char buf[4096];
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			ipc_client_destroy(client);
			return 0;
		}
		ipc_client_handle_input(server, client, buf, (size_t)n);
	}
	return 0;
}

static int ipc_handle_accept(int fd, uint32_t mask, void *data) {
	struct server *server = data;
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	int cfd = accept(fd, NULL, NULL);
	if (cfd < 0) {
		return 0;
	}
	if (fcntl(cfd, F_SETFL, O_NONBLOCK | O_CLOEXEC) < 0) {
		close(cfd);
		return 0;
	}
	struct ipc_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		close(cfd);
		return 0;
	}
	client->fd = cfd;
	client->server = server;
	client->source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), cfd, WL_EVENT_READABLE,
		ipc_client_handle_data, client);
	if (client->source == NULL) {
		close(cfd);
		free(client);
		return 0;
	}
	wl_list_insert(server->ipc_clients.prev, &client->link);
	/* a fresh client needs the current window list to draw the bar */
	ipc_send_window_list(server, client);
	return 0;
}

/* serialize + broadcast a window event to all connected clients */
static void ipc_send_window_event(struct server *server, const char *event,
		struct toplevel *tl) {
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "event", event);
	if (tl != NULL) {
		cJSON_AddNumberToObject(root, "id", tl->id);
		cJSON_AddStringToObject(root, "app_id",
			tl->app_id != NULL ? tl->app_id : "");
	} else {
		/* focus cleared: id 0, no window */
		cJSON_AddNumberToObject(root, "id", 0);
		cJSON_AddStringToObject(root, "app_id", "");
	}
	char *json = cJSON_PrintUnformatted(root);
	if (json != NULL) {
		struct ipc_client *client;
		wl_list_for_each(client, &server->ipc_clients, link) {
			ipc_client_queue(client, json);
		}
		free(json);
	}
	cJSON_Delete(root);
}

/* send the current mapped windows; target NULL broadcasts to everyone */
static void ipc_send_window_list(struct server *server,
		struct ipc_client *target) {
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "event", "window_list");
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToObject(root, "windows", arr);
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->xdg_toplevel->base == NULL ||
				!tl->xdg_toplevel->base->surface->mapped) {
			continue;
		}
		cJSON *w = cJSON_CreateObject();
		cJSON_AddNumberToObject(w, "id", tl->id);
		cJSON_AddStringToObject(w, "app_id",
			tl->app_id != NULL ? tl->app_id : "");
		cJSON_AddItemToArray(arr, w);
	}
	char *json = cJSON_PrintUnformatted(root);
	if (json != NULL) {
		if (target != NULL) {
			ipc_client_queue(target, json);
		} else {
			struct ipc_client *client;
			wl_list_for_each(client, &server->ipc_clients, link) {
				ipc_client_queue(client, json);
			}
		}
		free(json);
	}
	cJSON_Delete(root);
}

/* create the Unix socket; returns false on failure (compositor keeps
 * running without a status bar) */
static bool ipc_server_init(struct server *server, const char *path) {
	unlink(path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		return false;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return false;
	}
	strcpy(addr.sun_path, path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return false;
	}
	if (listen(fd, 8) < 0) {
		close(fd);
		return false;
	}
	server->ipc_fd = fd;
	server->ipc_source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), fd, WL_EVENT_READABLE,
		ipc_handle_accept, server);
	if (server->ipc_source == NULL) {
		close(fd);
		server->ipc_fd = -1;
		return false;
	}
	wl_list_init(&server->ipc_clients);
	wlr_log(WLR_INFO, "IPC socket listening on %s", path);
	return true;
}

static void ipc_server_destroy(struct server *server) {
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
		ipc_client_destroy(client);
	}
	if (server->ipc_source != NULL) {
		wl_event_source_remove(server->ipc_source);
		server->ipc_source = NULL;
	}
	if (server->ipc_fd >= 0) {
		close(server->ipc_fd);
		server->ipc_fd = -1;
	}
}

/* ------------------------------------------------------------------ */
/* window state                                                       */
/* ------------------------------------------------------------------ */

static void close_toplevel(struct toplevel *tl) {
	if (tl->xdg_toplevel->base != NULL) {
		wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
	}
}

static void set_fullscreen(struct server *server, struct toplevel *tl,
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
			wlr_xdg_toplevel_set_size(tl->xdg_toplevel,
				tl->restore_box.width, tl->restore_box.height);
			wlr_scene_node_set_position(&tl->scene_tree->node,
				tl->restore_box.x, tl->restore_box.y);
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

static void set_maximized(struct server *server, struct toplevel *tl,
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
static void restore_maximized_toplevel(struct toplevel *tl) {
	if (tl->xdg_toplevel->base == NULL ||
			!tl->xdg_toplevel->current.maximized) {
		return;
	}
	if (tl->has_restore_box && tl->restore_box.width > 0) {
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, tl->restore_box.width,
			tl->restore_box.height);
		wlr_scene_node_set_position(&tl->scene_tree->node, tl->restore_box.x,
			tl->restore_box.y);
	}
	wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, false);
	if (tl->fthandle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_maximized(tl->fthandle, false);
	}
	update_toplevel_decoration(tl);
}

static void set_minimized(struct server *server, struct toplevel *tl,
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
		}
	}
	/* hiding the window may expose a different surface under the cursor */
	update_cursor_style(server);
}

static void focus_toplevel(struct server *server, struct toplevel *tl) {
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
	/* notify status bars that the focus changed */
	ipc_send_window_event(server, "window_focus", tl);
}

static void update_toplevel_output(struct server *server, struct toplevel *tl) {
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
/* pointer interaction (move / maximize / minimize / close)           */
/* ------------------------------------------------------------------ */

static bool is_double_click(struct server *server, uint32_t button) {
	if (!server->last_was_click || button != server->last_click_button) {
		return false;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t dt = (now.tv_sec - server->last_release_time.tv_sec) * 1000000000L
		+ (now.tv_nsec - server->last_release_time.tv_nsec);
	return dt >= 0 && dt <= DOUBLE_CLICK_NS;
}

static void begin_move(struct server *server, struct toplevel *tl,
		double ref_x, double ref_y) {
	if (server->moving) {
		return;
	}
	server->moving = true;
	server->move_toplevel = tl;
	server->grab_x = ref_x - tl->scene_tree->node.x;
	server->grab_y = ref_y - tl->scene_tree->node.y;
}

static void end_move(struct server *server) {
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
 * client-side decoration) resize here; CSD windows handle their own edges. */
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
	if (ly >= box.y && ly <= box.y + box.height) {
		if (lx >= box.x - EDGE_THICKNESS && lx < box.x + EDGE_THICKNESS) {
			edges |= WLR_EDGE_LEFT;
		}
		if (lx > box.x + box.width - EDGE_THICKNESS &&
				lx <= box.x + box.width + EDGE_THICKNESS) {
			edges |= WLR_EDGE_RIGHT;
		}
	}
	if (lx >= box.x && lx <= box.x + box.width) {
		if (ly > box.y + box.height - EDGE_THICKNESS &&
				ly <= box.y + box.height + EDGE_THICKNESS) {
			edges |= WLR_EDGE_BOTTOM;
		}
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
	server->cursor_override = NULL;
}

/* decide which compositor-owned cursor to show at the current position */
static void update_cursor_style(struct server *server) {
	const char *name = NULL;

	if (server->moving && server->move_toplevel != NULL) {
		name = "grabbing";
	} else if (server->resizing && server->resize_toplevel != NULL) {
		name = resize_cursor_name(server->resize_edges);
	} else {
		struct toplevel *tl = toplevel_at(server);
		if (tl != NULL && !tl->minimized) {
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

static void end_resize(struct server *server) {
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
/* output                                                             */
/* ------------------------------------------------------------------ */

static void monitor_frame(struct wl_listener *listener, void *data) {
	struct monitor *mon = wl_container_of(listener, mon, frame);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!wlr_scene_output_commit(mon->scene_output, NULL)) {
		return;
	}
	wlr_scene_output_send_frame_done(mon->scene_output, &now);
}

static void monitor_destroy(struct wl_listener *listener, void *data) {
	struct monitor *mon = wl_container_of(listener, mon, destroy);
	wl_list_remove(&mon->frame.link);
	wl_list_remove(&mon->destroy.link);
	free(mon);
}

static void update_output_manager_config(struct server *server) {
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, lo->output);
		head->state.x = lo->x;
		head->state.y = lo->y;
	}
	wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

static void arrange_layer_surfaces(struct server *server) {
	struct layer_surface *ls;
	wl_list_for_each(ls, &server->layer_surfaces, link) {
		struct wlr_output *output = ls->layer_surface->output;
		if (output == NULL) {
			output = wlr_output_layout_get_center_output(server->output_layout);
		}
		if (output == NULL) {
			continue;
		}
		struct wlr_box full_area;
		wlr_output_layout_get_box(server->output_layout, output, &full_area);
		struct wlr_box usable_area = full_area;
		wlr_scene_layer_surface_v1_configure(ls->scene_layer, &full_area,
			&usable_area);
	}
}

static void update_scene_output_positions(struct server *server) {
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_scene_output *scene_output = lo->output->data;
		if (scene_output != NULL) {
			wlr_scene_output_set_position(scene_output, lo->x, lo->y);
		}
	}
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *output = data;

	wlr_output_init_render(output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	if (!wlr_output_commit_state(output, &state)) {
		wlr_log(WLR_ERROR, "failed to commit output state");
	}
	wlr_output_state_finish(&state);

	wlr_output_create_global(output, server->display);

	wlr_output_layout_add_auto(server->output_layout, output);
	struct wlr_output_layout_output *lo =
		wlr_output_layout_get(server->output_layout, output);
	if (lo == NULL) {
		wlr_log(WLR_ERROR, "failed to add output to layout");
		return;
	}

	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, output);
	if (scene_output == NULL) {
		wlr_log(WLR_ERROR, "failed to create scene output");
		return;
	}
	wlr_scene_output_set_position(scene_output, lo->x, lo->y);
	output->data = scene_output;

	struct monitor *mon = calloc(1, sizeof(*mon));
	if (mon == NULL) {
		return;
	}
	mon->server = server;
	mon->output = output;
	mon->scene_output = scene_output;
	mon->frame.notify = monitor_frame;
	wl_signal_add(&output->events.frame, &mon->frame);
	mon->destroy.notify = monitor_destroy;
	wl_signal_add(&output->events.destroy, &mon->destroy);

	wlr_xcursor_manager_load(server->xcursor_manager, output->scale);

	update_output_manager_config(server);
	arrange_layer_surfaces(server);
}

static void server_layout_change(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, layout_change);
	update_scene_output_positions(server);
	arrange_layer_surfaces(server);
	update_output_manager_config(server);
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		update_toplevel_output(server, tl);
	}
}

static void output_manager_apply(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;
	size_t states_len = 0;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &states_len);
	bool ok = wlr_backend_test(server->backend, states, states_len);
	if (ok) {
		ok = wlr_backend_commit(server->backend, states, states_len);
	}
	free(states);
	if (ok) {
		struct wlr_output_configuration_head_v1 *head;
		wl_list_for_each(head, &config->heads, link) {
			wlr_output_layout_add(server->output_layout, head->state.output,
				head->state.x, head->state.y);
		}
	}
	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
}

static void output_manager_test(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		output_manager_test);
	struct wlr_output_configuration_v1 *config = data;
	size_t states_len = 0;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &states_len);
	bool ok = wlr_backend_test(server->backend, states, states_len);
	free(states);
	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
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
			struct wlr_output *output = wlr_output_layout_output_at(
				server->output_layout, server->cursor->x, server->cursor->y);
			if (output != NULL) {
				struct wlr_box area;
				wlr_output_layout_get_box(server->output_layout, output, &area);
				if (x < area.x) x = area.x;
				if (y < area.y) y = area.y;
				if (x + 40 > area.x + area.width) x = area.x + area.width - 40;
				if (y + 40 > area.y + area.height) y = area.y + area.height - 40;
			}
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

static void server_new_toplevel(struct wl_listener *listener, void *data) {
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
	tl->scene_tree = wlr_scene_xdg_surface_create(tl->deco_tree, base);
	if (tl->scene_tree == NULL) {
		wlr_scene_node_destroy(&tl->deco_tree->node);
		free(tl);
		return;
	}
	xdg_surface_tag(tl->scene_tree, TAG_TOPLEVEL, tl);

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

static void server_new_decoration(struct wl_listener *listener, void *data) {
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

/* ------------------------------------------------------------------ */
/* wlr-layer-shell                                                    */
/* ------------------------------------------------------------------ */

static void configure_layer_surface(struct layer_surface *ls) {
	struct wlr_layer_surface_v1 *layer_surface = ls->layer_surface;
	if (!layer_surface->initialized) {
		return;
	}
	struct wlr_output *output = layer_surface->output;
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(ls->server->output_layout);
	}
	if (output == NULL) {
		return;
	}
	struct wlr_box full_area;
	wlr_output_layout_get_box(ls->server->output_layout, output, &full_area);
	struct wlr_box usable_area = full_area;
	wlr_scene_layer_surface_v1_configure(ls->scene_layer, &full_area,
		&usable_area);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, commit);
	configure_layer_surface(ls);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->link);
	free(ls);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	struct layer_surface *ls = calloc(1, sizeof(*ls));
	if (ls == NULL) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	ls->server = server;
	ls->layer_surface = layer_surface;
	layer_surface->data = ls;

	ls->scene_layer = wlr_scene_layer_surface_v1_create(
		server->layers[scene_layer_index(layer_surface->pending.layer)],
		layer_surface);
	if (ls->scene_layer == NULL) {
		free(ls);
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	xdg_surface_tag(ls->scene_layer->tree, TAG_LAYER, ls);

	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &ls->destroy);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &ls->commit);

	wl_list_insert(server->layer_surfaces.prev, &ls->link);

	/* the layer surface is only initialized after its first (empty) commit,
	 * so the initial configure is sent from the commit handler */
}

/* ------------------------------------------------------------------ */
/* seat / input                                                       */
/* ------------------------------------------------------------------ */

static void seat_request_set_cursor(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (event->seat_client == server->seat->pointer_state.focused_client) {
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

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void seat_request_start_drag(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		seat_request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	wlr_seat_start_drag(server->seat, event->drag, event->serial);
}

static void seat_start_drag(struct wl_listener *listener, void *data) {
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

static void process_cursor_motion(struct server *server, uint32_t time_msec) {
	if (server->drag_tree != NULL) {
		wlr_scene_node_set_position(&server->drag_tree->node,
			server->cursor->x, server->cursor->y);
	}

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
			if (dx * dx + dy * dy > DRAG_THRESHOLD * DRAG_THRESHOLD) {
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

	struct toplevel *tl = toplevel_at(server);
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

static void cursor_motion(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void cursor_button(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	process_cursor_button(server, event->time_msec, event->button,
		event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	process_cursor_axis(server, event->time_msec, event);
}

static void cursor_frame(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server,
		keyboard_modifiers);
	struct wlr_keyboard *keyboard = data;
	wlr_seat_set_keyboard(server->seat, keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->modifiers);
}

/* run a command in a detached child process */
static void spawn_command(const char *cmd) {
	if (fork() == 0) {
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
	}
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
static void focus_window(struct server *server, struct toplevel *tl) {
	if (tl == NULL) {
		return;
	}
	if (tl->minimized) {
		set_minimized(server, tl, false);
	}
	focus_toplevel(server, tl);
	update_cursor_style(server);
}

/* compositor keyboard shortcuts. Returns true when the key was consumed. */
static bool keyboard_shortcut(struct server *server, uint32_t keycode) {
	struct wlr_keyboard *keyboard =
		wlr_keyboard_from_input_device(server->keyboard_device);
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, keycode, &syms);
	uint32_t mods = keyboard->modifiers.depressed | keyboard->modifiers.latched;
	bool shift = (mods & WLR_MODIFIER_SHIFT) != 0;
	bool alt = (mods & WLR_MODIFIER_ALT) != 0;
	bool logo = (mods & WLR_MODIFIER_LOGO) != 0;
	bool sa = shift && alt; /* Shift+Alt combos */

	for (int i = 0; i < nsyms; i++) {
		/* with Shift held xkb reports the shifted keysym (e.g. 'Q'), so
		 * compare case-insensitively for the letter bindings */
		xkb_keysym_t sym = xkb_keysym_to_lower(syms[i]);
		if ((logo && sym == XKB_KEY_q) || (sa && sym == XKB_KEY_q)) {
			wl_display_terminate(server->display);
			return true;
		}
		if (sa && sym == XKB_KEY_Return) {
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
		if (sa && sym == XKB_KEY_m) {
			if (server->focused != NULL) {
				set_minimized(server, server->focused, true);
			}
			return true;
		}
		if (sa && sym == XKB_KEY_n) {
			focus_window(server, cycle_toplevel(server, true));
			return true;
		}
		if (sa && sym == XKB_KEY_p) {
			focus_window(server, cycle_toplevel(server, false));
			return true;
		}
		if (sa && sym == XKB_KEY_c) {
			if (server->focused != NULL) {
				close_toplevel(server->focused);
			}
			return true;
		}
		if (sa && sym == XKB_KEY_f) {
			spawn_command("foot");
			return true;
		}
	}
	return false;
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, keyboard_key);
	struct wlr_keyboard_key_event *event = data;
	if (server->keyboard_device == NULL) {
		return;
	}
	uint32_t keycode = event->keycode + 8;

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		handled = keyboard_shortcut(server, keycode);
	}
	if (!handled) {
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, keyboard_destroy);
	if (server->keyboard_device == data) {
		wl_list_remove(&server->keyboard_modifiers.link);
		wl_list_remove(&server->keyboard_key.link);
		wl_list_remove(&server->keyboard_destroy.link);
		server->keyboard_device = NULL;
	}
}

static void server_new_virtual_pointer(struct wl_listener *listener,
		void *data) {
	struct server *server = wl_container_of(listener, server,
		new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor,
		&event->new_pointer->pointer.base);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: {
		struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(device);
		server->keyboard_device = device;
		server->keyboard_modifiers.notify = keyboard_modifiers;
		wl_signal_add(&keyboard->events.modifiers, &server->keyboard_modifiers);
		server->keyboard_key.notify = keyboard_key;
		wl_signal_add(&keyboard->events.key, &server->keyboard_key);
		server->keyboard_destroy.notify = keyboard_destroy;
		wl_signal_add(&device->events.destroy, &server->keyboard_destroy);

		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		if (context == NULL) {
			wlr_log(WLR_ERROR, "failed to create xkb context");
			return;
		}
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap == NULL) {
			wlr_log(WLR_ERROR, "failed to create xkb keymap");
			xkb_context_unref(context);
			return;
		}
		wlr_keyboard_set_keymap(keyboard, keymap);
		wlr_keyboard_set_repeat_info(keyboard, 25, 600);
		xkb_keymap_unref(keymap);
		xkb_context_unref(context);

		wlr_seat_set_keyboard(server->seat, keyboard);

		if (server->focused != NULL &&
				server->focused->xdg_toplevel->base != NULL &&
				server->focused->xdg_toplevel->base->surface->mapped) {
			wlr_seat_keyboard_notify_enter(server->seat,
				server->focused->xdg_toplevel->base->surface,
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
		}
		break;
	}
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);

	char *startup_cmd = NULL;
	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-s startup-command]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	struct server server = {0};

	server.display = wl_display_create();
	if (server.display == NULL) {
		return EXIT_FAILURE;
	}
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create backend");
		return EXIT_FAILURE;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return EXIT_FAILURE;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return EXIT_FAILURE;
	}
	server.scene = wlr_scene_create();
	if (server.scene == NULL) {
		wlr_log(WLR_ERROR, "failed to create scene");
		return EXIT_FAILURE;
	}
	server.output_layout = wlr_output_layout_create(server.display);
	server.output_manager = wlr_output_manager_v1_create(server.display);
	server.foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server.display);

	server.seat = wlr_seat_create(server.display, "seat0");
	server.cursor = wlr_cursor_create();
	if (server.cursor == NULL) {
		wlr_log(WLR_ERROR, "failed to create cursor");
		return EXIT_FAILURE;
	}
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.xcursor_manager = wlr_xcursor_manager_create(NULL, 24);
	if (server.xcursor_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create xcursor manager");
		return EXIT_FAILURE;
	}

	for (int i = 0; i < LAYER_COUNT; i++) {
		server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
		if (server.layers[i] == NULL) {
			wlr_log(WLR_ERROR, "failed to create scene tree");
			return EXIT_FAILURE;
		}
	}
	wl_list_init(&server.toplevels);
	wl_list_init(&server.layer_surfaces);

	/* ---- core & stable protocols ---- */
	wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_shm_create_with_renderer(server.display, 1, server.renderer);
	wlr_linux_dmabuf_v1_create_with_renderer(server.display, 5,
		server.renderer);
	wlr_data_device_manager_create(server.display);
	struct wlr_xdg_shell *xdg_shell =
		wlr_xdg_shell_create(server.display, 6);
	wlr_viewporter_create(server.display);
	wlr_presentation_create(server.display, server.backend, 2);

	/* ---- wlroots protocols ---- */
	struct wlr_layer_shell_v1 *layer_shell =
		wlr_layer_shell_v1_create(server.display, 5);
	struct wlr_xdg_decoration_manager_v1 *decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server.display);
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager =
		wlr_virtual_pointer_manager_v1_create(server.display);

	/* ---- listeners ---- */
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.new_virtual_pointer.notify = server_new_virtual_pointer;
	wl_signal_add(&virtual_pointer_manager->events.new_virtual_pointer,
		&server.new_virtual_pointer);
	server.layout_change.notify = server_layout_change;
	wl_signal_add(&server.output_layout->events.change, &server.layout_change);
	server.output_manager_apply.notify = output_manager_apply;
	wl_signal_add(&server.output_manager->events.apply,
		&server.output_manager_apply);
	server.output_manager_test.notify = output_manager_test;
	wl_signal_add(&server.output_manager->events.test,
		&server.output_manager_test);
	server.new_xdg_toplevel.notify = server_new_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&layer_shell->events.new_surface, &server.new_layer_surface);
	server.new_decoration.notify = server_new_decoration;
	wl_signal_add(&decoration_manager->events.new_toplevel_decoration,
		&server.new_decoration);

	server.cursor_motion.notify = cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
		&server.cursor_motion_absolute);
	server.cursor_button.notify = cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	server.seat_request_set_cursor.notify = seat_request_set_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
		&server.seat_request_set_cursor);
	server.seat_request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.seat_request_set_selection);
	server.seat_request_set_primary_selection.notify =
		seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection,
		&server.seat_request_set_primary_selection);
	server.seat_request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag,
		&server.seat_request_start_drag);
	server.seat_start_drag.notify = seat_start_drag;
	wl_signal_add(&server.seat->events.start_drag, &server.seat_start_drag);

	wlr_seat_set_capabilities(server.seat,
		WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to add socket");
		return EXIT_FAILURE;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return EXIT_FAILURE;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd != NULL) {
		spawn_command(startup_cmd);
	}

	/* IPC socket for status bars (JSON over a Unix domain socket) */
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	char ipc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (runtime_dir != NULL && runtime_dir[0] != '\0') {
		snprintf(ipc_path, sizeof(ipc_path), "%s/xmonodywm.sock",
			runtime_dir);
	} else {
		snprintf(ipc_path, sizeof(ipc_path), "/tmp/xmonodywm.sock");
	}
	server.ipc_fd = -1;
	if (!ipc_server_init(&server, ipc_path)) {
		wlr_log(WLR_ERROR, "failed to start IPC socket at %s", ipc_path);
	}

	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
		socket);
	wl_display_run(server.display);

	ipc_server_destroy(&server);
	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return EXIT_SUCCESS;
}
