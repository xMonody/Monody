/*
 * blur.c - GLSL gaussian background blur for transparent windows
 *
 * Terminal emulators render a semi-transparent background by committing
 * ARGB8888 buffers; what shows through is whatever the compositor draws
 * below the window.  To give such windows a frosted-glass backdrop we:
 *
 *   1. detect that the window's committed buffer really contains
 *      semi-transparent pixels (GPU downscale + 1 readback),
 *   2. render the scene *without* the window into a snapshot buffer
 *      (wlr_scene_output_build_state with a private swapchain, the same
 *      trick wlr-randr test configurations use),
 *   3. run a separable gaussian blur on the GPU - two GLES2 passes
 *      (horizontal + vertical) on a downscaled intermediate, raw GL on
 *      wlroots' EGL context exactly like border.c does for the rounded
 *      border,
 *   4. drop the blurred texture into the window's decoration tree behind
 *      the content; the window's transparent pixels then blend over it.
 *
 * The blur is refreshed whenever the output needs a new frame, so moving
 * windows, wallpaper and the window itself always show a current backdrop.
 */

#include "server.h"

#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define BLUR_DOWNSCALE 3    /* blur texture is 1/3 the window size */
#define BLUR_RADIUS 5       /* gaussian taps on each side of the kernel */
#define BLUR_TAPS (2 * BLUR_RADIUS + 1)
#define BLUR_SIGMA (BLUR_RADIUS / 3.0f)
#define ALPHA_SAMPLE 64     /* readback grid used to detect transparency */
#define ALPHA_OPAQUE 250    /* alpha >= this counts as fully opaque */

/* ------------------------------------------------------------------ */
/* shaders                                                            */
/* ------------------------------------------------------------------ */

static const char *blur_vertex_src =
	"attribute vec2 a_pos;\n"
	"void main() {\n"
	"	gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/* horizontal pass: samples the full-size snapshot, downscales it by
 * u_stride buffer pixels per output texel and applies the gaussian kernel
 * along x; the source rectangle inside the snapshot is u_src_origin. */
static char *blur_h_fragment_src(void) {
	char *src = malloc(2048);
	if (src == NULL) {
		return NULL;
	}
	snprintf(src, 2048,
		"precision highp float;\n"
		"uniform sampler2D u_tex;\n"
		"uniform vec2 u_tex_size;\n"
		"uniform vec2 u_src_origin;\n"
		"uniform float u_stride;\n"
		"uniform float u_weights[%d];\n"
		"void main() {\n"
		"	vec2 uv = (u_src_origin + vec2(0.5, 0.5)\n"
		"		+ gl_FragCoord.xy * u_stride) / u_tex_size;\n"
		"	vec2 du = vec2(u_stride / u_tex_size.x, 0.0);\n"
		"	vec4 acc = vec4(0.0);\n"
		"	for (int i = 0; i < %d; i++) {\n"
		"		acc += u_weights[i] * texture2D(u_tex,\n"
		"			uv + du * (float(i) - %d.0));\n"
		"	}\n"
		"	gl_FragColor = vec4(acc.rgb, 1.0);\n"
		"}\n",
		BLUR_TAPS, BLUR_TAPS, BLUR_RADIUS);
	return src;
}

/* vertical pass: blurs the horizontal-pass output along y. */
static char *blur_v_fragment_src(void) {
	char *src = malloc(2048);
	if (src == NULL) {
		return NULL;
	}
	snprintf(src, 2048,
		"precision highp float;\n"
		"uniform sampler2D u_tex;\n"
		"uniform vec2 u_tex_size;\n"
		"uniform float u_weights[%d];\n"
		"void main() {\n"
		"	vec2 uv = (gl_FragCoord.xy + vec2(0.5)) / u_tex_size;\n"
		"	vec2 dv = vec2(0.0, 1.0 / u_tex_size.y);\n"
		"	vec4 acc = vec4(0.0);\n"
		"	for (int i = 0; i < %d; i++) {\n"
		"		acc += u_weights[i] * texture2D(u_tex,\n"
		"			uv + dv * (float(i) - %d.0));\n"
		"	}\n"
		"	gl_FragColor = vec4(acc.rgb, 1.0);\n"
		"}\n",
		BLUR_TAPS, BLUR_TAPS, BLUR_RADIUS);
	return src;
}

