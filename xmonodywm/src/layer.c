/*
 * layer.c - wlr-layer-shell surfaces (backgrounds, bars, overlays)
 *
 * Layer surfaces are stacked into their own scene trees between the
 * window layers; their exclusive zones shrink the work area that maximized
 * windows use.
 */

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

static int scene_layer_index(enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return LAYER_BACKGROUND;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:      return LAYER_BOTTOM;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:         return LAYER_TOP;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:     return LAYER_OVERLAY;
	}
	return LAYER_OVERLAY;
}

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

void get_work_area(struct server *server, struct wlr_output *output,
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

/* track whether the layer surface's exclusive-zone geometry changed since
 * the last commit (the first commit always counts as a change, so a bar
 * that appears right after windows already exist kicks them out of its
 * area) */
static bool layer_exclusive_zone_changed(struct layer_surface *ls) {
	struct wlr_layer_surface_v1_state *st = &ls->layer_surface->current;
	bool mapped = ls->layer_surface->surface->mapped;
	bool changed = !ls->has_last_state || ls->last_anchor != st->anchor ||
		ls->last_zone != st->exclusive_zone ||
		ls->last_margin_top != st->margin.top ||
		ls->last_margin_bottom != st->margin.bottom ||
		ls->last_margin_left != st->margin.left ||
		ls->last_margin_right != st->margin.right ||
		ls->last_mapped != mapped;
	ls->has_last_state = true;
	ls->last_anchor = st->anchor;
	ls->last_zone = st->exclusive_zone;
	ls->last_margin_top = st->margin.top;
	ls->last_margin_bottom = st->margin.bottom;
	ls->last_margin_left = st->margin.left;
	ls->last_margin_right = st->margin.right;
	ls->last_mapped = mapped;
	return changed;
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, commit);
	configure_layer_surface(ls);
	/* a bar appearing / resizing shrinks the work area: move existing
	 * windows back out of its exclusive zone instead of letting it cover
	 * them */
	if (layer_exclusive_zone_changed(ls)) {
		struct wlr_output *output = ls->layer_surface->output;
		if (output == NULL) {
			output = wlr_output_layout_get_center_output(
				ls->server->output_layout);
		}
		if (output != NULL) {
			arrange_toplevels_work_area(ls->server, output);
		}
	}
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->link);
	/* the exclusive zone is gone: let maximized windows expand again */
	struct wlr_output *output = ls->layer_surface->output;
	if (output == NULL) {
		output = wlr_output_layout_get_center_output(
			ls->server->output_layout);
	}
	if (output != NULL) {
		arrange_toplevels_work_area(ls->server, output);
	}
	free(ls);
}

void server_new_layer_surface(struct wl_listener *listener, void *data) {
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
