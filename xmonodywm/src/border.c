/*
 * border.c - rounded server-side border drawn around undecorated windows
 *
 * The border is rendered on the GPU: a GLES2 fragment shader evaluates the
 * rounded-rectangle SDF for every pixel of the border buffer, so resizing
 * costs no CPU rasterization at all.  The top edge of the border is the
 * same thin stroke as the other three sides but split into three equal
 * colored segments (left = minimize #87beaa, middle = maximize/restore
 * #f5a3a3, right = close #d55f6f); the other sides are a ring in the
 * focused/unfocused border color.  A maximized window follows the normal
 * window logic (focus-colored ring + three-segment top band);
 * CONFIG_MAXIMIZED_BORDER_ENABLED=0 drops the border entirely when
 * maximized.  A fullscreen window draws its band in a single unified
 * color CONFIG_FULLSCREEN_BORDER_COLOR instead of the three hints
 * (CONFIG_FULLSCREEN_GAP insets it from the screen edges so the ring
 * stays visible).  The focused window additionally gets a
 * Windows-11-style soft glow: CONFIG_BORDER_GLOW_SIZE px of the border
 * color fading out beyond the ring (drawn by the same shader); above the
 * top edge the glow follows the band's own colors (three segments for
 * normal windows, the single band color for dialogs/fullscreen), while
 * the sides and bottom keep the ring color.  Inactive windows never get
 * a glow.  We ride wlroots' own GLES2 context
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
	"uniform float u_overlap;\n"
	"uniform float u_glow;\n"
	"uniform float u_glow_alpha;\n"
	"uniform vec4 u_color;\n"
	"uniform vec4 u_color_min;\n"
	"uniform vec4 u_color_max;\n"
	"uniform vec4 u_color_close;\n"
	"uniform float u_strip;\n"
	"uniform float u_band_blend;\n"
	"/* rounded-rect SDF: negative inside, positive outside */\n"
	"float sdRoundBox(vec2 p, vec2 b, float r) {\n"
	"	vec2 q = abs(p) - b + r;\n"
	"	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"}\n"
	"void main() {\n"
	"	vec2 p = gl_FragCoord.xy - u_size * 0.5;\n"
	"	/* the buffer may be larger than the window box: the focused-window\n"
	"	 * glow extends beyond the ring, so the ring geometry is anchored to\n"
	"	 * the window box (u_size/2 - u_glow), not to the buffer edge */\n"
	"	vec2 half_out = u_size * 0.5 - vec2(u_glow);\n"
	"	/* the inner edge sits CONFIG_BORDER_OVERLAP px inside the window\n"
	"	 * box: the ring's anti-aliased transition then falls on the content\n"
	"	 * pixels, so no background is ever visible between ring and app */\n"
	"	vec2 half_in = half_out - vec2(u_width + u_overlap);\n"
	"	float r_in = max(u_radius - u_width - u_overlap, 0.0);\n"
	"	float d_out = sdRoundBox(p, half_out, u_radius);\n"
	"	float d_in = sdRoundBox(p, half_in, r_in);\n"
	"	float inside_out = 1.0 - smoothstep(0.0, 1.0, d_out);\n"
	"	float inside_in = 1.0 - smoothstep(0.0, 1.0, d_in);\n"
	"	float ring = inside_out - inside_in;\n"
	"	/* Windows-11-style soft halo outside the ring: peak opacity at the\n"
	"	 * ring's outer edge (d_out = 0), fading out over u_glow px.  Only\n"
	"	 * the focused window gets one (u_glow_alpha = 0 otherwise).  step()\n"
	"	 * gates it to the outside of the rounded rect (d_out > 0) so the\n"
	"	 * halo never tints the window content; the ring is opaque right at\n"
	"	 * the edge, so max() hands over smoothly.  The (1 - t^2)^2 falloff\n"
	"	 * is brightest at the ring and trails off progressively to exactly\n"
	"	 * zero at the buffer edge, for a natural ambient look.  Above the\n"
	"	 * top edge the halo is tinted by the band's segment colors (see\n"
	"	 * the top band section below). */\n"
	"	float t = clamp(d_out / max(u_glow, 0.001), 0.0, 1.0);\n"
	"	float fade = (1.0 - t * t) * (1.0 - t * t);\n"
	"	float halo = u_glow_alpha * step(0.0, d_out) * fade;\n"
	"	/* the top border band: the ring's own thickness (u_width +\n"
	"	 * u_overlap, same as the other three sides), spanning the\n"
	"	 * window width and split into three equal colored segments;\n"
	"	 * its rounded top corners match the ring's.  Popups (u_strip =\n"
	"	 * 0) get no band: only the plain ring. */\n"
	"	float strip_h = u_width + u_overlap;\n"
	"	float y_bottom = -half_out.y + strip_h;\n"
	"	float strip = u_strip * inside_out * (1.0 - smoothstep(-1.0, 1.0, p.y - y_bottom));\n"
	"	/* three equal segments over the window width: left = minimize,\n"
	"	 * middle = maximize/restore, right = close.  The seams blend over\n"
	"	 * u_band_blend px (CONFIG_BORDER_BAND_BLEND) so the color change is\n"
	"	 * a smooth gradient instead of a hard step; max() guards\n"
	"	 * smoothstep's undefined edge0==edge1 case when the blend is 0. */\n"
	"	float win_w = u_size.x - 2.0 * (u_width + u_glow);\n"
	"	float third = win_w / 6.0;\n"
	"	float blend = max(u_band_blend, 0.001);\n"
	"	vec3 seg = mix(u_color_min.rgb, u_color_max.rgb,\n"
	"		smoothstep(-third - blend, -third + blend, p.x));\n"
	"	seg = mix(seg, u_color_close.rgb,\n"
	"		smoothstep(third - blend, third + blend, p.x));\n"
	"	/* the glow above the top edge follows the band's own color: normal\n"
	"	 * windows get the three segment colors, dialogs and fullscreen a\n"
	"	 * single band color (their segment uniforms are set to it); the\n"
	"	 * sides/bottom halo stays the ring color.  Where halo > 0 the ring\n"
	"	 * and strip are already 0, so the mixes never fight. */\n"
	"	vec3 halo_tint = mix(u_color.rgb, seg,\n"
	"		clamp(halo / max(u_glow_alpha, 0.001), 0.0, 1.0));\n"
	"	vec3 rgb = mix(u_color.rgb, halo_tint, step(half_out.y, -p.y));\n"
	"	rgb = mix(rgb, seg, strip);\n"
	"	float alpha = max(max(ring, strip), halo);\n"
	"	/* output premultiplied color: rgb must be scaled by alpha, else\n"
	"	 * the 'transparent' pixels would contribute full color when\n"
	"	 * blended (wlroots composites with GL_ONE, GL_ONE_MINUS_SRC_ALPHA) */\n"
	"	gl_FragColor = vec4(rgb * alpha, u_color.a * alpha);\n"
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
 * for everything else.  Maximized windows follow the same focus-colored
 * logic as normal windows; only fullscreen windows use the dedicated
 * CONFIG_FULLSCREEN_BORDER_COLOR (which also tints their unified band). */