/* plain blit used to squash a client buffer down for the transparency
 * readback (u_size is the *output* size) */
static const char *blit_fragment_src =
	"precision highp float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_size;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(u_tex, gl_FragCoord.xy / u_size);\n"
	"}\n";

struct blur_shaders {
	GLuint blur_h, blur_v, blit;
};

static GLuint compile_shader(GLenum type, const char *src, const char *name) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "blur: %s shader failed: %s", name, log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

/* compile the three blur programs once; all-zero on failure */
static struct blur_shaders blur_shaders(void) {
	static struct blur_shaders shaders;
	static bool tried;
	if (tried) {
		return shaders;
	}
	tried = true;

	GLuint vs = compile_shader(GL_VERTEX_SHADER, blur_vertex_src, "vertex");
	if (vs == 0) {
		return shaders;
	}

	char *h_src = blur_h_fragment_src();
	char *v_src = blur_v_fragment_src();
	GLuint h_fs = h_src != NULL
		? compile_shader(GL_FRAGMENT_SHADER, h_src, "blur-h fragment") : 0;
	GLuint v_fs = v_src != NULL
		? compile_shader(GL_FRAGMENT_SHADER, v_src, "blur-v fragment") : 0;
	GLuint blit_fs = compile_shader(GL_FRAGMENT_SHADER, blit_fragment_src,
		"blit fragment");
	free(h_src);
	free(v_src);
	if (h_fs == 0 || v_fs == 0 || blit_fs == 0) {
		goto fail;
	}

	shaders.blur_h = glCreateProgram();
	glAttachShader(shaders.blur_h, vs);
	glAttachShader(shaders.blur_h, h_fs);
	shaders.blur_v = glCreateProgram();
	glAttachShader(shaders.blur_v, vs);
	glAttachShader(shaders.blur_v, v_fs);
	shaders.blit = glCreateProgram();
	glAttachShader(shaders.blit, vs);
	glAttachShader(shaders.blit, blit_fs);

	GLuint programs[] = { shaders.blur_h, shaders.blur_v, shaders.blit };
	for (int i = 0; i < 3; i++) {
		glLinkProgram(programs[i]);
		GLint ok = 0;
		glGetProgramiv(programs[i], GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[512];
			glGetProgramInfoLog(programs[i], sizeof(log), NULL, log);
			wlr_log(WLR_ERROR, "blur: program %d link failed: %s", i, log);
			goto fail;
		}
	}
	wlr_log(WLR_INFO, "blur: GLES2 gaussian blur shaders ready");
	goto out;

fail:
	glDeleteShader(vs);
	glDeleteShader(h_fs);
	glDeleteShader(v_fs);
	glDeleteShader(blit_fs);
	glDeleteProgram(shaders.blur_h);
	glDeleteProgram(shaders.blur_v);
	glDeleteProgram(shaders.blit);
	shaders = (struct blur_shaders){0};

out:
	return shaders;
}

/* gaussian kernel weights, normalized to sum to one */
static void blur_weights(float out[BLUR_TAPS]) {
	float sum = 0.0f;
	for (int i = 0; i < BLUR_TAPS; i++) {
		float d = (float)i - BLUR_RADIUS;
		out[i] = expf(-(d * d) / (2.0f * BLUR_SIGMA * BLUR_SIGMA));
		sum += out[i];
	}
	for (int i = 0; i < BLUR_TAPS; i++) {
		out[i] /= sum;
	}
}

/* ------------------------------------------------------------------ */
/* GLES2 state management (same discipline as border.c)                */
/* ------------------------------------------------------------------ */

struct gl_state {
	GLint program, fbo, array_buf, viewport[4];
	GLint blend_src, blend_dst, blend_eq, scissor_box[4];
	GLint tex_unit, bound_tex;
	GLboolean blend, scissor;
	EGLDisplay dpy;             /* wlroots' EGL display (for teardown) */
	EGLDisplay prev_dpy;
	EGLContext prev_ctx;
	EGLSurface prev_draw, prev_read;
};

