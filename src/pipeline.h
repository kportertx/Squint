// The GL half of squint. It has no picom dependency so test/pipeline_test.c can run it in
// a standalone context and compare the output with a CPU copy of the same math.
//
// Data flow, per frame that changed the screen:
//   capture  copy the damaged rectangles of the back buffer into S (the undimmed screen)
//   need     per pixel, d = 1 - gain(L): how much the pixel's own luminance L must be dimmed
//   dilate   spread d outward with a linear falloff, D(x) = max over y of d(y) - slope*|x-y|
//            (L1 distance), never across a monitor edge
//   smooth   optional per pixel: instant attack, exponential release with time constant tau
//   final    redraw every monitor pixel from S, scaled by 1 - D (and gamma), or copied
//            unchanged where D is zero
//
// slope = (1 - min_gain) / falloff_px, so the strongest possible dimming fades to nothing
// over falloff_px pixels and weaker dimming over proportionally less.
//
// All coordinates the caller passes are X coordinates (origin top left, y down). GL
// framebuffers and textures are y up, so every rectangle is flipped when it reaches GL.

#pragma once

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#define PL_MAXMON 8

struct pl_rect {
	int x1, y1, x2, y2;  // half open
};

struct pl_mon {
	int x, y, w, h;
};

struct pl {
	// Set by the caller before pl_init. The plugin uses 0 and GL_BACK.
	GLuint target_fbo;
	GLenum read_buf, draw_buf;

	bool ready;
	GLuint vao, fbo, prog[4];
	GLuint samp_nearest;

	int W, H;
	GLuint s_tex;           // undimmed screen, RGBA8
	GLuint tex_a, tex_b;    // dilation ping-pong
	GLuint dnow;            // dilated dimming amount D for the latest captured content
	GLuint st[2];           // smoothed D, used only when tau > 0
	int cur;
	bool s_valid, st_valid, dirty;

	struct pl_mon mon[PL_MAXMON];
	int nmon;

	// Parameters the current dnow was computed with.
	double used_target, used_min_gain, used_knee;
	int used_falloff;

	bool have_input, have_smooth_time, was_enabled;
	double last_input, last_smooth;
};

enum { PL_NEED, PL_DILATE, PL_SMOOTH, PL_FINAL };

static void (*pl_logger)(const char *msg);

static inline void pl_say(const char *fmt, ...) {
	char buf[1100];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (pl_logger) {
		pl_logger(buf);
	} else {
		fprintf(stderr, "squint: %s\n", buf);
	}
}

static const char *pl_vs =
    "#version 330\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

#define PL_COMMON                                                                          \
	"#version 330\n"                                                                   \
	"const vec3 W709 = vec3(0.2126, 0.7152, 0.0722);\n"                                \
	"vec3 to_lin(vec3 c) {\n"                                                          \
	"  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n" \
	"}\n"                                                                              \
	"vec3 to_enc(vec3 c) {\n"                                                          \
	"  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,\n"               \
	"             step(0.0031308, c));\n"                                              \
	"}\n"

// d = 1 - gain for the pixel's own luminance. The gain is target / (target + excess)
// with a quadratic knee around the target, floored at min_gain.
static const char *pl_fs_need =
    PL_COMMON
    "uniform sampler2D S;\n"
    "uniform float target, min_gain, knee;\n"
    "out float o;\n"
    "void main() {\n"
    "  vec3 c = texelFetch(S, ivec2(gl_FragCoord.xy), 0).rgb;\n"
    "  float L = dot(to_lin(c), W709);\n"
    "  float over = L - target;\n"
    "  float h = 0.5 * knee;\n"
    "  float excess = over <= -h ? 0.0 : (over >= h ? over : (over + h) * (over + h) / (2.0 * knee));\n"
    "  float g = clamp(target / (target + excess), min_gain, 1.0);\n"
    "  o = 1.0 - g;\n"
    "}\n";

// One step of exact cone dilation along one axis. After steps of 1, 2, 4, ... 2^(K-1) the
// value covers every offset up to 2^K - 1: each doubling shifts the previous result by
// the step and charges slope * step, and a shifted offset is either exact or an
// underestimate that another tap already covers exactly. Taps outside the monitor box
// (box is x0, y0, x1, y1 in GL coordinates, end exclusive) are ignored.
static const char *pl_fs_dilate =
    PL_COMMON
    "uniform sampler2D src;\n"
    "uniform ivec2 dir;\n"
    "uniform int step_px;\n"
    "uniform float slope;\n"
    "uniform ivec4 box;\n"
    "out float o;\n"
    "float tap(ivec2 p) {\n"
    "  bool in_box = p.x >= box.x && p.y >= box.y && p.x < box.z && p.y < box.w;\n"
    "  return in_box ? texelFetch(src, p, 0).r : -1.0;\n"
    "}\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float pen = slope * float(step_px);\n"
    "  float v = texelFetch(src, p, 0).r;\n"
    "  v = max(v, tap(p - dir * step_px) - pen);\n"
    "  v = max(v, tap(p + dir * step_px) - pen);\n"
    "  o = v;\n"
    "}\n";

