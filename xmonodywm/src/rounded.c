/*
 * rounded.c - offscreen rounded-corner compositing (wlroots gles2 + FBO)
 *
 * Each toplevel keeps a cached, rounded copy of its client content in an
 * offscreen FBO pair:
 *
 *   1. the client content (the xdg surface plus its subsurfaces, excluding
 *      popups) is composited into a first DMA-BUF via a normal wlroots
 *      render pass;
 *   2. a GLES2 fragment shader re-draws that buffer into a second DMA-BUF
 *      through a rounded-rectangle signed-distance-field mask.
 *
 * The rounded result is shown as a wlr_scene_buffer placed *underneath* the
 * client content.  The client content itself stays in the scene (so it keeps
 * receiving input, frame callbacks and popup stacking) but its opacity is set
 * to zero, so the scene only ever draws the rounded FBO copy on screen.
 *
 * The FBO pair is a cache: it is only re-rendered when the content is marked
 * dirty (client commit, subsurface commit, geometry change).  While the
 * content is unchanged the cached buffer is reused without any redraw.
 *
 * The offscreen FBO is sized to the xdg window geometry, so client-drawn
 * decorations outside the geometry (CSD shadow margins) are clipped away;
 * undecorated windows (geometry == surface) are unaffected.
 *
 * The composited result never leaves the GPU: no glReadPixels is used
 * anywhere in this file.
 */

#include "server.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

/*
 * Fullscreen-quad vertex shader.  a_pos is in [0,1] with (0,0) at the top
 * left.  The y mapping (clip.y = 2*y - 1) matches wlroots' gles2 renderer,
 * which renders buffers with the first row at the bottom of the GL
 * framebuffer and samples textures with UV (0,0) at the buffer's top row.
 */
static const char *rounded_vert_src =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_pos;\n"
	"  gl_Position = vec4(a_pos.x * 2.0 - 1.0, a_pos.y * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* Rounded-rectangle SDF mask over the sampled content texture. */
static const char *rounded_frag_src =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_size;\n"
	"uniform float u_radius;\n"
	"void main() {\n"
	"  vec2 p = v_uv * u_size;\n"
	"  vec2 b = u_size * 0.5;\n"
	"  vec2 q = abs(p - b) - (b - vec2(u_radius));\n"
	"  float sd = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - u_radius;\n"
	"  float alpha = 1.0 - smoothstep(-1.0, 1.0, sd);\n"
	"  vec4 c = texture2D(u_tex, v_uv);\n"
	"  gl_FragColor = vec4(c.rgb * alpha, c.a * alpha);\n"
	"}\n";

struct rounded_cache {
	struct server *server;
	struct toplevel *tl;
	struct wlr_scene_buffer *node;

	/* offscreen cache: content is composited here, then masked into the
	 * rounded buffer which is what the scene actually shows */
	struct wlr_buffer *content_buf;
	struct wlr_buffer *rounded_buf;
	GLuint content_tex;              /* GPU-side copy of content_buf */
	int content_tex_width, content_tex_height;

	int fbo_width, fbo_height;       /* current FBO size in physical pixels */
	int logical_width, logical_height; /* current size in layout pixels */
	float scale;                     /* output scale the FBO was rendered at */

	bool dirty;
	bool gl_ready;                   /* shader program compiled + linked */
	bool failed;                     /* permanently disabled (no gles2/GL) */

	GLuint program;
	GLuint vbo;
	GLint a_pos;
	GLint u_tex;
	GLint u_size;
	GLint u_radius;
};

/* --- small GL helpers -------------------------------------------------- */

struct egl_context_state {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw, read;
};

static bool rounded_begin_gl(struct wlr_renderer *renderer,
		struct egl_context_state *save) {
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
	if (egl == NULL) {
		return false;
	}

	save->display = eglGetCurrentDisplay();
	save->context = eglGetCurrentContext();
	save->draw = eglGetCurrentSurface(EGL_DRAW);
	save->read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(wlr_egl_get_display(egl), EGL_NO_SURFACE,
			EGL_NO_SURFACE, wlr_egl_get_context(egl))) {
		wlr_log(WLR_ERROR, "rounded: eglMakeCurrent failed");
		return false;
	}
	return true;
}

static void rounded_end_gl(struct egl_context_state *save) {
	if (save->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(save->display, save->draw, save->read, save->context);
	}
}

