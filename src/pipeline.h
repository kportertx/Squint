// The GL half of squint. It has no picom dependency so test/pipeline_test.c can run it in
// a standalone context and compare the output with a CPU copy of the same math.
//
// Data flow, per frame that changed the screen:
//   capture  copy the damaged rectangles of the back buffer into S (the undimmed screen)
//   size     optional: a local measure M of how large and bright the surroundings are, from
//            a power mean of luminance over size_px, lowers each pixel's target as M rises
//   need     per pixel, d = 1 - gain(L): how much the pixel's own luminance L must be dimmed
//   dilate   spread d outward with a linear falloff, D(x) = max over y of d(y) - slope*|x-y|
//            (L1 distance), never across a monitor edge
//   scene    optional: per monitor, whether more than scene_pct percent of the screen's
//            blurred brightness changed significantly since the previous frame. Only then do smooth
//            and ramp apply, for a window of five time constants. Scrolling a page moves many
//            pixels but barely changes the blurred brightness, so it never triggers them.
//   smooth   optional per pixel: instant attack, exponential release with time constant
//            darkening, armed by a scene change
//   ramp     optional, regional, armed by a scene change: a blurred local brightness and gamma strength of the
//            content as it will be displayed, versus the level that was displayed before.
//            New content that would be brighter is shown at the old level first, and the
//            level rises to the new one with time constant rise. Only blurred values carry
//            over from frame to frame, never the layout of the old content.
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
#define PL_LR 4  // the size field is computed at 1/PL_LR resolution
#define PL_NPROG 15

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
	GLuint vao, fbo, prog[PL_NPROG];
	GLuint samp_nearest, samp_linear;

	int W, H;
	GLuint s_tex;           // undimmed screen, RGBA8
	GLuint tex_a, tex_b;    // dilation ping-pong
	GLuint dnow;            // dilated dimming amount D for the latest captured content
	GLuint st[2];           // R16F: smoothed D, used only when tau > 0
	GLuint lr_a, lr_b;      // RG16F at 1/PL_LR resolution: the size field (red channel)
	GLuint rlr_a, rlr_b;    // RG16F at 1/PL_LR resolution: blurred (brightness, gamma strength)
	GLuint rs[2];           // RG16F at 1/PL_LR resolution: the ramp state, what was displayed
	GLuint rlr_p;           // RG16F at 1/PL_LR resolution: the previous frame's means
	GLuint line_tex;        // RG8: red marks horizontal line pixels, green vertical ones
	GLuint lhas, lidx;      // R32F, 2 * PL_MAXMON columns: per monitor, which rows (columns 0 to
	                        // PL_MAXMON - 1) and which screen columns (the rest) hold line pixels,
	                        // and the number of the line each belongs to
	GLuint scn[2];          // RG32F PL_MAXMON x 1: per monitor, until when (seconds since t0)
	                        // darkening (red) and brightening (green) smoothing stay armed
	int scur;
	int lrW, lrH;
	int cur, rcur;
	bool s_valid, st_valid, rs_valid, dirty, prev_valid, means_valid;
	bool ldirty, line_valid, lidx_valid;  // line_tex must be recomputed; it holds the current lines; lidx holds their numbers
	double used_lmin, used_lthick, used_lcontrast, used_lh, used_lv;

	struct pl_mon mon[PL_MAXMON];
	float tint[3];  // whole-screen color multipliers, 1 = unchanged
	int nmon;

	// Parameters the current dnow was computed with.
	double used_target, used_min_gain, used_knee, used_target_large, used_size_px, used_size_bias;
	int used_falloff;

	bool have_input, have_smooth_time, have_ramp_time, was_enabled, have_t0;
	double last_input, last_smooth, last_ramp, t0;
};

enum { PL_NEED, PL_DILATE, PL_SMOOTH, PL_FINAL, PL_DOWN, PL_BOX, PL_RDOWN, PL_RAMP, PL_SCENE, PL_LINEC, PL_LINER, PL_LSTEP, PL_LCLOSE, PL_LHAS, PL_LSCAN };

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

// Mean of L^bias over each PL_LR x PL_LR block of S. Blocks cut off by the screen edge
// average only the pixels that exist.
static const char *pl_fs_down =
    PL_COMMON
    "uniform sampler2D S;\n"
    "uniform float bias;\n"
    "uniform ivec2 size;\n"
    "out vec2 o;\n"
    "void main() {\n"
    "  ivec2 b = ivec2(gl_FragCoord.xy) * 4;\n"
    "  float sum = 0.0, cnt = 0.0;\n"
    "  for (int j = 0; j < 4; j++)\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      ivec2 p = b + ivec2(i, j);\n"
    "      if (p.x < size.x && p.y < size.y) {\n"
    "        float L = dot(to_lin(texelFetch(S, p, 0).rgb), W709);\n"
    "        sum += pow(max(L, 1e-6), bias);\n"
    "        cnt += 1.0;\n"
    "      }\n"
    "    }\n"
    "  o = vec2(sum / cnt, 0.0);\n"
    "}\n";

// Separable box mean (mode 0) or box maximum (mode 1) along one axis, clipped to the
// monitor box (x0, y0, x1, y1 in texels, end exclusive). The mean divides by the number of
// texels inside the box, so it stays a true mean at monitor edges.
static const char *pl_fs_box =
    PL_COMMON
    "uniform sampler2D src;\n"
    "uniform ivec2 dir;\n"
    "uniform int radius, mode;\n"
    "uniform ivec4 box;\n"
    "out vec2 o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec2 acc = mode == 1 ? vec2(-1.0) : vec2(0.0);\n"
    "  float cnt = 0.0;\n"
    "  for (int k = -radius; k <= radius; k++) {\n"
    "    ivec2 q = p + dir * k;\n"
    "    if (q.x >= box.x && q.y >= box.y && q.x < box.z && q.y < box.w) {\n"
    "      vec2 v = texelFetch(src, q, 0).rg;\n"
    "      if (mode == 1) acc = max(acc, v); else { acc += v; cnt += 1.0; }\n"
    "    }\n"
    "  }\n"
    "  o = mode == 1 ? acc : acc / max(cnt, 1.0);\n"
    "}\n";

// d = 1 - gain for the pixel's own luminance. The gain is target / (target + excess) with
// a quadratic knee around the target, floored at min_gain. With use_size, the target falls
// from `target` to `target_large` as the local measure M = mean(L^bias)^(1/bias) rises from
// 0.2 to 0.5. A patch of luminance L covering a fraction C of the neighborhood has
// M = L * C^(1/bias), so brighter patches count as large at smaller sizes.
static const char *pl_fs_need =
    PL_COMMON
    "uniform sampler2D S, LR;\n"
    "uniform float target, min_gain, knee, use_size, bias, target_large;\n"
    "uniform vec2 lr_size;\n"
    "uniform vec4 lr_box;\n"  // x0, y0, x1, y1 of the monitor in LR texels
    "out float o;\n"
    "void main() {\n"
    "  vec3 c = texelFetch(S, ivec2(gl_FragCoord.xy), 0).rgb;\n"
    "  float L = dot(to_lin(c), W709);\n"
    "  float tgt = target;\n"
    "  if (use_size > 0.5) {\n"
    "    vec2 q = gl_FragCoord.xy / float(4);\n"
    "    q = clamp(q, lr_box.xy + 0.5, lr_box.zw - 0.5);\n"
    "    float M = pow(max(texture(LR, q / lr_size).r, 0.0), 1.0 / bias);\n"
    "    float s = clamp((M - 0.2) / 0.3, 0.0, 1.0);\n"
    "    tgt = mix(target, min(target_large, target), s * s * (3.0 - 2.0 * s));\n"
    "  }\n"
    "  float over = L - tgt;\n"
    "  float h = 0.5 * knee;\n"
    "  float excess = over <= -h ? 0.0 : (over >= h ? over : (over + h) * (over + h) / (2.0 * knee));\n"
    "  float g = clamp(tgt / (tgt + excess), min_gain, 1.0);\n"
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
    "uniform sampler2D now_t, prev_t, scn;\n"
    "uniform float k_tau, init, t_rel;\n"
    "uniform int idx;\n"
    "out float o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float n = texelFetch(now_t, p, 0).r;\n"
    "  float prev = texelFetch(prev_t, p, 0).r;\n"
    "  bool armed = t_rel < texelFetch(scn, ivec2(idx, 0), 0).r;\n"
    "  o = init > 0.5 ? n : (armed ? max(n, mix(prev, n, k_tau)) : n);\n"
    "}\n";