static bool gl_begin(struct server *server, struct gl_state *st) {
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	if (egl == NULL) {
		return false;
	}
	EGLDisplay dpy = wlr_egl_get_display(egl);
	EGLContext ctx = wlr_egl_get_context(egl);
	if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) {
		return false;
	}
	st->prev_dpy = eglGetCurrentDisplay();
	st->prev_ctx = eglGetCurrentContext();
	st->prev_draw = eglGetCurrentSurface(EGL_DRAW);
	st->prev_read = eglGetCurrentSurface(EGL_READ);
	st->dpy = dpy;
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		wlr_log(WLR_ERROR, "blur: eglMakeCurrent failed");
		return false;
	}
	glGetIntegerv(GL_CURRENT_PROGRAM, &st->program);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &st->fbo);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &st->array_buf);
	glGetIntegerv(GL_VIEWPORT, st->viewport);
	st->blend = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB, &st->blend_src);
	glGetIntegerv(GL_BLEND_DST_RGB, &st->blend_dst);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &st->blend_eq);
	st->scissor = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, st->scissor_box);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &st->tex_unit);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &st->bound_tex);
	return true;
}

static void gl_end(struct gl_state *st) {
	glActiveTexture((GLenum)st->tex_unit);
	glBindTexture(GL_TEXTURE_2D, (GLuint)st->bound_tex);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)st->fbo);
	glUseProgram((GLuint)st->program);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)st->array_buf);
	glViewport(st->viewport[0], st->viewport[1], st->viewport[2],
		st->viewport[3]);
	if (st->blend) {
		glEnable(GL_BLEND);
	} else {
		glDisable(GL_BLEND);
	}
	glBlendFunc((GLenum)st->blend_src, (GLenum)st->blend_dst);
	glBlendEquation((GLenum)st->blend_eq);
	if (st->scissor) {
		glEnable(GL_SCISSOR_TEST);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
	glScissor(st->scissor_box[0], st->scissor_box[1], st->scissor_box[2],
		st->scissor_box[3]);
	if (st->prev_dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(st->prev_dpy, st->prev_draw, st->prev_read,
			st->prev_ctx);
	} else {
		eglMakeCurrent(st->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
			EGL_NO_CONTEXT);
	}
}

/* draw a full-viewport triangle strip through a_pos */
static void draw_quad(GLint attrib) {
	static const float verts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint vbo = 0;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
	glEnableVertexAttribArray((GLuint)attrib);
	glVertexAttribPointer((GLuint)attrib, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray((GLuint)attrib);
	glDeleteBuffers(1, &vbo);
}

/* the format used for blur render targets */
static const struct wlr_drm_format *blur_format(void) {
	static struct wlr_drm_format_set set;
	static bool init;
	if (!init) {
		init = true;
		wlr_drm_format_set_add(&set, DRM_FORMAT_ARGB8888,
			DRM_FORMAT_MOD_LINEAR);
		wlr_drm_format_set_add(&set, DRM_FORMAT_ARGB8888,
			DRM_FORMAT_MOD_INVALID);
	}
	return wlr_drm_format_set_get(&set, DRM_FORMAT_ARGB8888);
}

/* ------------------------------------------------------------------ */
/* transparency detection                                              */
/* ------------------------------------------------------------------ */

/* Render `buf` into a tiny ARGB buffer and scan the readback for any
 * pixel with alpha < ALPHA_OPAQUE: the window really is transparent.
 * Returns true when no verdict could be reached (conservative). */
static bool blur_buffer_has_transparency(struct server *server,
		struct wlr_surface *surface) {
	struct wlr_buffer *buf = &surface->buffer->base;
	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buf);
	if (tex == NULL) {
		return true; /* cannot tell: assume transparent */
	}
	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(tex, &attribs);
	if (attribs.target != GL_TEXTURE_2D) {
		wlr_texture_destroy(tex);
		return true;
	}

	const int w = ALPHA_SAMPLE, h = ALPHA_SAMPLE;
	struct wlr_buffer *small = wlr_allocator_create_buffer(server->allocator,
		w, h, blur_format());
	if (small == NULL) {
		wlr_texture_destroy(tex);
		return true;
	}

	struct gl_state st;
	uint8_t *pixels = malloc((size_t)w * h * 4);
	bool transparent = true;
	if (!gl_begin(server, &st)) {
		goto out;
	}

	/* sampling the client buffer directly in a custom GL pass: honor the
	 * explicit-sync acquire point exactly like the rounded-mask pass */
	if (!mask_wait_syncobj_acquire(server, surface)) {
		wlr_log(WLR_ERROR, "blur: failed to wait on acquire point");
		gl_end(&st);
		goto out;
	}

	struct blur_shaders shaders = blur_shaders();
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, small);
	if (shaders.blit != 0 && fbo != 0 && pixels != NULL) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, w, h);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(shaders.blit);
		glUniform1i(glGetUniformLocation(shaders.blit, "u_tex"), 0);
		glUniform2f(glGetUniformLocation(shaders.blit, "u_size"),
			(float)w, (float)h);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, attribs.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		draw_quad(glGetAttribLocation(shaders.blit, "a_pos"));
		glBindTexture(GL_TEXTURE_2D, 0);

		glReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels);
		transparent = false;
		for (int i = 0; i < w * h && !transparent; i++) {
			if (pixels[i * 4 + 3] < ALPHA_OPAQUE) {
				transparent = true;
			}
		}
	}

	gl_end(&st);