static uint32_t toplevel_border_color(struct toplevel *tl) {
	if (tl->fullscreen) {
		return CONFIG_FULLSCREEN_BORDER_COLOR;
	}
	if (tl->server->focused == tl) {
		return CONFIG_BORDER_COLOR;
	}
	return CONFIG_BORDER_COLOR_UNFOCUSED;
}

/* render the rounded border into a renderable buffer with a custom GLES2
 * pass on wlroots' EGL context; saves and restores all state it touches */
static bool border_render(struct server *server, struct wlr_buffer *buffer,
		int width, int height, uint32_t color, bool focused, bool dialog,
		bool show_strip, bool unified_strip, float ring_width) {
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
		glUniform1f(glGetUniformLocation(program, "u_width"), ring_width);
		glUniform1f(glGetUniformLocation(program, "u_overlap"), CONFIG_BORDER_OVERLAP);
		glUniform1f(glGetUniformLocation(program, "u_glow"),
			focused ? (float)CONFIG_BORDER_GLOW_SIZE : 0.0f);
		glUniform1f(glGetUniformLocation(program, "u_glow_alpha"),
			focused ? CONFIG_BORDER_GLOW_ALPHA : 0.0f);
		glUniform1f(glGetUniformLocation(program, "u_strip"),
			show_strip ? 1.0f : 0.0f);
		glUniform1f(glGetUniformLocation(program, "u_band_blend"),
			CONFIG_BORDER_BAND_BLEND);
		/* colors are 0xAARRGGBB */
		glUniform4f(glGetUniformLocation(program, "u_color"),
			(float)((color >> 16) & 0xFF) / 255.0f,
			(float)((color >> 8) & 0xFF) / 255.0f,
			(float)((color >> 0) & 0xFF) / 255.0f,
			(float)((color >> 24) & 0xFF) / 255.0f);
		/* a dialog's top border is one single close button: all three
		 * segments render in the close color (no minimize/maximize hint).
		 * A fullscreen window's band is a plain unified strip in the ring
		 * color (no minimize/maximize/close hints either).  Either way the
		 * shader's top glow reuses these segment colors, so the glow above
		 * the top always matches the band it sits over. */
		uint32_t seg_min = dialog ? CONFIG_BORDER_COLOR_CLOSE :
			(unified_strip ? color : CONFIG_BORDER_COLOR_MIN);
		uint32_t seg_max = dialog ? CONFIG_BORDER_COLOR_CLOSE :
			(unified_strip ? color : CONFIG_BORDER_COLOR_MAX);
		uint32_t seg_close = dialog ? CONFIG_BORDER_COLOR_CLOSE :
			(unified_strip ? color : CONFIG_BORDER_COLOR_CLOSE);
		glUniform4f(glGetUniformLocation(program, "u_color_min"),
			(float)((seg_min >> 16) & 0xFF) / 255.0f,
			(float)((seg_min >> 8) & 0xFF) / 255.0f,
			(float)((seg_min >> 0) & 0xFF) / 255.0f,
			(float)((seg_min >> 24) & 0xFF) / 255.0f);
		glUniform4f(glGetUniformLocation(program, "u_color_max"),
			(float)((seg_max >> 16) & 0xFF) / 255.0f,
			(float)((seg_max >> 8) & 0xFF) / 255.0f,
			(float)((seg_max >> 0) & 0xFF) / 255.0f,
			(float)((seg_max >> 24) & 0xFF) / 255.0f);
		glUniform4f(glGetUniformLocation(program, "u_color_close"),
			(float)((seg_close >> 16) & 0xFF) / 255.0f,
			(float)((seg_close >> 8) & 0xFF) / 255.0f,
			(float)((seg_close >> 0) & 0xFF) / 255.0f,
			(float)((seg_close >> 24) & 0xFF) / 255.0f);

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

/* allocate a buffer for border rendering (ARGB8888, linear) */
struct wlr_buffer *border_alloc_buffer(struct server *server,
		int width, int height) {
	return wlr_allocator_create_buffer(server->allocator, width, height,
		border_format());
}

/* a plain rounded ring (no title strip, no glow) for popups: the ring
 * outlines the rounded corners so they stay visible even when the popup
 * content and the background behind it share a color (an all-white menu
 * over a white window would otherwise look square). */
bool border_render_ring(struct server *server, struct wlr_buffer *buffer,
		int width, int height, uint32_t color, float ring_width) {
	return border_render(server, buffer, width, height, color, false, false,
		false, false, ring_width);
}

/* ------------------------------------------------------------------ */
/* decoration update                                                  */
/* ------------------------------------------------------------------ */

/* drop the border buffer and invalidate the render cache, so the next
 * call for a visible window re-renders instead of reusing a stale buffer.
 * The cache fields must be cleared together with the buffer: otherwise a
 * window minimized (or unmapped/maximized without a border) with a pending
 * commit drops the buffer, and when it is restored the cache still matches
 * the old dimensions/colors and the border is never drawn again. */
static void border_clear_buffer(struct toplevel *tl) {
	wlr_scene_buffer_set_buffer(tl->deco_border, NULL);
	tl->deco_w = 0;
	tl->deco_h = 0;
	tl->deco_color = 0;
	tl->deco_focused = false;
	tl->deco_dialog = false;
	tl->deco_unified = false;
}

/* (re)build the rounded border around the toplevel and position it */
void update_toplevel_decoration(struct toplevel *tl) {
	struct wlr_xdg_surface *base = tl->xdg_toplevel->base;
	if (tl->deco_border == NULL || base == NULL) {
		return;
	}
	bool maximized = tl->xdg_toplevel->current.maximized;
	bool unified = tl->fullscreen; /* only fullscreen draws the unified band */
	if (!base->surface->mapped || tl->minimized ||
			(maximized && !CONFIG_MAXIMIZED_BORDER_ENABLED)) {
		/* CONFIG_MAXIMIZED_BORDER_ENABLED=0 hides the border on maximized
		 * windows; fullscreen windows keep it (CONFIG_FULLSCREEN_GAP
		 * insets them from the screen edges so the ring stays visible) */
		border_clear_buffer(tl);
		return;
	}
	struct wlr_box box;
	toplevel_box(tl, &box);
	wlr_log(WLR_DEBUG, "border: %s box=%d,%d %dx%d",
		tl->app_id ? tl->app_id : "?", box.x, box.y, box.width,
		box.height);
	if (box.width <= 0 || box.height <= 0) {
		border_clear_buffer(tl);
		return;
	}
	/* the border buffer covers the window box plus the ring's outer extent
	 * on every side; inside it the shader draws the top band (the ring's
	 * own thickness, split into three equal colored segments) and the ring
	 * around the other three sides.  The ring's inner edge overlaps the
	 * window content by CONFIG_BORDER_OVERLAP px (so the anti-aliased edge
	 * blends onto the content and no background seam shows between the ring
	 * and the app); the invisible title-strip grab zone sits on the top
	 * CONFIG_TITLEBAR_HEIGHT px of the content.  The focused window's
	 * buffer grows by CONFIG_BORDER_GLOW_SIZE px on every side so the
	 * shader can draw the Windows-11-style soft halo outside the ring;
	 * unfocused windows stay compact and never get a glow. */
	bool focused = tl->server->focused == tl;
	int extent = CONFIG_BORDER_WIDTH +
		(focused ? CONFIG_BORDER_GLOW_SIZE : 0);
	int bw = box.width + 2 * extent;
	int bh = box.height + 2 * extent;
	wlr_log(WLR_DEBUG, "border: %s box=%d,%d %dx%d buf=%dx%d geo=%d,%d %dx%d",
		tl->app_id ? tl->app_id : "?", box.x, box.y, box.width, box.height,
		bw, bh, base->geometry.x, base->geometry.y, base->geometry.width,
		base->geometry.height);
	uint32_t color = toplevel_border_color(tl);
	bool dialog = toplevel_is_dialog(tl) || toplevel_is_fixed_size(tl);
	if (tl->deco_w != bw || tl->deco_h != bh || tl->deco_color != color ||
			tl->deco_focused != focused || tl->deco_dialog != dialog ||
			tl->deco_unified != unified) {
		struct wlr_buffer *buffer = border_alloc_buffer(tl->server, bw, bh);
		if (buffer == NULL) {
			wlr_log(WLR_ERROR, "border: failed to allocate %dx%d buffer",
				bw, bh);
			return;
		}
		if (!border_render(tl->server, buffer, bw, bh, color, focused,
				dialog, true, unified, CONFIG_BORDER_WIDTH)) {
			wlr_buffer_drop(buffer);
			return;
		}
		/* the scene locks the buffer; drop our reference */
		wlr_scene_buffer_set_buffer(tl->deco_border, buffer);
		wlr_buffer_drop(buffer);
		tl->deco_w = bw;
		tl->deco_h = bh;
		tl->deco_color = color;
		tl->deco_focused = focused;
		tl->deco_dialog = dialog;
		tl->deco_unified = unified;
	}
	wlr_scene_node_set_position(&tl->deco_border->node, box.x - extent,
		box.y - extent);
}