// Mean over each PL_LR x PL_LR block of the brightness the content will have once dimmed,
// L * (1 - D), and of the gamma strength a = D / (1 - min_gain). Red is brightness, green
// is strength.
static const char *pl_fs_rdown =
    PL_COMMON
    "uniform sampler2D S, D;\n"
    "uniform ivec2 size;\n"
    "uniform float min_gain;\n"
    "out vec2 o;\n"
    "void main() {\n"
    "  ivec2 b = ivec2(gl_FragCoord.xy) * 4;\n"
    "  vec2 sum = vec2(0.0);\n"
    "  float cnt = 0.0;\n"
    "  for (int j = 0; j < 4; j++)\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      ivec2 p = b + ivec2(i, j);\n"
    "      if (p.x < size.x && p.y < size.y) {\n"
    "        float L = dot(to_lin(texelFetch(S, p, 0).rgb), W709);\n"
    "        float d = texelFetch(D, p, 0).r;\n"
    "        float a = (1.0 - min_gain) > 1e-4 ? clamp(d / (1.0 - min_gain), 0.0, 1.0) : 0.0;\n"
    "        sum += vec2(L * (1.0 - d), a);\n"
    "        cnt += 1.0;\n"
    "      }\n"
    "    }\n"
    "  o = sum / cnt;\n"
    "}\n";

// The ramp state is what was displayed: a brightness b and a gamma strength a per area.
// Brightness may rise toward the new value with time constant rise but falls at once, so
// new content that is dimmer than before shows immediately. The gamma strength moves
// toward the new value in both directions, so the gamma changes smoothly too.
static const char *pl_fs_ramp =
    PL_COMMON
    "uniform sampler2D means, prev, scn;\n"
    "uniform float k, init, t_rel;\n"
    "uniform int idx;\n"
    "out vec2 o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec2 n = texelFetch(means, p, 0).rg;\n"
    "  vec2 pv = texelFetch(prev, p, 0).rg;\n"
    "  bool armed = t_rel < texelFetch(scn, ivec2(idx, 0), 0).g;\n"
    "  // Without a scene change the state follows the content at once.\n"
    "  if (init > 0.5 || !armed) { o = n; return; }\n"
    "  o = vec2(n.x > pv.x ? pv.x + (n.x - pv.x) * k : n.x, n.y + (pv.y - n.y) * (1.0 - k));\n"
    "}\n";

// Scene change detector, one fragment per monitor. It samples a 32 x 32 grid of the
// monitor's blurred brightness and counts the samples whose brightness differs from the
// previous frame by more than 0.03 or by more than 25 percent. When more than the
// fraction frac of them differ (scene_pct), smoothing stays armed until t_rel + dur_d and
// brightening until t_rel + dur_b (a duration of 0 leaves that one alone).
static const char *pl_fs_scene =
    PL_COMMON
    "uniform sampler2D means, prevm, scn_prev;\n"
    "uniform ivec4 box;\n"
    "uniform int idx;\n"
    "uniform float t_rel, dur_d, dur_b, frac;\n"
    "out vec2 o;\n"
    "void main() {\n"
    "  vec2 prev = texelFetch(scn_prev, ivec2(idx, 0), 0).rg;\n"
    "  float changed = 0.0;\n"
    "  for (int gy = 0; gy < 32; gy++)\n"
    "    for (int gx = 0; gx < 32; gx++) {\n"
    "      ivec2 q = box.xy + ivec2((vec2(gx, gy) + 0.5) / 32.0 * vec2(box.zw - box.xy));\n"
    "      float n = texelFetch(means, q, 0).r, pv = texelFetch(prevm, q, 0).r;\n"
    "      if (abs(n - pv) > max(0.03, 0.25 * max(n, pv))) changed += 1.0;\n"
    "    }\n"
    "  bool hit = changed / 1024.0 > frac;\n"
    "  o = hit ? vec2(dur_d > 0.0 ? t_rel + dur_d : prev.x, dur_b > 0.0 ? t_rel + dur_b : prev.y) : prev;\n"
    "}\n";

// Line detection. A horizontal line is a band of at most `thick` rows of one color, at least
// `min_px` pixels long, with a row on at least one side that differs from it in color
// by at least `contrast`, the largest difference in any encoded red, green, or blue value. A
// teal border on a dark gray background differs little in luma but a lot in blue. One side is enough because a window border or a scroll bar often
// touches a nearly matching color on one side, and a line on the edge of a monitor has only
// one side. Vertical lines are the same with the axes
// swapped. `along` is the axis of the
// line, so the thickness is measured across it. Pixels are the same color when no channel
// differs by more than 0.02. Taps outside the monitor box (x0, y0, x1, y1, end exclusive) never
// count, so a line stops at the edge of its monitor.
#define PL_LINE_COMMON                                                                     \
	"uniform ivec4 box;\n"                                                             \
	"bool inb(ivec2 p) { return p.x >= box.x && p.y >= box.y && p.x < box.z && p.y < box.w; }\n" \
	"bool same(vec3 a, vec3 b) { return all(lessThanEqual(abs(a - b), vec3(0.02))); }\n"                \
	"float color_diff(vec3 a, vec3 b) { vec3 d = abs(a - b); return max(d.r, max(d.g, d.b)); }\n"

// Marks the pixels that could belong to a line because of their thickness and contrast.
static const char *pl_fs_linec =
    PL_COMMON
    PL_LINE_COMMON
    "uniform sampler2D S;\n"
    "uniform ivec2 along;\n"
    "uniform int thick;\n"
    "uniform float contrast;\n"
    "out float o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  ivec2 across = ivec2(along.y, along.x);\n"
    "  vec3 c = texelFetch(S, p, 0).rgb;\n"
    "  int a = 1, b = 1;\n"
    "  for (; a <= thick; a++) {\n"
    "    ivec2 q = p - across * a;\n"
    "    if (!inb(q) || !same(texelFetch(S, q, 0).rgb, c)) break;\n"
    "  }\n"
    "  for (; b <= thick; b++) {\n"
    "    ivec2 q = p + across * b;\n"
    "    if (!inb(q) || !same(texelFetch(S, q, 0).rgb, c)) break;\n"
    "  }\n"
    "  ivec2 qa = p - across * a, qb = p + across * b;\n"
    "  bool in_a = inb(qa), in_b = inb(qb);\n"
    "  bool ok = a + b - 1 <= thick && (in_a || in_b);\n"
    "  if (ok) {\n"
    "    ok = (in_a && color_diff(texelFetch(S, qa, 0).rgb, c) >= contrast) ||\n"
    "         (in_b && color_diff(texelFetch(S, qb, 0).rgb, c) >= contrast);\n"
    "  }\n"
    "  o = ok ? 1.0 : 0.0;\n"
    "}\n";

// A line crossed by another line of the same color is not a candidate at the crossing,
// because the crossing line makes the band look thick there. A closing along the line, a
// maximum then a minimum over radius pixels (mode 1, then mode 0), fills crossings up to
// 2 * radius pixels wide. Taps outside the monitor box do not count either way.
static const char *pl_fs_lclose =
    PL_COMMON
    PL_LINE_COMMON
    "uniform sampler2D src;\n"
    "uniform ivec2 along;\n"
    "uniform int radius, mode;\n"
    "out float o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float acc = mode == 1 ? 0.0 : 1.0;\n"
    "  for (int k = -radius; k <= radius; k++) {\n"
    "    ivec2 q = p + along * k;\n"
    "    float v = inb(q) ? texelFetch(src, q, 0).r : acc;\n"
    "    acc = mode == 1 ? max(acc, v) : min(acc, v);\n"
    "  }\n"
    "  o = acc;\n"
    "}\n";

// Line numbers, for rgb_direction alternate. The texture has one column per monitor and
// orientation (the first PL_MAXMON columns for horizontal lines, indexed by GL row, the next
// PL_MAXMON for vertical lines, indexed by screen column). `lhas` marks the rows (or
// columns) of a monitor that hold any line pixel. `lscan` numbers the lines: a line is a
// group of consecutive marked rows, horizontal lines are counted from the top of the
// monitor, and vertical lines from the left, starting at 1.
static const char *pl_fs_lhas =
    PL_COMMON
    "uniform sampler2D LN;\n"
    "uniform int dir;\n"
    "uniform ivec4 box;\n"
    "out float o;\n"
    "void main() {\n"
    "  int r = int(gl_FragCoord.y);\n"
    "  bool any_px = false;\n"
    "  if (dir == 0) {\n"
    "    for (int x = box.x; x < box.z && !any_px; x++) any_px = texelFetch(LN, ivec2(x, r), 0).r > 0.5;\n"
    "  } else {\n"
    "    for (int y = box.y; y < box.w && !any_px; y++) any_px = texelFetch(LN, ivec2(r, y), 0).g > 0.5;\n"
    "  }\n"
    "  o = any_px ? 1.0 : 0.0;\n"
    "}\n";

