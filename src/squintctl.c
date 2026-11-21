// squintctl: edit ~/.config/squint.conf. The plugin watches the file, so each change
// takes effect on screen immediately.
//
//   squintctl                 interactive tuning screen
//   squintctl show            print the config file location and the current settings (--show works too)
//   squintctl set KEY VALUE   change one setting (a number is clamped to its range)
//   squintctl toggle NAME     flip one on/off setting: enabled, brightness, dim_tint, static, rgb, or shortcuts
//
// Shortcut keys are settings too. Write a chord like Ctrl+Alt+s or Super+Shift+F5:
//   squintctl set key_static Ctrl+Alt+s
// An empty value removes the key:
//   squintctl set key_static ""

#include <X11/Xlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "capture.h"
#include "config.h"
#include "keys.h"

static struct termios saved_tio;
static bool raw_on;

static void restore_terminal(void) {
	if (raw_on) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
		fputs("\x1b[?25h\x1b[?1049l", stdout);
		fflush(stdout);
		raw_on = false;
	}
}

static void on_winch(int sig) {
	(void)sig;  // only interrupts read, so the main loop draws again at the new size
}

static void on_signal(int sig) {
	restore_terminal();
	_exit(128 + sig);
}

static void enter_raw(void) {
	tcgetattr(STDIN_FILENO, &saved_tio);
	struct termios t = saved_tio;
	t.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
	fputs("\x1b[?1049h\x1b[?25l", stdout);
	raw_on = true;
	atexit(restore_terminal);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_winch;  // no SA_RESTART: read must return with EINTR
	sigaction(SIGWINCH, &sa, NULL);
}

// Checks that a chord names a key this keyboard has. An empty chord is fine. Fills why with a
// reason when it returns false. Without an X display only the syntax is checked.
static bool chord_ok(const char *chord, char *why, size_t whylen) {
	unsigned mods;
	char key[SQ_STRLEN];
	if (!sq_parse_chord(chord, &mods, key, sizeof key)) {
		snprintf(why, whylen, "'%s' is not a chord. Write modifiers and a key joined by +, like Ctrl+Alt+s", chord);
		return false;
	}
	if (!*key) {
		return true;
	}
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		return true;
	}
	KeySym ks = sq_keysym(d, key);
	bool ok = ks != NoSymbol && XKeysymToKeycode(d, ks) != 0;
	XCloseDisplay(d);
	if (!ok) {
		snprintf(why, whylen, "no key named '%s' on this keyboard", key);
	}
	return ok;
}

// Prints s word-wrapped to width columns on exactly nlines lines, so the screen layout does
// not move when the text changes. Text that does not fit is cut off.
static void print_wrapped(const char *s, int width, int nlines) {
	for (int ln = 0; ln < nlines; ln++) {
		while (*s == ' ') {
			s++;
		}
		int len = (int)strlen(s);
		if (len > width) {
			len = width;
			if (ln < nlines - 1) {
				while (len > 0 && s[len] != ' ') {
					len--;
				}
				if (len == 0) {
					len = width;
				}
			}
		}
		printf(" %.*s\r\n", len, s);
		s += len;
	}
}

#define FOOTER_LINES 12  // blank, 4 help, blank, 3 keys, blank, status

// What the screen lists, in order: a heading for each run of settings that share a prefix, and
// the settings themselves with the prefix and the group's own name taken off. The real key
// names are in the help line and in the file.
struct drow {
	int idx;  // the setting, or -1 for a heading
	int group_of;  // the setting a heading stands in front of, to grey it with its group
	int indent;
	char label[40];
};
static struct drow drows[SQ_N * 2];
static int ndrows, label_w, rowof[SQ_N];

// The key of setting i without the name of its group and the underscore after it.
static const char *short_name(int i) {
	int g = sq_group(i);
	const char *k = sq_params[i].key;
	if (g >= 0) {
		size_t n = strlen(sq_params[g].key);
		if (!strncmp(k, sq_params[g].key, n) && k[n] == '_') {
			return k + n + 1;
		}
	}
	return k;
}

