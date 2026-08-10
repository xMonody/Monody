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
 * animations and damage updates stay current.  It uses the same raw-GL-on-
 * wlroots'-EGL-context approach as border.c and blur.c.  The client
 * buffer's scale is handled by sampling the texture with normalized
 * coordinates; 90-degree transformed buffers are not supported (the
 * compositor never receives them in practice).
 */

#include "server.h"

#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stdlib.h>

#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
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
	"/* rounded-rect SDF: negative inside, positive outside */\n"
	"float sdRoundBox(vec2 p, vec2 b, float r) {\n"
	"	vec2 q = abs(p) - b + r;\n"
	"	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"void main() {\n"
	"	vec2 p = gl_FragCoord.xy - u_size * 0.5;\n"
	"	float d = sdRoundBox(p, u_size * 0.5, u_radius);\n"
	"	float mask = 1.0 - smoothstep(0.0, 1.0, d);\n"
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

/* render the surface's current buffer into `dst` with a rounded-corner
 * alpha mask of `radius`; radius 0 renders the content unclipped */
bool content_mask_render(struct server *server, struct wlr_surface *surface,
		struct wlr_buffer *dst, int width, int height, float radius) {
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
	GLuint program = mask_shader_program();
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, dst);
	if (program != 0 && fbo != 0) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, width, height);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "u_tex"), 0);
		glUniform2f(glGetUniformLocation(program, "u_size"),
			(float)width, (float)height);
		glUniform1f(glGetUniformLocation(program, "u_radius"), radius);

		glBindTexture(GL_TEXTURE_2D, attribs.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		GLint loc = glGetAttribLocation(program, "a_pos");
		static const float verts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
		GLuint vbo = 0;
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
		glEnableVertexAttribArray((GLuint)loc);
		glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray((GLuint)loc);
		glDeleteBuffers(1, &vbo);
		ok = true;
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
