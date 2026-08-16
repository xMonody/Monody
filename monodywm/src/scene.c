/*
 * scene.c - scene-graph helpers shared by the window / layer modules
 *
 * Every interesting scene node carries a struct scene_tag in node.data so
 * the compositor can find the owning object (toplevel, layer surface) from
 * an arbitrary hit-tested node by walking up the parent chain.
 */

#include "server.h"

#include <stdlib.h>

static void scene_tag_destroy(struct wl_listener *listener, void *data) {
	struct scene_tag *tag = wl_container_of(listener, tag, destroy);
	wl_list_remove(&tag->destroy.link);
	free(tag);
}

void xdg_surface_tag(struct wlr_scene_tree *tree,
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
void *scene_tag_at(struct server *server, enum scene_tag_type type,
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
			if (tag->type != TAG_POPUP) {
				return NULL; /* a closer tagged object won the hit test */
			}
			/* a popup belongs to a toplevel: keep walking up so the
			 * owning window is still found under an open menu */
		}
		n = n->parent != NULL ? &n->parent->node : NULL;
	}
	return NULL;
}

struct toplevel *toplevel_at(struct server *server) {
	return scene_tag_at(server, TAG_TOPLEVEL, server->cursor->x,
		server->cursor->y);
}

/* is the cursor over a popup (menu / dropdown / tooltip) surface?  A popup
 * floats above its parent toplevel, so a popup covering the window's border
 * wins the pointer: the compositor's frame grabs (resize edges, title
 * strip) are disabled there and the click reaches the popup instead of
 * starting a resize.  scene_tag_at() deliberately walks up past TAG_POPUP
 * to find the owning window, so the popup itself is detected with an
 * explicit hit test instead. */
bool pointer_over_popup(struct server *server) {
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, server->cursor->x, server->cursor->y,
		&sx, &sy);
	if (node == NULL) {
		return false;
	}
	struct wlr_scene_node *n = node;
	while (n != NULL) {
		if (n->data != NULL) {
			struct scene_tag *tag = n->data;
			return tag->type == TAG_POPUP;
		}
		n = n->parent != NULL ? &n->parent->node : NULL;
	}
	return false;
}
