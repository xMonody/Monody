/* interaction test: simulates the top-10px title bar gestures via the
 * wlr virtual pointer: drag-move, wheel maximize/minimize, double-click close */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-shell-client.h"
#include "wlr-vp-client.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#define OUT_W 1280
#define OUT_H 720
#define WIN_W 200
#define WIN_H 100

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *wl_pointer;
static struct zwlr_virtual_pointer_manager_v1 *vp_mgr;
static struct zwlr_virtual_pointer_v1 *vp;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct zxdg_toplevel_decoration_v1 *deco;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;

static bool got_button_press;
static bool got_button_release;
static bool got_close;
static int last_configure_w, last_configure_h;
static bool last_configure_maximized;
static int configure_count;
static struct wl_buffer *current_buffer;
static int current_buf_w, current_buf_h;
static int pending_resize_w, pending_resize_h; /* size from the latest toplevel configure */

static void recreate_buffer(int w, int h);

static int failures;

static void handle_toplevel_close(void *data, struct xdg_toplevel *t);

static void check(bool cond, const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("%s %s\n", cond ? "PASS" : "FAIL", buf);
	if (!cond) {
		failures++;
	}
}

static void handle_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = handle_ping };

static bool xdg_configured;

static void handle_xdg_configure(void *data, struct xdg_surface *s, uint32_t serial) {
	/* ack must be sent before the buffer commit that follows */
	xdg_surface_ack_configure(s, serial);
	xdg_configured = true;
	if (pending_resize_w > 0 && pending_resize_h > 0 &&
			(pending_resize_w != current_buf_w ||
				pending_resize_h != current_buf_h)) {
		recreate_buffer(pending_resize_w, pending_resize_h);
	}
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_xdg_configure,
};

static void recreate_buffer(int w, int h) {
	int stride = w * 4;
	int fd = syscall(SYS_memfd_create, "buf", 0);
	if (fd < 0) { perror("memfd"); exit(1); }
	ftruncate(fd, stride * h);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride * h);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h,
		stride, WL_SHM_FORMAT_ARGB8888);
	if (current_buffer) {
		wl_buffer_destroy(current_buffer);
	}
	current_buffer = buffer;
	current_buf_w = w;
	current_buf_h = h;
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_commit(surface);
}

static void handle_toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	last_configure_w = w;
	last_configure_h = h;
	last_configure_maximized = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED) last_configure_maximized = true;
	}
	configure_count++;
	printf("  [configure %d] %dx%d maximized=%d\n",
		configure_count, w, h, last_configure_maximized);
	/* remember the size; the buffer is (re)created after the ack */
	pending_resize_w = w;
	pending_resize_h = h;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = handle_toplevel_configure,
	.close = handle_toplevel_close,
};

static void handle_toplevel_close(void *data, struct xdg_toplevel *t) {
	printf("  [close event received]\n");
	got_close = true;
}
static void handle_pointer_enter(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
	(void)s; (void)sx; (void)sy;
}
static void handle_pointer_leave(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s) { (void)s; }
static void handle_pointer_motion(void *data, struct wl_pointer *p,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) { (void)sx; (void)sy; }
static void handle_pointer_button(void *data, struct wl_pointer *p,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	if (button == BTN_LEFT) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) got_button_press = true;
		else got_button_release = true;
		printf("  [client saw button %s]\n",
			state == WL_POINTER_BUTTON_STATE_PRESSED ? "press" : "release");
	}
}
static void handle_pointer_axis(void *data, struct wl_pointer *p,
		uint32_t time, uint32_t axis, wl_fixed_t value) { (void)axis; (void)value; }
static void handle_pointer_frame(void *data, struct wl_pointer *p) {}
static void handle_pointer_axis_source(void *data, struct wl_pointer *p,
		uint32_t axis_source) {}
static void handle_pointer_axis_stop(void *data, struct wl_pointer *p,
		uint32_t time, uint32_t axis) {}
