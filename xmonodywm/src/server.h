/*
 * server.h - shared types and cross-module declarations for xmonodywm
 *
 * The compositor is split into small modules (main, ipc, scene, border,
 * toplevel, layer, output, input, pointer); everything they need to share
 * lives here.
 */

#ifndef XMONODYWM_SERVER_H
#define XMONODYWM_SERVER_H

#define _POSIX_C_SOURCE 200809L

/* all tunables (shortcuts, border radius, blur, edge grab zone) */
#include "config.h"

#include <stdbool.h>
#include <stdint.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

/* ordering of scene-graph trees (bottom to top) */
enum scene_layer {
	LAYER_BACKGROUND = 0, /* wlr-layer-shell background   */
	LAYER_BOTTOM,         /* wlr-layer-shell bottom       */
	LAYER_TOPLEVELS,      /* normal windows               */
	LAYER_TOP,            /* wlr-layer-shell top          */
	LAYER_OVERLAY,        /* wlr-layer-shell overlay + drag icon */
	LAYER_COUNT,
};

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

struct ipc_client;     /* defined in ipc.c */
struct wlr_swapchain;  /* defined in wlr/render/swapchain.h */
struct server;
struct toplevel;
struct layer_surface;

/* one connected input method (fcitx5/ibus); only the first one is used */
struct ime {
	struct server *server;
	struct wl_list link; /* server.imes */

	struct wlr_input_method_v2 *input_method;
	struct wl_listener destroy;
	struct wl_listener grab_keyboard;
	struct wl_listener commit;
	struct wl_listener keyboard_grab_destroy;
	struct wl_listener new_popup_surface;

	/* seat keyboard while the IM holds the keyboard grab */
	struct wlr_keyboard *keyboard;
	struct wl_listener keyboard_grab_key;
	struct wl_listener keyboard_grab_modifiers;
	bool keyboard_grab_destroy_added; /* grab destroy listener attached */

	/* keycodes forwarded to the IM grab while pressed; used to suppress
	 * stray key-release events (releases without a matching forwarded
	 * press are not sent to the IM) */
	uint32_t forwarded_keys[16];
	size_t forwarded_key_count;

	/* candidate window shown by the IM (fcitx5) */
	struct wlr_input_popup_surface_v2 *popup_surface;
	struct wlr_scene_surface *popup_scene_surface;
	struct wl_listener popup_destroy;
};

/* one zwp_text_input_v3 object of a client (usually one per client) */
struct text_input {
	struct server *server;
	struct wl_list link; /* server.text_inputs */

	struct wlr_text_input_v3 *text_input;
	struct wl_listener destroy;
	struct wl_listener enable;
	struct wl_listener disable;
	struct wl_listener commit;
};

/* a manually managed subsurface tree under a toplevel's content tree
 * (wlr_scene_xdg_surface cannot be used because the content buffer is
 * re-rendered through the rounded mask instead) */
struct mask_subsurface {
	struct toplevel *tl;
	struct wlr_subsurface *subsurface;
	struct wlr_scene_tree *tree;
	struct wl_list link; /* tl->subsurfaces */
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
	int deco_w, deco_h;               /* border buffer size */
	uint32_t deco_color;              /* border color the buffer was rendered with */
	bool deco_focused;                /* whether the buffer was rendered with the focus glow */

	/* whether the border/interaction box wraps the committed surface
	 * instead of the xdg window geometry: set by the rounded-mask pass
	 * (mask.c) when the surface extends beyond the geometry with opaque
	 * pixels (clients like QQ's login window report a geometry smaller
	 * than what they actually draw).  When the extra area is a
	 * transparent drop shadow (Firefox/GTK CSD) this stays false and the
	 * geometry - the window bounds per xdg-shell - is used. */
	bool wrap_surface;

