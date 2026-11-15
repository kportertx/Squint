// Live test of the shortcut key grabs in src/hotkey.h, on the running X server. It sends key
// presses with the XTest extension. The chord is Ctrl+Alt plus a key almost nothing binds (F24
// if the keyboard has it, else one of the XF86Launch keys), and the test grabs it before
// sending it, so the grab consumes the key. If the X server has no XTest extension or the
// keyboard has none of those keys, the test prints a note and passes.

#include <X11/extensions/XTest.h>
#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>

#include "../src/capture.h"
#include "../src/hotkey.h"

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

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// Waits up to ms for key events and returns the actions that fired.
static int pump(struct hk *h, int ms, int *acts, int max) {
	struct pollfd p = {hk_fd(h), POLLIN, 0};
	double end = now() + ms / 1000.0;
	int n = 0;
	while (now() < end && n < max) {
		poll(&p, 1, 20);
		n += hk_poll(h, now(), acts + n, max - n);
		if (n > 0) {
			break;  // return as soon as something fired, so a following press lands inside the debounce window
		}
	}
	return n;
}

static void press_chord(Display *d, const KeyCode *mods, int nm, KeyCode key) {
	for (int i = 0; i < nm; i++) XTestFakeKeyEvent(d, mods[i], True, 0);
	XTestFakeKeyEvent(d, key, True, 0);
	XTestFakeKeyEvent(d, key, False, 0);
	for (int i = nm - 1; i >= 0; i--) XTestFakeKeyEvent(d, mods[i], False, 0);
	XFlush(d);
}

// The recording of a chord from real key presses, run in a child process so that the test can
// send the keys once the child holds the keyboard.
struct cap {
	int ok;
	char chord[SQ_STRLEN];
	char why[160];
};

struct seq {
	KeyCode mods[4];
	int nm;
	KeyCode key;  // 0 sends only the modifiers, pressed and released alone
};

static void send_seq(Display *d, const struct seq *q) {
	if (q->key) {
		press_chord(d, q->mods, q->nm, q->key);
	} else {
		for (int i = 0; i < q->nm; i++) {
			XTestFakeKeyEvent(d, q->mods[i], True, 0);
			XTestFakeKeyEvent(d, q->mods[i], False, 0);
		}
		XFlush(d);
	}
}

static struct cap capture_with(Display *d, const struct seq *first, const struct seq *second, int timeout_ms) {
	int ready[2], result[2];
	if (pipe(ready) || pipe(result)) {
		struct cap r = {0, "", "pipe failed"};
		return r;
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(ready[0]);
		close(result[0]);
		struct cap r = {0, "", ""};
		r.ok = sq_capture_chord(r.chord, sizeof r.chord, r.why, sizeof r.why, timeout_ms, ready[1]);
		if (write(result[1], &r, sizeof r) != (ssize_t)sizeof r) {
			_exit(2);
		}
		_exit(0);
	}
	close(ready[1]);
	close(result[1]);
	struct cap r = {0, "", "the capture never took the keyboard"};
	struct pollfd p = {ready[0], POLLIN, 0};
	char b;
	if (poll(&p, 1, 3000) > 0 && read(ready[0], &b, 1) == 1) {
		if (first) send_seq(d, first);
		if (second) send_seq(d, second);
		if (read(result[0], &r, sizeof r) != (ssize_t)sizeof r) {
			snprintf(r.why, sizeof r.why, "no result from the capture");
			r.ok = 0;
		}
	} else {
		kill(pid, 9);
	}
	waitpid(pid, NULL, 0);
	close(ready[0]);
	close(result[0]);
	return r;
}

static bool grabs(Display *d, const char *chord, char *why, size_t n) {
	struct hk x;
	if (!hk_open(&x)) {
		snprintf(why, n, "cannot open the connection");
		return false;
	}
	bool ok = hk_grab(&x, d, chord, 1, why, n);
	hk_close(&x);
	return ok;
}

