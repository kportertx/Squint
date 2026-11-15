// squintctl: edit ~/.config/squint.conf. The plugin watches the file, so each change
// takes effect on screen immediately.
//
//   squintctl                 interactive tuning screen
//   squintctl show            print the config file location and the current settings (--show works too)
//   squintctl set KEY VALUE   change one setting (a number is clamped to its range)
//   squintctl toggle NAME     flip one on/off setting: enabled, brightness, dim_tint, static, or shortcuts
//
// Shortcut keys are settings too. Write a chord like Ctrl+Alt+s or Super+Shift+F5:
//   squintctl set key_static Ctrl+Alt+s
// An empty value removes the key:
//   squintctl set key_static ""

#include <X11/Xlib.h>
#include <signal.h>
#include <string.h>
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

static void draw(const struct sq_cfg *c, int sel, const char *path, const char *status) {
	printf("\x1b[2J\x1b[H");
	printf("squint tuning\r\nconfig: %s\r\n\r\n", path);
	for (int i = 0; i < SQ_N; i++) {
		char v[SQ_STRLEN + 8], lo[32], hi[32];
		sq_format_cfg(c, i, v, sizeof v);
		bool is_str = sq_params[i].type == SQ_STR;
		if (!is_str) {
			sq_format_value(i, sq_params[i].min, lo, sizeof lo);
			sq_format_value(i, sq_params[i].max, hi, sizeof hi);
		}
		int g = sq_group(i);
		// Greyed out when its group toggle is off, or when the master switch is off. The
		// shortcuts stay lit when the master switch is off, because key_enabled must keep
		// working to turn squint back on.
		bool master_off = i != SQ_ENABLED && c->v[SQ_ENABLED] == 0 && i != SQ_SHORTCUTS && g != SQ_SHORTCUTS;
		bool inactive = (g >= 0 && c->v[g] == 0) || master_off;
		char label[40];
		snprintf(label, sizeof label, "%s%s", g >= 0 ? "  " : "", sq_params[i].key);
		if (inactive) {
			printf("\x1b[2m");
		}
		if (i == sel) {
			printf("\x1b[7m");
		}
		if (is_str) {
			printf(" %c %-19s %-30s", i == sel ? '>' : ' ', label, *v ? v : "(none)");
		} else {
			printf(" %c %-19s %-8s %6s to %-6s", i == sel ? '>' : ' ', label, v, lo, hi);
		}
		if (i == sel || inactive) {
			printf("\x1b[0m");
		}
		printf("\r\n");
	}
	printf("\r\n %s\r\n\r\n", sq_params[sel].help);
	printf(" up/down select   left/right adjust   PgUp/PgDn adjust x10   space toggle\r\n");
	printf(" Enter record a shortcut by pressing it   e type it   r reset selected   R reset all   q quit\r\n\r\n %s\r\n", status);
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

// Records the keys the user presses for text setting i. Returns true when the setting changed.
static bool record_chord(struct sq_cfg *c, int i, const char *path, char *msg, size_t msglen) {
	char prompt[160];
	snprintf(prompt, sizeof prompt, "Press the keys for %s now, for example Super+F1. Esc cancels. (20 s)", sq_params[i].key);
	draw(c, i, path, prompt);
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
static bool edit_string(struct sq_cfg *c, int i, const char *path, char *msg, size_t msglen) {
	char buf[SQ_STRLEN] = "";
	int n = 0;
	snprintf(buf, sizeof buf, "%s", c->s[i]);
	n = (int)strlen(buf);
	for (;;) {
		char prompt[120];
		snprintf(prompt, sizeof prompt, "%s = %s_   (Enter saves, Esc cancels, empty clears)", sq_params[i].key, buf);
		draw(c, i, path, prompt);
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
	enter_raw();
	int sel = 0;
	char msg[200] = "";
	const char *status = "";
	for (;;) {
		draw(&c, sel, path, status);
		unsigned char ch;
		if (read(STDIN_FILENO, &ch, 1) != 1) {
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
				changed = record_chord(&c, sel, path, msg, sizeof msg);
				status = msg;
			}
		} else if (ch == 'e') {
			if (sq_params[sel].type == SQ_STR) {
				msg[0] = '\0';
				changed = edit_string(&c, sel, path, msg, sizeof msg);
				status = msg;
			}
		} else if (ch == 'r') {
			if (sq_params[sel].type == SQ_STR) {
				c.s[sel][0] = '\0';
			} else {
				c.v[sel] = sq_params[sel].def;
			}
			changed = true;
		} else if (ch == 'R') {
			sq_defaults(&c);
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
