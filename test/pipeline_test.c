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
}

static const uint8_t *at(int x, int y) { return out + ((size_t)y * W + x) * 4; }

static void expect_px(const char *what, int x, int y, int want, int tol) {
	const uint8_t *p = at(x, y);
	int ok = abs(p[0] - want) <= tol && abs(p[1] - want) <= tol && abs(p[2] - want) <= tol;
	CHECK(ok, "%s at (%d,%d): got %d,%d,%d want %d (tol %d)", what, x, y, p[0], p[1], p[2], want, tol);
}

static struct sq_cfg base_cfg(void) {
	struct sq_cfg c;
	sq_defaults(&c);
	c.v[SQ_TARGET] = 0.45;
	c.v[SQ_TARGET_LARGE] = 0.45;  // size feature off unless a test turns it on
	c.v[SQ_MIN_GAIN] = 0.35;
	c.v[SQ_TAU_MS] = 0;
	c.v[SQ_RISE_MS] = 0;
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

// Compares the whole of every monitor against the CPU model. Pixels outside every monitor
// must equal what was written (they are never redrawn).
static void compare_all(const char *what, const struct sq_cfg *c, double D_override_scale) {
	(void)D_override_scale;
	double *D = cpu_D(c);
	int bad = 0, worst = 0, bx = -1, by = -1;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			bool in = false;
			for (int m = 0; m < nmons; m++)
				in = in || (x >= mons[m].x && x < mons[m].x + mons[m].w && y >= mons[m].y &&
				            y < mons[m].y + mons[m].h);
			const uint8_t *src = img + ((size_t)y * W + x) * 4;
			const uint8_t *got = at(x, y);
			for (int ch = 0; ch < 3; ch++) {
				int want = in ? apply(src[ch], D[(size_t)y * W + x], c) : src[ch];
				int diff = abs(got[ch] - want);
				if (diff > 2) {
					bad++;
					if (diff > worst) worst = diff, bx = x, by = y;
				}
			}
		}
	CHECK(bad == 0, "%s: %d channel values off by more than 2 (worst %d at %d,%d)", what, bad, worst, bx, by);
	free(D);
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
	printf("attack is instant, release follows exp(-dt/tau)\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_TAU_MS] = 300;
	c.v[SQ_FALLOFF_PX] = 0;
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	put((struct pl_rect){0, 0, 96, 64}, 40, 40, 40);
	capture_full(0);
	render(&c, 0);
	expect_px("dark before", 28, 18, 40, 0);
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);
	capture((struct pl_rect){20, 10, 36, 26}, 0.01);
	render(&c, 0.01);
	double Dw = need_for(1.0, &c, c.v[SQ_TARGET]);
	expect_px("white dimmed in the call that first shows it", 28, 18, apply(255, Dw, &c), 2);

	put((struct pl_rect){0, 0, 96, 64}, 40, 40, 40);
	capture_full(0.31);
	render(&c, 0.31);  // dt = 0.3 = one tau since the last smoothing step at 0.01
	expect_px("1 tau after the white left", 28, 18, apply(40, Dw * exp(-1.0), &c), 2);
	render(&c, 0.61);  // no new frame, smoothing only
	expect_px("2 tau, no new frame", 28, 18, apply(40, Dw * exp(-2.0), &c), 2);
	render(&c, 5.0);
	expect_px("fully released", 28, 18, 40, 0);

	CHECK(pl_active(&pl, &c, 0.5), "active shortly after input");
	CHECK(!pl_active(&pl, &c, 10.0), "inactive long after input");
	c.v[SQ_TAU_MS] = 0;
	CHECK(!pl_active(&pl, &c, 0.5), "with tau 0 only the short grace period is active");
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

// A newly bright area starts at min_gain and its gain floor rises toward 1 with time
// constant rise, so its brightness climbs to the target instead of appearing at once.
static void test_rise(void) {
	printf("rise: new bright areas climb to their target\n");
	struct sq_cfg c = base_cfg();
	c.v[SQ_RISE_MS] = 200;
	c.v[SQ_MIN_GAIN] = 0.05;  // a wide ramp: from 0.05 up to the 0.45 target gain
	c.v[SQ_FALLOFF_PX] = 0;
	struct pl_mon m[] = {{0, 0, 96, 64}};
	setup(96, 64, m, 1);
	fill_dark();
	capture_full(0);
	render(&c, 0);
	double mg = c.v[SQ_MIN_GAIN], Dw = need_for(1.0, &c, c.v[SQ_TARGET]), gt = 1 - Dw;
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);
	capture((struct pl_rect){20, 10, 36, 26}, 0.01);
	// The floor f follows f' = f + (1 - f)(1 - exp(-dt / rise)), starting from min_gain.
	double f = mg, t_prev = 0;
	double times[] = {0.01, 0.05, 0.1, 0.2, 0.4, 1.0, 3.0};
	for (int i = 0; i < 7; i++) {
		f = f + (1 - f) * (1 - exp(-(times[i] - t_prev) / 0.2));
		t_prev = times[i];
		render(&c, times[i]);
		double g = fmin(gt, f);
		char name[48];
		snprintf(name, sizeof name, "white at t=%.2f", times[i]);
		expect_px(name, 28, 18, apply(255, 1 - g, &c), 3);
		expect_px("dark pixel is never touched", 5, 60, 40, 0);
	}
	CHECK(f > gt, "the floor has passed the target by the end");
	expect_px("settled at the target", 28, 18, (int)lround(255 * enc(gt)), 2);

	// The white leaves and returns: the ramp starts over.
	fill_dark();
	capture_full(3.1);
	render(&c, 3.1);
	expect_px("white gone", 28, 18, 40, 0);
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);
	capture((struct pl_rect){20, 10, 36, 26}, 3.11);
	render(&c, 3.11);
	double f2 = mg + (1 - mg) * (1 - exp(-0.01 / 0.2));
	expect_px("ramp restarts near min_gain", 28, 18, apply(255, 1 - fmin(gt, f2), &c), 3);
	CHECK(at(28, 18)[0] < (int)lround(255 * enc(gt)) - 20, "restarted ramp is well below the target");

	// rise = 0 shows it at once.
	c.v[SQ_RISE_MS] = 0;
	setup(96, 64, m, 1);
	fill_dark();
	capture_full(0);
	render(&c, 0);
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);
	capture((struct pl_rect){20, 10, 36, 26}, 0.01);
	render(&c, 0.01);
	expect_px("rise 0 is instant", 28, 18, apply(255, Dw, &c), 2);
	teardown();

	// tau and rise together: release still uses tau.
	c.v[SQ_RISE_MS] = 200;
	c.v[SQ_TAU_MS] = 300;
	setup(96, 64, m, 1);
	fill_dark();
	put((struct pl_rect){20, 10, 36, 26}, 255, 255, 255);
	capture_full(0);
	render(&c, 0);  // the first frame has no history, so it starts at the target
	expect_px("first frame starts at the target", 28, 18, apply(255, Dw, &c), 2);
	fill_dark();
	capture_full(0.3);
	render(&c, 0.3);
	expect_px("release after 1 tau", 28, 18, apply(40, Dw * exp(-1.0), &c), 3);
	teardown();
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

int main(void) {
	make_context();
	test_cone();
	test_zero_falloff();
	test_monitors();
	test_gamma_and_enable();
	test_smoothing();
	test_size();
	test_rise();
	test_damage_shapes();
	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