static GLuint rounded_compile_shader(GLenum type, const char *src) {
	GLuint shader = glCreateShader(type);
	if (shader == 0) {
		return 0;
	}
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "rounded: shader compile failed: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static bool rounded_gl_init(struct rounded_cache *rc) {
	GLuint vert = rounded_compile_shader(GL_VERTEX_SHADER, rounded_vert_src);
	GLuint frag = rounded_compile_shader(GL_FRAGMENT_SHADER, rounded_frag_src);
	if (vert == 0 || frag == 0) {
		if (vert != 0) {
			glDeleteShader(vert);
		}
		if (frag != 0) {
			glDeleteShader(frag);
		}
		return false;
	}

	rc->program = glCreateProgram();
	glAttachShader(rc->program, vert);
	glAttachShader(rc->program, frag);
	glLinkProgram(rc->program);

	GLint ok = GL_FALSE;
	glGetProgramiv(rc->program, GL_LINK_STATUS, &ok);
	glDeleteShader(vert);
	glDeleteShader(frag);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(rc->program, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "rounded: program link failed: %s", log);
		glDeleteProgram(rc->program);
		rc->program = 0;
		return false;
	}

	rc->a_pos = glGetAttribLocation(rc->program, "a_pos");
	rc->u_tex = glGetUniformLocation(rc->program, "u_tex");
	rc->u_size = glGetUniformLocation(rc->program, "u_size");
	rc->u_radius = glGetUniformLocation(rc->program, "u_radius");

	static const float quad[] = {
		0.0f, 0.0f, /* top-left */
		1.0f, 0.0f, /* top-right */
		0.0f, 1.0f, /* bottom-left */
		1.0f, 1.0f, /* bottom-right */
	};
	glGenBuffers(1, &rc->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, rc->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	return true;
}

static void rounded_gl_fini(struct rounded_cache *rc) {
	if (rc->vbo != 0) {
		glDeleteBuffers(1, &rc->vbo);
	}
	if (rc->program != 0) {
		glDeleteProgram(rc->program);
	}
	if (rc->content_tex != 0) {
		glDeleteTextures(1, &rc->content_tex);
		rc->content_tex = 0;
	}
}

/* --- buffer cache ------------------------------------------------------ */

static struct wlr_buffer *rounded_alloc_buffer(struct rounded_cache *rc,
		int width, int height) {
	if (width <= 0 || height <= 0) {
		return NULL;
	}
	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format fmt = {
		.format = DRM_FORMAT_ARGB8888,
		.len = 1,
		.capacity = 1,
		.modifiers = mods,
	};
	return wlr_allocator_create_buffer(rc->server->allocator, width, height,
		&fmt);
}

static void rounded_drop_buffers(struct rounded_cache *rc) {
	if (rc->content_buf != NULL) {
		wlr_buffer_drop(rc->content_buf);
		rc->content_buf = NULL;
	}
	if (rc->rounded_buf != NULL) {
		wlr_buffer_drop(rc->rounded_buf);
		rc->rounded_buf = NULL;
	}
}

static void rounded_release_buffers(struct rounded_cache *rc) {
	rounded_drop_buffers(rc);
	rc->fbo_width = rc->fbo_height = 0;
	rc->logical_width = rc->logical_height = 0;
}

static bool rounded_alloc_buffers(struct rounded_cache *rc,
		int logical_width, int logical_height, float scale) {
	int fw = (int)ceilf((float)logical_width * scale);
	int fh = (int)ceilf((float)logical_height * scale);
	if (fw <= 0 || fh <= 0) {
		return false;
	}

	/* reuse the existing pair while the FBO size stays the same */
	if (rc->content_buf != NULL && rc->rounded_buf != NULL &&
			rc->fbo_width == fw && rc->fbo_height == fh) {
		rc->logical_width = logical_width;
		rc->logical_height = logical_height;
		rc->scale = scale;
		return true;
	}

	/* size changed: allocate a fresh pair and drop the old one afterwards */
	struct wlr_buffer *content_buf = rounded_alloc_buffer(rc, fw, fh);
	struct wlr_buffer *rounded_buf = rounded_alloc_buffer(rc, fw, fh);
	if (content_buf == NULL || rounded_buf == NULL) {
		if (content_buf != NULL) {
			wlr_buffer_drop(content_buf);
		}
		if (rounded_buf != NULL) {
			wlr_buffer_drop(rounded_buf);
		}
		return false;
	}

	rounded_drop_buffers(rc); /* the scene node keeps its own ref to the old
	                             rounded buffer until it is re-published */
	rc->content_buf = content_buf;
	rc->rounded_buf = rounded_buf;
	rc->fbo_width = fw;
	rc->fbo_height = fh;
	rc->logical_width = logical_width;
	rc->logical_height = logical_height;
	rc->scale = scale;
	return true;
}

