/*
 * mask.c - rounded-corner clipping of window content
 *
 * wlr_scene cannot clip a surface to a rounded rectangle (only rectangular
 * clips), so the compositor re-renders each toplevel's client buffer
 * through a GLES2 pass that multiplies it by a rounded-rectangle SDF alpha
 * mask.  The result is a buffer with transparent corners which the scene
 * composites like any other buffer; the rounded border ring from border.c
 * then outlines it.  The border ring's inner edge overlaps the content by
 * CONFIG_BORDER_OVERLAP px (its anti-aliased edge blends onto the content,
 * so no background seam shows between ring and app); the content arc has
 * radius CONFIG_BORDER_RADIUS - CONFIG_BORDER_WIDTH, matching the ring's
 * inner arc.
 *
 * The pass re-runs on every surface commit that carries a buffer, so
 * animations and damage updates stay current; commits that leave the
 * content untouched (no new buffer, no damage, geometry unchanged) are
 * skipped entirely, and the margin verdict (glReadPixels) is cached per
 * (buffer, geometry).  It uses the same raw-GL-on-
 * wlroots'-EGL-context approach as border.c.  The client
 * buffer's scale is handled by sampling the texture with normalized
 * coordinates; 90-degree transformed buffers are not supported (the
 * compositor never receives them in practice).
 *
 * Windows whose surface is larger than the xdg window geometry (GTK CSD
 * drop shadows, e.g. Firefox) get an extra pass: the window-geometry
 * corners are cut to the border ring's arc, so the content's own corners
 * are rounded too (the surface-level rounding alone only cuts the outer
 * shadow corners).  The shadow margin itself is preserved.
 */

#include "server.h"

#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* shader                                                             */
/* ------------------------------------------------------------------ */

static const char *mask_vertex_src =
	"attribute vec2 a_pos;\n"
	"void main() {\n"
	"	gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *mask_fragment_src =
	"precision highp float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_size;\n"
	"uniform float u_radius;\n"
	"uniform vec4 u_geom;\n"
	"uniform float u_geom_clip;\n"
	"/* rounded-rect SDF: negative inside, positive outside */\n"
	"float sdRoundBox(vec2 p, vec2 b, float r) {\n"
	"	vec2 q = abs(p) - b + r;\n"
	"	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"void main() {\n"
	"	vec2 p = gl_FragCoord.xy - u_size * 0.5;\n"
	"	float d = sdRoundBox(p, u_size * 0.5, u_radius);\n"
	"	float mask = 1.0 - smoothstep(0.0, 1.0, d);\n"
	"	/* windows whose surface is larger than the window geometry (GTK\n"
	"	 * CSD drop shadows, e.g. Firefox): the surface-level rounding above\n"
	"	 * only cuts the outer (shadow) corners, so the content's own\n"
	"	 * corners would stay sharp.  Clip the window-geometry corners to\n"
	"	 * the same arc as the border ring: the corner triangles of the\n"
	"	 * geometry box are cut away, the drop-shadow margin outside the\n"
	"	 * box is kept (max(geom_round, outside)) . */\n"
	"	if (u_geom_clip > 0.5) {\n"
	"		vec2 gc = u_geom.xy + u_geom.zw * 0.5;\n"
	"		float dg = sdRoundBox(gl_FragCoord.xy - gc, u_geom.zw * 0.5, u_radius);\n"
	"		float geom_round = 1.0 - smoothstep(0.0, 1.0, dg);\n"
	"		float db = sdRoundBox(gl_FragCoord.xy - gc, u_geom.zw * 0.5, 0.0);\n"
	"		float in_box = step(0.0, -db);\n"
	"		mask *= max(geom_round, 1.0 - in_box);\n"
	"	}\n"
	"	/* texture sampled at normalized coords; output premultiplied like\n"
	"	 * the rest of the scene (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) */\n"
	"	gl_FragColor = texture2D(u_tex, gl_FragCoord.xy / u_size) * mask;\n"
	"}\n";

static GLuint mask_shader_program(void) {
	static GLuint program;
	static bool tried;
	if (tried) {
		return program;
	}
	tried = true;

	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &mask_vertex_src, NULL);
	glCompileShader(vs);
	GLint ok = 0;
	glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(vs, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "mask: vertex shader failed: %s", log);
		return 0;
	}

	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &mask_fragment_src, NULL);
	glCompileShader(fs);
	ok = 0;
	glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(fs, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "mask: fragment shader failed: %s", log);
		glDeleteShader(vs);
		return 0;
	}

	program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "mask: program link failed: %s", log);
		return 0;
	}
	glDeleteShader(vs);
	glDeleteShader(fs);
	wlr_log(WLR_INFO, "mask: GLES2 rounded-content shader ready");
	return program;
}

