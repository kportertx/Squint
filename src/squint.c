// squint: a picom backend plugin that dims the bright regions of the composed screen.
//
// picom composes into the back buffer as usual. Just before present, this plugin has
// already copied what picom wrote into an undimmed screen copy (S), and it redraws every
// monitor pixel from S through the dimming pass in pipeline.h. picom then presents the
// result. Presenting from the plugin's own libev timer carries smoothing forward while
// picom has no frames to draw.
//
// Settings live in ~/.config/squint.conf (see config.h), edited with squintctl.
// SQUINT_LOG sets the log file, default /tmp/squint.log.

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <ev.h>
#include <stdlib.h>
#include <time.h>

#include "pipeline.h"

#include <picom/api.h>
#include <picom/backend.h>

#define NSLOT 4
#define STATS_EVERY 300

// GPU time of a code span, measured with timestamp queries. picom keeps a
// GL_TIME_ELAPSED query open for its own frame timing, and those cannot nest, so this
// uses GL_TIMESTAMP, which can.
struct span {
	GLuint q[NSLOT][2];
	bool issued[NSLOT];
	int head;
	double sum_us;
	long n;
	bool ready;
};

struct state {
	struct backend_base *b;
	bool (*orig_cqz)(struct backend_base *, ivec2, image_handle, image_handle, const region_t *);
	bool (*orig_present)(struct backend_base *);
	void (*orig_deinit)(backend_t *);
	void (*orig_root_change)(backend_t *, session_t *);

	Display *dpy;
	int W, H, nmon;
	struct pl_mon mons[PL_MAXMON];

	struct pl pl;
	bool pl_failed;

	struct sq_cfg cfg;
	char conf_path[512];
	ev_stat conf_watch;
	bool conf_watch_on, cfg_dirty;

	ev_timer timer;
	bool timer_on;
	double last_present, last_stats;

	struct span copy_span, dim_span;
	int n_entry;
	struct pl_rect *rects;
	int rects_cap;
	long n_cqz, n_cqz_empty, n_present, n_timer_calls, n_timer_present, n_skipped_busy, n_gl_errors;
};

static struct state st;
static FILE *log_file;

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void say(const char *fmt, ...) {
	if (!log_file) {
		const char *path = getenv("SQUINT_LOG");
		log_file = fopen(path ? path : "/tmp/squint.log", "a");
		if (!log_file) {
			return;
		}
	}
	va_list ap;
	va_start(ap, fmt);
	fprintf(log_file, "[%.3f] ", now_s());
	vfprintf(log_file, fmt, ap);
	fputc('\n', log_file);
	va_end(ap);
	fflush(log_file);
}

static void pl_log_cb(const char *msg) {
	say("pipeline: %s", msg);
}

static void span_init(struct span *s) {
	memset(s, 0, sizeof *s);
	for (int i = 0; i < NSLOT; i++) {
		glGenQueries(2, s->q[i]);
	}
	s->ready = true;
}

static void span_free(struct span *s) {
	if (!s->ready) {
		return;
	}
	for (int i = 0; i < NSLOT; i++) {
		glDeleteQueries(2, s->q[i]);
	}
	s->ready = false;
}

static void span_begin(struct span *s) {
	glQueryCounter(s->q[s->head][0], GL_TIMESTAMP);
}

static void span_end(struct span *s) {
	glQueryCounter(s->q[s->head][1], GL_TIMESTAMP);
	s->issued[s->head] = true;
	s->head = (s->head + 1) % NSLOT;
	// The slot at head is the oldest one. Collect it if its result is ready.
	int i = s->head;
	GLint avail = 0;
	if (s->issued[i]) {
		glGetQueryObjectiv(s->q[i][1], GL_QUERY_RESULT_AVAILABLE, &avail);
		if (avail) {
			GLuint64 t0, t1;
			glGetQueryObjectui64v(s->q[i][0], GL_QUERY_RESULT, &t0);
			glGetQueryObjectui64v(s->q[i][1], GL_QUERY_RESULT, &t1);
			s->sum_us += (double)(t1 - t0) / 1000.0;
			s->n++;
			s->issued[i] = false;
		}
	}
}

static void log_cfg(const char *why) {
	say("%s: enabled=%g target=%g min_gain=%g knee=%g tau_ms=%g falloff_px=%g gamma=%g", why,
	    st.cfg.v[SQ_ENABLED], st.cfg.v[SQ_TARGET], st.cfg.v[SQ_MIN_GAIN], st.cfg.v[SQ_KNEE],
	    st.cfg.v[SQ_TAU_MS], st.cfg.v[SQ_FALLOFF_PX], st.cfg.v[SQ_GAMMA]);
}