out:
	free(pixels);
	wlr_buffer_drop(small);
	wlr_texture_destroy(tex);
	return transparent;
}

/* ------------------------------------------------------------------ */
/* blur passes                                                         */
/* ------------------------------------------------------------------ */

/* Run the separable blur: horizontal + downscale from the snapshot
 * texture into `tmp`, then vertical into `out`.  (rx, ry, rw, rh) is the
 * window's rectangle inside the snapshot in buffer pixels. */
static bool blur_render(struct server *server, GLuint src_tex,
		int snap_w, int snap_h, int rx, int ry, float scale,
		struct wlr_buffer *tmp, struct wlr_buffer *out) {
	struct blur_shaders shaders = blur_shaders();
	if (shaders.blur_h == 0 || shaders.blur_v == 0) {
		return false;
	}

	int bw = out->width, bh = out->height;
	float stride = BLUR_DOWNSCALE * scale;
	float weights[BLUR_TAPS];
	blur_weights(weights);

	struct wlr_texture *tmp_tex = wlr_texture_from_buffer(server->renderer, tmp);
	if (tmp_tex == NULL) {
		return false;
	}
	struct wlr_gles2_texture_attribs tmp_attribs;
	wlr_gles2_texture_get_attribs(tmp_tex, &tmp_attribs);
	if (tmp_attribs.target != GL_TEXTURE_2D) {
		wlr_texture_destroy(tmp_tex);
		return false;
	}

	struct gl_state st;
	if (!gl_begin(server, &st)) {
		wlr_texture_destroy(tmp_tex);
		return false;
	}

	GLuint fbo_tmp = wlr_gles2_renderer_get_buffer_fbo(server->renderer, tmp);
	GLuint fbo_out = wlr_gles2_renderer_get_buffer_fbo(server->renderer, out);
	if (fbo_tmp == 0 || fbo_out == 0) {
		gl_end(&st);
		wlr_texture_destroy(tmp_tex);
		return false;
	}

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);

	/* pass 1: horizontal gaussian + downscale */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_tmp);
	glViewport(0, 0, bw, bh);
	glUseProgram(shaders.blur_h);
	glUniform1i(glGetUniformLocation(shaders.blur_h, "u_tex"), 0);
	glUniform2f(glGetUniformLocation(shaders.blur_h, "u_tex_size"),
		(float)snap_w, (float)snap_h);
	glUniform2f(glGetUniformLocation(shaders.blur_h, "u_src_origin"),
		(float)rx, (float)ry);
	glUniform1f(glGetUniformLocation(shaders.blur_h, "u_stride"), stride);
	glUniform1fv(glGetUniformLocation(shaders.blur_h, "u_weights"),
		BLUR_TAPS, weights);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	draw_quad(glGetAttribLocation(shaders.blur_h, "a_pos"));
	glBindTexture(GL_TEXTURE_2D, 0);

	/* pass 2: vertical gaussian */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_out);
	glViewport(0, 0, bw, bh);
	glUseProgram(shaders.blur_v);
	glUniform1i(glGetUniformLocation(shaders.blur_v, "u_tex"), 0);
	glUniform2f(glGetUniformLocation(shaders.blur_v, "u_tex_size"),
		(float)bw, (float)bh);
	glUniform1fv(glGetUniformLocation(shaders.blur_v, "u_weights"),
		BLUR_TAPS, weights);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tmp_attribs.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	draw_quad(glGetAttribLocation(shaders.blur_v, "a_pos"));
	glBindTexture(GL_TEXTURE_2D, 0);

	gl_end(&st);
	wlr_texture_destroy(tmp_tex);
	return true;
}

