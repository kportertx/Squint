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

static double need_for(double L, const struct sq_cfg *c) {
	double t = c->v[SQ_TARGET], knee = c->v[SQ_KNEE], h = 0.5 * knee, over = L - t;
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
	c.v[SQ_MIN_GAIN] = 0.35;
	c.v[SQ_TAU_MS] = 0;
	c.v[SQ_FALLOFF_PX] = 8;
	return c;
}

// Dimming amount at every pixel of the current image, brute force.
static double *cpu_D(const struct sq_cfg *c) {
	double *need = malloc((size_t)W * H * sizeof *need);
	double *D = calloc((size_t)W * H, sizeof *D);
	for (int i = 0; i < W * H; i++) {
		need[i] = need_for(lum(img + (size_t)i * 4), c);
	}
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
	double Dw = need_for(1.0, &c);
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
	test_damage_shapes();
	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