struct mask_gl {
	GLuint program;
	GLint u_tex, u_size, u_radius, u_geom, u_geom_clip;
	GLint a_pos;
	GLuint vbo;
};

/* the mask program, its uniform/attrib locations and the shared
 * fullscreen-quad VBO, initialized once (all-zero on failure).  The
 * locations and the VBO are cached because every mask render re-uses
 * them: glGetUniformLocation is a driver-side string lookup and
 * glGenBuffers/glBufferData/glDeleteBuffers churn a driver object on
 * every draw (once per commit, once per popup). */
static struct mask_gl mask_gl(void) {
	static struct mask_gl gl;
	static bool tried;
	if (tried) {
		return gl;
	}
	tried = true;
	gl.program = mask_shader_program();
	if (gl.program != 0) {
		gl.u_tex = glGetUniformLocation(gl.program, "u_tex");
		gl.u_size = glGetUniformLocation(gl.program, "u_size");
		gl.u_radius = glGetUniformLocation(gl.program, "u_radius");
		gl.u_geom = glGetUniformLocation(gl.program, "u_geom");
		gl.u_geom_clip = glGetUniformLocation(gl.program, "u_geom_clip");
		gl.a_pos = glGetAttribLocation(gl.program, "a_pos");
		static const float verts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
		glGenBuffers(1, &gl.vbo);
		glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	}
	return gl;
}

/* the format used for masked content render targets */
static const struct wlr_drm_format *mask_format(void) {
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

/* allocate a buffer to hold the masked content at the given logical size */
struct wlr_buffer *content_mask_buffer(struct server *server,
		int width, int height) {
	return wlr_allocator_create_buffer(server->allocator, width, height,
		mask_format());
}

/* decide whether the surface really extends beyond the xdg window geometry
 * with opaque pixels (then the border must wrap the surface) or whether the
 * extra area is a transparent drop shadow (then it must wrap the geometry).
 * Called right after the rounded mask has been rendered, with the mask
 * buffer's FBO still bound: the mask covers the whole surface, so its alpha
 * is the surface's alpha outside the (corner-only) rounded clip.
 *
 * Samples a few pixels just inside each surface edge where the surface
 * extends past the geometry; a drop shadow is (nearly) transparent there,
 * real content is opaque.  GL y=0 is the surface's top row, which matches
 * the mask pass (gl_FragCoord.xy / u_size).  Returns true when at least one
 * sampled pixel has alpha > 0.25. */
static bool mask_margin_has_content(const struct wlr_box *geom,
		int width, int height) {
	if (geom == NULL || wlr_box_empty(geom)) {
		return false; /* geometry unspecified: treat the surface as the window */
	}
	int gx = geom->x, gy = geom->y;
	int gw = geom->width, gh = geom->height;
	if (gx <= 0 && gy <= 0 && gx + gw >= width && gy + gh >= height) {
		return false; /* no margin: the geometry covers the whole surface */
	}
	/* distance from the surface edge to sample: 2px inside, or the middle
	 * of a thin band (never the row next to the geometry, where a drop
	 * shadow is darkest) */
	int top_t = gy;                 /* top band thickness */
	int bot_t = height - gy - gh;   /* bottom band thickness */
	int lef_t = gx;                 /* left band thickness */
	int rig_t = width - gx - gw;    /* right band thickness */
	int top_d = top_t >= 4 ? 2 : top_t / 2;
	int bot_d = bot_t >= 4 ? 2 : bot_t / 2;
	int lef_d = lef_t >= 4 ? 2 : lef_t / 2;
	int rig_d = rig_t >= 4 ? 2 : rig_t / 2;
	/* one sample line per side, spread along the middle of the edge */
	struct { int x, y; } samples[16];
	int n = 0;
	if (top_t > 0) {               /* top band: rows [0, gy) */
		for (int i = 1; i < 5 && n < 16; i++) {
			samples[n].x = width * i / 5;
			samples[n].y = top_d;
			n++;
		}
	}
	if (bot_t > 0) {               /* bottom band: rows [gy+gh, height) */
		for (int i = 1; i < 5 && n < 16; i++) {
			samples[n].x = width * i / 5;
			samples[n].y = height - 1 - bot_d;
			n++;
		}
	}
	if (lef_t > 0) {               /* left band: columns [0, gx) */
		for (int i = 1; i < 5 && n < 16; i++) {
			samples[n].x = lef_d;
			samples[n].y = height * i / 5;
			n++;
		}
	}
	if (rig_t > 0) {               /* right band: columns [gx+gw, width) */
		for (int i = 1; i < 5 && n < 16; i++) {
			samples[n].x = width - 1 - rig_d;
			samples[n].y = height * i / 5;
			n++;
		}
	}
	for (int i = 0; i < n; i++) {
		unsigned char px[4];
		glReadPixels(samples[i].x, samples[i].y, 1, 1, GL_RGBA,
			GL_UNSIGNED_BYTE, px);
		if (px[3] > 64) {             /* alpha > 0.25: real content */
			return true;
		}
	}
	return false;
}

/* Explicit-sync glue for the custom mask pass.  wlr_scene_surface would
 * wait on the client's acquire timeline before sampling and signal the
 * release point through wlr_buffer.events.release.  The rounded-corner mask
 * pass below bypasses wlr_scene_surface and samples the client buffer with
 * raw GL, so it must perform the equivalent GPU-side wait itself.
 *
 * The DRM syncobj <-> sync_file conversion is done by wlroots
 * (wlr_drm_syncobj_timeline_export_sync_file); the sync_file is then handed
 * to the EGL implementation as a native fence.  This is the same mechanism
 * wlroots' GLES2 renderer uses internally, exposed here only because the
 * mask pass is custom GL. */
static PFNEGLCREATESYNCKHRPROC mask_egl_create_sync;
static PFNEGLDESTROYSYNCKHRPROC mask_egl_destroy_sync;
static PFNEGLWAITSYNCKHRPROC mask_egl_wait_sync;
static bool mask_egl_sync_procs_loaded;

static void mask_load_egl_sync_procs(void) {
	if (mask_egl_sync_procs_loaded) {
		return;
	}
	mask_egl_create_sync = (PFNEGLCREATESYNCKHRPROC)
		eglGetProcAddress("eglCreateSyncKHR");
	mask_egl_destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)
		eglGetProcAddress("eglDestroySyncKHR");
	mask_egl_wait_sync = (PFNEGLWAITSYNCKHRPROC)
		eglGetProcAddress("eglWaitSyncKHR");
	mask_egl_sync_procs_loaded = true;
}