static void handle_pointer_axis_discrete(void *data, struct wl_pointer *p,
		uint32_t axis, int32_t discrete) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = handle_pointer_enter,
	.leave = handle_pointer_leave,
	.motion = handle_pointer_motion,
	.button = handle_pointer_button,
	.axis = handle_pointer_axis,
	.frame = handle_pointer_frame,
	.axis_source = handle_pointer_axis_source,
	.axis_stop = handle_pointer_axis_stop,
	.axis_discrete = handle_pointer_axis_discrete,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
	} else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		vp_mgr = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 1);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		deco_mgr = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
	}
}
static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void roundtrip(void) {
	wl_display_flush(display);
	wl_display_roundtrip(display);
}

static void vp_motion_abs(int x, int y) {
	zwlr_virtual_pointer_v1_motion_absolute(vp, 1, x, y, OUT_W, OUT_H);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
}

static void vp_move_rel(int dx, int dy) {
	zwlr_virtual_pointer_v1_motion(vp, 1, wl_fixed_from_int(dx), wl_fixed_from_int(dy));
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
}

static void vp_button(uint32_t state) {
	zwlr_virtual_pointer_v1_button(vp, 1, BTN_LEFT, state);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
}

static void vp_scroll(int discrete) {
	zwlr_virtual_pointer_v1_axis_discrete(vp, 1,
		WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(discrete * 15), discrete);
	zwlr_virtual_pointer_v1_axis(vp, 1, WL_POINTER_AXIS_VERTICAL_SCROLL,
		wl_fixed_from_int(discrete * 15));
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(display);
}

static void sync_state(void) {
	got_button_press = false;
	got_button_release = false;
	got_close = false;
	configure_count = 0;
}

static bool use_csd; /* request client-side decoration before the first commit */

/* create a 200x100 shm buffer + toplevel */
static void create_window(void) {
	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(xdg_toplevel, "interaction-test");
	if (use_csd && deco_mgr) {
		deco = zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr,
			xdg_toplevel);
		zxdg_toplevel_decoration_v1_set_mode(deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	}
	wl_surface_commit(surface);
	roundtrip();

	/* wait for the initial configure (the compositor schedules it on an idle
	 * callback, so a couple of roundtrips are needed) */
	for (int i = 0; i < 20 && !xdg_configured; i++) {
		roundtrip();
	}
	if (!xdg_configured) {
		fprintf(stderr, "initial configure never arrived\n");
		exit(1);
	}
	/* attach a buffer so the window maps */
	recreate_buffer(WIN_W, WIN_H);
	roundtrip();
	roundtrip();
}