static const char *pl_fs_smooth =
    PL_COMMON
    "uniform sampler2D now_t, prev_t;\n"
    "uniform float k, init;\n"
    "out float o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float n = texelFetch(now_t, p, 0).r;\n"
    "  float prev = texelFetch(prev_t, p, 0).r;\n"
    "  o = init > 0.5 ? n : max(n, mix(prev, n, k));\n"
    "}\n";

static const char *pl_fs_final =
    PL_COMMON
    "uniform sampler2D S, D;\n"
    "uniform float enabled, min_gain, gamma_dim;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 s = texelFetch(S, p, 0);\n"
    "  float d = texelFetch(D, p, 0).r;\n"
    "  // Pixels the falloff does not reach are copied bit for bit.\n"
    "  if (enabled < 0.5 || d < 0.002) { color = s; return; }\n"
    "  float a = (1.0 - min_gain) > 1e-4 ? clamp(d / (1.0 - min_gain), 0.0, 1.0) : 0.0;\n"
    "  vec3 e = pow(s.rgb, vec3(1.0 + (gamma_dim - 1.0) * a));\n"
    "  color = vec4(to_enc(to_lin(e) * (1.0 - d)), s.a);\n"
    "}\n";

static inline GLuint pl_compile(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char buf[1024];
		glGetShaderInfoLog(s, sizeof buf, NULL, buf);
		pl_say("shader compile failed: %s", buf);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static inline GLuint pl_program(GLuint vs, const char *fs_src) {
	GLuint fs = pl_compile(GL_FRAGMENT_SHADER, fs_src);
	if (!fs) {
		return 0;
	}
	GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glLinkProgram(p);
	glDeleteShader(fs);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char buf[1024];
		glGetProgramInfoLog(p, sizeof buf, NULL, buf);
		pl_say("program link failed: %s", buf);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

#define PL_U(p, name) glGetUniformLocation((p), (name))

// ---- GL state shared with the host ------------------------------------------------------

struct pl_glsave {
	GLint draw_fbo, read_fbo, prog, vao, active_tex, vp[4], sc[4];
	GLint tex[3], samp[3];
	GLint read_buf, draw_buf;
	GLint bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
	GLboolean blend, scissor, stencil, depth, cull, fb_srgb, mask[4];
};

static inline void pl_gl_save(struct pl_glsave *g) {
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &g->draw_fbo);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &g->read_fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &g->prog);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &g->vao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &g->active_tex);
	for (int u = 0; u < 3; u++) {
		glActiveTexture(GL_TEXTURE0 + (GLenum)u);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &g->tex[u]);
		glGetIntegerv(GL_SAMPLER_BINDING, &g->samp[u]);
	}
	glGetIntegerv(GL_VIEWPORT, g->vp);
	glGetIntegerv(GL_SCISSOR_BOX, g->sc);
	glGetIntegerv(GL_READ_BUFFER, &g->read_buf);
	glGetIntegerv(GL_DRAW_BUFFER0, &g->draw_buf);
	glGetIntegerv(GL_BLEND_SRC_RGB, &g->bsrc_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &g->bdst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &g->bsrc_a);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &g->bdst_a);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &g->beq_rgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &g->beq_a);
	g->blend = glIsEnabled(GL_BLEND);
	g->scissor = glIsEnabled(GL_SCISSOR_TEST);
	g->stencil = glIsEnabled(GL_STENCIL_TEST);
	g->depth = glIsEnabled(GL_DEPTH_TEST);
	g->cull = glIsEnabled(GL_CULL_FACE);
	g->fb_srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glGetBooleanv(GL_COLOR_WRITEMASK, g->mask);
}