static const char *pl_fs_lscan =
    PL_COMMON
    "uniform sampler2D HAS;\n"
    "uniform int dir;\n"
    "uniform ivec4 box;\n"
    "out float o;\n"
    "float has(int c, int r) { return texelFetch(HAS, ivec2(c, r), 0).r; }\n"
    "void main() {\n"
    "  int c = int(gl_FragCoord.x), r = int(gl_FragCoord.y);\n"
    "  float n = 0.0;\n"
    "  if (dir == 0) {\n"
    "    for (int y = r; y < box.w; y++) {\n"
    "      bool above = y + 1 < box.w && has(c, y + 1) > 0.5;\n"
    "      if (has(c, y) > 0.5 && !above) n += 1.0;\n"
    "    }\n"
    "  } else {\n"
    "    for (int x = box.x; x <= r; x++) {\n"
    "      bool left = x > box.x && has(c, x - 1) > 0.5;\n"
    "      if (has(c, x) > 0.5 && !left) n += 1.0;\n"
    "    }\n"
    "  }\n"
    "  o = n;\n"
    "}\n";

// 1 where this pixel and the next one along the line are both candidates of the same color.
static const char *pl_fs_liner =
    PL_COMMON
    PL_LINE_COMMON
    "uniform sampler2D S, C;\n"
    "uniform ivec2 along;\n"
    "out float o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy), q = p + along;\n"
    "  bool ok = inb(q) && texelFetch(C, p, 0).r > 0.5 && texelFetch(C, q, 0).r > 0.5 &&\n"
    "            same(texelFetch(S, p, 0).rgb, texelFetch(S, q, 0).rgb);\n"
    "  o = ok ? 1.0 : 0.0;\n"
    "}\n";

// min (mode 0) or max (mode 1) of the pixel and the pixel `off` steps along the line, with
// taps outside the monitor box counting as 0. The result goes to both channels scaled by
// `mask`, which lets the last pass write one channel of the line texture.
static const char *pl_fs_lstep =
    PL_COMMON
    PL_LINE_COMMON
    "uniform sampler2D src;\n"
    "uniform ivec2 along;\n"
    "uniform int off, mode;\n"
    "uniform vec2 mask;\n"
    "out vec4 o;\n"
    "float tap(ivec2 p) { return inb(p) ? texelFetch(src, p, 0).r : 0.0; }\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  float a = tap(p), b = tap(p + along * off);\n"
    "  float v = mode == 1 ? max(a, b) : min(a, b);\n"
    "  o = vec4(v * mask.x, v * mask.y, 0.0, 0.0);\n"
    "}\n";

