/*
 * place.c - initial window placement
 *
 * A freshly mapped window is placed with its size completely client-driven:
 * whatever size the client committed is used as-is, the compositor never
 * re-sizes it (no wlr_xdg_toplevel_set_size call here).  The position is
 * centered on the output (screen) both horizontally and vertically - the
 * window appears in the middle of the screen the cursor is on, instead of
 * being offset toward the cursor like the old cursor-centered placement.
 *
 * CONFIG_CENTER_AVOID_BARS switches the centering reference from the whole
 * output box to the work area (the output minus layer-shell exclusive
 * zones, i.e. status bars), so a bar can never cover the new window.
 */

#include "server.h"

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

/* place a freshly mapped toplevel: size untouched, position centered on
 * the output under the cursor (falling back to the center output when the
 * cursor is outside every output).  A window bigger than the centering
 * reference is anchored to the reference's top-left corner so it never
 * starts off-screen; anything smaller is clamped so no part falls outside
 * the screen. */
void place_toplevel(struct server *server, struct toplevel *tl) {
	/* the size comes straight from the client: toplevel_box() reads the
	 * committed surface/geometry, and nothing below calls
	 * wlr_xdg_toplevel_set_size(), so the window keeps its own size */
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}

	/* center on the output the cursor is on (the window should appear
	 * where the user is looking), falling back to the center output */
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output == NULL) {
		return;
	}

	struct wlr_box ref; /* centering reference: output box or work area */
#if CONFIG_CENTER_AVOID_BARS
	get_work_area(server, output, &ref);
#else
	wlr_output_layout_get_box(server->output_layout, output, &ref);
#endif

	/* horizontally and vertically centered on the reference */
	int x = ref.x + (ref.width - box.width) / 2;
	int y = ref.y + (ref.height - box.height) / 2;

	/* keep the window on screen */
	if (box.width >= ref.width) {
		x = ref.x; /* window wider than the screen: left-align */
	} else {
		if (x < ref.x) {
			x = ref.x;
		} else if (x + box.width > ref.x + ref.width) {
			x = ref.x + ref.width - box.width;
		}
	}
	if (box.height >= ref.height) {
		y = ref.y; /* window taller than the screen: top-align */
	} else {
		if (y < ref.y) {
			y = ref.y;
		} else if (y + box.height > ref.y + ref.height) {
			y = ref.y + ref.height - box.height;
		}
	}

	wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
}
