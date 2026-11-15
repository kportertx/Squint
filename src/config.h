// Shared by the plugin and squintctl so both agree on keys, ranges, and file format.
//
// File format: one "key = value" per line, "#" starts a comment, unknown keys are ignored.
// Values are clamped to the ranges in sq_params. The tool writes the file atomically
// (temp file, then rename) so the plugin never reads a half-written file.

#pragma once

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	SQ_ENABLED,
	SQ_TARGET,
	SQ_TARGET_LARGE,
	SQ_SIZE_PX,
	SQ_SIZE_BIAS,
	SQ_MIN_GAIN,
	SQ_KNEE,
	SQ_DARKENING_MS,
	SQ_BRIGHTENING_MS,
	SQ_FALLOFF_PX,
	SQ_GAMMA,
	SQ_DIM_RED,
	SQ_DIM_GREEN,
	SQ_DIM_BLUE,
	SQ_TINT_RED,
	SQ_TINT_GREEN,
	SQ_TINT_BLUE,
	SQ_N
};


enum sq_type { SQ_BOOL, SQ_FLOAT, SQ_INT };

struct sq_param {
	const char *key;
	enum sq_type type;
	double def, min, max, step;
	const char *help;
};

static const struct sq_param sq_params[SQ_N] = {
    [SQ_ENABLED] = {"enabled", SQ_BOOL, 1, 0, 1, 1, "Master switch."},
    [SQ_TARGET] = {"target", SQ_FLOAT, 0.45, 0.05, 1.0, 0.01,
                   "Linear luminance (0 to 1) that bright pixels are dimmed toward."},
    [SQ_TARGET_LARGE] = {"target_large", SQ_FLOAT, 0.20, 0.05, 1.0, 0.01,
                         "Target inside large bright areas. At or above target, size has no effect."},
    [SQ_SIZE_PX] = {"size_px", SQ_INT, 192, 16, 512, 1,
                    "Radius over which brightness is gathered to judge how large a bright area is. Also the scale of the rise."},
    [SQ_SIZE_BIAS] = {"size_bias", SQ_FLOAT, 2.0, 1.0, 4.0, 0.1,
                      "How much brightness outweighs size. 1 is the plain average, higher counts bright pixels more."},
    [SQ_MIN_GAIN] = {"min_gain", SQ_FLOAT, 0.35, 0.05, 1.0, 0.01,
                     "Lowest gain allowed, so a pixel never goes fully black."},
    [SQ_KNEE] = {"knee", SQ_FLOAT, 0.10, 0.0, 0.5, 0.01,
                 "Width of the soft transition around the target."},
    [SQ_DARKENING_MS] = {"darkening_ms", SQ_INT, 0, 0, 5000, 25,
                         "How slowly dimming fades after content gets darker. Applies only after a large scene change. 0 is instant."},
    [SQ_BRIGHTENING_MS] = {"brightening_ms", SQ_INT, 0, 0, 5000, 25,
                           "Time for newly brighter content to rise from the brightness and gamma shown before. Applies only after a large scene change. 0 is instant."},
    [SQ_FALLOFF_PX] = {"falloff_px", SQ_INT, 24, 0, 128, 1,
                       "Distance over which the strongest dimming fades out around bright pixels. 0 dims only the bright pixels."},
    [SQ_GAMMA] = {"gamma", SQ_FLOAT, 1.0, 0.5, 3.0, 0.05,
                  "Gamma where dimming is strongest (1 = off). Above 1 darkens midtones, below 1 lifts them."},
    [SQ_DIM_RED] = {"dim_red", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                    "Red multiplier where dimming is strongest. It fades in with the dimming. 1 leaves red alone."},
    [SQ_DIM_GREEN] = {"dim_green", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                      "Green multiplier where dimming is strongest."},
    [SQ_DIM_BLUE] = {"dim_blue", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                     "Blue multiplier where dimming is strongest."},
    [SQ_TINT_RED] = {"tint_red", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                     "Red multiplier for the whole screen, dimmed or not. 1 leaves red alone."},
    [SQ_TINT_GREEN] = {"tint_green", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                       "Green multiplier for the whole screen."},
    [SQ_TINT_BLUE] = {"tint_blue", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                      "Blue multiplier for the whole screen."},
};

