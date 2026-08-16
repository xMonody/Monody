/*
 * border.c - window border policy (width + colors)
 *
 * The border itself is drawn by the rounded-corner shader in rounded.c as a
 * ring just inside the window edge.  This module owns the border *policy*:
 * how wide the ring is and which colors it gets.
 *
 * The top band of the ring is split into three thirds (matching the
 * title-strip gesture zones: minimize / maximize / close), each with its own
 * color; the rest of the ring (sides and bottom) uses the focus-dependent
 * color.  Because that base color depends on focus, focus transitions call
 * border_focus_changed(), which marks the affected toplevels' rounded FBO
 * caches dirty; the next frame re-renders them with the new color.
 */

#include "server.h"

#include <stdint.h>

static struct wlr_render_color border_hex_to_color(uint32_t hex) {
	return (struct wlr_render_color){
		.r = (float)((hex >> 16) & 0xff) / 255.0f,
		.g = (float)((hex >> 8) & 0xff) / 255.0f,
		.b = (float)(hex & 0xff) / 255.0f,
		.a = 1.0f,
	};
}

float border_width(struct toplevel *tl) {
	/* fullscreen windows only get a border when explicitly enabled */
	if (tl->fullscreen && !CONFIG_FULLSCREEN_BORDER) {
		return 0.0f;
	}
	return (float)CONFIG_BORDER_WIDTH;
}

float border_gradient_width(struct toplevel *tl) {
	(void)tl;
	return (float)CONFIG_BORDER_GRADIENT_WIDTH;
}

struct wlr_render_color border_color(struct server *server,
		struct toplevel *tl) {
	/* fullscreen uses its own single border color (ignores focus) */
	if (tl->fullscreen) {
		return border_hex_to_color(CONFIG_FULLSCREEN_BORDER_COLOR);
	}
	return border_hex_to_color(server->focused == tl
		? CONFIG_BORDER_FOCUSED : CONFIG_BORDER_UNFOCUSED);
}

void border_top_colors(struct toplevel *tl,
		struct wlr_render_color *left, struct wlr_render_color *mid,
		struct wlr_render_color *right) {
	/* fullscreen border is a single color: no three-segment top band */
	if (tl->fullscreen) {
		struct wlr_render_color c =
			border_hex_to_color(CONFIG_FULLSCREEN_BORDER_COLOR);
		*left = *mid = *right = c;
		return;
	}
	/* dialogs and fixed-size windows have a single close-button strip, not
	 * three gesture zones: use one uniform (focus-dependent) color so the
	 * top band is not split into three segments */
	if (toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl)) {
		struct wlr_render_color c = border_color(tl->server, tl);
		*left = *mid = *right = c;
		return;
	}
	*left = border_hex_to_color(CONFIG_BORDER_TOP_LEFT);
	*mid = border_hex_to_color(CONFIG_BORDER_TOP_MID);
	*right = border_hex_to_color(CONFIG_BORDER_TOP_RIGHT);
}

void border_focus_changed(struct toplevel *tl, struct toplevel *prev) {
	/* the border color depends on focus: re-render both the newly focused
	 * window and the previously focused one so their rounded FBOs pick up
	 * the new focused/unfocused border color.  The content did not change,
	 * so this is a mask-only re-render: the cached content pass is reused
	 * and only the SDF shader is re-run (see rounded.c). */
	rounded_cache_dirty_mask(tl);
	if (prev != NULL) {
		rounded_cache_dirty_mask(prev);
	}
}
