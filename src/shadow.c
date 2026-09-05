/*
 * shadow.c - window shadow policy (scenefx-style gaussian drop shadow)
 *
 * The shadow is rendered by the rounded-corner shader (rounded.c) as a
 * soft gaussian falloff in the signed-distance field outside the window.
 * This module owns the shadow *policy*:
 *
 *   - only the focused window casts a shadow (unfocused -> sigma 0);
 *   - the softness (gaussian sigma), the color and the peak opacity come
 *     from config.h;
 *   - the shadow color is independent of the border color (the old
 *     implementation tinted the shadow with the border color, which made
 *     it read as a hard colored outline ring rather than a shadow).
 *
 * The FBO padding reserved around every window is derived from the sigma:
 * shadow_padding() = ceil(3.5 * sigma).  The gaussian has fallen to about
 * 0.2% of its peak there, so the cut at the padding edge is invisible.
 */

#include "server.h"

#include <math.h>

float shadow_sigma(struct toplevel *tl) {
	/* only the focused window casts a shadow */
	if (tl->server->focused != tl) {
		return 0.0f;
	}
	return CONFIG_SHADOW_BLUR_SIGMA;
}

float shadow_alpha(void) {
	return CONFIG_SHADOW_ALPHA;
}

struct wlr_render_color shadow_color(void) {
	return (struct wlr_render_color){
		.r = (float)((CONFIG_SHADOW_COLOR >> 16) & 0xff) / 255.0f,
		.g = (float)((CONFIG_SHADOW_COLOR >> 8) & 0xff) / 255.0f,
		.b = (float)(CONFIG_SHADOW_COLOR & 0xff) / 255.0f,
		.a = 1.0f,
	};
}

/* logical pixels of padding reserved on every side of a window's FBO so
 * the shadow never reaches the FBO edge; kept constant across focus
 * changes so focus transitions stay mask-only re-renders (rounded.c) */
int shadow_padding(void) {
	return (int)ceilf(CONFIG_SHADOW_BLUR_SIGMA * 3.5f);
}
