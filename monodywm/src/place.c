/*
 * place.c - window placement (auto-centered)
 *
 * Windows keep the size the client committed (the compositor never
 * re-sizes them, no wlr_xdg_toplevel_set_size call here) and are centered
 * on the output (screen) both horizontally and vertically.  A fresh window
 * is centered on the screen the cursor is on, and re-centered whenever its
 * surface size changes (Electron windows such as QQ often map with a small
 * placeholder surface and only commit the real size on a later frame), so
 * the window ends up centered no matter when its real size arrives.  Once
 * the user interacts with the window (move / resize / maximize / fullscreen
 * set toplevel.user_moved) auto-centering stops and the window stays where
 * the user puts it.
 *
 * The centering reference is the SURFACE's actual rendering extent, not the
 * xdg window geometry: Electron windows (QQ) commit a surface larger than
 * their window geometry, and centering on the smaller geometry would leave
 * the real content offset toward the bottom-right - far enough to run off
 * screen.  (For normal windows surface == geometry; for CSD windows with a
 * transparent drop shadow the surface center is only off by half the shadow
 * margin, which is negligible.)
 *
 * CONFIG_CENTER_AVOID_BARS switches the centering reference from the whole
 * output box to the work area (the output minus layer-shell exclusive
 * zones, i.e. status bars), so a bar can never cover the new window.
 */

#include "server.h"

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

/* center a toplevel on the output under the cursor (falling back to the
 * center output when the cursor is outside every output).  Returns false
 * when the window has no usable size yet; true after the scene node has
 * been positioned.  The size is untouched; the position centers the
 * surface's rendering extent (node - geometry + surface size).  A window
 * bigger than the centering reference is anchored to the reference's
 * top-left corner so it never starts off-screen; anything smaller is
 * clamped so no part falls outside the screen. */
bool place_toplevel(struct server *server, struct toplevel *tl) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return false;
	}
	struct wlr_surface *surface = base->surface;
	int sw = surface->current.width;
	int sh = surface->current.height;
	if (sw <= 0 || sh <= 0) {
		return false;
	}
	/* the xdg surface tree is anchored at the geometry top-left and the
	 * surface sits at -geometry inside it, so the surface's top-left
	 * corner is (node.x - gx, node.y - gy) and the window's real
	 * rendering extent is that corner plus the surface size.  Centering
	 * on this extent (instead of the xdg geometry) is what keeps windows
	 * whose surface is bigger than their geometry (QQ) on center. */
	int gx = base->geometry.x;
	int gy = base->geometry.y;

	/* center on the output the cursor is on (the window should appear
	 * where the user is looking), falling back to the center output */
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(server->output_layout);
	}
	if (output == NULL) {
		return false;
	}

	struct wlr_box ref; /* centering reference: output box or work area */
#if CONFIG_CENTER_AVOID_BARS
	get_work_area(server, output, &ref);
#else
	wlr_output_layout_get_box(server->output_layout, output, &ref);
#endif

	/* node position that puts the surface's top-left corner here */
	int rx = ref.x + (ref.width - sw) / 2;
	int ry = ref.y + (ref.height - sh) / 2;

	/* keep the surface on screen */
	if (sw >= ref.width) {
		rx = ref.x; /* wider than the screen: left-align */
	} else {
		if (rx < ref.x) {
			rx = ref.x;
		} else if (rx + sw > ref.x + ref.width) {
			rx = ref.x + ref.width - sw;
		}
	}
	if (sh >= ref.height) {
		ry = ref.y; /* taller than the screen: top-align */
	} else {
		if (ry < ref.y) {
			ry = ref.y;
		} else if (ry + sh > ref.y + ref.height) {
			ry = ref.y + ref.height - sh;
		}
	}

	wlr_scene_node_set_position(&tl->scene_tree->node, rx + gx, ry + gy);
	return true;
}