struct sq_cfg {
	double v[SQ_N];
};

static inline double sq_clamp(int i, double x) {
	const struct sq_param *p = &sq_params[i];
	if (!(x >= p->min)) {
		x = p->min;
	}
	if (x > p->max) {
		x = p->max;
	}
	if (p->type == SQ_INT || p->type == SQ_BOOL) {
		x = floor(x + 0.5);
	}
	return x;
}

static inline void sq_defaults(struct sq_cfg *c) {
	for (int i = 0; i < SQ_N; i++) {
		c->v[i] = sq_params[i].def;
	}
}

// The path must be the same in the plugin (running inside picom) and in the tool, so it
// depends only on $SQUINT_CONF and $HOME, not on $XDG_CONFIG_HOME.
static inline const char *sq_path(char *buf, size_t n) {
	const char *e = getenv("SQUINT_CONF");
	if (e && *e) {
		snprintf(buf, n, "%s", e);
		return buf;
	}
	const char *home = getenv("HOME");
	snprintf(buf, n, "%s/.config/squint.conf", home && *home ? home : "");
	return buf;
}

static inline int sq_find(const char *key) {
	for (int i = 0; i < SQ_N; i++) {
		if (strcmp(sq_params[i].key, key) == 0) {
			return i;
		}
	}
	// Names used before the renames, so an old config file keeps working.
	if (strcmp(key, "tau_ms") == 0) {
		return SQ_DARKENING_MS;
	}
	if (strcmp(key, "rise_ms") == 0) {
		return SQ_BRIGHTENING_MS;
	}
	return -1;
}

static inline bool sq_parse_value(int i, const char *s, double *out) {
	if (sq_params[i].type == SQ_BOOL) {
		if (!strcmp(s, "true") || !strcmp(s, "on") || !strcmp(s, "1")) {
			*out = 1;
			return true;
		}
		if (!strcmp(s, "false") || !strcmp(s, "off") || !strcmp(s, "0")) {
			*out = 0;
			return true;
		}
		return false;
	}
	char *end;
	double x = strtod(s, &end);
	if (end == s || *end != '\0') {
		return false;
	}
	*out = sq_clamp(i, x);
	return true;
}

// Sets one setting by key. Returns false for an unknown key or a bad value.
static inline bool sq_set(struct sq_cfg *c, const char *key, const char *value) {
	int i = sq_find(key);
	double x;
	if (i < 0 || !sq_parse_value(i, value, &x)) {
		return false;
	}
	c->v[i] = x;
	return true;
}

// Starts from defaults, so a missing or partial file is fine.
static inline void sq_load(const char *path, struct sq_cfg *c) {
	sq_defaults(c);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char *hash = strchr(line, '#');
		if (hash) {
			*hash = '\0';
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *k = line, *v = eq + 1;
		while (*k == ' ' || *k == '\t') k++;
		char *ke = k + strlen(k);
		while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
		while (*v == ' ' || *v == '\t') v++;
		char *ve = v + strlen(v);
		while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r')) *--ve = '\0';
		sq_set(c, k, v);
	}
	fclose(f);
}

static inline void sq_format_value(int i, double x, char *buf, size_t n) {
	switch (sq_params[i].type) {
	case SQ_BOOL: snprintf(buf, n, "%s", x != 0 ? "true" : "false"); break;
	case SQ_INT: snprintf(buf, n, "%d", (int)x); break;
	default: snprintf(buf, n, "%.2f", x); break;
	}
}

// Returns 0 on success, -1 with errno set on failure.
static inline int sq_save(const char *path, const struct sq_cfg *c) {
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return -1;
	}
	fprintf(f, "# squint settings. Edited by squintctl, reloaded by the plugin on change.\n");
	for (int i = 0; i < SQ_N; i++) {
		char v[32];
		sq_format_value(i, c->v[i], v, sizeof v);
		fprintf(f, "%s = %s\n", sq_params[i].key, v);
	}
	if (fclose(f) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		int e = errno;
		unlink(tmp);
		errno = e;
		return -1;
	}
	return 0;
}