bool mask_wait_syncobj_acquire(struct server *server,
		struct wlr_surface *surface) {
	struct wlr_linux_drm_syncobj_surface_v1_state *state =
		wlr_linux_drm_syncobj_v1_get_surface_state(surface);
	if (state == NULL || state->acquire_timeline == NULL) {
		return true;
	}

	if (!server->renderer->features.timeline) {
		wlr_log(WLR_ERROR, "mask: explicit sync surface but renderer "
			"timeline support is unavailable");
		return false;
	}

	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	if (egl == NULL) {
		return false;
	}
	EGLDisplay dpy = wlr_egl_get_display(egl);
	if (dpy == EGL_NO_DISPLAY) {
		return false;
	}

	mask_load_egl_sync_procs();
	if (mask_egl_create_sync == NULL || mask_egl_destroy_sync == NULL ||
			mask_egl_wait_sync == NULL) {
		wlr_log(WLR_ERROR, "mask: EGL native fence sync is unavailable");
		return false;
	}

	int sync_file_fd = wlr_drm_syncobj_timeline_export_sync_file(
		state->acquire_timeline, state->acquire_point);
	if (sync_file_fd < 0) {
		wlr_log(WLR_ERROR, "mask: failed to export acquire point as "
			"sync_file");
		return false;
	}

	int dup_fd = fcntl(sync_file_fd, F_DUPFD_CLOEXEC, 0);
	if (dup_fd < 0) {
		wlr_log_errno(WLR_ERROR, "mask: failed to dup acquire sync_file");
		close(sync_file_fd);
		return false;
	}

	EGLint attribs[3] = {
		EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
		dup_fd,
		EGL_NONE,
	};
	EGLSyncKHR sync = mask_egl_create_sync(dpy,
		EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
	if (sync == EGL_NO_SYNC_KHR) {
		wlr_log(WLR_ERROR, "mask: eglCreateSyncKHR failed for acquire "
			"point");
		close(dup_fd);
		close(sync_file_fd);
		return false;
	}
	close(sync_file_fd);

	bool ok = mask_egl_wait_sync(dpy, sync, 0) == EGL_TRUE;
	mask_egl_destroy_sync(dpy, sync);
	if (!ok) {
		wlr_log(WLR_ERROR, "mask: eglWaitSyncKHR failed for acquire point");
		return false;
	}
	return true;
}

/* render the surface's current buffer into `dst` with a rounded-corner
 * alpha mask of `radius`; radius 0 renders the content unclipped.  When
 * `geom` is non-NULL, `*margin_opaque` reports whether the surface has
 * opaque pixels outside that geometry box (see mask_margin_has_content).
 * `geom_clip` additionally cuts the window-geometry corners (sharp CSD
 * content corners under a drop-shadow margin); it implies a non-NULL
 * `geom`. */
bool content_mask_render(struct server *server, struct wlr_surface *surface,
		struct wlr_buffer *dst, int width, int height, float radius,
		const struct wlr_box *geom, bool geom_clip, bool *margin_opaque) {
	if (margin_opaque != NULL) {
		*margin_opaque = false;
	}
	if (geom == NULL) {
		geom_clip = false;
	}
	struct wlr_texture *tex = wlr_surface_get_texture(surface);
	if (tex == NULL) {
		return false;
	}
	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(tex, &attribs);
	if (attribs.target != GL_TEXTURE_2D) {
		return false;
	}

	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	if (egl == NULL) {
		return false;
	}
	EGLDisplay dpy = wlr_egl_get_display(egl);
	EGLContext ctx = wlr_egl_get_context(egl);
	if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) {
		return false;
	}

	/* save the current EGL context (usually none between frames) */
	EGLDisplay prev_dpy = eglGetCurrentDisplay();
	EGLContext prev_ctx = eglGetCurrentContext();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		wlr_log(WLR_ERROR, "mask: eglMakeCurrent failed");
		return false;
	}

	/* wait on the client's acquire timeline before sampling its buffer */
	if (!mask_wait_syncobj_acquire(server, surface)) {
		eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);
		return false;
	}

	/* save the GL state wlroots' renderer relies on */
	GLint prev_program, prev_fbo, prev_array_buf, prev_viewport[4];
	GLboolean prev_blend, prev_scissor;
	GLint prev_blend_src, prev_blend_dst, prev_scissor_box[4];
	GLint prev_tex_unit, prev_bound_tex;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	prev_blend = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB, &prev_blend_src);
	glGetIntegerv(GL_BLEND_DST_RGB, &prev_blend_dst);
	prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, prev_scissor_box);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_tex_unit);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bound_tex);

	bool ok = false;
	struct mask_gl gl = mask_gl();
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, dst);
	if (gl.program != 0 && gl.vbo != 0 && fbo != 0) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, width, height);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(gl.program);
		glUniform1i(gl.u_tex, 0);
		glUniform2f(gl.u_size, (float)width, (float)height);
		glUniform1f(gl.u_radius, radius);
		glUniform4f(gl.u_geom,
			geom != NULL ? (float)geom->x : 0.0f,
			geom != NULL ? (float)geom->y : 0.0f,
			geom != NULL ? (float)geom->width : (float)width,
			geom != NULL ? (float)geom->height : (float)height);
		glUniform1f(gl.u_geom_clip, geom_clip ? 1.0f : 0.0f);

		glBindTexture(GL_TEXTURE_2D, attribs.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
		glEnableVertexAttribArray((GLuint)gl.a_pos);
		glVertexAttribPointer((GLuint)gl.a_pos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray((GLuint)gl.a_pos);
		ok = true;
		/* the FBO is still bound: probe the margin band for opaque content
		 * (see mask_margin_has_content).  The caller passes NULL to skip
		 * the probe when the (buffer, geometry) verdict is cached. */
		if (geom != NULL && margin_opaque != NULL) {
			*margin_opaque = mask_margin_has_content(geom, width, height);
		}
	}

	/* restore GL state */
	glActiveTexture((GLenum)prev_tex_unit);
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound_tex);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
	glUseProgram((GLuint)prev_program);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_array_buf);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
		prev_viewport[3]);
	if (prev_blend) {
		glEnable(GL_BLEND);
	} else {
		glDisable(GL_BLEND);
	}
	glBlendFunc((GLenum)prev_blend_src, (GLenum)prev_blend_dst);
	if (prev_scissor) {
		glEnable(GL_SCISSOR_TEST);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
	glScissor(prev_scissor_box[0], prev_scissor_box[1], prev_scissor_box[2],
		prev_scissor_box[3]);

	/* restore the EGL context */
	if (prev_dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);
	} else {
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}
	return ok;
}