// With use_ramp, new content is scaled by s = displayed / new brightness (at most 1) and
// its gamma strength is shifted by the difference between the displayed and the new
// strength. Both are blurred quantities and apply with weight w = d / 0.1 (at most 1), so
// pixels that are not being dimmed, and the faint tail of the falloff, are not touched.
// dim_gain multiplies the color channels where dimming is strong: it fades in with the
// dimming strength, before any ramp shift. tint multiplies every pixel of the monitors and
// is skipped, so pixels stay bit-exact, when it is 1 on all channels. enabled switches
// off everything, tint and static included.
//
// RGB lines recolor the pixels that line_tex marks, after dimming and tint and before
// static. The new color keeps the luminance of the pixel it replaces, raised to
// max(L, rgb_minl) and multiplied by rgb_luma, at most 1. A hue and saturation set the
// direction of the color. When that color cannot reach the luminance without a channel over 1
// (blue tops out near 7%), it is mixed toward gray of the same luminance until it fits, so
// the luminance is exact and only the saturation gives way. u is the position along the
// line and v the position across it. Lines that do not share a phase are offset by 13 px
// per pixel of v. Rain is three layers of falling drops, each a lattice of cells that moves
// along the line by one cell per cycle. A cell holds one drop with probability 0.7, whose head
// sits at a random place in the cell with a trail of up to rgb_len behind it. Cells are
// numbered minus the whole cycle count, so the pattern does not jump when the cycle fraction
// wraps. The head is whiter (saturation 0.35), the trail fades with the square of the
// distance from the head, a glyph-like flicker changes it every 6 px of the drop, and the bare line glows
// at 0.1. rgb_cyc is the fraction of the current cycle, 0 to 1, computed on the CPU in
// double precision so a long uptime does not blur the animation.
//
// Static is mixed in last, in encoded color: out = mix(color, noise, static_amt). Every
// update (static_hz times a second) draws a fresh, independent pattern: the frame number
// goes into the hash seed, and nothing is blended between updates. A field is smooth value
// noise with a lattice spacing of `grain` pixels (stretched sideways by `smear`), plus a
// coarser field at three times the spacing weighted by `clusters`, standardized to unit
// variance. Each channel is sqrt(1 - scol) times a shared field plus sqrt(scol) times its
// own, and the result is level + contrast * field, clamped. Odd rows are darkened by
// `scanlines`, and each row is shifted sideways by a random amount up to `jitter` pixels.
static const char *pl_fs_final =
    PL_COMMON
    "uniform sampler2D S, D, RM, RS, LN, LI;\n"
    "uniform ivec2 mon_col;\n"
    "uniform float enabled, min_gain, gamma_dim, use_ramp, use_dim;\n"
    "uniform float rgb_on, rgb_cyc, rgb_luma, rgb_minl, rgb_hue, rgb_span, rgb_sat, rgb_len, rgb_gap, rgb_tail, rgb_sync, scr_h;\n"
    "uniform int rgb_fx, rgb_dir, rgb_cycn;\n"
    "uniform vec3 dim_gain, tint;\n"
    "uniform vec2 lr_size;\n"
    "uniform vec4 lr_box;\n"
    "uniform float static_amt, grain, scol, level, contrast, clusters, scanlines, smear, jitter;\n"
    "uniform uint frame;\n"
    "out vec4 color;\n"
    "uint pcg(uint v) {\n"
    "  uint s = v * 747796405u + 2891336453u;\n"
    "  uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;\n"
    "  return (w >> 22u) ^ w;\n"
    "}\n"
    "float hash2(ivec2 p, uint seed) {\n"
    "  return float(pcg(uint(p.x) + pcg(uint(p.y) + pcg(seed)))) * (1.0 / 4294967296.0);\n"
    "}\n"
    "float vnoise(vec2 q, uint seed) {\n"
    "  vec2 i = floor(q);\n"
    "  vec2 f = q - i;\n"
    "  vec2 s = f * f * (3.0 - 2.0 * f);\n"
    "  ivec2 ii = ivec2(i);\n"
    "  float a = hash2(ii, seed), b = hash2(ii + ivec2(1, 0), seed);\n"
    "  float c = hash2(ii + ivec2(0, 1), seed), d = hash2(ii + ivec2(1, 1), seed);\n"
    "  return (mix(mix(a, b, s.x), mix(c, d, s.x), s.y) - 0.5) / 0.2154;\n"
    "}\n"
    "float field(vec2 p, uint seed) {\n"
    "  float a = vnoise(p / vec2(grain * smear, grain), seed);\n"
    "  float b = vnoise(p / vec2(grain * smear * 3.0, grain * 3.0), seed + 7919u);\n"
    "  return (a + clusters * b) * inversesqrt(1.0 + clusters * clusters);\n"
    "}\n"
    "vec3 static_rgb(vec2 fc) {\n"
    "  int row = int(fc.y);\n"
    "  uint s0 = frame * 2654435769u;\n"
    "  float jit = jitter * (hash2(ivec2(row, 0), s0 + 99u) * 2.0 - 1.0);\n"
    "  vec2 p = vec2(fc.x + jit, fc.y);\n"
    "  float shr = field(p, s0 + 1u);\n"
    "  vec3 z = vec3(shr);\n"
    "  if (scol > 0.0) {\n"
    "    z = vec3(field(p, s0 + 2u), field(p, s0 + 3u), field(p, s0 + 4u));\n"
    "    z = sqrt(1.0 - scol) * shr + sqrt(scol) * z;\n"
    "  }\n"
    "  vec3 n = clamp(vec3(level) + contrast * z, 0.0, 1.0);\n"
    "  return n * (1.0 - scanlines * float(row & 1));\n"
    "}\n"
    "vec3 hue_rgb(float h, float s) {\n"
    "  vec3 k = clamp(abs(fract(vec3(h) + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);\n"
    "  return mix(vec3(1.0), k, s);\n"
    "}\n"
    "vec3 rgb_apply(vec3 enc, vec2 m, vec2 fc) {\n"
    "  bool horiz = m.x > 0.5;\n"
    "  float u = horiz ? fc.x : scr_h - fc.y, v = horiz ? fc.y : fc.x;\n"
    "  float sgn = rgb_dir == 1 ? -1.0 : 1.0;\n"
    "  if (rgb_dir == 2) {\n"
    "    float n = texelFetch(LI, ivec2(horiz ? mon_col.x : mon_col.y, horiz ? int(fc.y) : int(fc.x)), 0).r;\n"
    "    sgn = mod(n, 2.0) > 0.5 ? 1.0 : -1.0;\n"
    "  }\n"
    "  float pos = sgn * (u + (rgb_sync > 0.5 ? 0.0 : 13.0 * v));\n"
    "  float hue = rgb_hue / 360.0, span = rgb_span / 360.0;\n"
    "  float mult = 1.0, w = 1.0, sat_scale = 1.0;\n"
    "  if (rgb_fx == 1) {\n"
    "    mult = mix(0.1, 1.0, 0.5 + 0.5 * sin(6.2831853 * rgb_cyc));\n"
    "  } else if (rgb_fx == 2) {\n"
    "    float ph = fract(pos / rgb_len - rgb_cyc);\n"
    "    hue += span * (span >= 0.999 ? ph : 1.0 - abs(2.0 * ph - 1.0));\n"
    "  } else if (rgb_fx == 3) {\n"
    "    float per = rgb_len + rgb_gap;\n"
    "    float d = mod(rgb_cyc * per - pos, per);\n"
    "    if (d >= rgb_len) return enc;\n"
    "    w = mix(1.0, 1.0 - d / rgb_len, rgb_tail);\n"
    "    hue += span * (d / rgb_len);\n"
    "  } else if (rgb_fx == 4) {\n"
    "    mult = mix(0.2, 1.0, 0.5 + 0.5 * sin(6.2831853 * (pos / rgb_len - rgb_cyc)));\n"
    "  } else if (rgb_fx == 5) {\n"
    "    hue = 0.375;\n"
    "    float best = 0.1;\n"
    "    bool head = false;\n"
    "    for (int l = 0; l < 3; l++) {\n"
    "      float f = l == 0 ? 1.0 : (l == 1 ? 1.5 : 2.0);\n"
    "      float ln = rgb_len * (l == 0 ? 1.0 : (l == 1 ? 0.8 : 0.6));\n"
    "      float per = (ln + rgb_gap) * f;\n"
    "      float q = pos - rgb_cyc * per;\n"
    "      float cell = floor(q / per);\n"
    "      int id = int(cell) - rgb_cycn;\n"
    "      if (hash2(ivec2(id, l), 17u) < 0.7) {\n"
    "        float d = ln + hash2(ivec2(id, l + 3), 29u) * (per - ln) - (q - cell * per);\n"
    "        if (d >= 0.0 && d < ln) {\n"
    "          float t = 1.0 - d / ln;\n"
    "          float flick = mix(0.6, 1.0, hash2(ivec2(id * 7 + int(floor(d / 6.0)), l), 41u));\n"
    "          float v = (0.1 + 0.9 * t * t) * (d < 3.0 ? 1.0 : flick);\n"
    "          if (v > best) { best = v; head = d < 3.0; }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    mult = best;\n"
    "    if (head) sat_scale = 0.35;\n"
    "  }\n"
    "  vec3 lin = to_lin(enc);\n"
    "  float lt = clamp(max(dot(lin, W709), rgb_minl) * rgb_luma * mult, 0.0, 1.0);\n"
    "  vec3 col = to_lin(hue_rgb(hue, rgb_sat * sat_scale));\n"
    "  col *= lt / dot(col, W709);\n"
    "  float a = 0.0;\n"
    "  for (int i = 0; i < 3; i++)\n"
    "    if (col[i] > 1.0) a = max(a, (col[i] - 1.0) / (col[i] - lt));\n"
    "  col = mix(col, vec3(lt), a);\n"
    "  return to_enc(mix(lin, col, w));\n"
    "}\n"
    "vec4 shade() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 s = texelFetch(S, p, 0);\n"
    "  float d = use_dim > 0.5 ? texelFetch(D, p, 0).r : 0.0;\n"
    "  bool tinted = tint != vec3(1.0);\n"
    "  // Pixels the falloff does not reach are copied bit for bit, unless the monitor is tinted.\n"
    "  if (d < 0.002) {\n"
    "    return tinted ? vec4(to_enc(to_lin(s.rgb) * tint), s.a) : s;\n"
    "  }\n"
    "  float g = 1.0 - d;\n"
    "  float strength = (1.0 - min_gain) > 1e-4 ? clamp(d / (1.0 - min_gain), 0.0, 1.0) : 0.0;\n"
    "  float a = strength;\n"
    "  if (use_ramp > 0.5) {\n"
    "    vec2 q = clamp(gl_FragCoord.xy / float(4), lr_box.xy + 0.5, lr_box.zw - 0.5) / lr_size;\n"
    "    vec2 m = texture(RM, q).rg, st = texture(RS, q).rg;\n"
    "    float w = clamp(d / 0.1, 0.0, 1.0);\n"
    "    float sc = m.x > 1e-4 ? clamp(st.x / m.x, 0.0, 1.0) : 1.0;\n"
    "    g *= 1.0 - (1.0 - sc) * w;\n"
    "    a = clamp(a + (st.y - m.y) * w, 0.0, 1.0);\n"
    "  }\n"
    "  vec3 e = pow(s.rgb, vec3(1.0 + (gamma_dim - 1.0) * a));\n"
    "  vec3 lin = to_lin(e) * g * mix(vec3(1.0), dim_gain, strength);\n"
    "  return vec4(to_enc(lin * tint), s.a);\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = texelFetch(S, ivec2(gl_FragCoord.xy), 0);\n"
    "  if (enabled >= 0.5) {\n"
    "    c = shade();\n"
    "    if (rgb_on > 0.5) {\n"
    "      vec2 m = texelFetch(LN, ivec2(gl_FragCoord.xy), 0).rg;\n"
    "      if (m.x > 0.5 || m.y > 0.5) c.rgb = rgb_apply(c.rgb, m, gl_FragCoord.xy);\n"
    "    }\n"
    "    if (static_amt > 0.0) c.rgb = mix(c.rgb, static_rgb(gl_FragCoord.xy), static_amt);\n"
    "  }\n"
    "  color = c;\n"
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
	GLint tex[6], samp[6];
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
	for (int u = 0; u < 6; u++) {
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
	for (int u = 5; u >= 0; u--) {
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
	const char *fs[PL_NPROG] = {pl_fs_need, pl_fs_dilate, pl_fs_smooth, pl_fs_final, pl_fs_down,  pl_fs_box,
	                            pl_fs_rdown, pl_fs_ramp,  pl_fs_scene,  pl_fs_linec, pl_fs_liner, pl_fs_lstep,
                            pl_fs_lclose, pl_fs_lhas, pl_fs_lscan};
	bool ok = true;
	for (int i = 0; i < PL_NPROG; i++) {
		p->prog[i] = pl_program(vs, fs[i]);
		ok = ok && p->prog[i];
	}
	glDeleteShader(vs);
	if (!ok) {
		return false;
	}
	glGenVertexArrays(1, &p->vao);
	glGenFramebuffers(1, &p->fbo);
	p->tint[0] = p->tint[1] = p->tint[2] = 1.0f;
	glGenSamplers(1, &p->samp_nearest);
	glSamplerParameteri(p->samp_nearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(p->samp_nearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(p->samp_nearest, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(p->samp_nearest, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenSamplers(1, &p->samp_linear);
	glSamplerParameteri(p->samp_linear, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(p->samp_linear, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(p->samp_linear, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(p->samp_linear, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	p->ready = true;
	return true;
}

static inline void pl_free_screen(struct pl *p) {
	if (p->s_tex) {
		glDeleteTextures(18, (GLuint[]){p->s_tex, p->tex_a, p->tex_b, p->dnow, p->st[0], p->st[1],
		                                p->lr_a, p->lr_b, p->rlr_a, p->rlr_b, p->rs[0], p->rs[1],
		                                p->rlr_p, p->scn[0], p->scn[1], p->line_tex, p->lhas, p->lidx});
		p->s_tex = 0;
	}
}

static inline void pl_destroy(struct pl *p) {
	if (!p->ready) {
		return;
	}
	pl_free_screen(p);
	for (int i = 0; i < PL_NPROG; i++) {
		glDeleteProgram(p->prog[i]);
	}
	glDeleteVertexArrays(1, &p->vao);
	glDeleteFramebuffers(1, &p->fbo);
	glDeleteSamplers(2, (GLuint[]){p->samp_nearest, p->samp_linear});
	p->ready = p->s_valid = false;
	p->W = p->H = 0;
}

static inline GLuint pl_tex(GLenum fmt, GLenum base, int w, int h) {
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, base,
	             fmt == GL_RGBA8 || fmt == GL_RG8 ? GL_UNSIGNED_BYTE : GL_FLOAT, NULL);
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
	p->lrW = (W + PL_LR - 1) / PL_LR;
	p->lrH = (H + PL_LR - 1) / PL_LR;
	p->lr_a = pl_tex(GL_RG16F, GL_RG, p->lrW, p->lrH);
	p->lr_b = pl_tex(GL_RG16F, GL_RG, p->lrW, p->lrH);
	p->rlr_a = pl_tex(GL_RG16F, GL_RG, p->lrW, p->lrH);
	p->rlr_b = pl_tex(GL_RG16F, GL_RG, p->lrW, p->lrH);
	p->rs[0] = pl_tex(GL_RG16F, GL_RG, p->lrW, p->lrH);
	p->rs[1] = pl_tex(GL_RG16F, GL_RG, p->lrW, p->lrH);
	p->rlr_p = pl_tex(GL_RG16F, GL_RG, p->lrW, p->lrH);
	p->line_tex = pl_tex(GL_RG8, GL_RG, W, H);
	p->lhas = pl_tex(GL_R32F, GL_RED, 2 * PL_MAXMON, W > H ? W : H);
	p->lidx = pl_tex(GL_R32F, GL_RED, 2 * PL_MAXMON, W > H ? W : H);
	p->lidx_valid = false;
	p->ldirty = true;
	p->line_valid = false;
	float zeros[2 * PL_MAXMON] = {0};
	for (int i = 0; i < 2; i++) {
		p->scn[i] = pl_tex(GL_RG32F, GL_RG, PL_MAXMON, 1);
		glBindTexture(GL_TEXTURE_2D, p->scn[i]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PL_MAXMON, 1, GL_RG, GL_FLOAT, zeros);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	p->s_valid = p->st_valid = p->rs_valid = p->prev_valid = p->means_valid = false;
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
	p->ldirty = true;
	p->st_valid = p->rs_valid = p->prev_valid = p->means_valid = false;
	return true;
}

// ---- per-frame work ---------------------------------------------------------------------

// Sets the whole-screen tint. It takes effect on the next pl_render and needs no
// recomputation of the dimming.
static inline void pl_set_tint(struct pl *p, float r, float g, float b) {
	p->tint[0] = r;
	p->tint[1] = g;
	p->tint[2] = b;
}

static inline void pl_touch(struct pl *p, double now) {
	p->last_input = now;
	p->have_input = true;
}

// True while a smoothing transition may still be running. There is no readback, so this is
// a time bound: five time constants after the last change, plus a little.
static inline bool pl_active(const struct pl *p, const struct sq_cfg *cfg, double now) {
	if (p->s_valid && cfg->v[SQ_ENABLED] != 0 && cfg->v[SQ_STATIC] != 0 &&
	    cfg->v[SQ_STATIC_AMOUNT] > 0) {
		return true;  // static changes every 1 / static_hz seconds for as long as it is on
	}
	if (p->s_valid && cfg->v[SQ_ENABLED] != 0 && cfg->v[SQ_RGB] != 0 && cfg->v[SQ_RGB_EFFECT] != 0) {
		return true;  // every effect except solid moves for as long as it is on
	}
	double slow = cfg->v[SQ_DARKENING_MS] > cfg->v[SQ_BRIGHTENING_MS] ? cfg->v[SQ_DARKENING_MS] : cfg->v[SQ_BRIGHTENING_MS];
	if (cfg->v[SQ_BRIGHTNESS] == 0) {
		slow = 0;  // no dimming, so nothing to smooth
	}
	return p->s_valid && p->have_input && now - p->last_input < 5.0 * slow / 1000.0 + 0.1;
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
	p->ldirty = true;
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

static inline bool pl_size_active(const struct sq_cfg *cfg) {
	return cfg->v[SQ_TARGET_LARGE] < cfg->v[SQ_TARGET] - 1e-6;
}

// A monitor's rectangle in size-field texels. The size field is y up like every texture, so
// the rectangle is flipped first.
struct pl_lrbox {
	int x0, y0, x1, y1;
};

static inline struct pl_lrbox pl_lr_box(const struct pl *p, const struct pl_mon *m) {
	int gy0 = p->H - (m->y + m->h), gy1 = p->H - m->y;
	struct pl_lrbox b = {m->x / PL_LR, gy0 / PL_LR, (m->x + m->w + PL_LR - 1) / PL_LR,
	                     (gy1 + PL_LR - 1) / PL_LR};
	if (b.x1 > p->lrW) b.x1 = p->lrW;
	if (b.y1 > p->lrH) b.y1 = p->lrH;
	return b;
}

// Computes the size field into lr_a: the box mean of L^bias over size_px, then spread
// outward with a box maximum of half that radius so the edge of a large bright area keeps
// the measure of its interior instead of showing a bright rim.
static inline void pl_size_field(struct pl *p, const struct sq_cfg *cfg) {
	int rt = (int)lround(cfg->v[SQ_SIZE_PX] / PL_LR);
	int rm = (int)lround(cfg->v[SQ_SIZE_PX] / (2.0 * PL_LR));
	rt = rt < 1 ? 1 : rt;
	rm = rm < 1 ? 1 : rm;

	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, p->lrW, p->lrH);
	glDisable(GL_SCISSOR_TEST);

	GLuint pr = p->prog[PL_DOWN];
	pl_fbo_attach(p, p->lr_a);
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "S"), 0);
	glUniform1f(PL_U(pr, "bias"), (float)cfg->v[SQ_SIZE_BIAS]);
	glUniform2i(PL_U(pr, "size"), p->W, p->H);
	pl_bind(0, p->s_tex, p->samp_nearest);
	pl_draw();

	pr = p->prog[PL_BOX];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "src"), 0);
	glEnable(GL_SCISSOR_TEST);
	GLuint src = p->lr_a, dst = p->lr_b;
	for (int pass = 0; pass < 4; pass++) {
		int mode = pass >= 2, axis = pass & 1;
		pl_fbo_attach(p, dst);
		pl_bind(0, src, p->samp_nearest);
		glUniform2i(PL_U(pr, "dir"), axis == 0 ? 1 : 0, axis == 0 ? 0 : 1);
		glUniform1i(PL_U(pr, "radius"), mode ? rm : rt);
		glUniform1i(PL_U(pr, "mode"), mode);
		for (int i = 0; i < p->nmon; i++) {
			struct pl_lrbox b = pl_lr_box(p, &p->mon[i]);
			glUniform4i(PL_U(pr, "box"), b.x0, b.y0, b.x1, b.y1);
			glScissor(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
			pl_draw();
		}
		GLuint t = src;
		src = dst;
		dst = t;
	}
	glDisable(GL_SCISSOR_TEST);
	// Four passes leave the result in lr_a: a->b, b->a, a->b, b->a.
}

// size + need + dilate: leaves D in dnow.
static inline void pl_dilate(struct pl *p, const struct sq_cfg *cfg) {
	int R = (int)cfg->v[SQ_FALLOFF_PX];
	int K = R > 0 ? pl_dilate_levels(R) : 0;
	float min_gain = (float)cfg->v[SQ_MIN_GAIN];
	float slope = R > 0 ? (1.0f - min_gain) / (float)R : 0.0f;
	bool use_size = pl_size_active(cfg);

	if (use_size) {
		pl_size_field(p, cfg);
	}

	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, p->W, p->H);

	GLuint pr = p->prog[PL_NEED];
	pl_fbo_attach(p, K == 0 ? p->dnow : p->tex_a);
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "S"), 0);
	glUniform1i(PL_U(pr, "LR"), 1);
	glUniform1f(PL_U(pr, "target"), (float)cfg->v[SQ_TARGET]);
	glUniform1f(PL_U(pr, "min_gain"), min_gain);
	glUniform1f(PL_U(pr, "knee"), (float)cfg->v[SQ_KNEE]);
	glUniform1f(PL_U(pr, "use_size"), use_size ? 1.0f : 0.0f);
	glUniform1f(PL_U(pr, "bias"), (float)cfg->v[SQ_SIZE_BIAS]);
	glUniform1f(PL_U(pr, "target_large"), (float)cfg->v[SQ_TARGET_LARGE]);
	glUniform2f(PL_U(pr, "lr_size"), (float)p->lrW, (float)p->lrH);
	pl_bind(0, p->s_tex, p->samp_nearest);
	pl_bind(1, p->lr_a, p->samp_linear);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < p->nmon; i++) {
		struct pl_lrbox b = pl_lr_box(p, &p->mon[i]);
		glUniform4f(PL_U(pr, "lr_box"), (float)b.x0, (float)b.y0, (float)b.x1, (float)b.y1);
		pl_scissor_mon(p, &p->mon[i]);
		pl_draw();
	}

	pr = p->prog[PL_DILATE];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "src"), 0);
	glUniform1f(PL_U(pr, "slope"), slope);
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
	p->used_target_large = cfg->v[SQ_TARGET_LARGE];
	p->used_size_px = cfg->v[SQ_SIZE_PX];
	p->used_size_bias = cfg->v[SQ_SIZE_BIAS];
	p->dirty = false;
}

