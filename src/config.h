// Shared by the plugin and squintctl so both agree on keys, ranges, and file format.
//
// File format: one "key = value" per line, "#" starts a comment, unknown keys are ignored.
// Values are clamped to the ranges in sq_params. The tool writes the file atomically
// (temp file, then rename) so the plugin never reads a half-written file.
//
// Settings come in groups, each with an on/off toggle (brightness, dim_tint, static). The
// settings of a group follow its toggle in the enum and are written indented under it, and
// the plugin ignores them while the toggle is off. The leading spaces are only layout: the
// parser strips them, so the order and indentation of lines in the file never matter.

#pragma once

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum {
	SQ_ENABLED,
	SQ_BRIGHTNESS,
	SQ_TARGET,
	SQ_TARGET_LARGE,
	SQ_SIZE_PX,
	SQ_SIZE_BIAS,
	SQ_MIN_GAIN,
	SQ_KNEE,
	SQ_DARKENING_MS,
	SQ_BRIGHTENING_MS,
	SQ_SCENE_PCT,
	SQ_FALLOFF_PX,
	SQ_GAMMA,
	SQ_DIM_TINT,
	SQ_DIM_RED,
	SQ_DIM_GREEN,
	SQ_DIM_BLUE,
	SQ_TINT_RED,
	SQ_TINT_GREEN,
	SQ_TINT_BLUE,
	SQ_STATIC,
	SQ_STATIC_AMOUNT,
	SQ_STATIC_GRAIN_PX,
	SQ_STATIC_COLOR,
	SQ_STATIC_HZ,
	SQ_STATIC_LEVEL,
	SQ_STATIC_CONTRAST,
	SQ_STATIC_CLUSTERS,
	SQ_STATIC_SCANLINES,
	SQ_STATIC_SMEAR,
	SQ_STATIC_JITTER_PX,
	SQ_SHORTCUTS,
	SQ_KEY_ENABLED,
	SQ_KEY_BRIGHTNESS,
	SQ_KEY_DIM_TINT,
	SQ_KEY_STATIC,
	SQ_N
};


enum sq_type { SQ_BOOL, SQ_FLOAT, SQ_INT, SQ_STR };

#define SQ_STRLEN 48  // longest text setting, including the terminator

// The toggle that owns setting i, or -1 for a top-level setting or a toggle itself. Settings
// are grouped by position in the enum.
static inline int sq_group(int i) {
	if (i > SQ_BRIGHTNESS && i <= SQ_GAMMA) {
		return SQ_BRIGHTNESS;
	}
	if (i > SQ_DIM_TINT && i <= SQ_TINT_BLUE) {
		return SQ_DIM_TINT;
	}
	if (i > SQ_STATIC && i <= SQ_STATIC_JITTER_PX) {
		return SQ_STATIC;
	}
	if (i > SQ_SHORTCUTS && i <= SQ_KEY_STATIC) {
		return SQ_SHORTCUTS;
	}
	return -1;
}

static inline bool sq_is_toggle(int i) {
	return i == SQ_BRIGHTNESS || i == SQ_DIM_TINT || i == SQ_STATIC || i == SQ_SHORTCUTS;
}

// The toggle a shortcut key flips, or -1 if i is not a shortcut setting.
static inline int sq_key_target(int i) {
	switch (i) {
	case SQ_KEY_ENABLED: return SQ_ENABLED;
	case SQ_KEY_BRIGHTNESS: return SQ_BRIGHTNESS;
	case SQ_KEY_DIM_TINT: return SQ_DIM_TINT;
	case SQ_KEY_STATIC: return SQ_STATIC;
	default: return -1;
	}
}

struct sq_param {
	const char *key;
	enum sq_type type;
	double def, min, max, step;
	const char *help;
};