	/* rounded-corner masked content: the client's buffer re-rendered through
	 * an alpha mask (mask.c) into `masked`; the xdg surface's own scene
	 * node is replaced by this buffer, so output/frame events are forwarded
	 * to the client surface here.  Subsurfaces get their own scene trees
	 * (struct mask_subsurface). */
	struct wlr_scene_buffer *masked;
	struct wl_list subsurfaces;       /* struct mask_subsurface.link */
	struct wl_listener mask_enter;
	struct wl_listener mask_leave;
	struct wl_listener mask_sample;
	struct wl_listener mask_frame;
	struct wl_listener new_subsurface;


	/* GLSL gaussian blur behind transparent windows (terminal emulators);
	 * deco_blur sits in deco_tree behind the content and holds the blurred
	 * backdrop, blur_enabled tracks whether the committed buffer really
	 * contains semi-transparent pixels */
	struct wlr_scene_buffer *deco_blur;
	bool blur_enabled;

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

	/* last exclusive-zone signature: when it changes (a bar appears,
	 * resizes or goes away) the work area changes and existing windows
	 * must be re-arranged out of the exclusive zone */
	uint32_t last_anchor;
	int32_t last_zone;
	int32_t last_margin_top, last_margin_bottom;
	int32_t last_margin_left, last_margin_right;
	bool last_mapped;
	bool has_last_state;

	struct wl_listener destroy;
	struct wl_listener commit;
};

/* double-click action armed by a press on the top title strip: the action
 * fires when the (second) click is released without a drag */
enum zone_action {
	ZONE_NONE = 0,     /* no armed action */
	ZONE_MINIMIZE,     /* left third of the strip: minimize */
	ZONE_MAXIMIZE,     /* middle third: toggle maximize / restore */
	ZONE_CLOSE,        /* right third: close the window */
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
	struct wl_list keyboards; /* struct keyboard.link (per attached device) */

	/* cursor-shape-v1: lets clients pick a cursor shape which the compositor
	 * renders with its own theme (so the size always matches the output
	 * scale, no client-side guessing); handled like wl_pointer.set_cursor */
	struct wlr_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wl_listener cursor_shape_set_shape;


	struct wl_list toplevels;      /* struct toplevel.link */
	struct wl_list layer_surfaces; /* struct layer_surface.link */
	struct toplevel *focused;

	/* input method relay (fcitx5 / ibus) */
	struct wl_list imes;            /* struct ime.link */
	struct wl_list text_inputs;     /* struct text_input.link */
	struct wlr_input_method_v2 *input_method;     /* active input method */
	struct wlr_text_input_v3 *focused_text_input; /* enabled text input of the
							 * focused surface, i.e. the one the IM
							 * talks to (NULL when none) */
	struct wlr_surface *ime_focused_surface; /* surface owning text input */
	struct wl_listener ime_focused_surface_destroy;

	/* IPC socket for status bars (JSON events) */
	int ipc_fd;
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients; /* ipc_client.link */
	uint32_t next_window_id;

	/* blur module: swapchain used to snapshot the scene behind a window
	 * (sized to the output it was created for) */
	struct wlr_swapchain *blur_swapchain;
	int blur_swapchain_w, blur_swapchain_h;

	struct wlr_scene_tree *drag_tree;

	/* pointer interaction state */
	struct toplevel *zone_toplevel; /* toplevel under an active zone press */
	bool zone_press;                /* press in the title strip zone, swallowed */
	bool right_button_held;         /* right mouse button currently pressed */
	bool left_button_held;          /* left mouse button currently pressed */
	/* chord gestures: hold one button, then press the other (double-click
	 * the other = toggle maximize/restore, hold the other = move the window
	 * under the cursor; releasing restores the cursor style) */
	bool chord_active;               /* a chord gesture is in progress */
	uint32_t chord_button;           /* the button pressed second (trigger) */
	bool chord_pending;              /* first trigger press: disambiguating
	                                    double-click vs hold */
	bool chord_moving;               /* the hold turned into a window move */
	struct toplevel *chord_toplevel; /* window the chord acts on */
	bool chord_swallow_left;         /* left press was consumed by a chord:
	                                    swallow its release too */
	bool chord_swallow_right;        /* same for right */
	struct wl_event_source *chord_timer; /* double-click vs hold timer */
	bool moving;                    /* a window move is in progress */
	struct toplevel *move_toplevel;
	double grab_x, grab_y; /* cursor offset from the window origin */
	double press_x, press_y;
	bool dragged;       /* moved beyond CONFIG_DRAG_THRESHOLD during a press */
	enum zone_action zone_action; /* armed double-click action on the title
	                                strip, executed on release; ZONE_NONE when
	                                no double-click is in progress */
	struct wl_event_source *zone_timer; /* holding the title strip this long
	                                       grabs the window for moving */
	uint32_t zone_button;         /* the button held in the title strip zone */
	struct timespec last_release_time;
	struct timespec wheel_burst_start; /* first tick of the current wheel
	                                      burst: ticks inside the burst
	                                      window count as one action */
	struct timespec wheel_last_tick;   /* time of the previous wheel tick:
	                                      two ticks CONFIG_WHEEL_TICK_GAP_NS
	                                      apart start a new action */
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