static inline double pl_trel(struct pl *p, double now) {
	if (!p->have_t0) {
		p->t0 = now;
		p->have_t0 = true;
	}
	return now - p->t0;
}

static inline void pl_smooth(struct pl *p, const struct sq_cfg *cfg, double now) {
	double tau = cfg->v[SQ_DARKENING_MS] / 1000.0;
	double dt = p->have_smooth_time && now > p->last_smooth ? now - p->last_smooth : 0.0;
	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, p->W, p->H);
	GLuint pr = p->prog[PL_SMOOTH];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "now_t"), 0);
	glUniform1i(PL_U(pr, "prev_t"), 1);
	glUniform1i(PL_U(pr, "scn"), 2);
	glUniform1f(PL_U(pr, "k_tau"), (float)(1.0 - exp(-dt / tau)));
	glUniform1f(PL_U(pr, "init"), p->st_valid ? 0.0f : 1.0f);
	glUniform1f(PL_U(pr, "t_rel"), (float)pl_trel(p, now));
	pl_fbo_attach(p, p->st[1 - p->cur]);
	pl_bind(0, p->dnow, p->samp_nearest);
	pl_bind(1, p->st[p->cur], p->samp_nearest);
	pl_bind(2, p->scn[p->scur], p->samp_nearest);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < p->nmon; i++) {
		glUniform1i(PL_U(pr, "idx"), i);
		pl_scissor_mon(p, &p->mon[i]);
		pl_draw();
	}
	glDisable(GL_SCISSOR_TEST);
	p->cur = 1 - p->cur;
	p->st_valid = true;
	p->last_smooth = now;
	p->have_smooth_time = true;
}

