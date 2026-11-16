// Runs the GL pipeline from src/pipeline.h in its own GLX 3.3 core context (a 1x1 pbuffer,
// so it never touches picom or the visible screen) and compares the output with a CPU copy
// of the same math. The stand-in back buffer is an RGBA8 texture on an FBO.
//
// The CPU model is brute force: for every pixel, the maximum over every source pixel in the
// same monitor of need(source) - slope * L1 distance. The images are small enough for that,
// and the patterns are asymmetric in both axes so a y flip anywhere shows up as a failure.

#define GL_GLEXT_PROTOTYPES
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <stdint.h>
#include <stdlib.h>

#include "../src/pipeline.h"

static int failures, checks;

#define CHECK(cond, ...)                                                                   \
	do {                                                                               \
		checks++;                                                                  \
		if (!(cond)) {                                                             \
			failures++;                                                        \
			printf("  FAIL: ");                                                \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
		}                                                                          \
	} while (0)

// ---- CPU reference ----------------------------------------------------------------------

static double dec(double c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static double enc(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055; }
static double lum(const uint8_t *c) {
	return 0.2126 * dec(c[0] / 255.0) + 0.7152 * dec(c[1] / 255.0) + 0.0722 * dec(c[2] / 255.0);
}

static double need_for(double L, const struct sq_cfg *c, double t) {
	double knee = c->v[SQ_KNEE], h = 0.5 * knee, over = L - t;
	double ex = over <= -h ? 0 : (over >= h ? over : (over + h) * (over + h) / (2 * knee));
	double g = t / (t + ex);
	double lo = c->v[SQ_MIN_GAIN];
	g = g < lo ? lo : (g > 1 ? 1 : g);
	return 1 - g;
}

// Output channel value for input channel v (0..255) where the dimming amount is D.
static int apply(int v, double D, const struct sq_cfg *c) {
	if (D < 0.002) {
		return v;
	}
	double mg = c->v[SQ_MIN_GAIN];
	double a = (1 - mg) > 1e-4 ? fmin(fmax(D / (1 - mg), 0), 1) : 0;
	double e = pow(v / 255.0, 1 + (c->v[SQ_GAMMA] - 1) * a);
	return (int)lround(255 * enc(dec(e) * (1 - D)));
}

// ---- harness ----------------------------------------------------------------------------

static GLuint back_tex, back_fbo;
static uint8_t *img, *out;  // X-down RGBA
static int W, H;
static struct pl pl;
static struct pl_mon mons[PL_MAXMON];
static int nmons;
static float g_tint[3] = {1, 1, 1};  // the tint the model applies, set with set_tint()

static void model_step(const struct sq_cfg *c, double now);
static void model_reset(void);

static void make_context(void) {
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "no X display\n");
		exit(2);
	}
	int attr[] = {GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
	              GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None};
	int n;
	GLXFBConfig *cfgs = glXChooseFBConfig(d, DefaultScreen(d), attr, &n);
	if (!cfgs || n == 0) {
		fprintf(stderr, "no pbuffer fbconfig\n");
		exit(2);
	}
	int pbattr[] = {GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None};
	GLXPbuffer pb = glXCreatePbuffer(d, cfgs[0], pbattr);
	PFNGLXCREATECONTEXTATTRIBSARBPROC create =
	    (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
	int ca[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
	            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
	GLXContext ctx = create(d, cfgs[0], NULL, True, ca);
	if (!ctx || !glXMakeContextCurrent(d, pb, pb, ctx)) {
		fprintf(stderr, "cannot make a 3.3 core context current\n");
		exit(2);
	}
	printf("GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

static void setup(int w, int h, const struct pl_mon *m, int nm) {
	model_reset();
	g_tint[0] = g_tint[1] = g_tint[2] = 1.0f;
	W = w;
	H = h;
	nmons = nm;
	memcpy(mons, m, (size_t)nm * sizeof *m);
	free(img);
	free(out);
	img = calloc((size_t)w * h, 4);
	out = calloc((size_t)w * h, 4);
	if (back_tex) {
		glDeleteTextures(1, &back_tex);
		glDeleteFramebuffers(1, &back_fbo);
	}
	glGenTextures(1, &back_tex);
	glBindTexture(GL_TEXTURE_2D, back_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &back_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, back_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, back_tex, 0);
	memset(&pl, 0, sizeof pl);
	pl.target_fbo = back_fbo;
	pl.read_buf = pl.draw_buf = GL_COLOR_ATTACHMENT0;
	if (!pl_init(&pl)) {
		printf("pl_init failed\n");
		exit(1);
	}
	pl_set_monitors(&pl, m, nm);
	pl_ensure_screen(&pl, w, h);
}

static void teardown(void) {
	pl_destroy(&pl);
}

static void set_tint(float r, float g, float b) {
	g_tint[0] = r;
	g_tint[1] = g;
	g_tint[2] = b;
	pl_set_tint(&pl, r, g, b);
}

// "picom wrote this": set a rectangle (X coordinates) of the back buffer.
static void put(struct pl_rect r, int rr, int gg, int bb) {
	for (int y = r.y1; y < r.y2; y++)
		for (int x = r.x1; x < r.x2; x++) {
			uint8_t *p = img + ((size_t)y * W + x) * 4;
			p[0] = (uint8_t)rr, p[1] = (uint8_t)gg, p[2] = (uint8_t)bb, p[3] = 255;
		}
	size_t rw = (size_t)(r.x2 - r.x1), rh = (size_t)(r.y2 - r.y1);
	uint8_t *tmp = malloc(rw * rh * 4);
	for (size_t y = 0; y < rh; y++)  // texture rows are bottom-up
		memcpy(tmp + (rh - 1 - y) * rw * 4, img + ((size_t)(r.y1 + (int)y) * W + r.x1) * 4, rw * 4);
	glBindTexture(GL_TEXTURE_2D, back_tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, r.x1, H - r.y2, (int)rw, (int)rh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	free(tmp);
}

static void capture(struct pl_rect r, double now) { pl_capture(&pl, &r, 1, false, now); }
static void capture_full(double now) { pl_capture(&pl, NULL, 0, true, now); }

static void render(const struct sq_cfg *cfg, double now) {
	while (glGetError() != GL_NO_ERROR) {
	}
	pl_render(&pl, cfg, now);
	GLenum e = glGetError();
	CHECK(e == GL_NO_ERROR, "GL error 0x%x after render", e);
	uint8_t *tmp = malloc((size_t)W * H * 4);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, back_fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
	for (int y = 0; y < H; y++)
		memcpy(out + (size_t)y * W * 4, tmp + (size_t)(H - 1 - y) * W * 4, (size_t)W * 4);
	free(tmp);
	model_step(cfg, now);
}

static const uint8_t *at(int x, int y) { return out + ((size_t)y * W + x) * 4; }

static void expect_px(const char *what, int x, int y, int want, int tol) {
	const uint8_t *p = at(x, y);
	int ok = abs(p[0] - want) <= tol && abs(p[1] - want) <= tol && abs(p[2] - want) <= tol;
	CHECK(ok, "%s at (%d,%d): got %d,%d,%d want %d (tol %d)", what, x, y, p[0], p[1], p[2], want, tol);
}

// The baseline every test starts from. It sets every setting explicitly, so the tests do not
// depend on the defaults squint ships with: brightness and dim_tint on, static and shortcuts
// off, and neutral values everywhere.
static struct sq_cfg base_cfg(void) {
	struct sq_cfg c;
	sq_defaults(&c);
	double base[SQ_N] = {[SQ_ENABLED] = 1, [SQ_BRIGHTNESS] = 1, [SQ_TARGET] = 0.45, [SQ_TARGET_LARGE] = 0.20,
	                     [SQ_SIZE_PX] = 192, [SQ_SIZE_BIAS] = 2.0, [SQ_MIN_GAIN] = 0.35, [SQ_KNEE] = 0.10,
	                     [SQ_DARKENING_MS] = 0, [SQ_BRIGHTENING_MS] = 0, [SQ_SCENE_PCT] = 33, [SQ_FALLOFF_PX] = 24,
	                     [SQ_GAMMA] = 1.0, [SQ_DIM_TINT] = 1, [SQ_DIM_RED] = 1, [SQ_DIM_GREEN] = 1, [SQ_DIM_BLUE] = 1,
	                     [SQ_TINT_RED] = 1, [SQ_TINT_GREEN] = 1, [SQ_TINT_BLUE] = 1, [SQ_STATIC] = 0,
	                     [SQ_STATIC_AMOUNT] = 10, [SQ_STATIC_GRAIN_PX] = 2.0, [SQ_STATIC_COLOR] = 0.0, [SQ_STATIC_HZ] = 60,
	                     [SQ_STATIC_LEVEL] = 0.5, [SQ_STATIC_CONTRAST] = 0.22, [SQ_STATIC_CLUSTERS] = 0.3,
	                     [SQ_STATIC_SCANLINES] = 0.15, [SQ_STATIC_SMEAR] = 1.3, [SQ_STATIC_JITTER_PX] = 0.5,
	                     [SQ_SHORTCUTS] = 1};
	memcpy(c.v, base, sizeof base);
	c.v[SQ_TARGET] = 0.45;
	c.v[SQ_TARGET_LARGE] = 0.45;  // size feature off unless a test turns it on
	c.v[SQ_MIN_GAIN] = 0.35;
	c.v[SQ_DARKENING_MS] = 0;
	c.v[SQ_BRIGHTENING_MS] = 0;
	c.v[SQ_FALLOFF_PX] = 8;
	return c;
}

// Per pixel target. With the size feature on, it falls from target to target_large as the
// local measure M rises, computed the way the GPU does it: block means of L^bias at 1/4
// resolution (blocks aligned to the bottom of the screen, as GL rows are), a separable box
// mean and then box maximum clipped to each monitor, bilinear lookup clamped inside the
// monitor, and a smoothstep from 0.2 to 0.5.
static double *cpu_targets(const struct sq_cfg *c) {
	double *tgt = malloc((size_t)W * H * sizeof *tgt);
	double t0 = c->v[SQ_TARGET], tl = c->v[SQ_TARGET_LARGE];
	for (int i = 0; i < W * H; i++) tgt[i] = t0;
	if (!(tl < t0 - 1e-6)) {
		return tgt;
	}
	int f = 4, lw = (W + f - 1) / f, lh = (H + f - 1) / f;
	double a = c->v[SQ_SIZE_BIAS];
	double *A = malloc((size_t)lw * lh * sizeof *A), *B = malloc((size_t)lw * lh * sizeof *B);
	for (int i = 0; i < lw * lh; i++) A[i] = B[i] = -2.0;  // -2 marks "never written"
	for (int by = 0; by < lh; by++)
		for (int bx = 0; bx < lw; bx++) {
			double sum = 0, cnt = 0;
			for (int j = 0; j < f; j++)
				for (int i = 0; i < f; i++) {
					int gx = bx * f + i, gy = by * f + j;
					if (gx < W && gy < H) {
						sum += pow(fmax(lum(img + ((size_t)(H - 1 - gy) * W + gx) * 4), 1e-6), a);
						cnt += 1;
					}
				}
			A[by * lw + bx] = sum / cnt;
		}
	int rt = (int)lround(c->v[SQ_SIZE_PX] / f), rm = (int)lround(c->v[SQ_SIZE_PX] / (2.0 * f));
	rt = rt < 1 ? 1 : rt;
	rm = rm < 1 ? 1 : rm;
	double *src = A, *dst = B;
	for (int pass = 0; pass < 4; pass++) {
		int mode = pass >= 2, axis = pass & 1, radius = mode ? rm : rt;
		for (int m = 0; m < nmons; m++) {
			int gy0 = H - (mons[m].y + mons[m].h), gy1 = H - mons[m].y;
			int x0 = mons[m].x / f, x1 = (mons[m].x + mons[m].w + f - 1) / f;
			int y0 = gy0 / f, y1 = (gy1 + f - 1) / f;
			if (x1 > lw) x1 = lw;
			if (y1 > lh) y1 = lh;
			for (int y = y0; y < y1; y++)
				for (int x = x0; x < x1; x++) {
					double acc = mode ? -1.0 : 0.0, cnt = 0;
					for (int k = -radius; k <= radius; k++) {
						int qx = x + (axis == 0 ? k : 0), qy = y + (axis == 0 ? 0 : k);
						if (qx >= x0 && qx < x1 && qy >= y0 && qy < y1) {
							double v = src[qy * lw + qx];
							if (v < -1.5) printf("  note: read of an unwritten size texel\n");
							if (mode) acc = fmax(acc, v); else { acc += v; cnt += 1; }
						}
					}
					dst[y * lw + x] = mode ? acc : acc / fmax(cnt, 1);
				}
		}
		double *t = src; src = dst; dst = t;
	}
	// src now holds the result (a to b, b to a, a to b, b to a).
	for (int m = 0; m < nmons; m++) {
		int gy0 = H - (mons[m].y + mons[m].h), gy1 = H - mons[m].y;
		int x0 = mons[m].x / f, x1 = (mons[m].x + mons[m].w + f - 1) / f;
		int y0 = gy0 / f, y1 = (gy1 + f - 1) / f;
		if (x1 > lw) x1 = lw;
		if (y1 > lh) y1 = lh;
		for (int y = mons[m].y; y < mons[m].y + mons[m].h; y++)
			for (int x = mons[m].x; x < mons[m].x + mons[m].w; x++) {
				double qx = (x + 0.5) / f, qy = (H - y - 0.5) / f;
				qx = fmin(fmax(qx, x0 + 0.5), x1 - 0.5);
				qy = fmin(fmax(qy, y0 + 0.5), y1 - 0.5);
				double u = qx - 0.5, v = qy - 0.5;
				int i0 = (int)floor(u), j0 = (int)floor(v);
				double fx = u - i0, fy = v - j0;
				#define TEX(i, j) src[(j < 0 ? 0 : (j > lh - 1 ? lh - 1 : j)) * lw + (i < 0 ? 0 : (i > lw - 1 ? lw - 1 : i))]
				double val = (1 - fx) * (1 - fy) * TEX(i0, j0) + fx * (1 - fy) * TEX(i0 + 1, j0) +
				             (1 - fx) * fy * TEX(i0, j0 + 1) + fx * fy * TEX(i0 + 1, j0 + 1);
				#undef TEX
				double M = pow(fmax(val, 0), 1.0 / a);
				double sm = fmin(fmax((M - 0.2) / 0.3, 0), 1);
				tgt[(size_t)y * W + x] = t0 + sm * sm * (3 - 2 * sm) * (fmin(tl, t0) - t0);
			}
	}
	free(A);
	free(B);
	return tgt;
}

// Dimming amount at every pixel of the current image, brute force.
static double *cpu_D(const struct sq_cfg *c) {
	double *need = malloc((size_t)W * H * sizeof *need);
	double *D = calloc((size_t)W * H, sizeof *D);
	double *tgt = cpu_targets(c);
	for (int i = 0; i < W * H; i++) {
		need[i] = need_for(lum(img + (size_t)i * 4), c, tgt[i]);
	}
	free(tgt);
	int R = (int)c->v[SQ_FALLOFF_PX];
	double slope = R > 0 ? (1 - c->v[SQ_MIN_GAIN]) / R : 0;
	for (int m = 0; m < nmons; m++) {
		struct pl_mon b = mons[m];
		for (int y = b.y; y < b.y + b.h; y++)
			for (int x = b.x; x < b.x + b.w; x++) {
				double best = need[(size_t)y * W + x];
				// falloff 0 means no spread at all, not a slope of zero.
				for (int sy = b.y; R > 0 && sy < b.y + b.h; sy++)
					for (int sx = b.x; sx < b.x + b.w; sx++) {
						double d = need[(size_t)sy * W + sx] - slope * (abs(sx - x) + abs(sy - y));
						if (d > best) best = d;
					}
				D[(size_t)y * W + x] = best;
			}
	}
	free(need);
	return D;
}

// The CPU model is stateful, like the GPU pipeline: model_step is called once per GPU
// render. It carries the smoothed dimming amount (darkening), the ramp state (brightening),
// the previous frame's means, and the per-monitor scene-change windows across frames, and
// produces the expected image in exp_img.
static struct {
	bool ds_valid, rs_valid, prev_valid, have_smooth, have_ramp, was_enabled, have_t0, have_used;
	double last_smooth, last_ramp, t0;
	double *ds;      // W*H smoothed D
	double *rs;      // lw*lh*2 ramp state (brightness, gamma strength)
	double *prev;    // lw*lh*2 the previous frame's means
	double until[PL_MAXMON][2];  // per monitor: until when darkening and brightening are armed
	double used[7];
} M;
static uint8_t *exp_img;

static void model_reset(void) {
	free(M.ds);
	free(M.rs);
	free(M.prev);
	memset(&M, 0, sizeof M);
	free(exp_img);
	exp_img = NULL;
}

struct lrbox { int x0, y0, x1, y1; };

static struct lrbox lr_box_of(int m) {
	int f = 4, lw = (W + f - 1) / f, lh = (H + f - 1) / f;
	int gy0 = H - (mons[m].y + mons[m].h), gy1 = H - mons[m].y;
	struct lrbox b = {mons[m].x / f, gy0 / f, (mons[m].x + mons[m].w + f - 1) / f, (gy1 + f - 1) / f};
	if (b.x1 > lw) b.x1 = lw;
	if (b.y1 > lh) b.y1 = lh;
	return b;
}

// Bilinear lookup in a lw x lh x 2 array at pixel (x, y), clamped inside monitor box b.
static void lr_sample(const double *a, struct lrbox b, int x, int y, double out[2]) {
	int f = 4, lw = (W + f - 1) / f, lh = (H + f - 1) / f;
	double qx = fmin(fmax((x + 0.5) / f, b.x0 + 0.5), b.x1 - 0.5);
	double qy = fmin(fmax((H - y - 0.5) / f, b.y0 + 0.5), b.y1 - 0.5);
	double u = qx - 0.5, v = qy - 0.5;
	int i0 = (int)floor(u), j0 = (int)floor(v);
	double fx = u - i0, fy = v - j0;
	for (int ch = 0; ch < 2; ch++) {
#define T(i, j) a[(((j) < 0 ? 0 : ((j) > lh - 1 ? lh - 1 : (j))) * lw + ((i) < 0 ? 0 : ((i) > lw - 1 ? lw - 1 : (i)))) * 2 + ch]
		out[ch] = (1 - fx) * (1 - fy) * T(i0, j0) + fx * (1 - fy) * T(i0 + 1, j0) +
		          (1 - fx) * fy * T(i0, j0 + 1) + fx * fy * T(i0 + 1, j0 + 1);
#undef T
	}
}

// Blurred (brightness, gamma strength) of the content once dimmed by D: block means of
// L * (1 - D) and D / (1 - min_gain), then a box mean per monitor, clipped to the monitor.
static double *cpu_means(const double *D, const struct sq_cfg *c) {
	int f = 4, lw = (W + f - 1) / f, lh = (H + f - 1) / f;
	double mg = c->v[SQ_MIN_GAIN];
	double *A = malloc((size_t)lw * lh * 2 * sizeof *A), *B = malloc((size_t)lw * lh * 2 * sizeof *B);
	for (int i = 0; i < lw * lh * 2; i++) A[i] = B[i] = -2.0;
	for (int by = 0; by < lh; by++)
		for (int bx = 0; bx < lw; bx++) {
			double sum[2] = {0, 0}, cnt = 0;
			for (int j = 0; j < f; j++)
				for (int i = 0; i < f; i++) {
					int gx = bx * f + i, gy = by * f + j;
					if (gx < W && gy < H) {
						int px = (H - 1 - gy) * W + gx;
						double d = D[px];
						double a = (1 - mg) > 1e-4 ? fmin(fmax(d / (1 - mg), 0), 1) : 0;
						sum[0] += lum(img + (size_t)px * 4) * (1 - d);
						sum[1] += a;
						cnt += 1;
					}
				}
			A[(by * lw + bx) * 2] = sum[0] / cnt;
			A[(by * lw + bx) * 2 + 1] = sum[1] / cnt;
		}
	int rt = (int)lround(c->v[SQ_SIZE_PX] / f);
	rt = rt < 1 ? 1 : rt;
	double *src = A, *dst = B;
	for (int axis = 0; axis < 2; axis++) {
		for (int m = 0; m < nmons; m++) {
			struct lrbox b = lr_box_of(m);
			for (int y = b.y0; y < b.y1; y++)
				for (int x = b.x0; x < b.x1; x++) {
					double acc[2] = {0, 0}, cnt = 0;
					for (int k = -rt; k <= rt; k++) {
						int qx = x + (axis == 0 ? k : 0), qy = y + (axis == 0 ? 0 : k);
						if (qx >= b.x0 && qx < b.x1 && qy >= b.y0 && qy < b.y1) {
							acc[0] += src[(qy * lw + qx) * 2];
							acc[1] += src[(qy * lw + qx) * 2 + 1];
							cnt += 1;
						}
					}
					dst[(y * lw + x) * 2] = acc[0] / fmax(cnt, 1);
					dst[(y * lw + x) * 2 + 1] = acc[1] / fmax(cnt, 1);
				}
		}
		double *t = src; src = dst; dst = t;
	}
	free(dst);
	return src;
}

// The static noise, as in pl_fs_final: smooth value noise from a PCG hash.
static uint32_t pcg(uint32_t v) {
	uint32_t st = v * 747796405u + 2891336453u;
	uint32_t w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
	return (w >> 22u) ^ w;
}

static float hash2(int px, int py, uint32_t seed) {
	return (float)pcg((uint32_t)px + pcg((uint32_t)py + pcg(seed))) * (1.0f / 4294967296.0f);
}

static float vnoise(float qx, float qy, uint32_t seed) {
	float ix = floorf(qx), iy = floorf(qy);
	float fx = qx - ix, fy = qy - iy;
	float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
	int x = (int)ix, y = (int)iy;
	float a = hash2(x, y, seed), b = hash2(x + 1, y, seed), c2 = hash2(x, y + 1, seed), d = hash2(x + 1, y + 1, seed);
	float top = a + (b - a) * sx, bot = c2 + (d - c2) * sx;
	return (top + (bot - top) * sy - 0.5f) / 0.2154f;
}

static float cpu_field(const struct sq_cfg *c, float px, float py, uint32_t seed) {
	float grain = (float)c->v[SQ_STATIC_GRAIN_PX], smear = (float)c->v[SQ_STATIC_SMEAR];
	float cl = (float)c->v[SQ_STATIC_CLUSTERS];
	float a = vnoise(px / (grain * smear), py / grain, seed);
	float b = vnoise(px / (grain * smear * 3.0f), py / (grain * 3.0f), seed + 7919u);
	return (a + cl * b) / sqrtf(1.0f + cl * cl);
}

static void cpu_static(const struct sq_cfg *c, double fcx, double fcy, uint32_t frame, float out[3]) {
	float scol = (float)c->v[SQ_STATIC_COLOR];
	int row = (int)fcy;
	uint32_t s0 = frame * 2654435769u;
	float jit = (float)c->v[SQ_STATIC_JITTER_PX] * (hash2(row, 0, s0 + 99u) * 2.0f - 1.0f);
	float px = (float)fcx + jit, py = (float)fcy;
	float common = cpu_field(c, px, py, s0 + 1u);
	float z[3] = {common, common, common};
	if (scol > 0.0f) {
		for (int ch = 0; ch < 3; ch++) {
			z[ch] = sqrtf(1.0f - scol) * common + sqrtf(scol) * cpu_field(c, px, py, s0 + 2u + (uint32_t)ch);
		}
	}
	for (int ch = 0; ch < 3; ch++) {
		float v = (float)c->v[SQ_STATIC_LEVEL] + (float)c->v[SQ_STATIC_CONTRAST] * z[ch];
		v = v < 0 ? 0 : (v > 1 ? 1 : v);
		out[ch] = v * (1.0f - (float)c->v[SQ_STATIC_SCANLINES] * (float)(row & 1));
	}
}

static void model_step(const struct sq_cfg *c, double now) {
	int f = 4, lw = (W + f - 1) / f, lh = (H + f - 1) / f;
	free(exp_img);
	exp_img = malloc((size_t)W * H * 4);
	memcpy(exp_img, img, (size_t)W * H * 4);
	bool enabled = c->v[SQ_ENABLED] != 0;
	bool bright = enabled && c->v[SQ_BRIGHTNESS] != 0;  // the brightness group is on
	bool dimtint = c->v[SQ_DIM_TINT] != 0;
	bool ramp = bright && c->v[SQ_BRIGHTENING_MS] > 0, smoothing = bright && c->v[SQ_DARKENING_MS] > 0;
	float tv[3];
	double dgv[3];
	for (int ch = 0; ch < 3; ch++) {
		tv[ch] = dimtint ? g_tint[ch] : 1.0f;
		dgv[ch] = dimtint ? c->v[SQ_DIM_RED + ch] : 1.0;
	}
	if (!M.have_t0) {
		M.t0 = now;
		M.have_t0 = true;
	}
	double t_rel = now - M.t0;
	if (!enabled) {
		M.rs_valid = false;
		M.was_enabled = false;
		return;
	}
	if (!bright) {
		M.ds_valid = M.rs_valid = M.prev_valid = false;
		M.was_enabled = false;
	} else {
		if (!M.was_enabled) {
			M.ds_valid = M.rs_valid = M.prev_valid = false;
		}
		M.was_enabled = true;
	}
	double used[7] = {c->v[SQ_TARGET], c->v[SQ_MIN_GAIN], c->v[SQ_KNEE], c->v[SQ_FALLOFF_PX],
	                  c->v[SQ_TARGET_LARGE], c->v[SQ_SIZE_PX], c->v[SQ_SIZE_BIAS]};
	if (M.have_used && memcmp(used, M.used, sizeof used) != 0) {
		M.prev_valid = false;  // a settings change must not look like a scene change
	}
	memcpy(M.used, used, sizeof used);
	M.have_used = true;

	double *D = bright ? cpu_D(c) : calloc((size_t)W * H, sizeof *D);
	double mg = c->v[SQ_MIN_GAIN];
	double *RM = NULL, *RS = NULL;
	if (ramp || smoothing) {
		RM = cpu_means(D, c);
		if (M.prev_valid) {
			double dd = smoothing ? 5.0 * c->v[SQ_DARKENING_MS] / 1000.0 + 0.1 : 0;
			double db = ramp ? 5.0 * c->v[SQ_BRIGHTENING_MS] / 1000.0 + 0.1 : 0;
			for (int m = 0; m < nmons; m++) {
				struct lrbox b = lr_box_of(m);
				int changed = 0;
				for (int gy = 0; gy < 32; gy++)
					for (int gx = 0; gx < 32; gx++) {
						int qx = b.x0 + (int)((gx + 0.5) / 32.0 * (b.x1 - b.x0));
						int qy = b.y0 + (int)((gy + 0.5) / 32.0 * (b.y1 - b.y0));
						double n = RM[(qy * lw + qx) * 2], pv = M.prev[(qy * lw + qx) * 2];
						if (fabs(n - pv) > fmax(0.03, 0.25 * fmax(n, pv))) changed++;
					}
				if (changed / 1024.0 > c->v[SQ_SCENE_PCT] / 100.0) {
					if (dd > 0) M.until[m][0] = t_rel + dd;
					if (db > 0) M.until[m][1] = t_rel + db;
				}
			}
		}
		if (!M.prev) M.prev = malloc((size_t)lw * lh * 2 * sizeof *M.prev);
		memcpy(M.prev, RM, (size_t)lw * lh * 2 * sizeof *M.prev);
		M.prev_valid = true;
	} else {
		M.prev_valid = false;
	}

	// Smoothed dimming amount: instant attack, slow release, only while a monitor is armed.
	if (!M.ds) M.ds = calloc((size_t)W * H, sizeof *M.ds);
	if (smoothing) {
		double dt = M.have_smooth && now > M.last_smooth ? now - M.last_smooth : 0;
		double k = 1 - exp(-dt / (c->v[SQ_DARKENING_MS] / 1000.0));
		for (int m = 0; m < nmons; m++) {
			bool armed = t_rel < M.until[m][0];
			for (int y = mons[m].y; y < mons[m].y + mons[m].h; y++)
				for (int x = mons[m].x; x < mons[m].x + mons[m].w; x++) {
					int i = y * W + x;
					double n = D[i];
					M.ds[i] = !M.ds_valid ? n : (armed ? fmax(n, M.ds[i] + (n - M.ds[i]) * k) : n);
				}
		}
		M.ds_valid = true;
		M.last_smooth = now;
		M.have_smooth = true;
	} else {
		memcpy(M.ds, D, (size_t)W * H * sizeof *D);
		M.ds_valid = false;
	}
	free(D);

	if (ramp) {
		if (!M.rs) {
			M.rs = malloc((size_t)lw * lh * 2 * sizeof *M.rs);
			for (int i = 0; i < lw * lh * 2; i++) M.rs[i] = -2.0;
		}
		double dt = M.have_ramp && now > M.last_ramp ? now - M.last_ramp : 0;
		double k = 1 - exp(-dt / (c->v[SQ_BRIGHTENING_MS] / 1000.0));
		RS = malloc((size_t)lw * lh * 2 * sizeof *RS);
		memcpy(RS, M.rs, (size_t)lw * lh * 2 * sizeof *RS);
		for (int m = 0; m < nmons; m++) {
			struct lrbox b = lr_box_of(m);
			bool armed = t_rel < M.until[m][1];
			for (int y = b.y0; y < b.y1; y++)
				for (int x = b.x0; x < b.x1; x++) {
					int o = (y * lw + x) * 2;
					if (!M.rs_valid || !armed) {
						RS[o] = RM[o];
						RS[o + 1] = RM[o + 1];
					} else {
						double pb = M.rs[o], pa = M.rs[o + 1];
						RS[o] = RM[o] > pb ? pb + (RM[o] - pb) * k : RM[o];
						RS[o + 1] = RM[o + 1] + (pa - RM[o + 1]) * (1 - k);
					}
				}
		}
		memcpy(M.rs, RS, (size_t)lw * lh * 2 * sizeof *RS);
		M.rs_valid = true;
		M.last_ramp = now;
		M.have_ramp = true;
	} else {
		M.rs_valid = false;
	}

	for (int m = 0; m < nmons; m++) {
		struct lrbox b = lr_box_of(m);
		for (int y = mons[m].y; y < mons[m].y + mons[m].h; y++)
			for (int x = mons[m].x; x < mons[m].x + mons[m].w; x++) {
				double d = M.ds[(size_t)y * W + x];
				bool tinted = tv[0] != 1.0f || tv[1] != 1.0f || tv[2] != 1.0f;
				if (d < 0.002) {
					for (int ch = 0; ch < 3 && tinted; ch++) {
						double v = img[((size_t)y * W + x) * 4 + ch] / 255.0;
						exp_img[((size_t)y * W + x) * 4 + ch] = (uint8_t)lround(255 * enc(dec(v) * tv[ch]));
					}
					continue;
				}
				double g = 1 - d;
				double a = (1 - mg) > 1e-4 ? fmin(fmax(d / (1 - mg), 0), 1) : 0;
				double strength = a;
				if (ramp) {
					double mm[2], st[2];
					lr_sample(RM, b, x, y, mm);
					lr_sample(RS, b, x, y, st);
					double w = fmin(fmax(d / 0.1, 0), 1);
					double sc = mm[0] > 1e-4 ? fmin(fmax(st[0] / mm[0], 0), 1) : 1;
					g *= 1 - (1 - sc) * w;
					a = fmin(fmax(a + (st[1] - mm[1]) * w, 0), 1);
				}
				for (int ch = 0; ch < 3; ch++) {
					double e = pow(img[((size_t)y * W + x) * 4 + ch] / 255.0, 1 + (c->v[SQ_GAMMA] - 1) * a);
					double dg = dgv[ch];
					double lin = dec(e) * g * (1 + (dg - 1) * strength) * tv[ch];
					exp_img[((size_t)y * W + x) * 4 + ch] = (uint8_t)lround(255 * enc(lin));
				}
			}
	}
	if (c->v[SQ_STATIC] != 0 && c->v[SQ_STATIC_AMOUNT] > 0) {
		double hz = c->v[SQ_STATIC_HZ];
		uint32_t frame = (uint32_t)(unsigned long long)floor(t_rel * hz);
		float amt = (float)(c->v[SQ_STATIC_AMOUNT] / 100.0);
		for (int m = 0; m < nmons; m++)
			for (int y = mons[m].y; y < mons[m].y + mons[m].h; y++)
				for (int x = mons[m].x; x < mons[m].x + mons[m].w; x++) {
					float n[3];
					cpu_static(c, x + 0.5, H - y - 0.5, frame, n);
					for (int ch = 0; ch < 3; ch++) {
						uint8_t *px = &exp_img[((size_t)y * W + x) * 4 + ch];
						float v = *px / 255.0f;
						*px = (uint8_t)lroundf(255.0f * (v + (n[ch] - v) * amt));
					}
				}
	}
	free(RM);
	free(RS);
}

// Compares the whole of every monitor against the model's expected image. Pixels outside
// every monitor must equal what was written (they are never redrawn).
static void compare_all(const char *what, const struct sq_cfg *c, double unused) {
	(void)c;
	(void)unused;
	int bad = 0, worst = 0, bx = -1, by = -1;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			for (int ch = 0; ch < 3; ch++) {
				int want = exp_img[((size_t)y * W + x) * 4 + ch];
				int diff = abs(at(x, y)[ch] - want);
				if (diff > 3) {
					bad++;
					if (diff > worst) worst = diff, bx = x, by = y;
				}
			}
	CHECK(bad == 0, "%s: %d channel values off by more than 3 (worst %d at %d,%d)", what, bad, worst, bx, by);
}

// ---- scenarios --------------------------------------------------------------------------

static void test_cone(void) {
	printf("cone falloff against the brute force model\n");
	struct sq_cfg c = base_cfg();
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	put((struct pl_rect){0, 0, 96, 64}, 40, 40, 40);
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);  // asymmetric: top left of center
	put((struct pl_rect){70, 50, 74, 52}, 220, 220, 220);  // a small light-gray patch
	capture_full(0);
	render(&c, 0);
	compare_all("falloff 8", &c, 0);
	double full = enc(0.45);
	expect_px("inside the white block", 28, 18, (int)lround(255 * full), 2);
	expect_px("far dark is bit identical", 5, 60, 40, 0);
	expect_px("beyond the falloff is bit identical", 20 - 10, 18, 40, 0);
	CHECK(at(19, 18)[0] < 40, "pixel touching the block is dimmed, got %d", at(19, 18)[0]);
	CHECK(at(19, 18)[0] <= at(17, 18)[0] || at(17, 18)[0] <= 40, "gradient is monotonic");

	for (int R = 0; R <= 40; R += (R < 8 ? 4 : 16)) {
		c.v[SQ_FALLOFF_PX] = R;
		render(&c, 0.1);
		char name[48];
		snprintf(name, sizeof name, "falloff %d", R);
		compare_all(name, &c, 0);
	}
	teardown();
}

static void test_zero_falloff(void) {
	printf("falloff 0 dims only the bright pixels\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_FALLOFF_PX] = 0;
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	put((struct pl_rect){0, 0, 96, 64}, 40, 40, 40);
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);
	capture_full(0);
	render(&c, 0);
	expect_px("white dimmed", 28, 18, (int)lround(255 * enc(0.45)), 2);
	expect_px("pixel right next to it untouched", 36, 18, 40, 0);
	expect_px("pixel above it untouched", 28, 9, 40, 0);
	compare_all("falloff 0", &c, 0);
	teardown();
}

static void test_monitors(void) {
	printf("dimming does not cross a monitor edge; NPOT root, unaligned monitors\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_FALLOFF_PX] = 12;
	struct pl_mon m[] = {{7, 5, 50, 40}, {57, 5, 30, 55}};  // adjacent, different sizes
	setup(100, 70, m, 2);
	put((struct pl_rect){0, 0, 100, 70}, 255, 255, 255);  // white outside the monitors
	put((struct pl_rect){7, 5, 87, 60}, 40, 40, 40);
	put((struct pl_rect){40, 20, 57, 35}, 255, 255, 255);  // touches monitor 1's right edge
	capture_full(0);
	render(&c, 0);
	compare_all("two monitors", &c, 0);
	expect_px("outside every monitor is untouched", 2, 2, 255, 0);
	expect_px("beside the white block, other monitor", 58, 27, 40, 0);
	teardown();
}

static void test_gamma_and_enable(void) {
	printf("gamma, enabled, parameter changes without a new frame\n");
	struct sq_cfg c = base_cfg();
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	put((struct pl_rect){0, 0, 96, 64}, 40, 40, 40);
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);
	put((struct pl_rect){60, 30, 80, 50}, 200, 200, 200);
	capture_full(0);
	for (double gm = 0.7; gm < 2.6; gm += 0.9) {
		c.v[SQ_GAMMA] = gm;
		render(&c, 0);
		char name[40];
		snprintf(name, sizeof name, "gamma %.1f", gm);
		compare_all(name, &c, 0);
	}
	c.v[SQ_GAMMA] = 1;
	c.v[SQ_ENABLED] = 0;
	render(&c, 0.1);
	expect_px("disabled copies white", 28, 18, 255, 0);
	expect_px("disabled copies dark", 5, 60, 40, 0);
	c.v[SQ_ENABLED] = 1;
	render(&c, 0.2);
	compare_all("re-enabled", &c, 0);
	c.v[SQ_TARGET] = 0.25;  // no new frame: the parameters alone must trigger a recompute
	render(&c, 0.3);
	compare_all("target changed", &c, 0);
	c.v[SQ_KNEE] = 0.3;
	c.v[SQ_MIN_GAIN] = 0.1;
	c.v[SQ_FALLOFF_PX] = 20;
	render(&c, 0.4);
	compare_all("knee, min_gain, falloff changed", &c, 0);
	teardown();
}

static void test_smoothing(void) {
	printf("attack is instant; release follows exp(-dt/tau) after a scene change only\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_DARKENING_MS] = 300;
	c.v[SQ_FALLOFF_PX] = 0;
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	put((struct pl_rect){0, 0, 96, 64}, 40, 40, 40);
	capture_full(0);
	render(&c, 0);
	expect_px("dark before", 28, 18, 40, 0);
	struct pl_rect big = {8, 6, 88, 58};  // two thirds of the monitor
	put(big, 255, 255, 255);
	capture(big, 0.01);
	render(&c, 0.01);
	double Dw = need_for(1.0, &c, c.v[SQ_TARGET]);
	expect_px("white dimmed in the call that first shows it", 28, 18, apply(255, Dw, &c), 2);

	put((struct pl_rect){0, 0, 96, 64}, 40, 40, 40);
	capture_full(0.31);
	render(&c, 0.31);  // dt = 0.3 = one tau since the last smoothing step at 0.01
	expect_px("1 tau after the white left", 28, 18, apply(40, Dw * exp(-1.0), &c), 3);
	render(&c, 0.61);  // no new frame, smoothing only
	expect_px("2 tau, no new frame", 28, 18, apply(40, Dw * exp(-2.0), &c), 3);
	render(&c, 5.0);
	expect_px("fully released", 28, 18, 40, 0);

	CHECK(pl_active(&pl, &c, 0.5), "active shortly after input");
	CHECK(!pl_active(&pl, &c, 10.0), "inactive long after input");
	c.v[SQ_DARKENING_MS] = 0;
	CHECK(!pl_active(&pl, &c, 0.5), "with darkening 0 only the short grace period is active");
	teardown();
}

static void fill_dark(void) {
	put((struct pl_rect){0, 0, W, H}, 40, 40, 40);
}

static void test_size(void) {
	printf("target falls with the size and brightness of the surrounding bright area\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_TARGET_LARGE] = 0.15;
	c.v[SQ_MIN_GAIN] = 0.05;  // below target_large, so the floor does not hide the large target
	c.v[SQ_SIZE_PX] = 24;
	c.v[SQ_FALLOFF_PX] = 0;
	struct pl_mon m[] = {{0, 0, 128, 96}};
	setup(128, 96, m, 1);
	fill_dark();
	put((struct pl_rect){10, 10, 14, 14}, 255, 255, 255);  // small bright patch
	put((struct pl_rect){60, 24, 108, 72}, 255, 255, 255);  // large bright block
	capture_full(0);
	render(&c, 0);
	compare_all("size on, bias 2", &c, 0);
	int small = (int)lround(255 * enc(0.45)), big = (int)lround(255 * enc(0.15));
	expect_px("small patch keeps the small target", 11, 11, small, 3);
	expect_px("large block interior takes the large target", 84, 48, big, 3);
	CHECK(at(84, 48)[0] < at(11, 11)[0], "large block is dimmer than the small patch");

	// A dimmer patch of the same size counts as smaller, so it keeps more of its brightness.
	put((struct pl_rect){60, 24, 108, 72}, 160, 160, 160);
	capture((struct pl_rect){60, 24, 108, 72}, 0.1);
	render(&c, 0.1);
	compare_all("dimmer block", &c, 0);

	for (double bias = 1.0; bias <= 3.01; bias += 1.0) {
		put((struct pl_rect){60, 24, 108, 72}, 255, 255, 255);
		capture((struct pl_rect){60, 24, 108, 72}, 0.2);  // only what picom rewrote
		c.v[SQ_SIZE_BIAS] = bias;
		render(&c, 0.2);  // a parameter change alone must trigger the recompute
		char name[40];
		snprintf(name, sizeof name, "size_bias %.0f", bias);
		compare_all(name, &c, 0);
	}
	c.v[SQ_SIZE_PX] = 60;
	render(&c, 0.3);
	compare_all("size_px changed", &c, 0);
	c.v[SQ_TARGET_LARGE] = 0.30;
	c.v[SQ_FALLOFF_PX] = 10;
	render(&c, 0.4);
	compare_all("target_large and falloff changed", &c, 0);
	c.v[SQ_TARGET_LARGE] = c.v[SQ_TARGET];  // equal targets: the feature switches off
	render(&c, 0.5);
	compare_all("size feature off", &c, 0);
	expect_px("block dimmed to the small target when off", 84, 48, small, 3);
	teardown();

	printf("size field across two adjacent monitors and an unaligned NPOT monitor\n");
	struct sq_cfg c2 = base_cfg();
	c2.v[SQ_TARGET_LARGE] = 0.15;
	c2.v[SQ_SIZE_PX] = 24;
	c2.v[SQ_FALLOFF_PX] = 6;
	struct pl_mon two[] = {{0, 0, 64, 96}, {64, 0, 64, 96}};
	setup(128, 96, two, 2);
	fill_dark();
	put((struct pl_rect){40, 20, 90, 70}, 255, 255, 255);  // straddles the shared edge
	capture_full(0);
	render(&c2, 0);
	compare_all("adjacent monitors", &c2, 0);
	// A large bright block ends at the shared edge and a small patch touches the edge on the
	// other side. Neither monitor's size measure may leak into the other.
	fill_dark();
	put((struct pl_rect){10, 20, 64, 76}, 255, 255, 255);
	put((struct pl_rect){64, 40, 68, 44}, 255, 255, 255);
	capture_full(0.1);
	render(&c2, 0.1);
	compare_all("block and patch on either side of the edge", &c2, 0);
	expect_px("patch keeps the small target", 65, 41, (int)lround(255 * enc(0.45)), 4);
	teardown();
	struct pl_mon odd[] = {{7, 5, 60, 41}};
	setup(100, 70, odd, 1);
	fill_dark();
	put((struct pl_rect){20, 12, 60, 40}, 255, 255, 255);
	capture_full(0);
	render(&c2, 0);
	compare_all("NPOT root, unaligned monitor", &c2, 0);
	put((struct pl_rect){7, 5, 67, 46}, 255, 255, 255);  // bright all the way to the edges
	capture((struct pl_rect){7, 5, 67, 46}, 0.1);
	render(&c2, 0.1);
	compare_all("bright up to the monitor edges", &c2, 0);
	teardown();
}

// New content that would be brighter is shown at the brightness and gamma strength that was
// displayed before, and that level rises to the new one with time constant rise. Only blurred
// values carry over, so the layout of the old content must not show in the new content.
static void test_rise(void) {
	printf("rise: new content starts at the old level and rises, with no burn-in\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_BRIGHTENING_MS] = 200;
	c.v[SQ_MIN_GAIN] = 0.05;
	c.v[SQ_SIZE_PX] = 24;
	c.v[SQ_FALLOFF_PX] = 0;
	c.v[SQ_GAMMA] = 1.6;
	struct pl_mon m[] = {{0, 0, 128, 96}};
	setup(128, 96, m, 1);
	fill_dark();
	capture_full(0);
	render(&c, 0);
	compare_all("first frame has no history", &c, 0);
	double gt = 1 - need_for(1.0, &c, c.v[SQ_TARGET]);
	int settled = apply(255, 1 - gt, &c);

	// A large white block replaces dark content: it must start near the dark level.
	struct pl_rect blk = {40, 20, 100, 76};
	put(blk, 255, 255, 255);
	capture(blk, 0.01);
	double times[] = {0.01, 0.05, 0.1, 0.2, 0.4, 1.0, 3.0};
	int last = 0;
	for (int i = 0; i < 7; i++) {
		render(&c, times[i]);
		char name[48];
		snprintf(name, sizeof name, "rise, white block at t=%.2f", times[i]);
		compare_all(name, &c, 0);
		if (i == 0) {
			CHECK(at(70, 48)[0] < settled / 3, "block starts near the old dark level, got %d vs settled %d",
			      at(70, 48)[0], settled);
		}
		CHECK(at(70, 48)[0] >= last, "brightness only rises, got %d after %d", at(70, 48)[0], last);
		last = at(70, 48)[0];
	}
	expect_px("settled at the target", 70, 48, settled, 3);
	expect_px("dark area untouched throughout", 5, 90, 40, 0);

	// New content that is dimmer than before shows at once, and the displayed level falls
	// with it, so brightening again starts from the dimmer level, not the old high one.
	put(blk, 160, 160, 160);
	capture(blk, 3.1);
	render(&c, 3.1);
	compare_all("dimmer content shows at once", &c, 0);
	put(blk, 255, 255, 255);
	capture(blk, 3.12);
	render(&c, 3.12);
	compare_all("brightening again starts from the dimmer level", &c, 0);
	render(&c, 3.5);
	compare_all("and then rises", &c, 0);

	// The faint tail of the falloff is only partly affected by the ramp.
	struct sq_cfg tail = c;
	tail.v[SQ_FALLOFF_PX] = 14;
	setup(128, 96, m, 1);
	fill_dark();
	capture_full(0);
	render(&tail, 0);
	put(blk, 255, 255, 255);
	capture(blk, 0.01);
	double tt[] = {0.01, 0.1, 0.5};
	for (int i = 0; i < 3; i++) {
		render(&tail, tt[i]);
		char name[48];
		snprintf(name, sizeof name, "falloff tail under the ramp, t=%.2f", tt[i]);
		compare_all(name, &tail, 0);
	}
	CHECK(at(36, 48)[0] < 40, "a pixel in the falloff tail is dimmed below its original 40: %d", at(36, 48)[0]);

	// No burn-in: the old content has fine stripes, the new content is a plain white page.
	setup(128, 96, m, 1);
	put((struct pl_rect){0, 0, 128, 96}, 255, 255, 255);
	for (int x = 0; x < 128; x += 8) {
		put((struct pl_rect){x, 0, x + 1, 96}, 0, 0, 0);
	}
	capture_full(0);
	for (int i = 0; i <= 10; i++) {
		render(&c, 0.05 * i);  // let the ramp state settle on the striped page
	}
	put((struct pl_rect){0, 0, 128, 96}, 255, 255, 255);
	capture((struct pl_rect){0, 0, 128, 96}, 0.6);
	render(&c, 0.61);
	compare_all("striped page replaced by a plain page", &c, 0);
	int worst = 0;
	for (int y = 10; y < 86; y += 5)
		for (int x = 20; x < 100; x++) {
			int d = abs(at(x, y)[0] - at(x + 1, y)[0]);
			if (d > worst) worst = d;
		}
	CHECK(worst <= 3, "plain page shows no trace of the old stripes: neighboring pixels differ by %d", worst);

	// rise 0 shows content at once; toggling it resets the state.
	c.v[SQ_BRIGHTENING_MS] = 0;
	setup(128, 96, m, 1);
	fill_dark();
	capture_full(0);
	render(&c, 0);
	put(blk, 255, 255, 255);
	capture(blk, 0.01);
	render(&c, 0.01);
	compare_all("rise 0", &c, 0);
	expect_px("rise 0 is instant", 70, 48, settled, 3);
	c.v[SQ_BRIGHTENING_MS] = 200;
	render(&c, 0.02);
	compare_all("rise switched on", &c, 0);
	expect_px("switching rise on starts from the present", 70, 48, settled, 3);
	teardown();

	// Two monitors, tau and rise together.
	struct sq_cfg c2 = c;
	c2.v[SQ_DARKENING_MS] = 150;
	struct pl_mon two[] = {{0, 0, 64, 96}, {64, 0, 64, 96}};
	setup(128, 96, two, 2);
	fill_dark();
	capture_full(0);
	render(&c2, 0);
	put((struct pl_rect){40, 20, 100, 76}, 255, 255, 255);
	capture((struct pl_rect){40, 20, 100, 76}, 0.01);
	double t2[] = {0.01, 0.1, 0.3, 1.0};
	for (int i = 0; i < 4; i++) {
		render(&c2, t2[i]);
		char name[48];
		snprintf(name, sizeof name, "two monitors, tau and rise, t=%.2f", t2[i]);
		compare_all(name, &c2, 0);
	}
	fill_dark();
	capture_full(1.1);
	render(&c2, 1.1);
	compare_all("two monitors, content leaves", &c2, 0);
	// A large block ends at the shared edge and a small patch touches it from the other side
	// while both ramp: neither monitor's ramp state may leak into the other.
	struct sq_cfg c3 = c;
	setup(128, 96, two, 2);
	fill_dark();
	capture_full(0);
	render(&c3, 0);
	put((struct pl_rect){10, 20, 64, 76}, 255, 255, 255);
	put((struct pl_rect){64, 40, 68, 44}, 255, 255, 255);
	capture_full(0.01);
	double t3[] = {0.01, 0.1, 0.4};
	for (int i = 0; i < 3; i++) {
		render(&c3, t3[i]);
		char name[56];
		snprintf(name, sizeof name, "ramp across a shared edge, t=%.2f", t3[i]);
		compare_all(name, &c3, 0);
	}
	teardown();
}

static void test_color(void) {
	printf("channel gains where dimming is strong, and per-monitor tints\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_DIM_RED] = 0.85;
	c.v[SQ_DIM_BLUE] = 0.5;
	c.v[SQ_FALLOFF_PX] = 8;
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	fill_dark();
	put((struct pl_rect){20, 10, 40, 30}, 255, 255, 255);
	put((struct pl_rect){60, 36, 80, 56}, 200, 200, 200);
	capture_full(0);
	render(&c, 0);
	compare_all("dim_red and dim_blue", &c, 0);
	CHECK(at(30, 20)[1] > at(30, 20)[0] && at(30, 20)[0] > at(30, 20)[2],
	      "green untouched, red lowered a little, blue lowered most: %d,%d,%d", at(30, 20)[0],
	      at(30, 20)[1], at(30, 20)[2]);
	expect_px("far dark stays bit identical with no tint", 5, 60, 40, 0);
	c.v[SQ_DIM_RED] = c.v[SQ_DIM_BLUE] = 1.0;
	c.v[SQ_DIM_GREEN] = 0.6;
	render(&c, 0.1);
	compare_all("dim_green only", &c, 0);

	// A whole-screen tint applies to every monitor, even where nothing is dimmed.
	c.v[SQ_DIM_GREEN] = 1.0;
	struct pl_mon two[] = {{0, 0, 64, 64}, {64, 0, 64, 64}};
	setup(128, 64, two, 2);
	fill_dark();
	put((struct pl_rect){10, 10, 30, 30}, 255, 255, 255);
	put((struct pl_rect){80, 10, 100, 30}, 255, 255, 255);
	capture_full(0);
	set_tint(1.0f, 0.8f, 0.5f);
	render(&c, 0);
	compare_all("global tint on two monitors", &c, 0);
	for (int x = 50; x <= 110; x += 60) {
		CHECK(at(x, 55)[2] < at(x, 55)[0], "dark pixel at x=%d is tinted toward amber: %d,%d,%d", x,
		      at(x, 55)[0], at(x, 55)[1], at(x, 55)[2]);
	}

	set_tint(1.0f, 1.0f, 1.0f);
	render(&c, 0.1);  // a tint change alone, with no new frame
	compare_all("tint removed", &c, 0);
	expect_px("untinted pixels are bit identical", 110, 55, 40, 0);
	set_tint(0.9f, 1.0f, 1.0f);
	render(&c, 0.15);
	compare_all("a different tint", &c, 0);

	c.v[SQ_ENABLED] = 0;
	render(&c, 0.2);
	compare_all("disabled switches the tint off too", &c, 0);
	expect_px("disabled copies the dark pixel", 50, 55, 40, 0);
	c.v[SQ_ENABLED] = 1;

	// Tint together with the ramp and tau.
	c.v[SQ_BRIGHTENING_MS] = 200;
	c.v[SQ_DARKENING_MS] = 100;
	c.v[SQ_SIZE_PX] = 24;
	render(&c, 0.3);
	fill_dark();
	capture_full(0.31);
	render(&c, 0.31);
	put((struct pl_rect){10, 10, 50, 50}, 255, 255, 255);
	capture((struct pl_rect){10, 10, 50, 50}, 0.32);
	double tt[] = {0.32, 0.4, 0.6, 1.5};
	for (int i = 0; i < 4; i++) {
		render(&c, tt[i]);
		char name[48];
		snprintf(name, sizeof name, "tint with ramp and tau, t=%.2f", tt[i]);
		compare_all(name, &c, 0);
	}
	teardown();
}

// The config format: plain keys, clamping, and a save and load round trip.
static void test_config(void) {
	printf("config: keys, clamping, round trip\n");
	struct sq_cfg c;
	sq_defaults(&c);
	CHECK(sq_set(&c, "target", "0.5") && c.v[SQ_TARGET] == 0.5, "plain key");
	CHECK(!sq_set(&c, "bogus", "1"), "unknown key is rejected");
	CHECK(!sq_set(&c, "target", "abc"), "bad value is rejected");
	CHECK(!sq_set(&c, "tint_red.DP-5", "0.5"), "monitor suffixes are not supported");
	CHECK(sq_set(&c, "tint_red", "5") && c.v[SQ_TINT_RED] == 1.0, "clamped to the maximum");
	CHECK(sq_set(&c, "tint_red", "0") && fabs(c.v[SQ_TINT_RED] - 0.2) < 1e-9, "clamped to the minimum");
	CHECK(sq_set(&c, "tint_blue", "0.7") && sq_set(&c, "dim_green", "0.6"), "tint and channel gain keys");
	const char *path = "/tmp/squint-config-test.conf";
	CHECK(sq_save(path, &c) == 0, "save");
	struct sq_cfg d;
	sq_load(path, &d);
	CHECK(fabs(d.v[SQ_TINT_BLUE] - 0.7) < 0.006 && fabs(d.v[SQ_DIM_GREEN] - 0.6) < 0.006 &&
	          fabs(d.v[SQ_TARGET] - 0.5) < 1e-9,
	      "values round trip");
	FILE *f = fopen(path, "a");
	fputs("tint_red.DP-5 = 0.5\nunknown = 3\n", f);  // old per-monitor lines are ignored
	fclose(f);
	sq_load(path, &d);
	CHECK(fabs(d.v[SQ_TINT_BLUE] - 0.7) < 0.006 && d.v[SQ_TINT_RED] > 0.19 && d.v[SQ_TINT_RED] < 0.21,
	      "per-monitor lines in an old file are ignored");
	unlink(path);
}

// Darkening and brightening smoothing apply only after a scene change: more than a third of
// a monitor's blurred brightness changing significantly. Small changes, scrolling, and
// settings changes must show at once.
static void test_gate(void) {
	printf("smoothing applies only after a scene change\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_BRIGHTENING_MS] = 400;
	c.v[SQ_DARKENING_MS] = 400;
	c.v[SQ_MIN_GAIN] = 0.05;
	c.v[SQ_SIZE_PX] = 24;
	c.v[SQ_FALLOFF_PX] = 0;
	struct pl_mon m[] = {{0, 0, 128, 96}};
	setup(128, 96, m, 1);
	fill_dark();
	capture_full(0);
	render(&c, 0);
	double Dw = need_for(1.0, &c, c.v[SQ_TARGET]);
	int settled = apply(255, Dw, &c);

	// A small bright patch appears and goes away: both show at once.
	struct pl_rect patch = {50, 40, 62, 52};
	put(patch, 255, 255, 255);
	capture(patch, 0.01);
	render(&c, 0.01);
	compare_all("small patch appears", &c, 0);
	expect_px("small patch shows at once (no brightening ramp)", 56, 46, settled, 3);
	put(patch, 40, 40, 40);
	capture(patch, 0.02);
	render(&c, 0.02);
	compare_all("small patch leaves", &c, 0);
	expect_px("small patch releases at once (no darkening fade)", 56, 46, 40, 0);

	// Scrolling: a page with gray stripes moves 3 px per frame. Every frame shows at once.
	setup(128, 96, m, 1);
	double t = 0;
	for (int frame = 0; frame < 14; frame++) {
		put((struct pl_rect){0, 0, 128, 96}, 255, 255, 255);
		for (int x = -8 + (frame * 3) % 8; x < 128; x += 8) {
			int x1 = x < 0 ? 0 : x, x2 = x + 2 > 128 ? 128 : x + 2;
			if (x2 > x1) put((struct pl_rect){x1, 0, x2, 96}, 40, 40, 40);
		}
		capture_full(t);
		render(&c, t);
		char name[48];
		snprintf(name, sizeof name, "scrolling page, frame %d", frame);
		compare_all(name, &c, 0);
		if (frame > 0) {
			int x = (frame * 3) % 8 + 4;  // a white column, and a column that was white last frame
			expect_px("white shows at once", 8 * 4 + x % 8 + 2, 40, settled, 3);
			int gx = -8 + (frame * 3) % 8 + 8 * 6;  // a gray stripe's left pixel
			expect_px("a stripe is never held dim by the old white", gx, 40, 40, 0);
		}
		t += 0.05;
	}

	// A settings change on a static page is not a scene change.
	setup(128, 96, m, 1);
	put((struct pl_rect){0, 0, 128, 96}, 255, 255, 255);
	c.v[SQ_TARGET_LARGE] = 1.0;  // keep the size feature off while the target moves
	c.v[SQ_TARGET] = 0.25;
	capture_full(0);
	render(&c, 0);
	c.v[SQ_TARGET] = 0.6;
	render(&c, 0.05);
	compare_all("target raised on a static page", &c, 0);
	expect_px("the new target shows at once", 64, 48, apply(255, need_for(1.0, &c, 0.6), &c), 3);

	// A scene change on one monitor arms only that monitor.
	struct sq_cfg c2 = base_cfg();
	c2.v[SQ_BRIGHTENING_MS] = 400;
	c2.v[SQ_MIN_GAIN] = 0.05;
	c2.v[SQ_SIZE_PX] = 24;
	c2.v[SQ_FALLOFF_PX] = 0;
	struct pl_mon two[] = {{0, 0, 64, 96}, {64, 0, 64, 96}};
	setup(128, 96, two, 2);
	fill_dark();
	capture_full(0);
	render(&c2, 0);
	put((struct pl_rect){4, 4, 60, 92}, 255, 255, 255);  // most of monitor 0
	put((struct pl_rect){90, 40, 100, 50}, 255, 255, 255);  // a small patch on monitor 1
	capture_full(0.01);
	render(&c2, 0.01);
	compare_all("scene change on monitor 0 only, t=0.01", &c2, 0);
	CHECK(at(30, 48)[0] < settled / 3, "monitor 0 ramps up from dark: %d vs %d", at(30, 48)[0], settled);
	expect_px("monitor 1 patch shows at once", 95, 45, settled, 3);
	render(&c2, 0.3);
	compare_all("scene change on monitor 0 only, t=0.30", &c2, 0);
	// A later small change on monitor 1 while monitor 0 is still armed must show at once.
	put((struct pl_rect){70, 10, 80, 20}, 255, 255, 255);
	capture((struct pl_rect){70, 10, 80, 20}, 0.4);
	render(&c2, 0.4);
	compare_all("a small change on monitor 1 while monitor 0 is armed", &c2, 0);
	expect_px("monitor 1 is not armed by monitor 0", 75, 15, settled, 3);
	render(&c2, 3.0);
	expect_px("monitor 0 settles", 30, 48, settled, 3);
	teardown();

	// A change that is significant but covers well under a third of the monitor is not a
	// scene change.
	struct sq_cfg c3 = base_cfg();
	c3.v[SQ_BRIGHTENING_MS] = 400;
	c3.v[SQ_MIN_GAIN] = 0.05;
	c3.v[SQ_SIZE_PX] = 16;
	c3.v[SQ_FALLOFF_PX] = 0;
	setup(128, 96, m, 1);
	fill_dark();
	capture_full(0);
	render(&c3, 0);
	struct pl_rect mid = {20, 20, 60, 50};
	put(mid, 255, 255, 255);
	capture(mid, 0.01);
	render(&c3, 0.01);
	compare_all("a moderate block", &c3, 0);
	expect_px("a moderate block shows at once", 40, 35, settled, 3);
	teardown();

	// The same block counts as a scene change when the percentage is lowered, and not when it
	// is raised or set to 100.
	struct sq_cfg c4 = c3;
	c4.v[SQ_SCENE_PCT] = 5;
	setup(128, 96, m, 1);
	fill_dark();
	capture_full(0);
	render(&c4, 0);
	put(mid, 255, 255, 255);
	capture(mid, 0.01);
	render(&c4, 0.01);
	compare_all("the same block with scene_pct 5", &c4, 0);
	CHECK(at(40, 35)[0] < settled / 3, "the block ramps up from dark with scene_pct 5: %d vs %d",
	      at(40, 35)[0], settled);
	render(&c4, 3.0);
	expect_px("and then settles", 40, 35, settled, 3);
	teardown();
	for (int pct = 60; pct <= 100; pct += 40) {
		struct sq_cfg c5 = c3;
		c5.v[SQ_SCENE_PCT] = pct;
		setup(128, 96, m, 1);
		put((struct pl_rect){0, 0, 128, 96}, 40, 40, 40);
		capture_full(0);
		render(&c5, 0);
		put((struct pl_rect){2, 2, 126, 94}, 255, 255, 255);  // nearly the whole monitor
		capture((struct pl_rect){2, 2, 126, 94}, 0.01);
		render(&c5, 0.01);
		char name[48];
		snprintf(name, sizeof name, "nearly the whole monitor, scene_pct %d", pct);
		compare_all(name, &c5, 0);
		if (pct == 100) {
			expect_px("scene_pct 100 never arms", 64, 48, settled, 3);
		} else {
			CHECK(at(64, 48)[0] < settled / 3, "scene_pct %d still arms for a full-screen change: %d", pct,
			      at(64, 48)[0]);
		}
		teardown();
	}
}

// Static noise: exact against the CPU model, and statistics that follow the description of
// the video: gray, fresh independent frames, 1 to 3 pixel speckles with some clusters, a mean
// near middle gray, fine scan lines, and a slight sideways wobble.
static double corr3(const uint8_t *a, const uint8_t *b, int ch) {
	double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
	int n = W * H;
	for (int i = 0; i < n; i++) {
		double x = a[i * 4 + ch], y = b[i * 4 + ch];
		sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
	}
	double cov = sab / n - (sa / n) * (sb / n);
	return cov / sqrt((saa / n - (sa / n) * (sa / n)) * (sbb / n - (sb / n) * (sb / n)));
}

// Correlation of channel ch with itself shifted by (dx, dy) inside the image.
static double shift_corr(const uint8_t *a, int ch, int dx, int dy) {
	double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
	for (int y = 0; y + dy < H; y++)
		for (int x = 0; x + dx < W; x++) {
			double u = a[((size_t)y * W + x) * 4 + ch], v = a[((size_t)(y + dy) * W + x + dx) * 4 + ch];
			sa += u; sb += v; saa += u * u; sbb += v * v; sab += u * v; n += 1;
		}
	return (sab / n - sa / n * sb / n) / sqrt((saa / n - pow(sa / n, 2)) * (sbb / n - pow(sb / n, 2)));
}

static double plane_mean(const uint8_t *a, int ch, int parity) {  // parity: -1 all, 0 even rows, 1 odd rows
	double sum = 0, n = 0;
	for (int y = 0; y < H; y++) {
		if (parity >= 0 && (H - 1 - y) % 2 != parity) continue;  // GL row parity
		for (int x = 0; x < W; x++) {
			sum += a[((size_t)y * W + x) * 4 + ch];
			n += 1;
		}
	}
	return sum / n / 255.0;
}

static void render_black_static(const struct sq_cfg *c, double t) {
	put((struct pl_rect){0, 0, W, H}, 0, 0, 0);
	capture_full(t);
	render(c, t);
}

static void test_static(void) {
	printf("static noise\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_FALLOFF_PX] = 4;
	c.v[SQ_STATIC] = 1;
	c.v[SQ_STATIC_AMOUNT] = 100;
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	fill_dark();
	put((struct pl_rect){20, 10, 50, 40}, 255, 255, 255);
	capture_full(0);

	// Off: bit exact with the plain pipeline.
	struct sq_cfg off = c;
	off.v[SQ_STATIC] = 0;
	render(&off, 0);
	compare_all("static off", &off, 0);
	expect_px("static off leaves far dark bit exact", 90, 60, 40, 0);
	struct sq_cfg zero = c;
	zero.v[SQ_STATIC_AMOUNT] = 0;
	render(&zero, 0.01);
	expect_px("amount 0 leaves far dark bit exact", 90, 60, 40, 0);

	// Every pixel against the model, for several looks and several frames.
	struct { double grain, col, smear, clusters, scan, jitter, level, contrast; } look[] = {
	    {1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.5, 0.22},
	    {2.0, 0.0, 1.3, 0.3, 0.15, 0.5, 0.5, 0.22},
	    {3.5, 0.55, 2.0, 0.6, 0.3, 1.5, 0.4, 0.3},
	    {2.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.27, 0.15},
	    {8.5, 0.0, 3.0, 0.0, 0.5, 3.0, 0.6, 0.1},
	};
	for (int li = 0; li < 5; li++)
		for (int f = 0; f < 3; f++) {
			c.v[SQ_STATIC_GRAIN_PX] = look[li].grain;
			c.v[SQ_STATIC_COLOR] = look[li].col;
			c.v[SQ_STATIC_SMEAR] = look[li].smear;
			c.v[SQ_STATIC_CLUSTERS] = look[li].clusters;
			c.v[SQ_STATIC_SCANLINES] = look[li].scan;
			c.v[SQ_STATIC_JITTER_PX] = look[li].jitter;
			c.v[SQ_STATIC_LEVEL] = look[li].level;
			c.v[SQ_STATIC_CONTRAST] = look[li].contrast;
			render(&c, 0.02 + 0.017 * f);
			char name[64];
			snprintf(name, sizeof name, "static look %d, frame %d", li, f);
			compare_all(name, &c, 0);
		}
	// A mix of content and noise, with a tint on.
	c = base_cfg();
	c.v[SQ_FALLOFF_PX] = 4;
	c.v[SQ_STATIC] = 1;
	c.v[SQ_STATIC_AMOUNT] = 25;
	set_tint(1.0f, 0.85f, 0.7f);
	render(&c, 0.3);
	compare_all("static 25% over content and a tint", &c, 0);
	set_tint(1.0f, 1.0f, 1.0f);

	// Disabled switches static off too, and static keeps the timer running while it is on.
	c.v[SQ_ENABLED] = 0;
	render(&c, 0.4);
	compare_all("disabled switches static off", &c, 0);
	expect_px("disabled copies the far dark pixel", 90, 60, 40, 0);
	c.v[SQ_ENABLED] = 1;
	CHECK(pl_active(&pl, &c, 1e6), "static keeps the timer running");
	c.v[SQ_STATIC] = 0;
	CHECK(!pl_active(&pl, &c, 1e6), "and only while it is on");
	teardown();

	// Statistics on a black screen with 100% static, as the description says.
	struct sq_cfg sc = base_cfg();
	sc.v[SQ_STATIC] = 1;
	sc.v[SQ_STATIC_AMOUNT] = 100;
	sc.v[SQ_FALLOFF_PX] = 0;  // black content needs no dimming, and falloff 0 skips the slow model
	sc.v[SQ_STATIC_SCANLINES] = 0;
	sc.v[SQ_STATIC_JITTER_PX] = 0;
	sc.v[SQ_STATIC_SMEAR] = 1;
	sc.v[SQ_STATIC_CLUSTERS] = 0;
	sc.v[SQ_STATIC_GRAIN_PX] = 2;
	struct pl_mon big[] = {{0, 0, 320, 240}};
	setup(320, 240, big, 1);
	render_black_static(&sc, 0);
	double mean = plane_mean(out, 1, -1), sd = 0;
	for (int i = 0; i < W * H; i++) sd += pow(out[i * 4 + 1] / 255.0 - mean, 2);
	sd = sqrt(sd / (W * H));
	CHECK(fabs(mean - 0.5) < 0.02 && fabs(sd - 0.22) < 0.025, "mean %.3f (want 0.5), std %.3f (want 0.22)", mean, sd);
	int gray = 1;
	for (int i = 0; i < W * H && gray; i++) gray = out[i * 4] == out[i * 4 + 1] && out[i * 4 + 1] == out[i * 4 + 2];
	CHECK(gray, "static_color 0 gives strictly gray static");
	double l1 = shift_corr(out, 1, 1, 0), l3 = shift_corr(out, 1, 3, 0), l6 = shift_corr(out, 1, 6, 0);
	CHECK(l1 > 0.4 && l1 > l3 + 0.2 && l3 > l6 - 0.05 && l6 < 0.15,
	      "speckles are 1 to 3 pixels wide: correlation %.2f at 1, %.2f at 3, %.2f at 6", l1, l3, l6);

	// Colored static, for when static_color is raised.
	struct sq_cfg col = sc;
	col.v[SQ_STATIC_COLOR] = 0.55;
	render_black_static(&col, 0.5);
	CHECK(corr3(out, out, 0) > 0.99, "sanity");
	double rg = 0, sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
	for (int i = 0; i < W * H; i++) {
		double a = out[i * 4], b = out[i * 4 + 1];
		sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
	}
	int n = W * H;
	rg = (sab / n - sa / n * sb / n) / sqrt((saa / n - pow(sa / n, 2)) * (sbb / n - pow(sb / n, 2)));
	CHECK(fabs(rg - 0.45) < 0.07, "color 0.55 correlates red and green at %.2f (want 0.45)", rg);

	// Smear stretches speckles sideways; clusters add coarse structure; scan lines darken odd
	// rows; jitter breaks the vertical coherence.
	struct sq_cfg sm = sc;
	sm.v[SQ_STATIC_SMEAR] = 3;
	render_black_static(&sm, 1.0);
	CHECK(shift_corr(out, 1, 2, 0) > shift_corr(out, 1, 0, 2) + 0.15, "smear: horizontal correlation %.2f vs vertical %.2f",
	      shift_corr(out, 1, 2, 0), shift_corr(out, 1, 0, 2));
	struct sq_cfg cl = sc;
	cl.v[SQ_STATIC_CLUSTERS] = 1;
	render_black_static(&sc, 1.5);
	double far0 = shift_corr(out, 1, 3, 0);
	render_black_static(&cl, 1.5);
	CHECK(shift_corr(out, 1, 3, 0) > far0 + 0.1, "clusters: correlation at 3 px rises from %.2f to %.2f", far0,
	      shift_corr(out, 1, 3, 0));
	struct sq_cfg sl = sc;
	sl.v[SQ_STATIC_SCANLINES] = 0.3;
	render_black_static(&sl, 2.0);
	double ratio = plane_mean(out, 1, 1) / plane_mean(out, 1, 0);
	CHECK(fabs(ratio - 0.7) < 0.03, "odd rows are %.2f of even rows (want 0.70)", ratio);
	struct sq_cfg jt = sc;
	jt.v[SQ_STATIC_GRAIN_PX] = 4;
	render_black_static(&jt, 2.5);
	double v0 = shift_corr(out, 1, 0, 1);
	jt.v[SQ_STATIC_JITTER_PX] = 3;
	render_black_static(&jt, 2.5);
	CHECK(shift_corr(out, 1, 0, 1) < v0 - 0.05, "jitter: vertical correlation falls from %.2f to %.2f", v0,
	      shift_corr(out, 1, 0, 1));
	teardown();

	// Time: a fresh independent pattern at every static_hz step, none in between, the same
	// brightness throughout, and no repeat of a pattern.
	struct sq_cfg mv = sc;
	mv.v[SQ_STATIC_HZ] = 10;
	setup(320, 240, (struct pl_mon[]){{0, 0, 320, 240}}, 1);
	uint8_t *f0 = malloc((size_t)W * H * 4), *f1 = malloc((size_t)W * H * 4);
	render_black_static(&mv, 0);
	memcpy(f0, out, (size_t)W * H * 4);
	render(&mv, 0.05);
	CHECK(memcmp(f0, out, (size_t)W * H * 4) == 0, "no change within one static_hz step");
	render(&mv, 0.1);
	memcpy(f1, out, (size_t)W * H * 4);
	CHECK(memcmp(f0, f1, (size_t)W * H * 4) != 0, "a new pattern at the next step");
	CHECK(fabs(corr3(f0, f1, 1)) < 0.05, "consecutive patterns are independent: correlation %.3f", corr3(f0, f1, 1));
	double lo = 1, hi = 0;
	for (int k = 0; k < 20; k++) {
		render(&mv, 0.1 * k);
		double mu = plane_mean(out, 1, -1);
		lo = fmin(lo, mu);
		hi = fmax(hi, mu);
	}
	// The mean of 76,800 speckles still wobbles a little from frame to frame by chance.
	CHECK(hi - lo < 0.015, "average brightness is steady across updates: %.4f to %.4f", lo, hi);
	render(&mv, 0);
	CHECK(memcmp(f0, out, (size_t)W * H * 4) == 0, "the same step gives the same pattern");
	free(f0);
	free(f1);
	teardown();
}

// The toggles. Each of brightness, dim_tint, and static switches its group on and off, and
// they are independent of each other. Off means the group's settings have no effect.
static void test_toggles(void) {
	printf("toggles: brightness, dim_tint, static\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_FALLOFF_PX] = 4;
	c.v[SQ_DIM_BLUE] = 0.7;
	c.v[SQ_STATIC_AMOUNT] = 20;
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	fill_dark();
	put((struct pl_rect){10, 8, 46, 40}, 255, 255, 255);
	put((struct pl_rect){56, 30, 86, 58}, 200, 200, 200);
	capture_full(0);
	set_tint(0.9f, 0.8f, 0.5f);
	double t = 0;
	for (int mask = 0; mask < 8; mask++) {
		c.v[SQ_BRIGHTNESS] = (mask & 1) ? 1 : 0;
		c.v[SQ_DIM_TINT] = (mask & 2) ? 1 : 0;
		c.v[SQ_STATIC] = (mask & 4) ? 1 : 0;
		t += 0.1;
		render(&c, t);
		char name[64];
		snprintf(name, sizeof name, "brightness %d, dim_tint %d, static %d", mask & 1, (mask >> 1) & 1, mask >> 2);
		compare_all(name, &c, 0);
		if (mask == 0) {
			CHECK(memcmp(out, img, (size_t)W * H * 4) == 0, "all three groups off: the screen is copied bit for bit");
		}
		if (mask == 1) {  // brightness only
			CHECK(at(25, 20)[0] < 255 && at(25, 20)[0] == at(25, 20)[1] && at(25, 20)[1] == at(25, 20)[2],
			      "brightness only: white is dimmed and stays neutral: %d,%d,%d", at(25, 20)[0], at(25, 20)[1],
			      at(25, 20)[2]);
			expect_px("brightness only: far dark untouched", 90, 5, 40, 0);
		}
		if (mask == 2) {  // dim_tint only
			CHECK(at(25, 20)[0] < 255 && at(25, 20)[0] > 230 && at(25, 20)[1] < at(25, 20)[0] && at(25, 20)[2] < at(25, 20)[1],
			      "dim_tint only: white is tinted but not dimmed: %d,%d,%d", at(25, 20)[0], at(25, 20)[1],
			      at(25, 20)[2]);
			CHECK(at(90, 5)[2] < 40, "dim_tint only: the tint reaches dark pixels too: %d", at(90, 5)[2]);
		}
		if (mask == 3) {  // brightness and dim_tint
			CHECK(at(25, 20)[0] > at(25, 20)[1] && at(25, 20)[1] > at(25, 20)[2], "both: dimmed and tinted: %d,%d,%d",
			      at(25, 20)[0], at(25, 20)[1], at(25, 20)[2]);
		}
	}
	// Switching the master off, then the groups back on.
	c.v[SQ_BRIGHTNESS] = c.v[SQ_DIM_TINT] = c.v[SQ_STATIC] = 1;
	c.v[SQ_ENABLED] = 0;
	render(&c, 1.0);
	CHECK(memcmp(out, img, (size_t)W * H * 4) == 0, "enabled off overrides every group");
	c.v[SQ_ENABLED] = 1;
	render(&c, 1.1);
	compare_all("everything back on", &c, 0);
	teardown();

	// Turning brightness off and on again must not leave a stale ramp or smoothing behind.
	struct sq_cfg r = base_cfg();
	r.v[SQ_BRIGHTENING_MS] = 400;
	r.v[SQ_DARKENING_MS] = 400;
	r.v[SQ_MIN_GAIN] = 0.05;
	r.v[SQ_SIZE_PX] = 24;
	r.v[SQ_FALLOFF_PX] = 0;
	setup(128, 96, (struct pl_mon[]){{0, 0, 128, 96}}, 1);
	fill_dark();
	capture_full(0);
	render(&r, 0);
	put((struct pl_rect){6, 6, 122, 90}, 255, 255, 255);  // a scene change: the ramp starts
	capture((struct pl_rect){6, 6, 122, 90}, 0.01);
	render(&r, 0.01);
	compare_all("a ramp in progress", &r, 0);
	r.v[SQ_BRIGHTNESS] = 0;
	render(&r, 0.05);
	compare_all("brightness off in the middle of a ramp", &r, 0);
	expect_px("the screen is plain while it is off", 64, 48, 255, 0);
	r.v[SQ_BRIGHTNESS] = 1;
	render(&r, 0.1);
	compare_all("brightness back on", &r, 0);
	expect_px("it comes back dimmed at once, with no stale ramp", 64, 48, apply(255, need_for(1.0, &r, r.v[SQ_TARGET]), &r), 3);
	teardown();
}

// The config file: groups with toggles, settings indented under their toggle.
static void test_groups(void) {
	printf("config groups and indentation\n");
	for (int i = 0; i < SQ_N; i++) {
		int g = sq_group(i);
		CHECK(g < 0 || (sq_is_toggle(g) && g < i && sq_params[g].type == SQ_BOOL), "setting %s belongs to a toggle before it",
		      sq_params[i].key);
		CHECK(!(sq_is_toggle(i) && g >= 0), "toggle %s is not nested", sq_params[i].key);
	}
	CHECK(sq_group(SQ_TARGET) == SQ_BRIGHTNESS && sq_group(SQ_GAMMA) == SQ_BRIGHTNESS, "brightness group spans target to gamma");
	CHECK(sq_group(SQ_DIM_RED) == SQ_DIM_TINT && sq_group(SQ_TINT_BLUE) == SQ_DIM_TINT, "dim_tint group spans the dim and tint channels");
	CHECK(sq_group(SQ_STATIC_JITTER_PX) == SQ_STATIC && sq_group(SQ_ENABLED) < 0, "static group and the top level");
	struct sq_cfg c;
	sq_defaults(&c);
	c.v[SQ_DIM_TINT] = 0;
	c.v[SQ_TARGET] = 0.33;
	const char *path = "/tmp/squint-group-test.conf";
	CHECK(sq_save(path, &c) == 0, "save");
	FILE *f = fopen(path, "r");
	char line[200];
	int bad = 0, members = 0, toggles = 0;
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || line[0] == '\n') continue;
		char key[64];
		sscanf(line, " %63s", key);
		int i = sq_find(key);
		bool indented = line[0] == ' ';
		if (i < 0 || indented != (sq_group(i) >= 0)) bad++;
		if (sq_group(i) >= 0) members++;
		if (sq_is_toggle(i)) toggles++;
	}
	fclose(f);
	CHECK(bad == 0, "%d lines are indented wrongly", bad);
	CHECK(toggles == 5 && members == SQ_N - 6, "5 toggles and %d members were written (got %d toggles, %d members)", SQ_N - 6, toggles, members);
	struct sq_cfg d;
	sq_load(path, &d);
	CHECK(d.v[SQ_DIM_TINT] == 0 && fabs(d.v[SQ_TARGET] - 0.33) < 1e-9, "indented lines load");
	// Order and indentation in a hand-edited file do not matter.
	f = fopen(path, "w");
	fputs("target=0.7\n\t\t  dim_tint = false\nstatic = true\n        static_amount =   40   # comment\n", f);
	fclose(f);
	sq_load(path, &d);
	CHECK(fabs(d.v[SQ_TARGET] - 0.7) < 1e-9 && d.v[SQ_DIM_TINT] == 0 && d.v[SQ_STATIC] == 1 && d.v[SQ_STATIC_AMOUNT] == 40,
	      "hand-edited indentation and order are accepted");
	// An old file without the toggles keeps today's behavior: brightness and dim_tint on, static off.
	f = fopen(path, "w");
	fputs("target = 0.5\ntint_blue = 0.5\n", f);
	fclose(f);
	sq_load(path, &d);
	CHECK(d.v[SQ_ENABLED] == 1 && d.v[SQ_BRIGHTNESS] == 1 && d.v[SQ_DIM_TINT] == 0 && d.v[SQ_STATIC] == 0 && d.v[SQ_SHORTCUTS] == 0,
	      "a file without the toggles gets the shipped toggle defaults: only enabled and brightness on");
	for (int i = 0; i < SQ_N; i++) {
		if (sq_params[i].type == SQ_STR) continue;
		CHECK(sq_clamp(i, sq_params[i].def) == sq_params[i].def, "the default of %s is inside its range", sq_params[i].key);
		if (sq_params[i].type == SQ_BOOL) {
			// rgb_horizontal and rgb_vertical are on/off members of the rgb group, not toggles.
			bool on = i == SQ_ENABLED || i == SQ_BRIGHTNESS || i == SQ_RGB_HORIZONTAL || i == SQ_RGB_VERTICAL;
			CHECK(sq_params[i].def == (on ? 1 : 0), "the default of toggle %s is %s", sq_params[i].key, on ? "on" : "off");
		}
	}
	unlink(path);
}

// Shortcut chords and the text settings that hold them.
static void test_keys(void) {
	printf("shortcut chords and text settings\n");
	unsigned mods;
	char key[SQ_STRLEN];
	CHECK(sq_parse_chord("Ctrl+Alt+s", &mods, key, sizeof key) && mods == (SQ_MOD_CTRL | SQ_MOD_ALT) && !strcmp(key, "s"), "Ctrl+Alt+s");
	CHECK(sq_parse_chord("super+SHIFT+F5", &mods, key, sizeof key) && mods == (SQ_MOD_SUPER | SQ_MOD_SHIFT) && !strcmp(key, "F5"), "modifier names ignore case");
	CHECK(sq_parse_chord("Control+Mod1+space", &mods, key, sizeof key) && mods == (SQ_MOD_CTRL | SQ_MOD_ALT) && !strcmp(key, "space"), "Control and Mod1 are aliases");
	CHECK(sq_parse_chord(" Ctrl + s ", &mods, key, sizeof key) && mods == SQ_MOD_CTRL && !strcmp(key, "s"), "spaces around the parts are ignored");
	CHECK(sq_parse_chord("F5", &mods, key, sizeof key) && mods == 0 && !strcmp(key, "F5"), "a key alone");
	CHECK(sq_parse_chord("", &mods, key, sizeof key) && mods == 0 && !*key, "an empty chord means no key");
	CHECK(!sq_parse_chord("Ctrl+", &mods, key, sizeof key), "a trailing + is malformed");
	CHECK(!sq_parse_chord("+s", &mods, key, sizeof key), "a leading + is malformed");
	CHECK(!sq_parse_chord("Foo+s", &mods, key, sizeof key), "an unknown modifier is malformed");
	CHECK(!sq_parse_chord("Ctrl+a b", &mods, key, sizeof key), "a space inside a key name is malformed");
	CHECK(!sq_parse_chord("Ctrl+Alt+Shift+Super+Mod2+Mod3+Mod5+Mod1+s", &mods, key, sizeof key), "too many parts are malformed");

	struct sq_cfg c;
	sq_defaults(&c);
	for (int i = SQ_KEY_ENABLED; i <= SQ_KEY_RGB_EFFECT; i++) {
		CHECK(c.s[i][0] != '\0' && sq_params[i].type == SQ_STR && sq_group(i) == SQ_SHORTCUTS, "%s is a text setting with a default key, in the shortcuts group", sq_params[i].key);
		CHECK(sq_key_target(i) >= 0 && (sq_params[sq_key_target(i)].type == SQ_BOOL || sq_params[sq_key_target(i)].type == SQ_ENUM), "%s changes an on/off or enum setting", sq_params[i].key);
	}
	CHECK(sq_key_target(SQ_KEY_ENABLED) == SQ_ENABLED && sq_key_target(SQ_KEY_BRIGHTNESS) == SQ_BRIGHTNESS &&
	          sq_key_target(SQ_KEY_DIM_TINT) == SQ_DIM_TINT && sq_key_target(SQ_KEY_STATIC) == SQ_STATIC && sq_key_target(SQ_TARGET) < 0,
	      "each key flips the toggle it is named for");
	CHECK(!strcmp(c.s[SQ_KEY_ENABLED], "Super+F1") && !strcmp(c.s[SQ_KEY_BRIGHTNESS], "Super+F2") &&
	          !strcmp(c.s[SQ_KEY_DIM_TINT], "Super+F3") && !strcmp(c.s[SQ_KEY_STATIC], "Super+F4"),
	      "the default keys are Super+F1 to Super+F4");
	CHECK(c.v[SQ_SHORTCUTS] == 0, "shortcuts default to off");
	CHECK(sq_set(&c, "key_dim_tint", "") && c.s[SQ_KEY_DIM_TINT][0] == '\0', "a default key can be cleared");
	c.v[SQ_SHORTCUTS] = 1;
	CHECK(sq_set(&c, "key_static", "Ctrl+Alt+s") && !strcmp(c.s[SQ_KEY_STATIC], "Ctrl+Alt+s"), "set a key");
	CHECK(!sq_set(&c, "key_static", "Foo+s") && !strcmp(c.s[SQ_KEY_STATIC], "Ctrl+Alt+s"), "a bad chord is rejected and the old one stays");
	CHECK(sq_set(&c, "key_brightness", "Super+b") && sq_set(&c, "key_enabled", "Super+Shift+F12"), "set two more");
	const char *path = "/tmp/squint-keys-test.conf";
	CHECK(sq_save(path, &c) == 0, "save");
	FILE *f = fopen(path, "r");
	char line[200], all[4000] = "";
	while (fgets(line, sizeof line, f)) strncat(all, line, sizeof all - strlen(all) - 1);
	fclose(f);
	CHECK(strstr(all, "\nshortcuts = true\n  key_enabled = Super+Shift+F12\n  key_brightness = Super+b\n  key_dim_tint =\n  key_static = Ctrl+Alt+s\n"),
	      "the shortcuts group is written with its keys indented, and an empty key without a trailing space:\n%s", strstr(all, "shortcuts") ? strstr(all, "shortcuts") : "(missing)");
	struct sq_cfg d;
	sq_load(path, &d);
	CHECK(!strcmp(d.s[SQ_KEY_STATIC], "Ctrl+Alt+s") && !strcmp(d.s[SQ_KEY_BRIGHTNESS], "Super+b") &&
	          !strcmp(d.s[SQ_KEY_ENABLED], "Super+Shift+F12") && d.s[SQ_KEY_DIM_TINT][0] == '\0',
	      "keys round trip, empty ones stay empty");
	CHECK(sq_set(&d, "key_static", "") && d.s[SQ_KEY_STATIC][0] == '\0', "an empty value clears a key");
	f = fopen(path, "w");
	fputs("shortcuts = false\n    key_static   =    Ctrl+Alt+x   # comment\nkey_enabled=\n", f);
	fclose(f);
	sq_load(path, &d);
	CHECK(d.v[SQ_SHORTCUTS] == 0 && !strcmp(d.s[SQ_KEY_STATIC], "Ctrl+Alt+x") && d.s[SQ_KEY_ENABLED][0] == '\0',
	      "a hand-edited file with odd spacing and a comment loads");
	f = fopen(path, "w");
	fputs("key_static = Foo+Bar+s\n", f);
	fclose(f);
	sq_load(path, &d);
	CHECK(!strcmp(d.s[SQ_KEY_STATIC], "Super+F4"), "a malformed chord in the file is ignored, so the default stays");
	unlink(path);
}

// The back buffer holds dimmed output wherever picom did not write. A frame with an empty
// damage region, or with many small rectangles, must not pull that into S.
static void test_damage_shapes(void) {
	printf("empty damage and many rectangles\n");
	struct sq_cfg c = base_cfg();
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	put((struct pl_rect){0, 0, 96, 64}, 40, 40, 40);
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);
	capture_full(0);
	render(&c, 0);
	int white = (int)lround(255 * enc(0.45));
	expect_px("dimmed once", 28, 18, white, 2);
	for (int i = 0; i < 6; i++) {  // frames with nothing damaged, as picom produces them
		pl_capture(&pl, NULL, 0, false, 0.01 * (i + 1));
		render(&c, 0.01 * (i + 1));
	}
	expect_px("still dimmed once after empty frames", 28, 18, white, 2);
	expect_px("dark untouched after empty frames", 5, 60, 40, 0);

	struct pl_rect many[100];
	for (int i = 0; i < 100; i++) {
		many[i] = (struct pl_rect){40 + (i % 10) * 5, 40 + (i / 10) * 2, 40 + (i % 10) * 5 + 2,
		                           40 + (i / 10) * 2 + 1};
		put(many[i], 255, 255, 255);
	}
	pl_capture(&pl, many, 100, false, 0.1);
	render(&c, 0.1);
	// Compare against a CPU model of the image as picom last wrote the damaged parts.
	// Everything outside the damage still matches because img keeps the undimmed content.
	compare_all("after 100 small rectangles", &c, 0);
	expect_px("white block still dimmed once", 28, 18, white, 2);
	teardown();
}

// ---- RGB lines --------------------------------------------------------------------------

static int px_at(int x, int y, int ch) { return img[((size_t)y * W + x) * 4 + ch]; }
static bool inbox(const struct pl_mon *m, int x, int y) {
	return x >= m->x && y >= m->y && x < m->x + m->w && y < m->y + m->h;
}
static bool same_px(int x1, int y1, int x2, int y2) {
	for (int ch = 0; ch < 3; ch++)
		if (abs(px_at(x1, y1, ch) - px_at(x2, y2, ch)) > 5) return false;
	return true;
}
static double eluma(int x, int y) {
	return 0.2126 * px_at(x, y, 0) / 255.0 + 0.7152 * px_at(x, y, 1) / 255.0 + 0.0722 * px_at(x, y, 2) / 255.0;
}

// Thin enough and different enough from at least one side, measured across the line.
static bool cpu_cand(const struct pl_mon *m, int x, int y, int dir, int thick, double contrast) {
	int ax = dir == 0 ? 0 : 1, ay = dir == 0 ? 1 : 0;  // across
	int a = 1, b = 1;
	for (; a <= thick; a++) {
		int qx = x - ax * a, qy = y - ay * a;
		if (!inbox(m, qx, qy) || !same_px(qx, qy, x, y)) break;
	}
	for (; b <= thick; b++) {
		int qx = x + ax * b, qy = y + ay * b;
		if (!inbox(m, qx, qy) || !same_px(qx, qy, x, y)) break;
	}
	int ax1 = x - ax * a, ay1 = y - ay * a, bx1 = x + ax * b, by1 = y + ay * b;
	if (a + b - 1 > thick || !inbox(m, ax1, ay1) || !inbox(m, bx1, by1)) return false;
	return fabs(eluma(ax1, ay1) - eluma(x, y)) >= contrast || fabs(eluma(bx1, by1) - eluma(x, y)) >= contrast;
}

// Candidates after a closing along the line: a maximum then a minimum over radius pixels,
// with taps outside the monitor ignored. Then a window of X consecutive linked pixels through
// each pixel, brute force.
static void cpu_lines(const struct sq_cfg *c, bool *mask[2]) {
	int X = (int)c->v[SQ_RGB_LINE_MIN_PX], thick = (int)c->v[SQ_RGB_LINE_THICK_PX];
	double contrast = c->v[SQ_RGB_LINE_CONTRAST];
	int radius = (thick + 1) / 2;
	bool *cd = malloc((size_t)W * H), *ce = malloc((size_t)W * H), *cc = malloc((size_t)W * H);
	for (int dir = 0; dir < 2; dir++) {
		memset(mask[dir], 0, (size_t)W * H);
		if (c->v[dir == 0 ? SQ_RGB_HORIZONTAL : SQ_RGB_VERTICAL] == 0) continue;
		int dx = dir == 0 ? 1 : 0, dy = dir == 0 ? 0 : 1;
		for (int mi = 0; mi < nmons; mi++) {
			const struct pl_mon *m = &mons[mi];
			for (int y = m->y; y < m->y + m->h; y++)
				for (int x = m->x; x < m->x + m->w; x++) cc[(size_t)y * W + x] = cpu_cand(m, x, y, dir, thick, contrast);
			for (int y = m->y; y < m->y + m->h; y++)
				for (int x = m->x; x < m->x + m->w; x++) {
					bool v = false;
					for (int k = -radius; k <= radius; k++) {
						int qx = x + dx * k, qy = y + dy * k;
						if (inbox(m, qx, qy) && cc[(size_t)qy * W + qx]) v = true;
					}
					cd[(size_t)y * W + x] = v;
				}
			for (int y = m->y; y < m->y + m->h; y++)
				for (int x = m->x; x < m->x + m->w; x++) {
					bool v = true;
					for (int k = -radius; k <= radius; k++) {
						int qx = x + dx * k, qy = y + dy * k;
						if (inbox(m, qx, qy) && !cd[(size_t)qy * W + qx]) v = false;
					}
					ce[(size_t)y * W + x] = v;
				}
			for (int y = m->y; y < m->y + m->h; y++)
				for (int x = m->x; x < m->x + m->w; x++) {
					// A window starts at (sx, sy) and covers X pixels along the line.
					for (int back = 0; back < X; back++) {
						int sx = x - dx * back, sy = y - dy * back;
						bool ok = true;
						for (int i = 0; i < X - 1 && ok; i++) {
							int px = sx + dx * i, py = sy + dy * i, qx = px + dx, qy = py + dy;
							ok = inbox(m, px, py) && inbox(m, qx, qy) && ce[(size_t)py * W + px] &&
							     ce[(size_t)qy * W + qx] && same_px(px, py, qx, qy);
						}
						if (ok) {
							mask[dir][(size_t)y * W + x] = true;
							break;
						}
					}
				}
		}
	}
	free(cd);
	free(ce);
	free(cc);
}

// line_tex as the GPU holds it, indexed with X-down rows.
static void read_lines(uint8_t *g) {
	uint8_t *tmp = malloc((size_t)W * H * 2);
	glBindTexture(GL_TEXTURE_2D, pl.line_tex);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_UNSIGNED_BYTE, tmp);
	for (int y = 0; y < H; y++) memcpy(g + (size_t)y * W * 2, tmp + (size_t)(H - 1 - y) * W * 2, (size_t)W * 2);
	free(tmp);
}

static struct sq_cfg rgb_cfg(void) {
	struct sq_cfg c = base_cfg();
	c.v[SQ_BRIGHTNESS] = 0;
	c.v[SQ_DIM_TINT] = 0;
	c.v[SQ_RGB] = 1;
	c.v[SQ_RGB_EFFECT] = 0;
	c.v[SQ_RGB_SPEED] = 1.0;
	c.v[SQ_RGB_LUMA] = 1.0;
	c.v[SQ_RGB_MIN_LUMA] = 0.0;
	c.v[SQ_RGB_HUE] = 200;
	c.v[SQ_RGB_HUE_SPAN] = 360;
	c.v[SQ_RGB_SATURATION] = 1.0;
	c.v[SQ_RGB_LENGTH_PX] = 60;
	c.v[SQ_RGB_GAP_PX] = 60;
	c.v[SQ_RGB_TAIL] = 0.0;
	c.v[SQ_RGB_DIRECTION] = 0;
	c.v[SQ_RGB_SYNC] = 1;
	c.v[SQ_RGB_LINE_MIN_PX] = 48;
	c.v[SQ_RGB_LINE_THICK_PX] = 3;
	c.v[SQ_RGB_LINE_CONTRAST] = 0.15;
	c.v[SQ_RGB_HORIZONTAL] = 1;
	c.v[SQ_RGB_VERTICAL] = 1;
	return c;
}

// White page with lines of gray 100: H1 1 px (y 20), H2 3 px (y 40 to 42), H3 4 px (y 60 to 63),
// H4 short (y 80, 30 px), H5 low contrast gray 235 (y 100), V1 2 px wide and 60 long (x 200,
// 201), V2 20 long (x 100).
static void draw_lines_scene(void) {
	put((struct pl_rect){0, 0, 240, 160}, 255, 255, 255);
	put((struct pl_rect){10, 20, 230, 21}, 100, 100, 100);
	put((struct pl_rect){10, 40, 230, 43}, 100, 100, 100);
	put((struct pl_rect){10, 60, 230, 64}, 100, 100, 100);
	put((struct pl_rect){10, 80, 40, 81}, 100, 100, 100);
	put((struct pl_rect){10, 100, 150, 101}, 235, 235, 235);
	put((struct pl_rect){200, 90, 202, 150}, 100, 100, 100);
	put((struct pl_rect){100, 110, 101, 130}, 100, 100, 100);
}

static void compare_lines(const char *what, const struct sq_cfg *c) {
	bool *m0 = malloc((size_t)W * H), *m1 = malloc((size_t)W * H);
	bool *mm[2] = {m0, m1};
	cpu_lines(c, mm);
	uint8_t *g = malloc((size_t)W * H * 2);
	read_lines(g);
	int bad = 0, first = -1;
	for (int i = 0; i < W * H; i++)
		for (int d = 0; d < 2; d++)
			if ((g[i * 2 + d] > 127) != mm[d][i]) {
				if (!bad++) first = i * 2 + d;
			}
	CHECK(bad == 0, "%s: %d line pixels differ from the CPU model, first at (%d,%d) channel %d", what, bad,
	      first >= 0 ? (first / 2) % W : 0, first >= 0 ? (first / 2) / W : 0, first >= 0 ? first % 2 : 0);
	free(m0);
	free(m1);
	free(g);
}

static void test_rgb_lines(void) {
	printf("rgb line detection\n");
	struct pl_mon m1[] = {{0, 0, 240, 160}};
	setup(240, 160, m1, 1);
	draw_lines_scene();
	capture_full(0);
	struct sq_cfg c = rgb_cfg();
	render(&c, 0);
	compare_lines("defaults", &c);
	uint8_t *g = malloc((size_t)W * H * 2);
	read_lines(g);
	#define MASK(x, y, d) (g[((size_t)(y) * W + (x)) * 2 + (d)] > 127)
	CHECK(MASK(100, 20, 0) && !MASK(100, 20, 1), "a 1 px horizontal line is marked horizontal only");
	CHECK(MASK(10, 20, 0) && MASK(229, 20, 0) && !MASK(9, 20, 0) && !MASK(230, 20, 0), "the line is marked from end to end");
	CHECK(MASK(100, 40, 0) && MASK(100, 41, 0) && MASK(100, 42, 0), "all three rows of a 3 px line");
	CHECK(!MASK(100, 60, 0) && !MASK(100, 61, 0), "a 4 px band is too thick");
	CHECK(!MASK(20, 80, 0), "30 px is too short");
	CHECK(!MASK(100, 100, 0), "gray 235 on white is below the contrast limit");
	CHECK(MASK(200, 120, 1) && MASK(201, 120, 1) && !MASK(200, 120, 0), "a 2 px vertical line is marked vertical only");
	CHECK(!MASK(100, 120, 1), "a 20 px vertical line is too short");
	CHECK(!MASK(150, 130, 0) && !MASK(150, 130, 1), "plain white is not a line");
	free(g);
	#undef MASK

	struct sq_cfg d = c;
	d.v[SQ_RGB_LINE_MIN_PX] = 16;
	d.v[SQ_RGB_LINE_THICK_PX] = 5;
	d.v[SQ_RGB_LINE_CONTRAST] = 0.05;
	render(&d, 0.1);  // changed detection settings recompute the lines without new damage
	compare_lines("min 16, thickness 5, contrast 0.05", &d);
	g = malloc((size_t)W * H * 2);
	read_lines(g);
	#define MASK(x, y, d) (g[((size_t)(y) * W + (x)) * 2 + (d)] > 127)
	CHECK(MASK(100, 61, 0) && MASK(20, 80, 0) && MASK(100, 100, 0) && MASK(100, 120, 1), "looser limits find the band, the short lines, and the faint line");
	free(g);
	#undef MASK

	struct sq_cfg e = c;
	e.v[SQ_RGB_VERTICAL] = 0;
	render(&e, 0.2);
	compare_lines("horizontal only", &e);
	e.v[SQ_RGB_VERTICAL] = 1;
	e.v[SQ_RGB_HORIZONTAL] = 0;
	render(&e, 0.3);
	compare_lines("vertical only", &e);
	e.v[SQ_RGB_VERTICAL] = 0;
	render(&e, 0.4);
	compare_lines("neither direction", &e);
	teardown();

	// Contrast on one side is enough. These lines have a gray 110 region on one side and white
	// on the other. The third line, gray 120 inside the gray 110 region, contrasts on neither side.
	struct pl_mon m4[] = {{0, 0, 120, 100}};
	setup(120, 100, m4, 1);
	put((struct pl_rect){0, 0, 120, 100}, 255, 255, 255);
	put((struct pl_rect){0, 31, 120, 70}, 110, 110, 110);
	put((struct pl_rect){10, 30, 110, 31}, 100, 100, 100);
	put((struct pl_rect){10, 70, 110, 71}, 100, 100, 100);
	put((struct pl_rect){10, 50, 110, 51}, 120, 120, 120);
	capture_full(0);
	struct sq_cfg h = rgb_cfg();
	render(&h, 0);
	compare_lines("lines with low contrast on one side", &h);
	g = malloc((size_t)W * H * 2);
	read_lines(g);
	#define MASK(x, y, d) (g[((size_t)(y) * W + (x)) * 2 + (d)] > 127)
	CHECK(MASK(60, 30, 0) && MASK(60, 70, 0), "a line that contrasts on one side is a line");
	CHECK(!MASK(60, 50, 0), "a line that contrasts on neither side is not a line");
	free(g);
	#undef MASK
	teardown();

	// Lines of one color cross without breaking each other.
	struct pl_mon m5[] = {{0, 0, 200, 160}};
	setup(200, 160, m5, 1);
	put((struct pl_rect){0, 0, 200, 160}, 255, 255, 255);
	for (int y = 20; y <= 140; y += 20) put((struct pl_rect){10, y, 190, y + 1}, 100, 100, 100);
	put((struct pl_rect){100, 5, 101, 155}, 100, 100, 100);
	capture_full(0);
	struct sq_cfg x = rgb_cfg();
	render(&x, 0);
	compare_lines("a grid of lines", &x);
	g = malloc((size_t)W * H * 2);
	read_lines(g);
	#define MASK(x, y, d) (g[((size_t)(y) * W + (x)) * 2 + (d)] > 127)
	CHECK(MASK(100, 30, 1) && MASK(100, 20, 1) && MASK(100, 100, 1), "the vertical line is whole, 19 px between crossings and all");
	CHECK(MASK(50, 40, 0) && MASK(100, 40, 0) && MASK(150, 40, 0), "the horizontal line is whole across the vertical one");
	free(g);
	#undef MASK
	teardown();

	// A line that runs across two monitors counts on each one separately.
	struct pl_mon m2[] = {{0, 0, 120, 40}, {120, 0, 120, 40}};
	setup(240, 40, m2, 2);
	put((struct pl_rect){0, 0, 240, 40}, 255, 255, 255);
	put((struct pl_rect){0, 20, 240, 21}, 100, 100, 100);
	capture_full(0);
	struct sq_cfg f = rgb_cfg();
	f.v[SQ_RGB_LINE_MIN_PX] = 100;
	render(&f, 0);
	compare_lines("a line split over two monitors, long enough on each", &f);
	g = malloc((size_t)W * H * 2);
	read_lines(g);
	#define MASK(x, y, d) (g[((size_t)(y) * W + (x)) * 2 + (d)] > 127)
	CHECK(MASK(60, 20, 0) && MASK(119, 20, 0) && MASK(120, 20, 0) && MASK(200, 20, 0), "both halves are lines at 100 px");
	free(g);
	f.v[SQ_RGB_LINE_MIN_PX] = 130;
	render(&f, 0.1);
	compare_lines("a line split over two monitors, too short on each", &f);
	g = malloc((size_t)W * H * 2);
	read_lines(g);
	CHECK(!MASK(60, 20, 0) && !MASK(200, 20, 0), "the line does not count across the monitor edge");
	free(g);
	#undef MASK
	teardown();
}

// Mean absolute difference between the pixel and a gray of the same value, as a measure of color.
static int chroma(int x, int y) {
	const uint8_t *p = at(x, y);
	int hi = p[0] > p[1] ? p[0] : p[1], lo = p[0] < p[1] ? p[0] : p[1];
	hi = hi > p[2] ? hi : p[2];
	lo = lo < p[2] ? lo : p[2];
	return hi - lo;
}

static void expect_lum(const char *what, int x, int y, double want, double tol) {
	double got = lum(at(x, y));
	CHECK(fabs(got - want) <= tol, "%s at (%d,%d): luminance %.4f, want %.4f (tol %.3f)", what, x, y, got, want, tol);
}

static void test_rgb_effects(void) {
	printf("rgb effects\n");
	struct pl_mon m1[] = {{0, 0, 240, 160}};
	setup(240, 160, m1, 1);
	draw_lines_scene();
	capture_full(0);
	struct sq_cfg c = rgb_cfg();
	double L0 = lum((uint8_t[]){100, 100, 100});

	// Solid: the luminance of the pixel it replaces, times rgb_luma.
	render(&c, 100.0);
	expect_lum("solid keeps the luminance", 100, 20, L0, 0.006);
	CHECK(chroma(100, 20) > 40, "solid is colored (chroma %d)", chroma(100, 20));
	expect_px("pixels that are not on a line are untouched", 120, 5, 255, 0);
	expect_px("a short line is untouched", 20, 80, 100, 0);
	c.v[SQ_RGB_LUMA] = 2.0;
	render(&c, 100.1);
	expect_lum("rgb_luma 2 doubles it", 100, 20, 2 * L0, 0.008);
	c.v[SQ_RGB_LUMA] = 4.0;
	render(&c, 100.2);
	expect_lum("rgb_luma 4 keeps the exact luminance although blue runs out", 100, 20, 4 * L0, 0.01);
	c.v[SQ_RGB_LUMA] = 1.0;

	// Breathe: 0.1 + 0.9 * (0.5 + 0.5 sin(2 pi cycle)). The first render fixed cycle 0 at t = 100.
	c.v[SQ_RGB_EFFECT] = 1;
	render(&c, 100.0);
	expect_lum("breathe at cycle 0", 100, 20, 0.55 * L0, 0.008);
	render(&c, 100.25);
	expect_lum("breathe at the top", 100, 20, L0, 0.008);
	render(&c, 100.75);
	expect_lum("breathe at the bottom", 100, 20, 0.1 * L0, 0.008);

	// Snake: 60 px segments with 60 px gaps would be too dense to test; use 100 and 100.
	c.v[SQ_RGB_EFFECT] = 3;
	c.v[SQ_RGB_LENGTH_PX] = 100;
	c.v[SQ_RGB_GAP_PX] = 100;
	render(&c, 101.0);
	expect_lum("snake body keeps the luminance", 150, 20, L0, 0.006);
	CHECK(chroma(150, 20) > 40, "the snake body is colored");
	expect_px("outside the snake the line is as it was", 50, 20, 100, 0);
	c.v[SQ_RGB_DIRECTION] = 1;
	render(&c, 101.0);
	expect_px("backward: the lit and unlit parts swap", 150, 20, 100, 0);
	CHECK(chroma(50, 20) > 40, "backward: the first part is lit");
	c.v[SQ_RGB_DIRECTION] = 0;
	render(&c, 101.5);  // half a cycle: the head has moved 100 px
	expect_px("the segment has moved on", 150, 20, 100, 0);
	CHECK(chroma(50, 20) > 40, "the segment reached the other half");

	// Rainbow: one hue cycle every rgb_length_px.
	c.v[SQ_RGB_EFFECT] = 2;
	c.v[SQ_RGB_LENGTH_PX] = 60;
	render(&c, 102.0);
	const uint8_t a1[3] = {at(40, 20)[0], at(40, 20)[1], at(40, 20)[2]};
	const uint8_t a2[3] = {at(100, 20)[0], at(100, 20)[1], at(100, 20)[2]};
	const uint8_t a3[3] = {at(70, 20)[0], at(70, 20)[1], at(70, 20)[2]};
	CHECK(abs(a1[0] - a2[0]) + abs(a1[1] - a2[1]) + abs(a1[2] - a2[2]) <= 6, "rainbow repeats every length: %d,%d,%d and %d,%d,%d", a1[0], a1[1], a1[2], a2[0], a2[1], a2[2]);
	CHECK(abs(a1[0] - a3[0]) + abs(a1[1] - a3[1]) + abs(a1[2] - a3[2]) > 60, "half a length away the hue differs");
	expect_lum("rainbow keeps the luminance", 40, 20, L0, 0.008);
	expect_lum("rainbow keeps the luminance elsewhere", 70, 20, L0, 0.008);
	render(&c, 102.25);
	CHECK(abs(at(40, 20)[0] - a1[0]) + abs(at(40, 20)[1] - a1[1]) + abs(at(40, 20)[2] - a1[2]) > 40, "rainbow moves with time");

	// Wave: brightness 0.2 to 1 along the line, one cycle per length.
	c.v[SQ_RGB_EFFECT] = 4;
	c.v[SQ_RGB_LENGTH_PX] = 80;
	render(&c, 103.0);
	double hi = lum(at(20, 20)), lo = lum(at(60, 20));
	CHECK(hi > 0.9 * L0 && lo / hi > 0.17 && lo / hi < 0.24, "wave crest %.4f and trough %.4f", hi, lo);

	// Phase: in step with rgb_sync, apart without it.
	double l1 = lum(at(20, 20)), l2 = lum(at(20, 41));
	CHECK(fabs(l1 - l2) < 0.006, "synced lines match: %.4f vs %.4f", l1, l2);
	c.v[SQ_RGB_SYNC] = 0;
	render(&c, 103.0);
	l1 = lum(at(20, 20));
	l2 = lum(at(20, 41));
	CHECK(fabs(l1 - l2) > 0.01, "unsynced lines differ: %.4f vs %.4f", l1, l2);
	c.v[SQ_RGB_SYNC] = 1;

	// Directions off, switch off, master off.
	c.v[SQ_RGB_EFFECT] = 0;
	c.v[SQ_RGB_VERTICAL] = 0;
	render(&c, 104.0);
	expect_px("vertical off leaves the vertical line", 200, 120, 100, 0);
	CHECK(chroma(100, 20) > 40, "horizontal lines are still lit");
	c.v[SQ_RGB_VERTICAL] = 1;
	c.v[SQ_RGB] = 0;
	render(&c, 104.1);
	CHECK(memcmp(out, img, (size_t)W * H * 4) == 0, "rgb off changes nothing");
	c.v[SQ_RGB] = 1;
	c.v[SQ_ENABLED] = 0;
	render(&c, 104.2);
	CHECK(memcmp(out, img, (size_t)W * H * 4) == 0, "enabled off overrides rgb");
	c.v[SQ_ENABLED] = 1;
	render(&c, 104.3);
	CHECK(chroma(100, 20) > 40, "rgb comes back");

	// Active: moving effects keep the timer running, solid does not.
	pl.last_input = 0;
	c.v[SQ_RGB_EFFECT] = 0;
	CHECK(!pl_active(&pl, &c, 500.0), "solid needs no timer ticks");
	c.v[SQ_RGB_EFFECT] = 3;
	CHECK(pl_active(&pl, &c, 500.0), "snake needs timer ticks");
	teardown();

	// The line is recolored after dimming and from the dimmed luminance.
	struct pl_mon m3[] = {{0, 0, 120, 40}};
	setup(120, 40, m3, 1);
	put((struct pl_rect){0, 0, 120, 40}, 0, 0, 0);
	put((struct pl_rect){10, 20, 110, 21}, 255, 255, 255);
	capture_full(0);
	struct sq_cfg w = rgb_cfg();
	w.v[SQ_RGB_LUMA] = 1.0;
	render(&w, 0);
	CHECK(at(60, 20)[0] >= 252 && at(60, 20)[1] >= 252 && at(60, 20)[2] >= 252, "a white line stays white at luma 1 (%d,%d,%d)", at(60, 20)[0], at(60, 20)[1], at(60, 20)[2]);
	w.v[SQ_BRIGHTNESS] = 1;
	w.v[SQ_TARGET] = 0.3;
	w.v[SQ_TARGET_LARGE] = 0.3;
	w.v[SQ_MIN_GAIN] = 0.05;
	w.v[SQ_FALLOFF_PX] = 0;
	render(&w, 0.1);
	expect_lum("a dimmed white line takes the dimmed luminance", 60, 20, 0.3, 0.012);
	w.v[SQ_RGB_LUMA] = 2.0;
	render(&w, 0.2);
	expect_lum("rgb_luma 2 raises the dimmed luminance", 60, 20, 0.6, 0.015);
	teardown();

	// A saturated blue cannot be as bright as gray 200, so it turns paler and keeps the luminance.
	setup(120, 40, m3, 1);
	put((struct pl_rect){0, 0, 120, 40}, 0, 0, 0);
	put((struct pl_rect){10, 20, 110, 21}, 200, 200, 200);
	capture_full(0);
	struct sq_cfg b = rgb_cfg();
	b.v[SQ_RGB_HUE] = 240;
	render(&b, 0);
	expect_lum("blue at the luminance of gray 200", 60, 20, lum((uint8_t[]){200, 200, 200}), 0.01);
	CHECK(at(60, 20)[2] > at(60, 20)[0] + 20, "it is still bluish (%d,%d,%d)", at(60, 20)[0], at(60, 20)[1], at(60, 20)[2]);
	b.v[SQ_RGB_SATURATION] = 0.0;
	render(&b, 0.1);
	CHECK(chroma(60, 20) <= 2, "saturation 0 is gray");
	expect_lum("and keeps the luminance", 60, 20, lum((uint8_t[]){200, 200, 200}), 0.01);
	teardown();

	// rgb_min_luma lets a black line light up.
	setup(120, 40, m3, 1);
	put((struct pl_rect){0, 0, 120, 40}, 255, 255, 255);
	put((struct pl_rect){10, 20, 110, 21}, 0, 0, 0);
	capture_full(0);
	struct sq_cfg k = rgb_cfg();
	render(&k, 0);
	expect_px("a black line stays black by default", 60, 20, 0, 2);
	k.v[SQ_RGB_MIN_LUMA] = 0.3;
	render(&k, 0.1);
	expect_lum("rgb_min_luma gives it a floor", 60, 20, 0.3, 0.01);
	teardown();
}

static void test_rgb_config(void) {
	printf("rgb settings\n");
	struct sq_cfg c;
	sq_defaults(&c);
	CHECK(c.v[SQ_RGB] == 0, "rgb is off by default");
	CHECK(sq_is_toggle(SQ_RGB) && sq_group(SQ_RGB) < 0 && sq_group(SQ_RGB_EFFECT) == SQ_RGB && sq_group(SQ_RGB_VERTICAL) == SQ_RGB &&
	          sq_group(SQ_SHORTCUTS) < 0,
	      "rgb is a toggle that owns the settings up to rgb_vertical");
	CHECK(sq_params[SQ_RGB_EFFECT].type == SQ_ENUM && sq_params[SQ_RGB_DIRECTION].type == SQ_ENUM, "effect and direction are enums");
	CHECK(sq_set(&c, "rgb_effect", "snake") && c.v[SQ_RGB_EFFECT] == 3, "an effect is set by name");
	CHECK(sq_set(&c, "rgb_effect", "WAVE") && c.v[SQ_RGB_EFFECT] == 4, "names match in any case");
	CHECK(sq_set(&c, "rgb_effect", "1") && c.v[SQ_RGB_EFFECT] == 1, "an index works too");
	CHECK(sq_set(&c, "rgb_effect", "9") && c.v[SQ_RGB_EFFECT] == 4, "an index past the end is clamped");
	CHECK(!sq_set(&c, "rgb_effect", "disco") && c.v[SQ_RGB_EFFECT] == 4, "an unknown name is rejected");
	char v[SQ_STRLEN];
	sq_format_cfg(&c, SQ_RGB_EFFECT, v, sizeof v);
	CHECK(!strcmp(v, "wave"), "it is shown by name: %s", v);
	sq_format_value(SQ_RGB_DIRECTION, 2, v, sizeof v);
	CHECK(!strcmp(v, "alternate"), "the last direction is named: %s", v);
	sq_format_value(SQ_RGB_EFFECT, sq_params[SQ_RGB_EFFECT].min, v, sizeof v);
	CHECK(!strcmp(v, "solid"), "the range starts at solid: %s", v);
	CHECK(sq_set(&c, "rgb", "true") && sq_set(&c, "rgb_direction", "alternate") && sq_set(&c, "rgb_line_thick_px", "9") &&
	          c.v[SQ_RGB_LINE_THICK_PX] == 5,
	      "thickness is limited to 5");
	const char *path = "/tmp/squint-rgb-test.conf";
	CHECK(sq_save(path, &c) == 0, "save");
	FILE *f = fopen(path, "r");
	char line[200], all[6000] = "";
	while (fgets(line, sizeof line, f)) strncat(all, line, sizeof all - strlen(all) - 1);
	fclose(f);
	CHECK(strstr(all, "\nrgb = true\n  rgb_effect = wave\n  rgb_speed = ") && strstr(all, "  rgb_direction = alternate\n"),
	      "the file holds the names, indented under rgb");
	struct sq_cfg d;
	sq_load(path, &d);
	CHECK(d.v[SQ_RGB] == 1 && d.v[SQ_RGB_EFFECT] == 4 && d.v[SQ_RGB_DIRECTION] == 2, "the names load back");
	unlink(path);
	CHECK(!strcmp(sq_params[SQ_KEY_RGB].sdef, "Super+F5") && !strcmp(sq_params[SQ_KEY_RGB_EFFECT].sdef, "Super+F6"), "the default keys are Super+F5 and Super+F6");
	CHECK(sq_key_target(SQ_KEY_RGB) == SQ_RGB && sq_params[SQ_RGB].type == SQ_BOOL, "key_rgb flips rgb");
	CHECK(sq_key_target(SQ_KEY_RGB_EFFECT) == SQ_RGB_EFFECT && sq_params[SQ_RGB_EFFECT].type == SQ_ENUM, "key_rgb_effect steps the effect");
	for (int i = 0; i < SQ_N; i++) {
		if (sq_params[i].type == SQ_ENUM) {
			char first[SQ_STRLEN], last[SQ_STRLEN];
			sq_enum_name(i, (int)sq_params[i].min, first, sizeof first);
			sq_enum_name(i, (int)sq_params[i].max, last, sizeof last);
			CHECK(sq_enum_find(i, first) == (int)sq_params[i].min && sq_enum_find(i, last) == (int)sq_params[i].max &&
			          sq_params[i].def >= sq_params[i].min && sq_params[i].def <= sq_params[i].max,
			      "the names of %s match its range", sq_params[i].key);
		}
	}
}

int main(void) {
	make_context();
	test_cone();
	test_zero_falloff();
	test_monitors();
	test_gamma_and_enable();
	test_smoothing();
	test_size();
	test_rise();
	test_gate();
	test_static();
	test_color();
	test_config();
	test_groups();
	test_keys();
	test_toggles();
	test_damage_shapes();
	test_rgb_config();
	test_rgb_lines();
	test_rgb_effects();
	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