static const struct sq_param sq_params[SQ_N] = {
    [SQ_ENABLED] = {"enabled", SQ_BOOL, 1, 0, 1, 1, "Master switch for everything squint does."},
    [SQ_BRIGHTNESS] = {"brightness", SQ_BOOL, 1, 0, 1, 1,
                       "Dim bright content. Off leaves brightness alone; the settings below only apply while on."},
    [SQ_TARGET] = {"target", SQ_FLOAT, 0.50, 0.05, 1.0, 0.01,
                   "Linear luminance (0 to 1) that bright pixels are dimmed toward."},
    [SQ_TARGET_LARGE] = {"target_large", SQ_FLOAT, 0.35, 0.05, 1.0, 0.01,
                         "Target inside large bright areas. At or above target, size has no effect."},
    [SQ_SIZE_PX] = {"size_px", SQ_INT, 32, 16, 512, 1,
                    "Radius over which brightness is gathered to judge how large a bright area is. Also the scale of the rise."},
    [SQ_SIZE_BIAS] = {"size_bias", SQ_FLOAT, 2.0, 1.0, 4.0, 0.1,
                      "How much brightness outweighs size. 1 is the plain average, higher counts bright pixels more."},
    [SQ_MIN_GAIN] = {"min_gain", SQ_FLOAT, 0.05, 0.05, 1.0, 0.01,
                     "Lowest gain allowed, so a pixel never goes fully black."},
    [SQ_KNEE] = {"knee", SQ_FLOAT, 0.10, 0.0, 0.5, 0.01,
                 "Width of the soft transition around the target."},
    [SQ_DARKENING_MS] = {"darkening_ms", SQ_INT, 0, 0, 5000, 25,
                         "How slowly dimming fades after content gets darker. Applies only after a large scene change. 0 is instant."},
    [SQ_BRIGHTENING_MS] = {"brightening_ms", SQ_INT, 1500, 0, 5000, 25,
                           "Time for newly brighter content to rise from the brightness and gamma shown before. Applies only after a large scene change. 0 is instant."},
    [SQ_SCENE_PCT] = {"scene_pct", SQ_INT, 33, 1, 100, 1,
                      "Percent of a monitor whose brightness must change significantly to count as a scene change. 100 never does."},
    [SQ_FALLOFF_PX] = {"falloff_px", SQ_INT, 8, 0, 128, 1,
                       "Distance over which the strongest dimming fades out around bright pixels. 0 dims only the bright pixels."},
    [SQ_GAMMA] = {"gamma", SQ_FLOAT, 1.50, 0.5, 3.0, 0.05,
                  "Gamma where dimming is strongest (1 = off). Above 1 darkens midtones, below 1 lifts them."},
    [SQ_DIM_TINT] = {"dim_tint", SQ_BOOL, 0, 0, 1, 1,
                     "Apply the color multipliers below. Off leaves colors alone."},
    [SQ_DIM_RED] = {"dim_red", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                    "Red multiplier where dimming is strongest. It fades in with the dimming. 1 leaves red alone."},
    [SQ_DIM_GREEN] = {"dim_green", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                      "Green multiplier where dimming is strongest."},
    [SQ_DIM_BLUE] = {"dim_blue", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                     "Blue multiplier where dimming is strongest."},
    [SQ_TINT_RED] = {"tint_red", SQ_FLOAT, 1.0, 0.2, 1.0, 0.01,
                     "Red multiplier for the whole screen, dimmed or not. 1 leaves red alone."},
    [SQ_TINT_GREEN] = {"tint_green", SQ_FLOAT, 0.80, 0.2, 1.0, 0.01,
                       "Green multiplier for the whole screen."},
    [SQ_TINT_BLUE] = {"tint_blue", SQ_FLOAT, 0.50, 0.2, 1.0, 0.01,
                      "Blue multiplier for the whole screen."},
    [SQ_STATIC] = {"static", SQ_BOOL, 0, 0, 1, 1,
                   "Lay static over the whole screen, a fresh random pattern every update. Off by default."},
    [SQ_STATIC_AMOUNT] = {"static_amount", SQ_INT, 4, 0, 100, 1,
                          "How much of the screen is static. 100 is pure static, lower mixes it over the content."},
    [SQ_STATIC_GRAIN_PX] = {"static_grain_px", SQ_FLOAT, 16.0, 1.0, 16.0, 0.5,
                            "Size of one speckle in pixels. The description of the video says 1 to 3."},
    [SQ_STATIC_COLOR] = {"static_color", SQ_FLOAT, 0.50, 0.0, 1.0, 0.05,
                         "0 is gray static, 1 is independent noise per color channel."},
    [SQ_STATIC_HZ] = {"static_hz", SQ_INT, 60, 1, 60, 1,
                      "How many times a second the static changes. Each change costs a frame of GPU work."},
    [SQ_STATIC_LEVEL] = {"static_level", SQ_FLOAT, 0.50, 0.0, 1.0, 0.01,
                         "Average brightness of the static, in encoded 0 to 1 values. 0.5 is middle gray."},
    [SQ_STATIC_CONTRAST] = {"static_contrast", SQ_FLOAT, 0.10, 0.0, 0.5, 0.01,
                            "How far speckles stray from the average, as a standard deviation."},
    [SQ_STATIC_CLUSTERS] = {"static_clusters", SQ_FLOAT, 0.30, 0.0, 1.0, 0.05,
                            "How much coarse clustering, speckles grouped 3 to 6 pixels wide, is mixed in."},
    [SQ_STATIC_SCANLINES] = {"static_scanlines", SQ_FLOAT, 0.15, 0.0, 1.0, 0.05,
                             "Darkens every other row to give fine horizontal line structure."},
    [SQ_STATIC_SMEAR] = {"static_smear", SQ_FLOAT, 1.3, 1.0, 6.0, 0.1,
                         "Stretches speckles sideways. 1 is round."},
    [SQ_STATIC_JITTER_PX] = {"static_jitter_px", SQ_FLOAT, 0.5, 0.0, 4.0, 0.1,
                             "Random sideways shift of each row, in pixels, for a slight tracking wobble."},
    [SQ_SHORTCUTS] = {"shortcuts", SQ_BOOL, 0, 0, 1, 1,
                      "Turn on to listen for the shortcut keys below. Off releases every key. Keys are empty until you set them."},
    [SQ_KEY_ENABLED] = {"key_enabled", SQ_STR, 0, 0, 0, 0,
                        "Key that flips enabled. Written like Ctrl+Alt+s or Super+Shift+F5. Empty means none."},
    [SQ_KEY_BRIGHTNESS] = {"key_brightness", SQ_STR, 0, 0, 0, 0,
                           "Key that flips brightness. Empty means none."},
    [SQ_KEY_DIM_TINT] = {"key_dim_tint", SQ_STR, 0, 0, 0, 0,
                         "Key that flips dim_tint. Empty means none."},
    [SQ_KEY_STATIC] = {"key_static", SQ_STR, 0, 0, 0, 0,
                       "Key that flips static. Empty means none."},
};