// Compares this frame's means (rlr_a) with the previous frame's (rlr_p) and arms the
// smoothing windows of every monitor whose scene changed.
static inline void pl_scene(struct pl *p, const struct sq_cfg *cfg, double now) {
	double dd = cfg->v[SQ_DARKENING_MS] > 0 ? 5.0 * cfg->v[SQ_DARKENING_MS] / 1000.0 + 0.1 : 0.0;
	double db = cfg->v[SQ_BRIGHTENING_MS] > 0 ? 5.0 * cfg->v[SQ_BRIGHTENING_MS] / 1000.0 + 0.1 : 0.0;
	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, PL_MAXMON, 1);
	GLuint pr = p->prog[PL_SCENE];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "means"), 0);
	glUniform1i(PL_U(pr, "prevm"), 1);
	glUniform1i(PL_U(pr, "scn_prev"), 2);
	glUniform1f(PL_U(pr, "t_rel"), (float)pl_trel(p, now));
	glUniform1f(PL_U(pr, "dur_d"), (float)dd);
	glUniform1f(PL_U(pr, "dur_b"), (float)db);
	glUniform1f(PL_U(pr, "frac"), (float)(cfg->v[SQ_SCENE_PCT] / 100.0));
	pl_fbo_attach(p, p->scn[1 - p->scur]);
	pl_bind(0, p->rlr_a, p->samp_nearest);
	pl_bind(1, p->rlr_p, p->samp_nearest);
	pl_bind(2, p->scn[p->scur], p->samp_nearest);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < p->nmon; i++) {
		struct pl_lrbox b = pl_lr_box(p, &p->mon[i]);
		glUniform4i(PL_U(pr, "box"), b.x0, b.y0, b.x1, b.y1);
		glUniform1i(PL_U(pr, "idx"), i);
		glScissor(i, 0, 1, 1);
		pl_draw();
	}
	glDisable(GL_SCISSOR_TEST);
	p->scur = 1 - p->scur;
}

// Blurred (brightness, gamma strength) of the content as it will be displayed: block means
// of L * (1 - D) and D / (1 - min_gain), then a box mean of radius size_px clipped to each
// monitor. The result is in rlr_a.
static inline void pl_ramp_means(struct pl *p, const struct sq_cfg *cfg, GLuint dtex) {
	int rt = (int)lround(cfg->v[SQ_SIZE_PX] / PL_LR);
	rt = rt < 1 ? 1 : rt;
	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, p->lrW, p->lrH);
	glDisable(GL_SCISSOR_TEST);

	GLuint pr = p->prog[PL_RDOWN];
	pl_fbo_attach(p, p->rlr_a);
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "S"), 0);
	glUniform1i(PL_U(pr, "D"), 1);
	glUniform2i(PL_U(pr, "size"), p->W, p->H);
	glUniform1f(PL_U(pr, "min_gain"), (float)cfg->v[SQ_MIN_GAIN]);
	pl_bind(0, p->s_tex, p->samp_nearest);
	pl_bind(1, dtex, p->samp_nearest);
	pl_draw();

	pr = p->prog[PL_BOX];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "src"), 0);
	glUniform1i(PL_U(pr, "radius"), rt);
	glUniform1i(PL_U(pr, "mode"), 0);
	glEnable(GL_SCISSOR_TEST);
	GLuint src = p->rlr_a, dst = p->rlr_b;
	for (int axis = 0; axis < 2; axis++) {
		pl_fbo_attach(p, dst);
		pl_bind(0, src, p->samp_nearest);
		glUniform2i(PL_U(pr, "dir"), axis == 0 ? 1 : 0, axis == 0 ? 0 : 1);
		for (int i = 0; i < p->nmon; i++) {
			struct pl_lrbox b = pl_lr_box(p, &p->mon[i]);
			glUniform4i(PL_U(pr, "box"), b.x0, b.y0, b.x1, b.y1);
			glScissor(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
			pl_draw();
		}
		GLuint t = src;
		src = dst;
		dst = t;
	}
	glDisable(GL_SCISSOR_TEST);
	// Two passes leave the result in rlr_a: a to b, then b to a.
}

// Advances the ramp state toward the means with time constant rise.
static inline void pl_ramp_state(struct pl *p, const struct sq_cfg *cfg, double now) {
	double rise = cfg->v[SQ_BRIGHTENING_MS] / 1000.0;
	double dt = p->have_ramp_time && now > p->last_ramp ? now - p->last_ramp : 0.0;
	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, p->lrW, p->lrH);
	GLuint pr = p->prog[PL_RAMP];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "means"), 0);
	glUniform1i(PL_U(pr, "prev"), 1);
	glUniform1i(PL_U(pr, "scn"), 2);
	glUniform1f(PL_U(pr, "k"), (float)(1.0 - exp(-dt / rise)));
	glUniform1f(PL_U(pr, "init"), p->rs_valid ? 0.0f : 1.0f);
	glUniform1f(PL_U(pr, "t_rel"), (float)pl_trel(p, now));
	pl_fbo_attach(p, p->rs[1 - p->rcur]);
	pl_bind(0, p->rlr_a, p->samp_nearest);
	pl_bind(1, p->rs[p->rcur], p->samp_nearest);
	pl_bind(2, p->scn[p->scur], p->samp_nearest);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < p->nmon; i++) {
		struct pl_lrbox b = pl_lr_box(p, &p->mon[i]);
		glUniform1i(PL_U(pr, "idx"), i);
		glScissor(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
		pl_draw();
	}
	glDisable(GL_SCISSOR_TEST);
	p->rcur = 1 - p->rcur;
	p->rs_valid = true;
	p->last_ramp = now;
	p->have_ramp_time = true;
}

static inline void pl_lclose(struct pl *p, GLuint src, GLuint dst, int dx, int dy, int radius, int mode) {
	GLuint pr = p->prog[PL_LCLOSE];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "src"), 0);
	glUniform2i(PL_U(pr, "along"), dx, dy);
	glUniform1i(PL_U(pr, "radius"), radius);
	glUniform1i(PL_U(pr, "mode"), mode);
	pl_fbo_attach(p, dst);
	pl_bind(0, src, p->samp_nearest);
	for (int i = 0; i < p->nmon; i++) {
		const struct pl_mon *m = &p->mon[i];
		glUniform4i(PL_U(pr, "box"), m->x, p->H - (m->y + m->h), m->x + m->w, p->H - m->y);
		pl_scissor_mon(p, m);
		pl_draw();
	}
}

static inline int pl_floor_log2(int n) {
	int k = 0;
	while ((2 << k) <= n) {
		k++;
	}
	return k;
}