// Length of the part of n before its first underscore, or 0 when there is none.
static size_t prefix_len(const char *n) {
	const char *u = strchr(n, '_');
	return u ? (size_t)(u - n) : 0;
}

static void build_rows(void) {
	if (ndrows) {
		return;
	}
	int i = 0;
	while (i < SQ_N) {
		int g = sq_group(i);
		int base = g >= 0 ? 2 : 0;
		const char *a = short_name(i);
		size_t pl = prefix_len(a);
		int j = i + 1;
		while (pl && j < SQ_N && sq_group(j) == g) {
			const char *b = short_name(j);
			if (prefix_len(b) != pl || strncmp(a, b, pl) != 0) {
				break;
			}
			j++;
		}
		if (pl && j - i >= 2) {
			struct drow *h = &drows[ndrows++];
			h->idx = -1;
			h->group_of = i;
			h->indent = base;
			snprintf(h->label, sizeof h->label, "%.*s", (int)pl, a);
			for (int k = i; k < j; k++) {
				struct drow *r = &drows[ndrows];
				rowof[k] = ndrows++;
				r->idx = k;
				r->indent = base + 2;
				snprintf(r->label, sizeof r->label, "%s", short_name(k) + pl + 1);
			}
			i = j;
		} else {
			struct drow *r = &drows[ndrows];
			rowof[i] = ndrows++;
			r->idx = i;
			r->indent = base;
			snprintf(r->label, sizeof r->label, "%s", a);
			i++;
		}
	}
	label_w = 12;
	for (int k = 0; k < ndrows; k++) {
		int w = drows[k].indent + (int)strlen(drows[k].label);
		label_w = w > label_w ? w : label_w;
	}
}

// The first row shown. It moves only as far as needed to keep the selected setting in view.
static int view_top;