struct sq_cfg {
	double v[SQ_N];
	char s[SQ_N][SQ_STRLEN];  // text settings (the shortcut keys), indexed like v
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
	memset(c, 0, sizeof *c);
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

// Shortcut chords are written like "Ctrl+Alt+s": modifiers, then one key name, joined by "+".
// Modifier names are Shift, Ctrl or Control, Alt or Mod1, Super or Mod4, and Mod2, Mod3, Mod5,
// in any case. The key name is an X keysym name such as s, F5, space, or Return. The bits of
// the modifier mask are the X11 ones.
enum { SQ_MOD_SHIFT = 1, SQ_MOD_CTRL = 4, SQ_MOD_ALT = 8, SQ_MOD_2 = 16, SQ_MOD_3 = 32, SQ_MOD_SUPER = 64, SQ_MOD_5 = 128 };

// Splits a chord into a modifier mask and a key name. Returns false for a malformed chord.
// An empty chord is not malformed, it is "no key": the result is true with an empty key name.
static inline bool sq_parse_chord(const char *chord, unsigned *mods, char *key, size_t keylen) {
	*mods = 0;
	if (keylen) {
		key[0] = '\0';
	}
	if (!chord || !*chord) {
		return true;
	}
	char buf[SQ_STRLEN];
	if (strlen(chord) >= sizeof buf) {
		return false;
	}
	snprintf(buf, sizeof buf, "%s", chord);
	char *last = NULL;
	char *toks[8];
	int n = 0;
	// Split on every '+' by hand, so an empty part ("Ctrl+" or "+s") is an error and not skipped.
	for (char *p = buf;;) {
		char *plus = strchr(p, '+');
		if (plus) {
			*plus = '\0';
		}
		while (*p == ' ') p++;
		char *e = p + strlen(p);
		while (e > p && e[-1] == ' ') *--e = '\0';
		if (!*p || n >= 8) {
			return false;
		}
		toks[n++] = p;
		if (!plus) {
			break;
		}
		p = plus + 1;
	}
	last = toks[n - 1];
	for (int k = 0; k < n - 1; k++) {
		const char *m = toks[k];
		if (!strcasecmp(m, "shift")) *mods |= SQ_MOD_SHIFT;
		else if (!strcasecmp(m, "ctrl") || !strcasecmp(m, "control")) *mods |= SQ_MOD_CTRL;
		else if (!strcasecmp(m, "alt") || !strcasecmp(m, "mod1")) *mods |= SQ_MOD_ALT;
		else if (!strcasecmp(m, "super") || !strcasecmp(m, "mod4")) *mods |= SQ_MOD_SUPER;
		else if (!strcasecmp(m, "mod2")) *mods |= SQ_MOD_2;
		else if (!strcasecmp(m, "mod3")) *mods |= SQ_MOD_3;
		else if (!strcasecmp(m, "mod5")) *mods |= SQ_MOD_5;
		else return false;
	}
	for (const char *p = last; *p; p++) {
		bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
		if (!ok) {
			return false;
		}
	}
	if (strlen(last) >= keylen) {
		return false;
	}
	snprintf(key, keylen, "%s", last);
	return true;
}

// Sets one setting by key. Returns false for an unknown key, a bad value, or a malformed
// shortcut chord.
static inline bool sq_set(struct sq_cfg *c, const char *key, const char *value) {
	int i = sq_find(key);
	if (i < 0) {
		return false;
	}
	if (sq_params[i].type == SQ_STR) {
		unsigned mods;
		char keyname[SQ_STRLEN];
		if (!sq_parse_chord(value, &mods, keyname, sizeof keyname)) {
			return false;
		}
		snprintf(c->s[i], SQ_STRLEN, "%s", value);
		return true;
	}
	double x;
	if (!sq_parse_value(i, value, &x)) {
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

// The text of setting i for display and for the file: the number for numeric settings and
// the string for text settings.
static inline void sq_format_cfg(const struct sq_cfg *c, int i, char *buf, size_t n) {
	if (sq_params[i].type == SQ_STR) {
		snprintf(buf, n, "%s", c->s[i]);
	} else {
		sq_format_value(i, c->v[i], buf, n);
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
		char v[SQ_STRLEN + 8];
		sq_format_cfg(c, i, v, sizeof v);
		if (sq_is_toggle(i)) {
			fputc('\n', f);
		}
		fprintf(f, "%s%s =%s%s\n", sq_group(i) >= 0 ? "  " : "", sq_params[i].key, *v ? " " : "", v);
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