// One step of the line search: reads src, writes dst, per monitor. off is the tap distance
// along the line, mode 0 is min and 1 is max. With blend, the result is merged into dst by
// taking the larger value in each channel, and mask picks the channel.
static inline void pl_lstep(struct pl *p, GLuint src, GLuint dst, int dx, int dy, int off, int mode,
                            float mx, float my, bool blend) {
	GLuint pr = p->prog[PL_LSTEP];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "src"), 0);
	glUniform2i(PL_U(pr, "along"), dx, dy);
	glUniform1i(PL_U(pr, "off"), off);
	glUniform1i(PL_U(pr, "mode"), mode);
	glUniform2f(PL_U(pr, "mask"), mx, my);
	pl_fbo_attach(p, dst);
	pl_bind(0, src, p->samp_nearest);
	if (blend) {
		glEnable(GL_BLEND);
		glBlendEquation(GL_MAX);
		glBlendFunc(GL_ONE, GL_ONE);
	}
	for (int i = 0; i < p->nmon; i++) {
		const struct pl_mon *m = &p->mon[i];
		glUniform4i(PL_U(pr, "box"), m->x, p->H - (m->y + m->h), m->x + m->w, p->H - m->y);
		pl_scissor_mon(p, m);
		pl_draw();
	}
	if (blend) {
		glDisable(GL_BLEND);
		glBlendEquation(GL_FUNC_ADD);
	}
}

// Finds the lines in S and writes them to line_tex. For each direction: candidates (thin
// enough, enough contrast), a closing across crossings, links between neighboring candidates of one color, then a window
// test on the links. A pixel is on a line when some run of min_px consecutive pixels that
// includes it is linked all the way. That is a sliding minimum over min_px - 1 links
// followed by a sliding maximum over min_px start positions, each built from power of two
// windows: F_k covers 2^k links, and a window of any length is two overlapping F_k taps.
static inline void pl_lines(struct pl *p, const struct sq_cfg *cfg) {
	int X = (int)cfg->v[SQ_RGB_LINE_MIN_PX];
	int N = X - 1;
	int k = pl_floor_log2(N), j = pl_floor_log2(X);
	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, p->W, p->H);
	pl_fbo_attach(p, p->line_tex);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	for (int dir = 0; dir < 2; dir++) {
		if (cfg->v[dir == 0 ? SQ_RGB_HORIZONTAL : SQ_RGB_VERTICAL] == 0) {
			continue;
		}
		int dx = dir == 0 ? 1 : 0, dy = dir == 0 ? 0 : 1;
		GLuint pr = p->prog[PL_LINEC];
		glUseProgram(pr);
		glUniform1i(PL_U(pr, "S"), 0);
		glUniform2i(PL_U(pr, "along"), dx, dy);
		glUniform1i(PL_U(pr, "thick"), (int)cfg->v[SQ_RGB_LINE_THICK_PX]);
		glUniform1f(PL_U(pr, "contrast"), (float)cfg->v[SQ_RGB_LINE_CONTRAST]);
		pl_fbo_attach(p, p->tex_a);
		pl_bind(0, p->s_tex, p->samp_nearest);
		for (int i = 0; i < p->nmon; i++) {
			const struct pl_mon *m = &p->mon[i];
			glUniform4i(PL_U(pr, "box"), m->x, p->H - (m->y + m->h), m->x + m->w, p->H - m->y);
			pl_scissor_mon(p, m);
			pl_draw();
		}
		int radius = ((int)cfg->v[SQ_RGB_LINE_THICK_PX] + 1) / 2;
		pl_lclose(p, p->tex_a, p->tex_b, dx, dy, radius, 1);
		pl_lclose(p, p->tex_b, p->tex_a, dx, dy, radius, 0);
		pr = p->prog[PL_LINER];
		glUseProgram(pr);
		glUniform1i(PL_U(pr, "S"), 0);
		glUniform1i(PL_U(pr, "C"), 1);
		glUniform2i(PL_U(pr, "along"), dx, dy);
		pl_fbo_attach(p, p->tex_b);
		pl_bind(0, p->s_tex, p->samp_nearest);
		pl_bind(1, p->tex_a, p->samp_nearest);
		for (int i = 0; i < p->nmon; i++) {
			const struct pl_mon *m = &p->mon[i];
			glUniform4i(PL_U(pr, "box"), m->x, p->H - (m->y + m->h), m->x + m->w, p->H - m->y);
			pl_scissor_mon(p, m);
			pl_draw();
		}
		GLuint cur = p->tex_b, oth = p->tex_a, t;
		for (int i = 0; i < k; i++) {
			pl_lstep(p, cur, oth, dx, dy, 1 << i, 0, 1, 0, false);
			t = cur, cur = oth, oth = t;
		}
		pl_lstep(p, cur, oth, dx, dy, N - (1 << k), 0, 1, 0, false);
		t = cur, cur = oth, oth = t;
		for (int i = 0; i < j; i++) {
			pl_lstep(p, cur, oth, dx, dy, -(1 << i), 1, 1, 0, false);
			t = cur, cur = oth, oth = t;
		}
		pl_lstep(p, cur, p->line_tex, dx, dy, -(X - (1 << j)), 1, dir == 0 ? 1.0f : 0.0f,
		         dir == 0 ? 0.0f : 1.0f, true);
	}
	glDisable(GL_SCISSOR_TEST);
	p->used_lmin = cfg->v[SQ_RGB_LINE_MIN_PX];
	p->used_lthick = cfg->v[SQ_RGB_LINE_THICK_PX];
	p->used_lcontrast = cfg->v[SQ_RGB_LINE_CONTRAST];
	p->used_lh = cfg->v[SQ_RGB_HORIZONTAL];
	p->used_lv = cfg->v[SQ_RGB_VERTICAL];
	p->ldirty = false;
	p->line_valid = true;
	p->lidx_valid = false;
}

// Numbers the lines on each monitor for rgb_direction alternate. Horizontal lines and
// vertical lines are numbered separately, and each monitor starts again at 1.
static inline void pl_line_index(struct pl *p) {
	int th = p->W > p->H ? p->W : p->H;
	pl_base_state();
	glBindVertexArray(p->vao);
	glViewport(0, 0, 2 * PL_MAXMON, th);
	glEnable(GL_SCISSOR_TEST);
	for (int pass = 0; pass < 2; pass++) {
		GLuint pr = p->prog[pass == 0 ? PL_LHAS : PL_LSCAN];
		glUseProgram(pr);
		glUniform1i(PL_U(pr, pass == 0 ? "LN" : "HAS"), 0);
		pl_fbo_attach(p, pass == 0 ? p->lhas : p->lidx);
		pl_bind(0, pass == 0 ? p->line_tex : p->lhas, p->samp_nearest);
		for (int dir = 0; dir < 2; dir++) {
			glUniform1i(PL_U(pr, "dir"), dir);
			for (int i = 0; i < p->nmon; i++) {
				const struct pl_mon *m = &p->mon[i];
				int x0 = m->x, y0 = p->H - (m->y + m->h), x1 = m->x + m->w, y1 = p->H - m->y;
				glUniform4i(PL_U(pr, "box"), x0, y0, x1, y1);
				int lo = dir == 0 ? y0 : x0, hi = dir == 0 ? y1 : x1;
				glScissor(dir * PL_MAXMON + i, lo, 1, hi - lo);
				pl_draw();
			}
		}
	}
	glDisable(GL_SCISSOR_TEST);
	p->lidx_valid = true;
}