int main(void) {
	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	roundtrip();
	if (!compositor || !xdg_wm_base || !shm || !seat || !vp_mgr) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	wl_pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);
	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vp_mgr, seat);
	roundtrip();

	/* position the cursor before mapping window A: it will be centered there */
	vp_motion_abs(640, 360);
	roundtrip();
	/* ---- window A: drag to move + double click to close ---- */
	create_window();
	/* window A centered at (640,360) -> top-left (540,310), strip y in [310,320) */

	printf("== test: drag from top strip moves the window ==\n");
	sync_state();
	vp_motion_abs(640, 315);        /* into the top strip */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_motion_abs(740, 420);        /* drag */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	/* grab offset was (100,5); window moved to (640,415)-(840,515). Click at
	 * (740,465): inside the window but below its strip, must be delivered */
	sync_state();
	vp_motion_abs(740, 465);
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	check(got_button_press, "window moved: click at (740,465) reached the window");

	printf("== test: double click in top strip closes the window ==\n");
	sync_state();
	vp_motion_abs(740, 420);        /* strip of the moved window is y in [415,425) */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	usleep(100000); /* wait for the click to be recorded */
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	usleep(300000);
	check(got_close, "double click in top strip sent close");
	/* clean up window A: destroy the role object first, then the surface */
	xdg_toplevel_destroy(xdg_toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	roundtrip();

	/* ---- window B: wheel up = maximize, wheel down = minimize ---- */
	vp_motion_abs(640, 360);        /* place cursor before mapping */
	roundtrip();
	create_window();
	/* window B centered at (640,360): pos (540,310), strip y in [310,320) */
	printf("== test: wheel up (holding) maximizes, wheel down minimizes ==\n");
	sync_state();
	vp_motion_abs(640, 315);        /* strip of the windowed window */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_scroll(1);                    /* scroll up -> maximize */
	roundtrip();
	check(last_configure_maximized, "wheel up maximized the window");
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	roundtrip();
	/* the maximized window's top strip is the screen top; hold there and
	 * scroll down -> minimize */
	vp_motion_abs(640, 5);
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_scroll(-1);                   /* scroll down */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	/* minimized -> window B is hidden; a click at its old position must NOT
	 * reach the client */
	sync_state();
	vp_motion_abs(640, 315);
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	check(!got_button_press, "wheel down minimized the window (click not delivered)");
	/* clean up window B */
	xdg_toplevel_destroy(xdg_toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	roundtrip();

	/* ---- window C: client-side decorated -> clicks in the top 10px are
	 * forwarded to the client (its own title bar handles the window) ---- */
	printf("== test: CSD window keeps native interaction in the top strip ==\n");
	if (deco_mgr) {
		vp_motion_abs(640, 360);
		roundtrip();
		use_csd = true;
		create_window();
		/* window C centered at (640,360): pos (540,310), strip y in [310,320) */
		sync_state();
		vp_motion_abs(640, 315); /* in the top 10 px of the CSD window */
		roundtrip();
		vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
		roundtrip();
		vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
		roundtrip();
		check(got_button_press,
			"CSD window: press in top 10px reached the client (not swallowed)");
	} else {
		printf("  (no decoration manager, skipping)\n");
	}
	printf("note: decoration mode negotiation covered by test-client\n");

	/* ---- window D: edge resize (right + bottom) ---- */
	vp_motion_abs(640, 360);
	roundtrip();
	use_csd = false;
	create_window();
	/* window D centered at (640,360): pos (540,310), size 200x100,
	 * right edge at x=740, bottom edge at y=410 */
	printf("== test: drag from the right edge resizes ==\n");
	sync_state();
	vp_motion_abs(738, 360); /* in the right-edge zone (x>732) */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_motion_abs(838, 360); /* drag 100 px right */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	check(last_configure_w >= 300,
		"right-edge drag resized the window (width>=300, got %d)",
		last_configure_w);
	check(last_configure_w == 300,
		"right-edge drag resized the window to exactly 300 (got %d)",
		last_configure_w);

	printf("== test: drag from the bottom edge resizes ==\n");
	sync_state();
	vp_motion_abs(640, 407); /* in the bottom-edge zone (y>402) */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_motion_abs(640, 457); /* drag 50 px down */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	check(last_configure_h == 150,
		"bottom-edge drag resized the window to height 150 (got %d)",
		last_configure_h);

	/* ---- window D: maximize then drag title bar -> restore (Windows) ---- */
	printf("== test: dragging a maximized window's title bar restores it ==\n");
	sync_state();
	vp_motion_abs(640, 315); /* in the top strip of the restored window */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_scroll(1); /* wheel up: maximize (saves the 540,310 300x150 box) */
	roundtrip();
	check(last_configure_maximized, "wheel up maximized the window");
	/* drag from the maximized window's top strip (the screen top) */
	vp_motion_abs(640, 5);
	roundtrip();
	vp_motion_abs(740, 200); /* drag: crosses threshold -> restore + move */
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	roundtrip();
	roundtrip();
	check(!last_configure_maximized,
		"drag restored the window (unmaximized configure received)");
	/* the window should now be at ~(640,200) (restore box 540,310 300x150,
	 * grab clamped to (100,0) -> pos = cursor(740,200) - (100,0) = (640,200)).
	 * Click at (740,260): inside the window (y 200..350) below the strip
	 * (200..210), must reach the client. */
	sync_state();
	vp_motion_abs(740, 260);
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_PRESSED);
	roundtrip();
	vp_button(WL_POINTER_BUTTON_STATE_RELEASED);
	roundtrip();
	check(got_button_press,
		"restored window followed the drag (click at (740,260) reached it)");

	/* cleanup window D */
	xdg_toplevel_destroy(xdg_toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	roundtrip();

	printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
	return failures ? 1 : 0;
}
