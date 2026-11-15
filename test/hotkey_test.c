// Live test of the shortcut key grabs in src/hotkey.h, on the running X server. It sends key
// presses with the XTest extension. The chord is Ctrl+Alt plus a key almost nothing binds (F24
// if the keyboard has it, else one of the XF86Launch keys), and the test grabs it before
// sending it, so the grab consumes the key. If the X server has no XTest extension or the
// keyboard has none of those keys, the test prints a note and passes.

#include <X11/extensions/XTest.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>

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

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