/* --- content compositing pass (wlroots render pass) -------------------- */

struct content_texture {
	struct wlr_texture *texture;
	bool owned; /* created by us, must be destroyed after the pass */
};

struct content_pass_ctx {
	struct rounded_cache *rc;
	struct wlr_render_pass *pass;
	struct wlr_surface *root_surface;
	float scale;
	bool rendered_main;
	bool main_texture_failed;
	struct wl_array textures; /* struct content_texture */
};

/* root surface of a surface's subsurface chain (itself if not a subsurface) */
static struct wlr_surface *rounded_surface_root(struct wlr_surface *surface) {
	struct wlr_subsurface *sub = wlr_subsurface_try_from_wlr_surface(surface);
	while (sub != NULL && sub->parent != NULL) {
		surface = sub->parent;
		sub = wlr_subsurface_try_from_wlr_surface(surface);
	}
	return surface;
}

static void rounded_content_pass_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct content_pass_ctx *ctx = data;

	/* only the toplevel's own surface tree (surface + subsurfaces) is
	 * composited; popups and our own rounded node are skipped */
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL) {
		return;
	}
	if (rounded_surface_root(scene_surface->surface) != ctx->root_surface) {
		return;
	}
	if (buffer->buffer == NULL) {
		return;
	}

	struct wlr_texture *texture = NULL;
	bool owns_texture = false;

	/* Prefer the client buffer's cached texture - exactly the one the scene
	 * renderer samples.  Calling wlr_texture_from_buffer() here instead would
	 * re-import the buffer and, for SHM buffers whose source has already been
	 * released by the client, that data-pointer access fails (source == NULL),
	 * leaving the FBO stale. */
	struct wlr_client_buffer *client_buffer =
		wlr_client_buffer_get(buffer->buffer);
	if (client_buffer != NULL && client_buffer->texture != NULL) {
		texture = client_buffer->texture;
	} else {
		texture = wlr_texture_from_buffer(ctx->rc->server->renderer,
			buffer->buffer);
		if (texture == NULL) {
			if (scene_surface->surface == ctx->root_surface) {
				ctx->main_texture_failed = true;
				wlr_log(WLR_ERROR, "rounded: failed to create texture for "
					"main surface buffer %p (%dx%d)",
					(void *)buffer->buffer,
					buffer->buffer->width, buffer->buffer->height);
			}
			return;
		}
		owns_texture = true;
	}

	/* honour explicit buffer synchronization (linux-drm-syncobj-v1), exactly
	 * like wlroots' scene renderer does: wait for the client's acquire
	 * timeline before sampling the buffer */
	struct wlr_linux_drm_syncobj_surface_v1_state *syncobj_state =
		wlr_linux_drm_syncobj_v1_get_surface_state(scene_surface->surface);
	struct wlr_drm_syncobj_timeline *wait_timeline = NULL;
	uint64_t wait_point = 0;
	if (syncobj_state != NULL) {
		wait_timeline = syncobj_state->acquire_timeline;
		wait_point = syncobj_state->acquire_point;
	}

	struct content_texture *slot =
		wl_array_add(&ctx->textures, sizeof(struct content_texture));
	if (slot == NULL) {
		if (owns_texture) {
			wlr_texture_destroy(texture);
		}
		return;
	}
	slot->texture = texture;
	slot->owned = owns_texture;

	float alpha = 1.0f; /* content is hidden in the scene via opacity 0, but
	                       the FBO copy passes the client alpha through
	                       untouched (multiplier 1.0, not 0) */
	/* match wlroots' scene renderer: invert the buffer transform for our
	 * non-rotated offscreen FBO */
	enum wl_output_transform transform =
		wlr_output_transform_invert(buffer->transform);
	/* wlr_scene_node_for_each_buffer() reports layout (absolute) coordinates;
	 * the FBO is anchored at the scene tree's origin, so make the position
	 * tree-relative before scaling */
	int rel_x = sx - ctx->rc->tl->scene_tree->node.x;
	int rel_y = sy - ctx->rc->tl->scene_tree->node.y;
	struct wlr_box dst_box = {
		.x = (int)lroundf((float)rel_x * ctx->scale),
		.y = (int)lroundf((float)rel_y * ctx->scale),
		.width = (int)lroundf((float)buffer->dst_width * ctx->scale),
		.height = (int)lroundf((float)buffer->dst_height * ctx->scale),
	};
	wlr_render_pass_add_texture(ctx->pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.src_box = buffer->src_box,
		.dst_box = dst_box,
		.transform = transform,
		.filter_mode = buffer->filter_mode,
		.alpha = &alpha,
		.wait_timeline = wait_timeline,
		.wait_point = wait_point,
	});

	if (scene_surface->surface == ctx->root_surface) {
		ctx->rendered_main = true;
	}
}