	/* last cursor image the focused client set (wl_pointer.set_cursor or
	 * wp_cursor_shape_device_v1.set_shape); restored by pointer.c when the
	 * compositor cursor override ends so the cursor doesn't stay stuck on
	 * resize/move.  client_cursor_shape is an xcursor-theme-independent
	 * shape enum (0 = none); the shape is rendered by the compositor, the
	 * surface by the client.  When both are set (mixed-protocol clients)
	 * the shape wins: it is always rendered at the correct scale. */
	struct wlr_surface *client_cursor_surface;
	int client_cursor_hotspot_x, client_cursor_hotspot_y;
	enum wp_cursor_shape_device_v1_shape client_cursor_shape;
	/* the seat client that owns client_cursor_shape (0 = none): the shape is
	 * only restored while that client is focused, so a stale shape from a
	 * previous client is never shown */
	struct wlr_seat_client *client_cursor_shape_client;
	struct wl_listener client_cursor_shape_client_destroy;
	struct wl_listener client_cursor_destroy;

	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener new_virtual_pointer;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener layout_change;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_layer_surface;
	struct wl_listener new_decoration;
	struct wl_listener new_ime;
	struct wl_listener new_text_input;
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

/* ---- main.c ---- */
void spawn_command(const char *cmd);

/* ---- scene.c: scene-graph tagging / hit-testing ---- */
void xdg_surface_tag(struct wlr_scene_tree *tree, enum scene_tag_type type,
	void *ptr);
void *scene_tag_at(struct server *server, enum scene_tag_type type,
	double lx, double ly);
struct toplevel *toplevel_at(struct server *server);

/* ---- toplevel.c: xdg-shell windows, window state, decorations ---- */
void toplevel_box(struct toplevel *tl, struct wlr_box *box);
struct wlr_output *toplevel_output(struct server *server,
	struct toplevel *tl);
struct toplevel *neighbor_toplevel(struct server *server,
	struct toplevel *tl, bool next, bool include_minimized);
void close_toplevel(struct toplevel *tl);
void set_fullscreen(struct server *server, struct toplevel *tl,
	bool fullscreen);
void set_maximized(struct server *server, struct toplevel *tl,
	bool maximized);
void restore_maximized_toplevel(struct toplevel *tl);
void set_minimized(struct server *server, struct toplevel *tl,
	bool minimized);
void focus_toplevel(struct server *server, struct toplevel *tl);
void update_toplevel_output(struct server *server, struct toplevel *tl);
void arrange_toplevels_work_area(struct server *server,
	struct wlr_output *output);
/* maximized window geometry for an output (see toplevel.c): used by the
 * restore/drag clamps so they land on the same pixel as the maximized box */
void maximized_box(struct server *server, struct wlr_output *output,
	struct wlr_box *box);
void server_new_toplevel(struct wl_listener *listener, void *data);
void server_new_decoration(struct wl_listener *listener, void *data);

/* ---- border.c: rounded server-side border ---- */
bool border_buffer_no_input(struct wlr_scene_buffer *buffer,
	double *sx, double *sy);
void update_toplevel_decoration(struct toplevel *tl);

/* ---- blur.c: GLSL gaussian background blur for transparent windows ---- */
void blur_toplevel_init(struct toplevel *tl);
void blur_toplevel_commit(struct toplevel *tl);
void blur_toplevel_update(struct toplevel *tl);
void blur_refresh_output(struct server *server,
	struct wlr_scene_output *scene_output);
void blur_finish(struct server *server);

/* ---- mask.c: rounded-corner clipping of window content ---- */
struct wlr_buffer *content_mask_buffer(struct server *server,
	int width, int height);
bool content_mask_render(struct server *server, struct wlr_surface *surface,
	struct wlr_buffer *dst, int width, int height, float radius,
	const struct wlr_box *geom, bool geom_clip, bool *margin_opaque);
void mask_toplevel_content(struct toplevel *tl);
void mask_toplevel_destroy(struct toplevel *tl);

/* ---- layer.c: wlr-layer-shell + work area ---- */
void get_work_area(struct server *server, struct wlr_output *output,
	struct wlr_box *area);
void server_new_layer_surface(struct wl_listener *listener, void *data);

/* ---- output.c: monitors + output management ---- */
void server_new_output(struct wl_listener *listener, void *data);
void server_layout_change(struct wl_listener *listener, void *data);
void output_manager_apply(struct wl_listener *listener, void *data);
void output_manager_test(struct wl_listener *listener, void *data);

/* ---- input.c: seat, keyboard, shortcuts ---- */
void focus_window(struct server *server, struct toplevel *tl);
void seat_request_set_cursor(struct wl_listener *listener, void *data);
void seat_request_set_shape(struct wl_listener *listener, void *data);
void seat_request_set_selection(struct wl_listener *listener, void *data);
void seat_request_set_primary_selection(struct wl_listener *listener,
	void *data);
void seat_request_start_drag(struct wl_listener *listener, void *data);
void seat_start_drag(struct wl_listener *listener, void *data);
void server_new_virtual_pointer(struct wl_listener *listener, void *data);
void server_new_virtual_keyboard(struct wl_listener *listener, void *data);
void server_new_input(struct wl_listener *listener, void *data);

/* one attached keyboard device (real or virtual): keeps per-device key /
 * modifiers listeners and owns the IM-grab key forwarding state */
struct keyboard {
	struct server *server;
	struct wlr_keyboard *keyboard;
	struct wl_list link; /* server.keyboards */

	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener destroy;
};

/* ---- input.c: seat, keyboard focus, shortcuts ---- */
void server_new_virtual_keyboard(struct wl_listener *listener, void *data);
bool keyboard_is_typing(struct wlr_input_device *device);

/* ---- ime.c: input method relay (fcitx5 / ibus) ---- */
void ime_set_focus(struct server *server, struct wlr_surface *surface);
void ime_attach_keyboard(struct server *server,
	struct wlr_keyboard *keyboard);
void ime_detach_keyboard(struct server *server,
	struct wlr_keyboard *keyboard);
/* true if the input method's keyboard grab is connected to this keyboard */
bool ime_keyboard_grabbed(struct server *server,
	struct wlr_keyboard *keyboard);
void ime_update_popup(struct server *server);
void ime_new_input_method(struct wl_listener *listener, void *data);
void ime_new_text_input(struct wl_listener *listener, void *data);

/* ---- pointer.c: cursor interaction (move / resize / gestures) ---- */
void begin_move(struct server *server, struct toplevel *tl,
	double ref_x, double ref_y);
void end_move(struct server *server);
void end_resize(struct server *server);
void update_cursor_style(struct server *server);
/* show the cursor the focused client currently wants (shape, surface or the
 * default arrow); used when a compositor cursor override ends */
void reapply_client_cursor(struct server *server);
void cursor_motion(struct wl_listener *listener, void *data);
void cursor_motion_absolute(struct wl_listener *listener, void *data);
void cursor_button(struct wl_listener *listener, void *data);
void cursor_axis(struct wl_listener *listener, void *data);
void cursor_frame(struct wl_listener *listener, void *data);

#endif /* XMONODYWM_SERVER_H */