static void draw(const struct sq_cfg *c, const struct sq_cfg *def, const struct sq_cfg *start, int sel, const char *path, const char *status) {
	build_rows();
	struct winsize ws;
	int rows = 24, cols = 80;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
		rows = ws.ws_row;
		cols = ws.ws_col;
	}
	int avail = rows - 4 - FOOTER_LINES;
	if (avail < 5) {
		avail = 5;
	}
	if (avail > ndrows) {
		avail = ndrows;
	}
	int want = rowof[sel];
	if (want > 0 && drows[want - 1].idx < 0) {
		want--;  // keep the heading of the selected setting in view
	}
	if (want < view_top) {
		view_top = want;
	}
	if (rowof[sel] >= view_top + avail) {
		view_top = rowof[sel] - avail + 1;
	}
	if (view_top > ndrows - avail) {
		view_top = ndrows - avail;
	}
	if (view_top < 0) {
		view_top = 0;
	}
	printf("\x1b[2J\x1b[H");
	printf("squint tuning\r\n config: %.*s\r\n\r\n", cols - 10 > 0 ? cols - 10 : 1, path);
	char pos[40] = "";
	if (avail < ndrows) {
		snprintf(pos, sizeof pos, "%d-%d of %d%s%s", view_top + 1, view_top + avail, ndrows, view_top > 0 ? "  \xe2\x86\x91" : "",
		         view_top + avail < ndrows ? "  \xe2\x86\x93" : "");
	}
	printf("   %-*s %-14s %-14s %s\r\n", label_w, "", "value", "default", *pos ? pos : "range");
	for (int r = view_top; r < view_top + avail; r++) {
		const struct drow *d = &drows[r];
		int i = d->idx >= 0 ? d->idx : d->group_of;
		int g = sq_group(i);
		// Greyed out when its group toggle is off, or when the master switch is off. The
		// shortcuts stay lit when the master switch is off, because key_enabled must keep
		// working to turn squint back on.
		bool master_off = i != SQ_ENABLED && c->v[SQ_ENABLED] == 0 && i != SQ_SHORTCUTS && g != SQ_SHORTCUTS;
		bool inactive = (g >= 0 && c->v[g] == 0) || master_off;
		if (d->idx < 0) {
			printf("\x1b[%sm   %*s%s\x1b[0m\r\n", inactive ? "2" : "1;4", d->indent, "", d->label);
			continue;
		}
		char v[SQ_STRLEN + 8], dv[SQ_STRLEN + 8], lo[32], hi[32];
		sq_format_cfg(c, i, v, sizeof v);
		sq_format_cfg(def, i, dv, sizeof dv);
		bool is_str = sq_params[i].type == SQ_STR;
		if (!is_str) {
			sq_format_value(i, sq_params[i].min, lo, sizeof lo);
			sq_format_value(i, sq_params[i].max, hi, sizeof hi);
		}
		bool changed = is_str ? strcmp(c->s[i], start->s[i]) != 0 : c->v[i] != start->v[i];
		if (inactive) {
			printf("\x1b[2m");
		}
		if (i == sel) {
			printf("\x1b[7m");
		}
		printf("%c%c %*s%-*s ", changed ? '*' : ' ', i == sel ? '>' : ' ', d->indent, "", label_w - d->indent, d->label);
		if (is_str) {
			printf("%-14s %-14s", *v ? v : "(none)", *dv ? dv : "(none)");
		} else {
			printf("%-14s %-14s %6s to %-6s", v, dv, lo, hi);
		}
		if (i == sel || inactive) {
			printf("\x1b[0m");
		}
		printf("\r\n");
	}
	printf("\r\n");
	char help[700];
	snprintf(help, sizeof help, "%s: %s", sq_params[sel].key, sq_params[sel].help);
	print_wrapped(help, cols - 2, 4);
	printf("\r\n");
	print_wrapped("up/down select  left/right adjust  PgUp/PgDn x10  space toggle  q quit", cols - 2, 1);
	print_wrapped("r reset to start   d reset to default   R/D all   * changed since start", cols - 2, 1);
	print_wrapped("Enter record a shortcut by pressing it   e type it", cols - 2, 1);
	printf("\r\n %.*s", cols - 2 > 0 ? cols - 2 : 1, status);
	fflush(stdout);
}

static void adjust(struct sq_cfg *c, int i, int steps) {
	const struct sq_param *p = &sq_params[i];
	if (p->type == SQ_STR) {
		return;
	}
	if (p->type == SQ_BOOL) {
		c->v[i] = c->v[i] != 0 ? 0 : 1;
		return;
	}
	// Round to the step grid so repeated presses do not accumulate float error.
	double x = (c->v[i] + steps * p->step) / p->step;
	c->v[i] = sq_clamp(i, floor(x + 0.5) * p->step);
}

// Copies setting i from src into c.
static void reset_one(struct sq_cfg *c, const struct sq_cfg *src, int i) {
	c->v[i] = src->v[i];
	memcpy(c->s[i], src->s[i], SQ_STRLEN);
}

// Records the keys the user presses for text setting i. Returns true when the setting changed.
static bool record_chord(struct sq_cfg *c, const struct sq_cfg *def, const struct sq_cfg *start, int i, const char *path, char *msg, size_t msglen) {
	char prompt[160];
	snprintf(prompt, sizeof prompt, "Press the keys for %s now, for example Super+F1. Esc cancels. (20 s)", sq_params[i].key);
	draw(c, def, start, i, path, prompt);
	char chord[SQ_STRLEN], why[160];
	if (!sq_capture_chord(chord, sizeof chord, why, sizeof why, 20000, -1)) {
		snprintf(msg, msglen, "%s. Press e to type the chord instead", why);
		return false;
	}
	tcflush(STDIN_FILENO, TCIFLUSH);  // the terminal saw some of those keys too
	if (!chord_ok(chord, why, sizeof why)) {
		snprintf(msg, msglen, "%s", why);
		return false;
	}
	if (!strcmp(chord, c->s[i])) {
		snprintf(msg, msglen, "unchanged (%s)", chord);
		return false;
	}
	snprintf(c->s[i], SQ_STRLEN, "%s", chord);
	snprintf(msg, msglen, "recorded %s", chord);
	return true;
}