static bool rounded_render_content(struct rounded_cache *rc) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return false;
	}

	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(rc->server->renderer, rc->content_buf,
			NULL);
	if (pass == NULL) {
		return false;
	}

	/* clear the cache buffer to transparent black first, so areas not
	 * covered by the content surface stay transparent instead of showing
	 * uninitialized DMA-BUF memory */
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0,
			.width = rc->fbo_width, .height = rc->fbo_height },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});

	struct content_pass_ctx ctx = {
		.rc = rc,
		.pass = pass,
		.root_surface = base->surface,
		.scale = rc->scale,
	};
	wl_array_init(&ctx.textures);

	wlr_scene_node_for_each_buffer(&rc->tl->scene_tree->node,
		rounded_content_pass_cb, &ctx);

	bool ok = wlr_render_pass_submit(pass);

	struct content_texture *ref;
	wl_array_for_each(ref, &ctx.textures) {
		if (ref->owned) {
			wlr_texture_destroy(ref->texture);
		}
	}
	wl_array_release(&ctx.textures);

	if (!ok) {
		wlr_log(WLR_ERROR, "rounded: content pass submit failed");
		return false;
	}
	if (ctx.main_texture_failed) {
		wlr_log(WLR_ERROR, "rounded: main surface texture creation failed, "
			"falling back to raw content");
		return false;
	}
	/* only treat the composite as valid if the main surface was actually
	 * rendered; otherwise the FBO would be empty/partial and hiding the
	 * client content would leave the window transparent */
	if (!ctx.rendered_main) {
		wlr_log(WLR_DEBUG, "rounded: main surface not rendered into FBO, "
			"falling back to raw content");
		return false;
	}
	return true;
}

/* --- rounded mask pass (raw GLES2) ------------------------------------- */

