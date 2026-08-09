/*
 * border.c - rounded server-side border drawn around undecorated windows
 *
 * The border is rendered on the GPU: a GLES2 fragment shader evaluates the
 * rounded-rectangle SDF for every pixel of the border buffer, so resizing
 * costs no CPU rasterization at all.  We ride wlroots' own GLES2 context
 * (wlr_gles2_renderer_get_egl) and render into a wlr_allocator buffer
 * through its FBO (wlr_gles2_renderer_get_buffer_fbo); the scene graph
 * then composites that texture like any other buffer.
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
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

/* the border is purely visual: never let it intercept pointer input */
bool border_buffer_no_input(struct wlr_scene_buffer *buffer,
		double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

/* ------------------------------------------------------------------ */
/* shader                                                             */
/* ------------------------------------------------------------------ */

static const char *border_vertex_src =
	"attribute vec2 a_pos;\n"
	"void main() {\n"
	"	gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *border_fragment_src =
	"precision highp float;\n"
	"uniform vec2 u_size;\n"
	"uniform float u_radius;\n"
	"uniform float u_width;\n"
	"uniform vec4 u_color;\n"
	"/* rounded-rect SDF: negative inside, positive outside */\n"
	"float sdRoundBox(vec2 p, vec2 b, float r) {\n"
	"	vec2 q = abs(p) - b + r;\n"
	"	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"void main() {\n"
	"	vec2 p = gl_FragCoord.xy - u_size * 0.5;\n"
	"	vec2 half_out = u_size * 0.5;\n"
	"	vec2 half_in = half_out - vec2(u_width);\n"
	"	float r_in = max(u_radius - u_width, 0.0);\n"
	"	float d_out = sdRoundBox(p, half_out, u_radius);\n"
	"	float d_in = sdRoundBox(p, half_in, r_in);\n"
	"	float inside_out = 1.0 - smoothstep(0.0, 1.0, d_out);\n"
	"	float inside_in = 1.0 - smoothstep(0.0, 1.0, d_in);\n"
	"	float alpha = inside_out - inside_in;\n"
	"	/* output premultiplied color: rgb must be scaled by alpha, else\n"
	"	 * the 'transparent' pixels would contribute full color when\n"
	"	 * blended (wlroots composites with GL_ONE, GL_ONE_MINUS_SRC_ALPHA) */\n"
	"	gl_FragColor = vec4(u_color.rgb * alpha, u_color.a * alpha);\n"
	"}\n";

/* compile the border shader once; returns 0 on failure */
static GLuint border_shader_program(void) {
	static GLuint program;
	static bool tried;
	if (tried) {
		return program;
	}
	tried = true;

	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &border_vertex_src, NULL);
	glCompileShader(vs);
	GLint ok = 0;
	glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(vs, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "border: vertex shader failed: %s", log);
		return 0;
	}

	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &border_fragment_src, NULL);
	glCompileShader(fs);
	ok = 0;
	glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(fs, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "border: fragment shader failed: %s", log);
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
		wlr_log(WLR_ERROR, "border: program link failed: %s", log);
		return 0;
	}
	glDeleteShader(vs);
	glDeleteShader(fs);
	wlr_log(WLR_INFO, "border: GLES2 rounded-border shader ready");
	return program;
}

/* the format used for border render targets */
static const struct wlr_drm_format *border_format(void) {
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

/* the border color for a toplevel: brighter for the focused window, muted
 * for everything else */
static uint32_t toplevel_border_color(struct toplevel *tl) {
	if (tl->server->focused == tl) {
		return CONFIG_BORDER_COLOR;
	}
	return CONFIG_BORDER_COLOR_UNFOCUSED;
}

/* render the rounded border into a renderable buffer with a custom GLES2
 * pass on wlroots' EGL context; saves and restores all state it touches */
static bool border_render(struct server *server, struct wlr_buffer *buffer,
		int width, int height, uint32_t color) {
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
		wlr_log(WLR_ERROR, "border: eglMakeCurrent failed");
		return false;
	}

	/* save the GL state wlroots' renderer relies on */
	GLint prev_program, prev_fbo, prev_array_buf, prev_viewport[4];
	GLboolean prev_blend, prev_scissor;
	GLint prev_blend_src, prev_blend_dst, prev_scissor_box[4];
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	prev_blend = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB, &prev_blend_src);
	glGetIntegerv(GL_BLEND_DST_RGB, &prev_blend_dst);
	prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, prev_scissor_box);

	bool ok = false;
	GLuint program = border_shader_program();
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);
	if (program != 0 && fbo != 0) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, width, height);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(program);
		glUniform2f(glGetUniformLocation(program, "u_size"),
			(float)width, (float)height);
		glUniform1f(glGetUniformLocation(program, "u_radius"), CONFIG_BORDER_RADIUS);
		glUniform1f(glGetUniformLocation(program, "u_width"), CONFIG_BORDER_WIDTH);
		/* color is 0xAARRGGBB */
		glUniform4f(glGetUniformLocation(program, "u_color"),
			(float)((color >> 16) & 0xFF) / 255.0f,
			(float)((color >> 8) & 0xFF) / 255.0f,
			(float)((color >> 0) & 0xFF) / 255.0f,
			(float)((color >> 24) & 0xFF) / 255.0f);

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

/* ------------------------------------------------------------------ */
/* decoration update                                                  */
/* ------------------------------------------------------------------ */

/* (re)build the rounded border around the toplevel and position it */
void update_toplevel_decoration(struct toplevel *tl) {
	/* the blur backdrop follows the same geometry rules as the border */
	blur_toplevel_update(tl);

	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (tl->deco_border == NULL || base == NULL) {
		return;
	}
	if (!base->surface->mapped || tl->minimized || tl->fullscreen) {
		/* fullscreen windows don't show the rounded border either */
		wlr_scene_buffer_set_buffer(tl->deco_border, NULL);
		return;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	if (box.width <= 0 || box.height <= 0) {
		wlr_scene_buffer_set_buffer(tl->deco_border, NULL);
		return;
	}
	int bw = box.width + 2 * CONFIG_BORDER_WIDTH;
	int bh = box.height + 2 * CONFIG_BORDER_WIDTH;
	uint32_t color = toplevel_border_color(tl);
	if (tl->deco_w != bw || tl->deco_h != bh || tl->deco_color != color) {
		struct wlr_buffer *buffer = wlr_allocator_create_buffer(
			tl->server->allocator, bw, bh, border_format());
		if (buffer == NULL) {
			wlr_log(WLR_ERROR, "border: failed to allocate %dx%d buffer",
				bw, bh);
			return;
		}
		if (!border_render(tl->server, buffer, bw, bh, color)) {
			wlr_buffer_drop(buffer);
			return;
		}
		/* the scene locks the buffer; drop our reference */
		wlr_scene_buffer_set_buffer(tl->deco_border, buffer);
		wlr_buffer_drop(buffer);
		tl->deco_w = bw;
		tl->deco_h = bh;
		tl->deco_color = color;
	}
	wlr_scene_node_set_position(&tl->deco_border->node, box.x - CONFIG_BORDER_WIDTH,
		box.y - CONFIG_BORDER_WIDTH);
}
