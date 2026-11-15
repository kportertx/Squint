// squintctl: edit ~/.config/squint.conf. The plugin watches the file, so each change
// takes effect on screen immediately.
//
//   squintctl                 interactive tuning screen
//   squintctl show            print the current settings
//   squintctl set KEY VALUE   change one setting (value is clamped to its range)

#include <signal.h>
#include <string.h>
#include <termios.h>

#include "config.h"

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

static void draw(const struct sq_cfg *c, int sel, const char *path, const char *status) {
	printf("\x1b[2J\x1b[H");
	printf("squint tuning\r\nconfig: %s\r\n\r\n", path);
	for (int i = 0; i < SQ_N; i++) {
		char v[32], lo[32], hi[32];
		sq_format_value(i, c->v[i], v, sizeof v);
		sq_format_value(i, sq_params[i].min, lo, sizeof lo);
		sq_format_value(i, sq_params[i].max, hi, sizeof hi);
		if (i == sel) {
			printf("\x1b[7m");
		}
		printf(" %c %-13s %-8s %6s to %-6s", i == sel ? '>' : ' ', sq_params[i].key, v,
		       sq_params[i].type == SQ_BOOL ? "" : lo, sq_params[i].type == SQ_BOOL ? "" : hi);
		if (i == sel) {
			printf("\x1b[0m");
		}
		printf("\r\n");
	}
	printf("\r\n %s\r\n\r\n", sq_params[sel].help);
	printf(" up/down select   left/right adjust   PgUp/PgDn adjust x10   space toggle\r\n");
	printf(" r reset selected   R reset all   q quit\r\n\r\n %s\r\n", status);
	fflush(stdout);
}

static void adjust(struct sq_cfg *c, int i, int steps) {
	const struct sq_param *p = &sq_params[i];
	if (p->type == SQ_BOOL) {
		c->v[i] = c->v[i] != 0 ? 0 : 1;
		return;
	}
	// Round to the step grid so repeated presses do not accumulate float error.
	double x = (c->v[i] + steps * p->step) / p->step;
	c->v[i] = sq_clamp(i, floor(x + 0.5) * p->step);
}

static int interactive(const char *path) {
	if (!isatty(STDIN_FILENO)) {
		fprintf(stderr, "squintctl: stdin is not a terminal (use 'show' or 'set')\n");
		return 1;
	}
	struct sq_cfg c;
	sq_load(path, &c);
	enter_raw();
	int sel = 0;
	const char *status = "";
	char msg[160];
	for (;;) {
		draw(&c, sel, path, status);
		unsigned char ch;
		if (read(STDIN_FILENO, &ch, 1) != 1) {
			break;
		}
		bool changed = false;
		if (ch == 'q') {
			break;
		} else if (ch == 'k') {
			sel = (sel + SQ_N - 1) % SQ_N;
		} else if (ch == 'j') {
			sel = (sel + 1) % SQ_N;
		} else if (ch == 'h' || ch == 'l') {
			adjust(&c, sel, ch == 'l' ? 1 : -1);
			changed = true;
		} else if (ch == ' ') {
			if (sq_params[sel].type == SQ_BOOL) {
				adjust(&c, sel, 1);
				changed = true;
			}
		} else if (ch == 'r') {
			c.v[sel] = sq_params[sel].def;
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
					changed = true;
				} else if (seq[1] == '5' || seq[1] == '6') {
					read(STDIN_FILENO, &seq[2], 1);  // trailing '~'
					adjust(&c, sel, seq[1] == '6' ? -10 : 10);
					changed = true;
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
	if (argc == 2 && !strcmp(argv[1], "show")) {
		for (int i = 0; i < SQ_N; i++) {
			char v[32];
			sq_format_value(i, c.v[i], v, sizeof v);
			printf("%s = %s\n", sq_params[i].key, v);
		}
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "set")) {
		int i = sq_find(argv[2]);
		double x;
		if (i < 0) {
			fprintf(stderr, "squintctl: unknown key '%s'\n", argv[2]);
			return 1;
		}
		if (!sq_parse_value(i, argv[3], &x)) {
			fprintf(stderr, "squintctl: bad value '%s' for %s\n", argv[3], argv[2]);
			return 1;
		}
		c.v[i] = x;
		if (sq_save(path, &c) != 0) {
			fprintf(stderr, "squintctl: cannot write %s: %s\n", path, strerror(errno));
			return 1;
		}
		char v[32];
		sq_format_value(i, x, v, sizeof v);
		printf("%s = %s\n", sq_params[i].key, v);
		return 0;
	}
	fprintf(stderr, "usage: squintctl [show | set KEY VALUE]\n");
	return 2;
}