static bool rounded_render_mask(struct rounded_cache *rc) {
	struct wlr_renderer *renderer = rc->server->renderer;
	if (!wlr_renderer_is_gles2(renderer)) {
		return false;
	}

	GLuint content_fbo =
		wlr_gles2_renderer_get_buffer_fbo(renderer, rc->content_buf);
	GLuint out_fbo =
		wlr_gles2_renderer_get_buffer_fbo(renderer, rc->rounded_buf);
	if (content_fbo == 0 || out_fbo == 0) {
		return false;
	}

	struct egl_context_state saved = {0};
	if (!rounded_begin_gl(renderer, &saved)) {
		return false;
	}

	/* (re)allocate the GPU-side content texture when the FBO size changes */
	if (rc->content_tex == 0 || rc->content_tex_width != rc->fbo_width ||
			rc->content_tex_height != rc->fbo_height) {
		if (rc->content_tex != 0) {
			glDeleteTextures(1, &rc->content_tex);
		}
		glGenTextures(1, &rc->content_tex);
		glBindTexture(GL_TEXTURE_2D, rc->content_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rc->fbo_width,
			rc->fbo_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		rc->content_tex_width = rc->fbo_width;
		rc->content_tex_height = rc->fbo_height;
	}

	/* copy the freshly composited content FBO into the texture (a pure
	 * GPU-side copy, no glReadPixels) */
	glBindFramebuffer(GL_FRAMEBUFFER, content_fbo);
	glBindTexture(GL_TEXTURE_2D, rc->content_tex);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
		rc->fbo_width, rc->fbo_height);
	glBindTexture(GL_TEXTURE_2D, 0);

	/* rounded-rectangle mask pass into the output FBO */
	glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
	glViewport(0, 0, rc->fbo_width, rc->fbo_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND); /* the mask pass overwrites the whole buffer */

	glUseProgram(rc->program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, rc->content_tex);
	glUniform1i(rc->u_tex, 0);
	glUniform2f(rc->u_size, (float)rc->fbo_width, (float)rc->fbo_height);
	glUniform1f(rc->u_radius, (float)CONFIG_ROUNDED_RADIUS * rc->scale);

	glBindBuffer(GL_ARRAY_BUFFER, rc->vbo);
	glEnableVertexAttribArray(rc->a_pos);
	glVertexAttribPointer(rc->a_pos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(rc->a_pos);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	/* the mask draw commands are ordered before the scene render in the same
	 * GL context, so the freshly written rounded FBO is visible when the
	 * scene samples it */

	rounded_end_gl(&saved);
	return true;
}

/* --- public API -------------------------------------------------------- */

/* the rounded node never intercepts pointer input: the (transparent) client
 * content above it is what receives clicks, so hit-testing is unchanged */
static bool rounded_no_input(struct wlr_scene_buffer *buffer,
		double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

struct content_opacity_ctx {
	struct wlr_surface *root;
	float opacity;
};

static void rounded_set_content_opacity_cb(struct wlr_scene_buffer *buffer,
		int sx, int sy, void *data) {
	struct content_opacity_ctx *ctx = data;
	(void)sx;
	(void)sy;

	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL) {
		return;
	}
	if (rounded_surface_root(scene_surface->surface) != ctx->root) {
		return;
	}
	wlr_scene_buffer_set_opacity(buffer, ctx->opacity);
}

static void rounded_set_content_opacity(struct rounded_cache *rc,
		float opacity) {
	struct wlr_xdg_surface *base = rc->tl->xdg_toplevel->base;
	if (base == NULL || base->surface == NULL) {
		return;
	}
	struct content_opacity_ctx ctx = {
		.root = base->surface,
		.opacity = opacity,
	};
	wlr_scene_node_for_each_buffer(&rc->tl->scene_tree->node,
		rounded_set_content_opacity_cb, &ctx);
}

void rounded_cache_hide_content(struct toplevel *tl) {
	struct rounded_cache *rc = tl->rounded;
	if (rc == NULL || rc->failed || rc->node == NULL) {
		return;
	}
	/* Only hide the client content once there is a valid rounded FBO to
	 * show in its place.  Before the first successful publish (or while a
	 * re-render is in progress and the old FBO has been dropped) the raw
	 * content must stay visible: hiding it here would leave a fully
	 * transparent window. */
	if (rc->node->buffer == NULL) {
		return;
	}
	rounded_set_content_opacity(rc, 0.0f);
}

struct rounded_cache *rounded_cache_create(struct server *server,
		struct toplevel *tl) {
	struct rounded_cache *rc = calloc(1, sizeof(*rc));
	if (rc == NULL) {
		return NULL;
	}
	rc->server = server;
	rc->tl = tl;
	rc->dirty = true;

	if (!wlr_renderer_is_gles2(server->renderer)) {
		wlr_log(WLR_INFO, "rounded: renderer is not gles2, disabling rounded corners");
		rc->failed = true;
		return rc;
	}

	rc->node = wlr_scene_buffer_create(tl->scene_tree, NULL);
	if (rc->node == NULL) {
		rc->failed = true;
		return rc;
	}
	rc->node->point_accepts_input = rounded_no_input;
	wlr_scene_node_lower_to_bottom(&rc->node->node);

	return rc;
}

void rounded_cache_destroy(struct rounded_cache *rc) {
	if (rc == NULL) {
		return;
	}
	struct egl_context_state saved = {0};
	bool have_gl = !rc->failed && rc->gl_ready &&
		wlr_renderer_is_gles2(rc->server->renderer);
	if (have_gl && rounded_begin_gl(rc->server->renderer, &saved)) {
		rounded_gl_fini(rc);
		rounded_end_gl(&saved);
	}
	rounded_release_buffers(rc);
	free(rc);
}

void rounded_cache_dirty(struct toplevel *tl) {
	if (tl->rounded != NULL) {
		tl->rounded->dirty = true;
	}
}

/* Keep showing the last published rounded FBO when a re-render fails
 * transiently, instead of revealing the raw (unrounded) content.  Only when
 * no rounded buffer was ever published do we fall back to raw content -
 * otherwise the window would be fully transparent. */
static void rounded_fallback(struct rounded_cache *rc) {
	if (rc->node->buffer == NULL) {
		rounded_set_content_opacity(rc, 1.0f);
	} else {
		/* re-hide in case a surface commit reset the opacity to 1.0 */
		rounded_cache_hide_content(rc->tl);
	}
}

/* Render any dirty rounded caches.  Called from the output frame handler
 * before the scene is committed, so the scene always samples fresh FBOs. */
void rounded_render_all(struct server *server) {
	if (!wlr_renderer_is_gles2(server->renderer)) {
		return;
	}

	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		struct rounded_cache *rc = tl->rounded;
		if (rc == NULL || rc->failed) {
			continue;
		}
		struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
		if (base == NULL || base->surface == NULL ||
				!base->surface->mapped || tl->minimized) {
			continue;
		}

		struct wlr_box box;
		toplevel_box(tl, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}

		struct wlr_output *output = toplevel_output(server, tl);
		float scale = output != NULL ? output->scale : 1.0f;
		if (scale <= 0.0f) {
			scale = 1.0f;
		}

		if (!rc->dirty && rc->logical_width == box.width &&
				rc->logical_height == box.height && rc->scale == scale) {
			/* cache is fresh: wlroots' scene_surface re-applies opacity
			 * 1.0 on every surface commit (surface_reconfigure), so re-hide
			 * the content right before the scene renders.  A valid FBO is
			 * already published, so this never leaves the window
			 * transparent. */
			rounded_cache_hide_content(tl);
			continue;
		}

		if (!rc->gl_ready) {
			struct egl_context_state saved = {0};
			if (!rounded_begin_gl(server->renderer, &saved)) {
				rounded_set_content_opacity(rc, 1.0f);
				rc->failed = true;
				continue;
			}
			rc->gl_ready = rounded_gl_init(rc);
			rounded_end_gl(&saved);
			if (!rc->gl_ready) {
				wlr_log(WLR_ERROR, "rounded: GL init failed, disabling rounded corners");
				rounded_set_content_opacity(rc, 1.0f);
				rc->failed = true;
				continue;
			}
		}

		/* Clear dirty *before* compositing.  If the client commits again
		 * while we are rendering (between our scene sampling and the
		 * publish below), the commit handler sets dirty=true again and the
		 * next frame re-renders - the update is never silently dropped. */
		rc->dirty = false;

		if (!rounded_alloc_buffers(rc, box.width, box.height, scale)) {
			wlr_log(WLR_ERROR, "rounded: failed to allocate FBO buffers");
			rc->dirty = true;
			rounded_fallback(rc);
			continue;
		}

		if (!rounded_render_content(rc)) {
			/* the client content could not be composited into the FBO (for
			 * example a DMA-BUF/texture that isn't ready yet); keep the
			 * previous rounded FBO and retry on the next frame */
			wlr_log(WLR_DEBUG, "rounded: content render failed for app_id "
				"\"%s\" (%dx%d), keeping previous FBO",
				tl->app_id != NULL ? tl->app_id : "?",
				box.width, box.height);
			rc->dirty = true;
			rounded_fallback(rc);
			continue;
		}

		if (!rounded_render_mask(rc)) {
			wlr_log(WLR_ERROR, "rounded: offscreen render failed");
			rc->dirty = true;
			rounded_fallback(rc);
			continue;
		}

		/* publish the fresh result; NULL damage = whole buffer */
		wlr_scene_buffer_set_buffer_with_damage(rc->node, rc->rounded_buf,
			NULL);
		wlr_scene_buffer_set_dest_size(rc->node, box.width, box.height);
		/* NOTE: do not clear dirty here - it was cleared before the render,
		 * and any commit that arrived mid-render has already set it true
		 * again, so the next frame re-renders the newer content. */
		/* the FBO is now valid: hide the raw client content and show the
		 * rounded copy in its place */
		rounded_cache_hide_content(tl);
		wlr_log(WLR_DEBUG, "rounded: published FBO for app_id \"%s\" "
			"(%dx%d logical, %dx%d physical, scale %.2f)",
			tl->app_id != NULL ? tl->app_id : "?",
			box.width, box.height, rc->fbo_width, rc->fbo_height, rc->scale);
	}
}