int main(void) {
	Display *d = XOpenDisplay(NULL);
	int ev, er, maj, min;
	if (!d || !XTestQueryExtension(d, &ev, &er, &maj, &min)) {
		printf("hotkey test skipped: no X display or no XTest extension\n");
		return 0;
	}
	const char *candidates[] = {"F24", "F23", "XF86Launch8", "XF86Launch7", "XF86Launch6", "XF86Launch5"};
	const char *kname = NULL;
	KeyCode f24 = 0;
	for (int i = 0; i < 6 && !f24; i++) {
		f24 = XKeysymToKeycode(d, XStringToKeysym(candidates[i]));
		kname = candidates[i];
	}
	KeyCode ctrl = XKeysymToKeycode(d, XStringToKeysym("Control_L"));
	KeyCode alt = XKeysymToKeycode(d, XStringToKeysym("Alt_L"));
	if (!f24 || !ctrl || !alt) {
		printf("hotkey test skipped: this keyboard has none of the spare keys, or no Control_L or Alt_L\n");
		return 0;
	}
	printf("shortcut key grabs\n");
	struct hk h;
	CHECK(hk_open(&h), "open the XCB connection");
	char why[120], chord[80], chord3[80], bad[80];
	snprintf(chord, sizeof chord, "Ctrl+Alt+%s", kname);
	snprintf(chord3, sizeof chord3, "Shift+Ctrl+Alt+%s", kname);
	snprintf(bad, sizeof bad, "Ctrl+NoSuchKeyName");
	printf("  using %s\n", chord);
	CHECK(hk_grab(&h, d, chord, 7, why, sizeof why), "grab %s: %s", chord, why);
	KeyCode mods[2] = {ctrl, alt};
	int acts[8];

	press_chord(d, mods, 2, f24);
	int n = pump(&h, 300, acts, 8);
	CHECK(n == 1 && acts[0] == 7, "the chord fires its action once (%d fired)", n);

	press_chord(d, mods, 2, f24);
	CHECK(pump(&h, 150, acts, 8) == 0, "a second press within 0.3 s is ignored (a held key repeats)");
	struct timespec wait = {0, 350 * 1000000L};
	nanosleep(&wait, NULL);
	press_chord(d, mods, 2, f24);
	n = pump(&h, 300, acts, 8);
	CHECK(n == 1 && acts[0] == 7, "after the pause it fires again (%d fired)", n);

	nanosleep(&wait, NULL);
	press_chord(d, mods, 1, f24);  // Ctrl and the key, without Alt
	CHECK(pump(&h, 200, acts, 8) == 0, "the wrong modifiers do not fire");

	struct hk other;
	CHECK(hk_open(&other), "open a second connection");
	CHECK(!hk_grab(&other, d, chord, 3, why, sizeof why) && strstr(why, "another program"),
	      "a chord that is already grabbed is refused and says why: '%s'", why);
	CHECK(other.n == 0, "a refused chord leaves nothing registered");
	hk_close(&other);

	CHECK(!hk_grab(&h, d, bad, 1, why, sizeof why) && strstr(why, "no key named"), "an unknown key name is refused: '%s'", why);
	CHECK(!hk_grab(&h, d, "Bogus+s", 1, why, sizeof why), "an unknown modifier is refused");
	CHECK(!hk_grab(&h, d, "", 1, why, sizeof why), "an empty chord is refused");
	CHECK(h.n == 1, "the refused chords were not registered (n = %d)", h.n);

	// Two chords at once, then release them all.
	CHECK(hk_grab(&h, d, chord3, 9, why, sizeof why), "a second chord on the same key with more modifiers: %s", why);
	nanosleep(&wait, NULL);
	KeyCode mods3[3] = {XKeysymToKeycode(d, XStringToKeysym("Shift_L")), ctrl, alt};
	press_chord(d, mods3, 3, f24);
	n = pump(&h, 300, acts, 8);
	CHECK(n == 1 && acts[0] == 9, "the chord with the extra modifier fires only its own action (%d fired, first %d)", n, n ? acts[0] : -1);

	hk_ungrab_all(&h);
	nanosleep(&wait, NULL);
	press_chord(d, mods, 2, f24);
	CHECK(pump(&h, 200, acts, 8) == 0, "after the keys are released nothing fires");
	CHECK(h.n == 0, "and none stay registered");
	// Released means another program can take the keys.
	struct hk later;
	CHECK(hk_open(&later) && hk_grab(&later, d, chord, 5, why, sizeof why) && hk_grab(&later, d, chord3, 6, why, sizeof why),
	      "after the release a new connection can grab both chords: %s", why);
	hk_close(&later);
	hk_close(&h);
	// Closing the connection releases the grabs too.
	struct hk again;
	CHECK(hk_open(&again) && hk_grab(&again, d, chord, 5, why, sizeof why), "the keys are free after a close: %s", why);
	hk_close(&again);

	// Key names match without regard to case.
	char lower[80], upper[80];
	snprintf(lower, sizeof lower, "ctrl+alt+%s", kname);
	snprintf(upper, sizeof upper, "CTRL+ALT+%s", kname);
	for (char *c = lower; *c; c++) *c = (char)tolower((unsigned char)*c);
	for (char *c = upper; *c; c++) *c = (char)toupper((unsigned char)*c);
	CHECK(grabs(d, lower, why, sizeof why), "a lowercase chord '%s' grabs: %s", lower, why);
	CHECK(grabs(d, upper, why, sizeof why), "an uppercase chord '%s' grabs: %s", upper, why);
	CHECK(grabs(d, "Super+f1", why, sizeof why) || !XKeysymToKeycode(d, XStringToKeysym("F1")), "Super+f1 grabs: %s", why);

	// Recording a chord from key presses, the way the tuning screen does it.
	printf("recording a chord from key presses\n");
	KeyCode super = XKeysymToKeycode(d, XStringToKeysym("Super_L"));
	KeyCode shift = XKeysymToKeycode(d, XStringToKeysym("Shift_L"));
	KeyCode f1 = XKeysymToKeycode(d, XStringToKeysym("F1"));
	KeyCode f5 = XKeysymToKeycode(d, XStringToKeysym("F5"));
	KeyCode esc = XKeysymToKeycode(d, XStringToKeysym("Escape"));
	KeyCode ks = XKeysymToKeycode(d, XStringToKeysym("s"));
	struct cap r;
	if (super && f1) {
		struct seq q = {{super}, 1, f1};
		r = capture_with(d, &q, NULL, 4000);
		CHECK(r.ok && !strcmp(r.chord, "Super+F1"), "pressing Super+F1 records Super+F1 (got '%s', %s)", r.chord, r.why);
	}
	struct seq q2 = {{ctrl, alt}, 2, f24};
	r = capture_with(d, &q2, NULL, 4000);
	char want[80];
	snprintf(want, sizeof want, "Ctrl+Alt+%s", XKeysymToString(XkbKeycodeToKeysym(d, f24, 0, 0)));
	CHECK(r.ok && !strcmp(r.chord, want), "pressing Ctrl+Alt+%s records it (got '%s', %s)", kname, r.chord, r.why);
	if (shift && ks) {
		struct seq q3 = {{shift, ctrl}, 2, ks};
		r = capture_with(d, &q3, NULL, 4000);
		CHECK(r.ok && !strcmp(r.chord, "Shift+Ctrl+s"), "Shift+Ctrl+s records the modifiers in a fixed order (got '%s')", r.chord);
	}
	if (super && shift && f5) {
		struct seq alone = {{super, shift, ctrl}, 3, 0};
		struct seq key = {{0}, 0, f5};
		r = capture_with(d, &alone, &key, 4000);
		CHECK(r.ok && !strcmp(r.chord, "F5"), "modifiers pressed alone are ignored (got '%s')", r.chord);
	}
	if (esc) {
		struct seq q4 = {{0}, 0, esc};
		r = capture_with(d, &q4, NULL, 4000);
		CHECK(!r.ok && strstr(r.why, "cancelled"), "Escape alone cancels (%s)", r.why);
		struct seq q5 = {{ctrl}, 1, esc};
		r = capture_with(d, &q5, NULL, 4000);
		CHECK(r.ok && !strcmp(r.chord, "Ctrl+Escape"), "Ctrl+Escape records as a chord (got '%s')", r.chord);
	}
	r = capture_with(d, NULL, NULL, 500);
	CHECK(!r.ok && strstr(r.why, "timed out"), "no key before the timeout fails: %s", r.why);
	{
		// What the recorder writes must be accepted by the parser and by the grab.
		char c2[SQ_STRLEN];
		snprintf(c2, sizeof c2, "Super+F1");
		unsigned mods;
		char key[SQ_STRLEN];
		CHECK(sq_parse_chord(c2, &mods, key, sizeof key) && mods == SQ_MOD_SUPER && !strcmp(key, "F1"), "a recorded chord parses");
	}

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
