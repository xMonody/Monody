/*
 * shadow.c - window shadow policy
 *
 * The shadow is rendered by the rounded-corner shader (rounded.c) as a
 * ring outside the window that fades outward.  This module owns the shadow
 * *policy*:
 *
 *   - only the focused window casts a shadow (unfocused -> width 0);
 *   - the shadow width and opacity come from config.h;
 *   - the shadow color is the window's border color (see border.c), so it
 *     follows the top border's three accent colors.
 */

#include "server.h"

float shadow_width(struct toplevel *tl) {
	/* only the focused window casts a shadow */
	if (tl->server->focused != tl) {
		return 0.0f;
	}
	return (float)CONFIG_SHADOW_WIDTH;
}

float shadow_alpha(void) {
	return CONFIG_SHADOW_ALPHA;
}