static void log_stats(const char *why) {
	double t = now_s();
	double secs = st.last_stats > 0 ? t - st.last_stats : 0;
	st.last_stats = t;
	say("%s: entry=%d cqz=%ld (empty %ld) present=%ld timer_calls=%ld timer_present=%ld skipped_busy=%ld "
	    "gl_errors=%ld copy_us=%.1f (n=%ld) pipeline_us=%.1f (n=%ld) active=%d "
	    "(%.1f s since last stats)",
	    why, st.n_entry, st.n_cqz, st.n_cqz_empty, st.n_present, st.n_timer_calls, st.n_timer_present,
	    st.n_skipped_busy, st.n_gl_errors,
	    st.copy_span.n ? st.copy_span.sum_us / (double)st.copy_span.n : 0.0, st.copy_span.n,
	    st.dim_span.n ? st.dim_span.sum_us / (double)st.dim_span.n : 0.0, st.dim_span.n,
	    pl_active(&st.pl, &st.cfg, t), secs);
}

// Root size and monitor rectangles come from the plugin's own X connection. RandR
// rectangles are already in root coordinates, rotation included. A monitor change that
// leaves the root size unchanged does not reach the plugin, because picom only calls
// root_change on a size change.
static void query_screen(void) {
	if (!st.dpy) {
		st.dpy = XOpenDisplay(NULL);
	}
	if (!st.dpy) {
		return;
	}
	Window root = DefaultRootWindow(st.dpy);
	XWindowAttributes a;
	if (!XGetWindowAttributes(st.dpy, root, &a)) {
		return;
	}
	st.W = a.width;
	st.H = a.height;
	st.nmon = 0;
	int n = 0;
	XRRMonitorInfo *mi = XRRGetMonitors(st.dpy, root, True, &n);
	for (int i = 0; mi && i < n && st.nmon < PL_MAXMON; i++) {
		if (mi[i].width > 0 && mi[i].height > 0) {
			st.mons[st.nmon++] =
			    (struct pl_mon){mi[i].x, mi[i].y, mi[i].width, mi[i].height};
		}
	}
	if (mi) {
		XRRFreeMonitors(mi);
	}
	if (st.nmon == 0) {
		st.mons[st.nmon++] = (struct pl_mon){0, 0, st.W, st.H};
	}
	say("screen %dx%d, %d monitor(s)", st.W, st.H, st.nmon);
	for (int i = 0; i < st.nmon; i++) {
		say("  monitor %d: %dx%d+%d+%d", i, st.mons[i].w, st.mons[i].h, st.mons[i].x,
		    st.mons[i].y);
	}
}

// Context must be current and the caller's GL state saved.
static bool ensure_gl(void) {
	if (st.pl_failed) {
		return false;
	}
	if (st.W <= 0) {
		query_screen();
	}
	if (st.W <= 0 || st.nmon <= 0) {
		return false;
	}
	if (!st.pl.ready) {
		st.pl.target_fbo = 0;
		st.pl.read_buf = GL_BACK;
		st.pl.draw_buf = GL_BACK;
		if (!pl_init(&st.pl)) {
			st.pl_failed = true;
			say("pipeline init failed, dimming disabled until picom restarts");
			return false;
		}
		span_init(&st.copy_span);
		span_init(&st.dim_span);
		say("pipeline ready");
	}
	if (pl_set_monitors(&st.pl, st.mons, st.nmon)) {
		say("monitors set (%d)", st.nmon);
	}
	if (pl_ensure_screen(&st.pl, st.W, st.H)) {
		say("screen textures %dx%d", st.W, st.H);
	}
	return true;
}

static void check_gl(const char *where) {
	GLenum e;
	while ((e = glGetError()) != GL_NO_ERROR) {
		st.n_gl_errors++;
		say("GL error 0x%x after %s", e, where);
	}
}

static bool w_cqz(struct backend_base *b, ivec2 origin, image_handle target,
                  image_handle source, const region_t *region) {
	bool ok = st.orig_cqz(b, origin, target, source, region);
	st.n_cqz++;
	if (target != b->ops.back_buffer(b)) {
		return ok;
	}
	if (origin.x != 0 || origin.y != 0) {
		say("unexpected origin %d,%d", origin.x, origin.y);
	}
	struct pl_glsave g;
	pl_gl_save(&g);
	while (glGetError() != GL_NO_ERROR) {
	}
	if (ensure_gl()) {
		int n = 0;
		pixman_box32_t *box = pixman_region32_rectangles((region_t *)region, &n);
		if (n == 0) {
			st.n_cqz_empty++;
		}
		// Every rectangle is copied on its own. A bounding box or a full copy would pull
		// this plugin's own dimmed pixels from undamaged areas into S.
		if (n > st.rects_cap) {
			st.rects_cap = n * 2;
			st.rects = realloc(st.rects, (size_t)st.rects_cap * sizeof *st.rects);
		}
		for (int i = 0; i < n; i++) {
			st.rects[i] = (struct pl_rect){box[i].x1, box[i].y1, box[i].x2, box[i].y2};
		}
		span_begin(&st.copy_span);
		pl_capture(&st.pl, st.rects, n, false, now_s());
		span_end(&st.copy_span);
		check_gl("capture");
	}
	pl_gl_restore(&g);
	return ok;
}