static inline void pl_final(struct pl *p, const struct sq_cfg *cfg, GLuint dtex, bool use_ramp,
                            bool use_dim, double now) {
	pl_base_state();
	glBindVertexArray(p->vao);
	glBindFramebuffer(GL_FRAMEBUFFER, p->target_fbo);
	glDrawBuffer(p->draw_buf);
	glViewport(0, 0, p->W, p->H);
	GLuint pr = p->prog[PL_FINAL];
	glUseProgram(pr);
	glUniform1i(PL_U(pr, "S"), 0);
	glUniform1i(PL_U(pr, "D"), 1);
	glUniform1i(PL_U(pr, "RM"), 2);
	glUniform1i(PL_U(pr, "RS"), 3);
	glUniform1f(PL_U(pr, "enabled"), (float)cfg->v[SQ_ENABLED]);
	glUniform1f(PL_U(pr, "min_gain"), (float)cfg->v[SQ_MIN_GAIN]);
	glUniform1f(PL_U(pr, "gamma_dim"), (float)cfg->v[SQ_GAMMA]);
	glUniform1f(PL_U(pr, "use_ramp"), use_ramp ? 1.0f : 0.0f);
	glUniform1f(PL_U(pr, "use_dim"), use_dim ? 1.0f : 0.0f);
	// The dim_tint toggle switches off the color multipliers.
	bool dt = cfg->v[SQ_DIM_TINT] != 0;
	glUniform3f(PL_U(pr, "dim_gain"), dt ? (float)cfg->v[SQ_DIM_RED] : 1.0f,
	            dt ? (float)cfg->v[SQ_DIM_GREEN] : 1.0f, dt ? (float)cfg->v[SQ_DIM_BLUE] : 1.0f);
	glUniform3f(PL_U(pr, "tint"), dt ? p->tint[0] : 1.0f, dt ? p->tint[1] : 1.0f,
	            dt ? p->tint[2] : 1.0f);
	bool stat = cfg->v[SQ_STATIC] != 0 && cfg->v[SQ_STATIC_AMOUNT] > 0;
	double hz = cfg->v[SQ_STATIC_HZ];
	glUniform1f(PL_U(pr, "static_amt"), stat ? (float)(cfg->v[SQ_STATIC_AMOUNT] / 100.0) : 0.0f);
	glUniform1f(PL_U(pr, "grain"), (float)cfg->v[SQ_STATIC_GRAIN_PX]);
	glUniform1f(PL_U(pr, "scol"), (float)cfg->v[SQ_STATIC_COLOR]);
	glUniform1f(PL_U(pr, "level"), (float)cfg->v[SQ_STATIC_LEVEL]);
	glUniform1f(PL_U(pr, "contrast"), (float)cfg->v[SQ_STATIC_CONTRAST]);
	glUniform1f(PL_U(pr, "clusters"), (float)cfg->v[SQ_STATIC_CLUSTERS]);
	glUniform1f(PL_U(pr, "scanlines"), (float)cfg->v[SQ_STATIC_SCANLINES]);
	glUniform1f(PL_U(pr, "smear"), (float)cfg->v[SQ_STATIC_SMEAR]);
	glUniform1f(PL_U(pr, "jitter"), (float)cfg->v[SQ_STATIC_JITTER_PX]);
	bool rgb = cfg->v[SQ_RGB] != 0 && p->line_valid;
	glUniform1i(PL_U(pr, "LN"), 4);
	glUniform1f(PL_U(pr, "rgb_on"), rgb ? 1.0f : 0.0f);
	glUniform1i(PL_U(pr, "rgb_fx"), (int)cfg->v[SQ_RGB_EFFECT]);
	glUniform1i(PL_U(pr, "rgb_dir"), (int)cfg->v[SQ_RGB_DIRECTION]);
	double cyc = pl_trel(p, now) * cfg->v[SQ_RGB_SPEED];
	glUniform1f(PL_U(pr, "rgb_cyc"), (float)(cyc - floor(cyc)));
	glUniform1i(PL_U(pr, "rgb_cycn"), (int)fmod(floor(cyc), 1048576.0));
	glUniform1f(PL_U(pr, "rgb_luma"), (float)cfg->v[SQ_RGB_LUMA]);
	glUniform1f(PL_U(pr, "rgb_minl"), (float)cfg->v[SQ_RGB_MIN_LUMA]);
	glUniform1f(PL_U(pr, "rgb_hue"), (float)cfg->v[SQ_RGB_HUE]);
	glUniform1f(PL_U(pr, "rgb_span"), (float)cfg->v[SQ_RGB_HUE_SPAN]);
	glUniform1f(PL_U(pr, "rgb_sat"), (float)cfg->v[SQ_RGB_SATURATION]);
	glUniform1f(PL_U(pr, "rgb_len"), (float)cfg->v[SQ_RGB_LENGTH_PX]);
	glUniform1f(PL_U(pr, "rgb_gap"), (float)cfg->v[SQ_RGB_GAP_PX]);
	glUniform1f(PL_U(pr, "rgb_tail"), (float)cfg->v[SQ_RGB_TAIL]);
	glUniform1f(PL_U(pr, "rgb_sync"), (float)cfg->v[SQ_RGB_SYNC]);
	glUniform1f(PL_U(pr, "scr_h"), (float)p->H);
	glUniform1ui(PL_U(pr, "frame"), (GLuint)(unsigned long long)floor(pl_trel(p, now) * hz));
	glUniform2f(PL_U(pr, "lr_size"), (float)p->lrW, (float)p->lrH);
	pl_bind(0, p->s_tex, p->samp_nearest);
	pl_bind(1, dtex, p->samp_nearest);
	pl_bind(2, p->rlr_a, p->samp_linear);
	pl_bind(3, p->rs[p->rcur], p->samp_linear);
	pl_bind(4, p->line_tex, p->samp_nearest);
	pl_bind(5, p->lidx, p->samp_nearest);
	glUniform1i(PL_U(pr, "LI"), 5);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < p->nmon; i++) {
		struct pl_lrbox b = pl_lr_box(p, &p->mon[i]);
		glUniform4f(PL_U(pr, "lr_box"), (float)b.x0, (float)b.y0, (float)b.x1, (float)b.y1);
		glUniform2i(PL_U(pr, "mon_col"), i, PL_MAXMON + i);
		pl_scissor_mon(p, &p->mon[i]);
		pl_draw();
	}
	glDisable(GL_SCISSOR_TEST);
}

// Runs the whole pipeline. With nothing damaged since the last call it only advances the
// smoothing and the ramp, which is how release and rise continue while the screen is
// otherwise still. When disabled it still redraws every monitor pixel from S, because
// picom's buffer-age repaint would otherwise leave stale dimmed pixels in the other back
// buffers. The caller must have saved and must restore the host's GL state.
static inline void pl_render(struct pl *p, const struct sq_cfg *cfg, double now) {
	if (!p->ready || !p->s_valid || p->nmon == 0) {
		return;
	}
	bool enabled = cfg->v[SQ_ENABLED] != 0;
	bool bright = enabled && cfg->v[SQ_BRIGHTNESS] != 0;  // the brightness group is on
	bool ramp = cfg->v[SQ_BRIGHTENING_MS] > 0;
	bool smoothing = cfg->v[SQ_DARKENING_MS] > 0;
	GLuint dtex = p->dnow;
	if (bright) {
		if (!p->was_enabled) {
			p->dirty = true;
			p->st_valid = p->rs_valid = p->prev_valid = p->means_valid = false;
		}
		if (p->used_target != cfg->v[SQ_TARGET] || p->used_min_gain != cfg->v[SQ_MIN_GAIN] ||
		    p->used_knee != cfg->v[SQ_KNEE] || p->used_falloff != (int)cfg->v[SQ_FALLOFF_PX] ||
		    p->used_target_large != cfg->v[SQ_TARGET_LARGE] ||
		    p->used_size_px != cfg->v[SQ_SIZE_PX] || p->used_size_bias != cfg->v[SQ_SIZE_BIAS]) {
			p->dirty = true;
			p->prev_valid = false;  // a settings change must not look like a scene change
		}
		bool dilated = p->dirty;
		if (dilated) {
			pl_dilate(p, cfg);
		}
		if (ramp || smoothing) {
			if (dilated || !p->means_valid) {
				GLuint t = p->rlr_a;  // the old means become the previous means
				p->rlr_a = p->rlr_p;
				p->rlr_p = t;
				pl_ramp_means(p, cfg, p->dnow);
				if (p->prev_valid) {
					pl_scene(p, cfg, now);
				}
				p->prev_valid = p->means_valid = true;
			}
		} else {
			p->prev_valid = p->means_valid = false;
		}
		if (smoothing) {
			pl_smooth(p, cfg, now);
			dtex = p->st[p->cur];
		} else {
			p->st_valid = false;  // the next time smoothing turns on, start from the present
		}
		if (ramp) {
			pl_ramp_state(p, cfg, now);
		} else {
			p->rs_valid = false;
		}
	} else {
		p->dirty = true;
		p->st_valid = p->rs_valid = p->prev_valid = p->means_valid = false;
	}
	p->was_enabled = bright;
	if (enabled && cfg->v[SQ_RGB] != 0) {
		if (p->ldirty || !p->line_valid || p->used_lmin != cfg->v[SQ_RGB_LINE_MIN_PX] ||
		    p->used_lthick != cfg->v[SQ_RGB_LINE_THICK_PX] ||
		    p->used_lcontrast != cfg->v[SQ_RGB_LINE_CONTRAST] || p->used_lh != cfg->v[SQ_RGB_HORIZONTAL] ||
		    p->used_lv != cfg->v[SQ_RGB_VERTICAL]) {
			pl_lines(p, cfg);
		}
		if (cfg->v[SQ_RGB_DIRECTION] == 2 && p->line_valid && !p->lidx_valid) {
			pl_line_index(p);
		}
	} else {
		p->ldirty = true;  // S may change while the lines are not being followed
	}
	pl_final(p, cfg, dtex, bright && ramp, bright, now);
}