// Reads a line for text setting i at the bottom of the screen. Enter keeps it, Esc cancels,
// and an empty line clears the setting. Returns true when the setting changed.
static bool edit_string(struct sq_cfg *c, const struct sq_cfg *def, const struct sq_cfg *start, int i, const char *path, char *msg, size_t msglen) {
	char buf[SQ_STRLEN] = "";
	int n = 0;
	snprintf(buf, sizeof buf, "%s", c->s[i]);
	n = (int)strlen(buf);
	for (;;) {
		char prompt[120];
		snprintf(prompt, sizeof prompt, "%s = %s_   (Enter saves, Esc cancels, empty clears)", sq_params[i].key, buf);
		draw(c, def, start, i, path, prompt);
		unsigned char ch;
		if (read(STDIN_FILENO, &ch, 1) != 1) {
			return false;
		}
		if (ch == 0x1b) {
			tcflush(STDIN_FILENO, TCIFLUSH);  // drop the rest of an arrow key sequence
			snprintf(msg, msglen, "cancelled");
			return false;
		}
		if (ch == '\r' || ch == '\n') {
			char why[160];
			if (!chord_ok(buf, why, sizeof why)) {
				snprintf(msg, msglen, "%s", why);
				return false;
			}
			if (!strcmp(buf, c->s[i])) {
				snprintf(msg, msglen, "unchanged");
				return false;
			}
			snprintf(c->s[i], SQ_STRLEN, "%s", buf);
			return true;
		}
		if ((ch == 0x7f || ch == 0x08) && n > 0) {
			buf[--n] = '\0';
		} else if (ch >= 0x20 && ch < 0x7f && n < SQ_STRLEN - 1) {
			buf[n++] = (char)ch;
			buf[n] = '\0';
		}
	}
}

static int interactive(const char *path) {
	if (!isatty(STDIN_FILENO)) {
		fprintf(stderr, "squintctl: stdin is not a terminal (use 'show', 'set', or 'toggle')\n");
		return 1;
	}
	struct sq_cfg c;
	sq_load(path, &c);
	const struct sq_cfg start = c;  // what the file held when the tool opened
	struct sq_cfg def;
	sq_defaults(&def);
	enter_raw();
	int sel = 0;
	char msg[200] = "";
	const char *status = "";
	for (;;) {
		draw(&c, &def, &start, sel, path, status);
		unsigned char ch;
		ssize_t got = read(STDIN_FILENO, &ch, 1);
		if (got < 0 && errno == EINTR) {
			continue;  // the terminal was resized
		}
		if (got != 1) {
			break;
		}
		bool changed = false;
		status = "";
		if (ch == 'q') {
			break;
		} else if (ch == 'k') {
			sel = (sel + SQ_N - 1) % SQ_N;
		} else if (ch == 'j') {
			sel = (sel + 1) % SQ_N;
		} else if (ch == 'h' || ch == 'l') {
			adjust(&c, sel, ch == 'l' ? 1 : -1);
			changed = sq_params[sel].type != SQ_STR;
		} else if (ch == ' ') {
			if (sq_params[sel].type == SQ_BOOL) {
				adjust(&c, sel, 1);
				changed = true;
			}
		} else if (ch == '\r' || ch == '\n') {
			if (sq_params[sel].type == SQ_STR) {
				msg[0] = '\0';
				changed = record_chord(&c, &def, &start, sel, path, msg, sizeof msg);
				status = msg;
			}
		} else if (ch == 'e') {
			if (sq_params[sel].type == SQ_STR) {
				msg[0] = '\0';
				changed = edit_string(&c, &def, &start, sel, path, msg, sizeof msg);
				status = msg;
			}
		} else if (ch == 'r' || ch == 'd') {
			reset_one(&c, ch == 'r' ? &start : &def, sel);
			changed = true;
		} else if (ch == 'R' || ch == 'D') {
			c = ch == 'R' ? start : def;
			changed = true;
		} else if (ch == 0x1b) {
			unsigned char seq[3] = {0, 0, 0};
			if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
			    read(STDIN_FILENO, &seq[1], 1) == 1) {
				if (seq[1] == 'A') {
					sel = (sel + SQ_N - 1) % SQ_N;
				} else if (seq[1] == 'B') {
					sel = (sel + 1) % SQ_N;
				} else if (seq[1] == 'C' || seq[1] == 'D') {
					adjust(&c, sel, seq[1] == 'C' ? 1 : -1);
					changed = sq_params[sel].type != SQ_STR;
				} else if (seq[1] == '5' || seq[1] == '6') {
					if (read(STDIN_FILENO, &seq[2], 1) != 1) {
						break;
					}
					adjust(&c, sel, seq[1] == '6' ? -10 : 10);
					changed = sq_params[sel].type != SQ_STR;
				}
			}
		}
		if (changed) {
			if (sq_save(path, &c) == 0) {
				status = "saved";
			} else {
				snprintf(msg, sizeof msg, "save failed: %s", strerror(errno));
				status = msg;
			}
		}
	}
	return 0;
}