/* ------------------------------------------------------------------ */
/* per-window refresh                                                  */
/* ------------------------------------------------------------------ */

/* Snapshot the scene behind `tl` (with its tree hidden) into a swapchain
 * buffer the size of the output, blur it down to a small texture and
 * install it as the window's deco_blur scene buffer. */
static void blur_refresh_toplevel(struct server *server, struct toplevel *tl,
		struct wlr_scene_output *scene_output) {
	if (tl->deco_blur == NULL || tl->deco_tree == NULL || !tl->blur_enabled) {
		return;
	}
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || !base->surface->mapped || tl->minimized) {
		return;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	struct wlr_output *output = toplevel_output(server, tl);
	if (output != scene_output->output) {
		return;
	}

	float scale = output->scale;
	int rw = (int)llroundf(box.width * scale);
	int rh = (int)llroundf(box.height * scale);
	if (rw <= 0 || rh <= 0) {
		return;
	}
	int rx = (int)llroundf((box.x - scene_output->x) * scale);
	int ry = (int)llroundf((box.y - scene_output->y) * scale);

	/* keep a snapshot swapchain matching the output resolution */
	if (server->blur_swapchain == NULL ||
			server->blur_swapchain_w != output->width ||
			server->blur_swapchain_h != output->height) {
		wlr_swapchain_destroy(server->blur_swapchain);
		server->blur_swapchain = NULL;
		server->blur_swapchain =
			wlr_swapchain_create(server->allocator, output->width,
				output->height, blur_format());
		if (server->blur_swapchain == NULL) {
			wlr_log(WLR_ERROR, "blur: failed to create snapshot swapchain");
			return;
		}
		server->blur_swapchain_w = output->width;
		server->blur_swapchain_h = output->height;
	}

	/* render the scene without this window into the snapshot */
	wlr_scene_node_set_enabled(&tl->deco_tree->node, false);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	struct wlr_scene_output_state_options opts = {
		.swapchain = server->blur_swapchain,
	};
	bool ok = wlr_scene_output_build_state(scene_output, &state, &opts);
	struct wlr_buffer *snap = NULL;
	if (ok && state.buffer != NULL) {
		snap = wlr_buffer_lock(state.buffer);
	}
	wlr_output_state_finish(&state);
	wlr_scene_node_set_enabled(&tl->deco_tree->node, true);
	if (snap == NULL) {
		wlr_log(WLR_ERROR, "blur: snapshot render failed");
		return;
	}

	struct wlr_texture *snap_tex = wlr_texture_from_buffer(server->renderer, snap);
	struct wlr_gles2_texture_attribs snap_attribs;
	if (snap_tex == NULL) {
		wlr_log(WLR_ERROR, "blur: failed to import snapshot texture");
		wlr_buffer_unlock(snap);
		return;
	}
	wlr_gles2_texture_get_attribs(snap_tex, &snap_attribs);
	if (snap_attribs.target != GL_TEXTURE_2D) {
		wlr_texture_destroy(snap_tex);
		wlr_buffer_unlock(snap);
		return;
	}

	int bw = box.width / BLUR_DOWNSCALE;
	if (bw < 1) {
		bw = 1;
	}
	int bh = box.height / BLUR_DOWNSCALE;
	if (bh < 1) {
		bh = 1;
	}
	struct wlr_buffer *tmp = wlr_allocator_create_buffer(server->allocator,
		bw, bh, blur_format());
	struct wlr_buffer *blur = wlr_allocator_create_buffer(server->allocator,
		bw, bh, blur_format());
	if (tmp == NULL || blur == NULL) {
		wlr_log(WLR_ERROR, "blur: failed to allocate %dx%d buffer", bw, bh);
		goto out_buffers;
	}

	if (!blur_render(server, snap_attribs.tex, output->width, output->height,
			rx, ry, scale, tmp, blur)) {
		goto out_buffers;
	}

	/* the scene locks the buffer; drop our reference */
	wlr_scene_buffer_set_buffer(tl->deco_blur, blur);
	wlr_scene_buffer_set_dest_size(tl->deco_blur, box.width, box.height);
	wlr_scene_node_set_position(&tl->deco_blur->node, box.x, box.y);
	wlr_log(WLR_DEBUG, "blur: %dx%d backdrop for '%s' (snapshot %d,%d)",
		bw, bh, tl->app_id != NULL ? tl->app_id : "?", rw, rh);

	wlr_buffer_drop(blur);
	blur = NULL;

out_buffers:
	if (tmp != NULL) {
		wlr_buffer_drop(tmp);
	}
	if (blur != NULL) {
		wlr_buffer_drop(blur);
	}
	wlr_texture_destroy(snap_tex);
	wlr_buffer_unlock(snap);
}