// Runs the pipeline with the host's GL state saved and restored around it.
static void run_pipeline(void) {
	struct pl_glsave g;
	pl_gl_save(&g);
	while (glGetError() != GL_NO_ERROR) {
	}
	if (ensure_gl() && st.pl.s_valid) {
		span_begin(&st.dim_span);
		pl_render(&st.pl, &st.cfg, now_s());
		span_end(&st.dim_span);
		check_gl("render");
	}
	pl_gl_restore(&g);
}

static bool w_present(struct backend_base *b) {
	if (st.cfg_dirty) {
		pl_touch(&st.pl, now_s());
		st.cfg_dirty = false;
	}
	run_pipeline();
	bool ok = st.orig_present(b);
	st.last_present = now_s();
	st.n_present++;
	if (st.n_present % STATS_EVERY == 0) {
		log_stats("present");
	}
	return ok;
}

static void w_root_change(backend_t *b, session_t *ps) {
	say("root_change");
	if (st.orig_root_change) {
		st.orig_root_change(b, ps);
	}
	query_screen();
}

static void w_deinit(backend_t *b) {
	say("deinit");
	log_stats("deinit");
	if (st.timer_on) {
		ev_timer_stop(b->loop, &st.timer);
		st.timer_on = false;
	}
	if (st.conf_watch_on) {
		ev_stat_stop(b->loop, &st.conf_watch);
		st.conf_watch_on = false;
	}
	span_free(&st.copy_span);
	span_free(&st.dim_span);
	pl_destroy(&st.pl);
	memset(&st.pl, 0, sizeof st.pl);
	st.pl_failed = false;
	st.b = NULL;
	st.orig_deinit(b);
}

// libev calls this when the config file is replaced, changed, or created. squintctl
// writes by rename, which libev reports as a new inode. No GL work happens here. The next
// present or timer tick picks the new values up, with the context known to be current.
static void conf_cb(struct ev_loop *loop, ev_stat *w, int revents) {
	(void)loop;
	(void)w;
	(void)revents;
	sq_load(st.conf_path, &st.cfg);
	st.cfg_dirty = true;
	log_cfg("config reloaded");
}

// Presents on its own while a smoothing transition may still be running and picom has
// drawn nothing, so release continues and config changes show up without any X damage.
static void timer_cb(struct ev_loop *loop, ev_timer *w, int revents) {
	(void)loop;
	(void)w;
	(void)revents;
	st.n_timer_calls++;
	if (!st.b || st.pl_failed || !st.pl.s_valid) {
		return;
	}
	double now = now_s();
	if (st.cfg_dirty) {
		pl_touch(&st.pl, now);
		st.cfg_dirty = false;
	}
	if (!pl_active(&st.pl, &st.cfg, now) || now - st.last_present < 0.015) {
		return;
	}
	if (st.b->busy) {
		st.n_skipped_busy++;
		return;
	}
	// picom's present ends the GL_TIME_ELAPSED query that ops.prepare begins. Without this
	// call the present raises GL_INVALID_OPERATION and desynchronizes picom's frame timing.
	region_t none;
	pixman_region32_init(&none);
	if (st.b->ops.prepare) {
		st.b->ops.prepare(st.b, &none);
	}
	pixman_region32_fini(&none);
	run_pipeline();
	st.orig_present(st.b);
	st.last_present = now_s();
	st.n_timer_present++;
	if (st.n_timer_present % STATS_EVERY == 0) {
		log_stats("timer");
	}
}

static void entry(struct backend_base *b, void *user_data) {
	(void)user_data;
	st.n_entry++;
	if (!b) {
		// picom calls plugins before it checks that backend init succeeded.
		say("entry #%d with NULL backend", st.n_entry);
		return;
	}
	say("entry #%d backend=%p", st.n_entry, (void *)b);
	pl_logger = pl_log_cb;

	st.b = b;
	st.W = st.H = st.nmon = 0;
	st.orig_cqz = b->ops.copy_area_quantize;
	st.orig_present = b->ops.present;
	st.orig_deinit = b->ops.deinit;
	st.orig_root_change = b->ops.root_change;
	b->ops.copy_area_quantize = w_cqz;
	b->ops.present = w_present;
	b->ops.deinit = w_deinit;
	b->ops.root_change = w_root_change;

	sq_path(st.conf_path, sizeof st.conf_path);
	sq_load(st.conf_path, &st.cfg);
	log_cfg("config loaded");
	ev_stat_init(&st.conf_watch, conf_cb, st.conf_path, 0.0);
	ev_stat_start(b->loop, &st.conf_watch);
	st.conf_watch_on = true;

	ev_timer_init(&st.timer, timer_cb, 0.0, 1.0 / 60.0);
	ev_timer_start(b->loop, &st.timer);
	st.timer_on = true;
	say("wrapped");
}

__attribute__((constructor)) static void squint_load(void) {
	const struct picom_api *api =
	    picom_api_get_interfaces(PICOM_API_MAJOR, PICOM_API_MINOR, "squint");
	if (!api) {
		say("picom_api_get_interfaces failed");
		return;
	}
	if (!api->add_backend_plugin("glx", PICOM_BACKEND_MAJOR, PICOM_BACKEND_MINOR, entry,
	                             NULL)) {
		say("add_backend_plugin failed");
	}
	say("loaded");
}