int main(int argc, char **argv) {
	char path[512];
	sq_path(path, sizeof path);
	if (argc == 1) {
		return interactive(path);
	}
	struct sq_cfg c;
	sq_load(path, &c);
	if (argc == 2 && (!strcmp(argv[1], "show") || !strcmp(argv[1], "--show"))) {
		printf("# config file: %s%s\n", path, access(path, F_OK) == 0 ? "" : " (not created yet, showing defaults)");
		for (int i = 0; i < SQ_N; i++) {
			char v[SQ_STRLEN + 8];
			sq_format_cfg(&c, i, v, sizeof v);
			printf("%s%s =%s%s\n", sq_group(i) >= 0 ? "  " : "", sq_params[i].key, *v ? " " : "", v);
		}
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "set")) {
		int i = sq_find(argv[2]);
		if (i < 0) {
			fprintf(stderr, "squintctl: unknown key '%s'\n", argv[2]);
			return 1;
		}
		char why[160];
		if (sq_params[i].type == SQ_STR && !chord_ok(argv[3], why, sizeof why)) {
			fprintf(stderr, "squintctl: %s\n", why);
			return 1;
		}
		if (!sq_set(&c, argv[2], argv[3])) {
			fprintf(stderr, "squintctl: bad value '%s' for %s\n", argv[3], argv[2]);
			return 1;
		}
		if (sq_save(path, &c) != 0) {
			fprintf(stderr, "squintctl: cannot write %s: %s\n", path, strerror(errno));
			return 1;
		}
		char v[SQ_STRLEN + 8];
		sq_format_cfg(&c, i, v, sizeof v);
		printf("%s = %s\n", sq_params[i].key, v);
		if (sq_group(i) == SQ_SHORTCUTS && *v && c.v[SQ_SHORTCUTS] == 0) {
			printf("note: shortcuts is off, so this key does nothing until you run: squintctl toggle shortcuts\n");
		}
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "toggle")) {
		int i = sq_find(argv[2]);
		if (i < 0 || sq_params[i].type != SQ_BOOL) {
			fprintf(stderr, "squintctl: '%s' is not an on/off setting\n", argv[2]);
			return 1;
		}
		c.v[i] = c.v[i] != 0 ? 0 : 1;
		if (sq_save(path, &c) != 0) {
			fprintf(stderr, "squintctl: cannot write %s: %s\n", path, strerror(errno));
			return 1;
		}
		printf("%s = %s\n", sq_params[i].key, c.v[i] != 0 ? "true" : "false");
		return 0;
	}
	fprintf(stderr, "usage: squintctl [show | set KEY VALUE | toggle NAME]\n");
	return 2;
}