static inline void pl_gl_restore(const struct pl_glsave *g) {
	glColorMask(g->mask[0], g->mask[1], g->mask[2], g->mask[3]);
	if (g->fb_srgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
	if (g->cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	if (g->depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (g->stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
	if (g->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if (g->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	glScissor(g->sc[0], g->sc[1], g->sc[2], g->sc[3]);
	glBlendEquationSeparate((GLenum)g->beq_rgb, (GLenum)g->beq_a);
	glBlendFuncSeparate((GLenum)g->bsrc_rgb, (GLenum)g->bdst_rgb, (GLenum)g->bsrc_a,
	                    (GLenum)g->bdst_a);
	glViewport(g->vp[0], g->vp[1], g->vp[2], g->vp[3]);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)g->read_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)g->draw_fbo);
	glReadBuffer((GLenum)g->read_buf);
	glDrawBuffer((GLenum)g->draw_buf);
	for (int u = 2; u >= 0; u--) {
		glActiveTexture(GL_TEXTURE0 + (GLenum)u);
		glBindSampler((GLuint)u, (GLuint)g->samp[u]);
		glBindTexture(GL_TEXTURE_2D, (GLuint)g->tex[u]);
	}
	glActiveTexture((GLenum)g->active_tex);
	glBindVertexArray((GLuint)g->vao);
	glUseProgram((GLuint)g->prog);
}

// ---- resources --------------------------------------------------------------------------

static inline bool pl_init(struct pl *p) {
	GLuint vs = pl_compile(GL_VERTEX_SHADER, pl_vs);
	if (!vs) {
		return false;
	}
	const char *fs[4] = {pl_fs_need, pl_fs_dilate, pl_fs_smooth, pl_fs_final};
	bool ok = true;
	for (int i = 0; i < 4; i++) {
		p->prog[i] = pl_program(vs, fs[i]);
		ok = ok && p->prog[i];
	}
	glDeleteShader(vs);
	if (!ok) {
		return false;
	}
	glGenVertexArrays(1, &p->vao);
	glGenFramebuffers(1, &p->fbo);
	glGenSamplers(1, &p->samp_nearest);
	glSamplerParameteri(p->samp_nearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(p->samp_nearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(p->samp_nearest, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(p->samp_nearest, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	p->ready = true;
	return true;
}

static inline void pl_free_screen(struct pl *p) {
	if (p->s_tex) {
		glDeleteTextures(6, (GLuint[]){p->s_tex, p->tex_a, p->tex_b, p->dnow, p->st[0], p->st[1]});
		p->s_tex = 0;
	}
}

static inline void pl_destroy(struct pl *p) {
	if (!p->ready) {
		return;
	}
	pl_free_screen(p);
	for (int i = 0; i < 4; i++) {
		glDeleteProgram(p->prog[i]);
	}
	glDeleteVertexArrays(1, &p->vao);
	glDeleteFramebuffers(1, &p->fbo);
	glDeleteSamplers(1, &p->samp_nearest);
	p->ready = p->s_valid = false;
	p->W = p->H = 0;
}

static inline GLuint pl_tex(GLenum fmt, GLenum base, int w, int h) {
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, base,
	             fmt == GL_RGBA8 ? GL_UNSIGNED_BYTE : GL_FLOAT, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	return t;
}

// (Re)allocates the screen-sized textures when the root size changes. S is empty
// afterwards, so the caller must capture the whole back buffer before the next pl_render.
static inline bool pl_ensure_screen(struct pl *p, int W, int H) {
	if (p->W == W && p->H == H && p->s_tex) {
		return false;
	}
	pl_free_screen(p);
	p->W = W;
	p->H = H;
	p->s_tex = pl_tex(GL_RGBA8, GL_RGBA, W, H);
	p->tex_a = pl_tex(GL_R16F, GL_RED, W, H);
	p->tex_b = pl_tex(GL_R16F, GL_RED, W, H);
	p->dnow = pl_tex(GL_R16F, GL_RED, W, H);
	p->st[0] = pl_tex(GL_R16F, GL_RED, W, H);
	p->st[1] = pl_tex(GL_R16F, GL_RED, W, H);
	p->s_valid = p->st_valid = false;
	p->dirty = true;
	return true;
}

// Sets the monitor rectangles. A change invalidates the dimming amounts and the smoothing.
static inline bool pl_set_monitors(struct pl *p, const struct pl_mon *mons, int nmon) {
	if (nmon > PL_MAXMON) {
		nmon = PL_MAXMON;
	}
	if (p->nmon == nmon && !memcmp(p->mon, mons, (size_t)nmon * sizeof *mons)) {
		return false;
	}
	memcpy(p->mon, mons, (size_t)nmon * sizeof *mons);
	p->nmon = nmon;
	p->dirty = true;
	p->st_valid = false;
	return true;
}

// ---- per-frame work ---------------------------------------------------------------------

static inline void pl_touch(struct pl *p, double now) {
	p->last_input = now;
	p->have_input = true;
}

// True while a smoothing transition may still be running. There is no readback, so this is
// a time bound: five time constants after the last change, plus a little.
static inline bool pl_active(const struct pl *p, const struct sq_cfg *cfg, double now) {
	return p->s_valid && p->have_input &&
	       now - p->last_input < 5.0 * cfg->v[SQ_TAU_MS] / 1000.0 + 0.1;
}

// Copies rectangles (X coordinates) of the target framebuffer into S. The rectangles must
// be exactly what the host just wrote: the rest of the framebuffer holds this plugin's own
// dimmed output and must never reach S. An empty list means nothing changed and does
// nothing. full copies the whole framebuffer, which is only correct when the host has just
// redrawn all of it. An invalid S is always captured in full, which assumes the host's
// first frame after (re)initialization covers the screen. The caller must have the host's
// GL state saved.
static inline void pl_capture(struct pl *p, const struct pl_rect *r, int n, bool full,
                              double now) {
	struct pl_rect whole = {0, 0, p->W, p->H};
	if (!p->s_valid) {
		full = true;
	}
	if (full) {
		r = &whole;
		n = 1;
	}
	if (n == 0) {
		return;
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, p->target_fbo);
	glReadBuffer(p->read_buf);
	glBindTexture(GL_TEXTURE_2D, p->s_tex);
	for (int i = 0; i < n; i++) {
		int x1 = r[i].x1 < 0 ? 0 : r[i].x1, x2 = r[i].x2 > p->W ? p->W : r[i].x2;
		int y1 = r[i].y1 < 0 ? 0 : r[i].y1, y2 = r[i].y2 > p->H ? p->H : r[i].y2;
		if (x2 <= x1 || y2 <= y1) {
			continue;
		}
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x1, p->H - y2, x1, p->H - y2, x2 - x1, y2 - y1);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	p->s_valid = true;
	p->dirty = true;
	pl_touch(p, now);
}

static inline void pl_bind(int unit, GLuint tex, GLuint samp) {
	glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
	glBindTexture(GL_TEXTURE_2D, tex);
	glBindSampler((GLuint)unit, samp);
}

static inline void pl_fbo_attach(struct pl *p, GLuint tex) {
	glBindFramebuffer(GL_FRAMEBUFFER, p->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
}

static inline void pl_draw(void) {
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

static inline void pl_scissor_mon(const struct pl *p, const struct pl_mon *m) {
	glScissor(m->x, p->H - (m->y + m->h), m->w, m->h);
}

static inline int pl_dilate_levels(int falloff_px) {
	int k = 0;
	while (((1 << k) - 1) < falloff_px) {
		k++;
	}
	return k;
}

static inline void pl_base_state(void) {
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// need + dilate: leaves D in dnow.
static inline void pl_dilate(struct pl *p, const struct sq_cfg *cfg) {
	int R = (int)cfg->v[SQ_FALLOFF_PX];
	int K = R > 0 ? pl_dilate_levels(R) : 0;
	float min_gain = (float)cfg->v[SQ_MIN_GAIN];
	float slope = R > 0 ? (1.0f - min_gain) / (float)R : 0.0f;

	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, p->W, p->H);

	GLuint pr = p->prog[PL_NEED];
	pl_fbo_attach(p, K == 0 ? p->dnow : p->tex_a);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "S"), 0);
	glUniform1f(PL_U(pr, "target"), (float)cfg->v[SQ_TARGET]);
	glUniform1f(PL_U(pr, "min_gain"), min_gain);
	glUniform1f(PL_U(pr, "knee"), (float)cfg->v[SQ_KNEE]);
	pl_bind(0, p->s_tex, p->samp_nearest);
	pl_draw();

	pr = p->prog[PL_DILATE];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "src"), 0);
	glUniform1f(PL_U(pr, "slope"), slope);
	glEnable(GL_SCISSOR_TEST);
	GLuint src = p->tex_a;
	int total = 2 * K, pass = 0;
	for (int axis = 0; axis < 2; axis++) {
		for (int k = 0; k < K; k++, pass++) {
			GLuint dst = (pass == total - 1) ? p->dnow : (src == p->tex_a ? p->tex_b : p->tex_a);
			pl_fbo_attach(p, dst);
			pl_bind(0, src, p->samp_nearest);
			glUniform2i(PL_U(pr, "dir"), axis == 0 ? 1 : 0, axis == 0 ? 0 : 1);
			glUniform1i(PL_U(pr, "step_px"), 1 << k);
			for (int i = 0; i < p->nmon; i++) {
				const struct pl_mon *m = &p->mon[i];
				glUniform4i(PL_U(pr, "box"), m->x, p->H - (m->y + m->h), m->x + m->w,
				            p->H - m->y);
				pl_scissor_mon(p, m);
				pl_draw();
			}
			src = dst;
		}
	}
	glDisable(GL_SCISSOR_TEST);

	p->used_target = cfg->v[SQ_TARGET];
	p->used_min_gain = cfg->v[SQ_MIN_GAIN];
	p->used_knee = cfg->v[SQ_KNEE];
	p->used_falloff = R;
	p->dirty = false;
}

static inline void pl_smooth(struct pl *p, const struct sq_cfg *cfg, double now) {
	double tau = cfg->v[SQ_TAU_MS] / 1000.0;
	double dt = p->have_smooth_time && now > p->last_smooth ? now - p->last_smooth : 0.0;
	float k = (float)(1.0 - exp(-dt / tau));
	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, p->W, p->H);
	GLuint pr = p->prog[PL_SMOOTH];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "now_t"), 0);
	glUniform1i(PL_U(pr, "prev_t"), 1);
	glUniform1f(PL_U(pr, "k"), k);
	glUniform1f(PL_U(pr, "init"), p->st_valid ? 0.0f : 1.0f);
	pl_fbo_attach(p, p->st[1 - p->cur]);
	pl_bind(0, p->dnow, p->samp_nearest);
	pl_bind(1, p->st[p->cur], p->samp_nearest);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < p->nmon; i++) {
		pl_scissor_mon(p, &p->mon[i]);
		pl_draw();
	}
	glDisable(GL_SCISSOR_TEST);
	p->cur = 1 - p->cur;
	p->st_valid = true;
	p->last_smooth = now;
	p->have_smooth_time = true;
}

static inline void pl_final(struct pl *p, const struct sq_cfg *cfg, GLuint dtex) {
	pl_base_state();
	glBindVertexArray(p->vao);
	glBindFramebuffer(GL_FRAMEBUFFER, p->target_fbo);
	glDrawBuffer(p->draw_buf);
	glViewport(0, 0, p->W, p->H);
	GLuint pr = p->prog[PL_FINAL];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "S"), 0);
	glUniform1i(PL_U(pr, "D"), 1);
	glUniform1f(PL_U(pr, "enabled"), (float)cfg->v[SQ_ENABLED]);
	glUniform1f(PL_U(pr, "min_gain"), (float)cfg->v[SQ_MIN_GAIN]);
	glUniform1f(PL_U(pr, "gamma_dim"), (float)cfg->v[SQ_GAMMA]);
	pl_bind(0, p->s_tex, p->samp_nearest);
	pl_bind(1, dtex, p->samp_nearest);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < p->nmon; i++) {
		pl_scissor_mon(p, &p->mon[i]);
		pl_draw();
	}
	glDisable(GL_SCISSOR_TEST);
}