/* ------------------------------------------------------------------ */
/* module interface                                                    */
/* ------------------------------------------------------------------ */

/* create the blur scene buffer behind the window content */
void blur_toplevel_init(struct toplevel *tl) {
	if (!CONFIG_BLUR_ENABLED || tl->deco_tree == NULL) {
		return;
	}
	tl->deco_blur = wlr_scene_buffer_create(tl->deco_tree, NULL);
	if (tl->deco_blur != NULL) {
		tl->deco_blur->point_accepts_input = border_buffer_no_input;
	}
}

/* re-examine the window's committed buffer for transparency */
void blur_toplevel_commit(struct toplevel *tl) {
	if (!CONFIG_BLUR_ENABLED) {
		return;
	}
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	bool enabled = false;
	if (base != NULL && base->surface->buffer != NULL &&
			!wlr_buffer_is_opaque(&base->surface->buffer->base)) {
		enabled = blur_buffer_has_transparency(tl->server,
			base->surface);
	}
	if (enabled == tl->blur_enabled) {
		return;
	}
	tl->blur_enabled = enabled;
	if (!enabled && tl->deco_blur != NULL && tl->deco_blur->buffer != NULL) {
		wlr_scene_buffer_set_buffer(tl->deco_blur, NULL);
	}
}

/* keep the blur node glued to the window geometry (called from
 * update_toplevel_decoration); hides it while the window is unmapped */
void blur_toplevel_update(struct toplevel *tl) {
	if (tl->deco_blur == NULL) {
		return;
	}
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (base == NULL || !base->surface->mapped) {
		if (tl->deco_blur->buffer != NULL) {
			wlr_scene_buffer_set_buffer(tl->deco_blur, NULL);
		}
		return;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	wlr_scene_node_set_position(&tl->deco_blur->node, box.x, box.y);
	wlr_scene_buffer_set_dest_size(tl->deco_blur, box.width, box.height);
}

/* refresh every visible transparent window on this output; called from the
 * output frame handler right before the scene commit */
void blur_refresh_output(struct server *server,
		struct wlr_scene_output *scene_output) {
	if (!CONFIG_BLUR_ENABLED) {
		return;
	}
	if (!wlr_scene_output_needs_frame(scene_output)) {
		return;
	}
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		blur_refresh_toplevel(server, tl, scene_output);
	}
}

void blur_finish(struct server *server) {
	wlr_swapchain_destroy(server->blur_swapchain);
	server->blur_swapchain = NULL;
	server->blur_swapchain_w = 0;
	server->blur_swapchain_h = 0;
}