// Runs the whole pipeline. With nothing damaged since the last call it only advances the
// smoothing, which is how release continues while the screen is otherwise still. When
// disabled it still redraws every monitor pixel from S, because picom's buffer-age repaint
// would otherwise leave stale dimmed pixels in the other back buffers.
// The caller must have saved and must restore the host's GL state.
static inline void pl_render(struct pl *p, const struct sq_cfg *cfg, double now) {
	if (!p->ready || !p->s_valid || p->nmon == 0) {
		return;
	}
	bool enabled = cfg->v[SQ_ENABLED] != 0;
	bool smoothing = cfg->v[SQ_TAU_MS] > 0;
	GLuint dtex = p->dnow;
	if (enabled) {
		if (!p->was_enabled) {
			p->dirty = true;
			p->st_valid = false;
		}
		if (p->used_target != cfg->v[SQ_TARGET] || p->used_min_gain != cfg->v[SQ_MIN_GAIN] ||
		    p->used_knee != cfg->v[SQ_KNEE] || p->used_falloff != (int)cfg->v[SQ_FALLOFF_PX]) {
			p->dirty = true;
		}
		if (p->dirty) {
			pl_dilate(p, cfg);
		}
		if (smoothing) {
			pl_smooth(p, cfg, now);
			dtex = p->st[p->cur];
		} else {
			p->st_valid = false;  // the next time smoothing turns on, start from the present
		}
	} else {
		p->dirty = true;
	}
	p->was_enabled = enabled;
	pl_final(p, cfg, dtex);
}
