// Reads a shortcut chord from the keys the user presses, for squintctl. A terminal cannot see
// most chords (Super never reaches it, and F1 arrives as an escape sequence), so this takes an
// active keyboard grab on the X server for a few seconds and reads the key press itself. An
// active grab also wins over any program that has a passive grab on the same chord, such as an
// i3 binding.

#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "config.h"

// Waits for one chord and writes it into chord, for example "Super+F1". Modifier keys pressed
// alone are ignored, and Escape alone cancels. Returns false with a reason in why on a cancel,
// a timeout, or a failure. ready_fd, when it is not -1, gets one byte as soon as the grab is held,
// which lets a test know when it can send keys.
static inline bool sq_capture_chord(char *chord, size_t n, char *why, size_t whylen, int timeout_ms, int ready_fd) {
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		snprintf(why, whylen, "there is no X display to read keys from");
		return false;
	}
	int r = XGrabKeyboard(d, DefaultRootWindow(d), False, GrabModeAsync, GrabModeAsync, CurrentTime);
	if (r != GrabSuccess) {
		snprintf(why, whylen, "cannot take the keyboard (another program holds it, code %d)", r);
		XCloseDisplay(d);
		return false;
	}
	if (ready_fd >= 0) {
		char b = 1;
		if (write(ready_fd, &b, 1) != 1) {
			/* the reader went away, nothing to do */
		}
	}
	bool ok = false;
	snprintf(why, whylen, "timed out waiting for a key");
	int left_ms = timeout_ms;
	const unsigned all_mods = ShiftMask | ControlMask | Mod1Mask | Mod3Mask | Mod4Mask | Mod5Mask;
	for (;;) {
		while (XPending(d)) {
			XEvent ev;
			XNextEvent(d, &ev);
			if (ev.type != KeyPress) {
				continue;
			}
			KeySym ks = XLookupKeysym(&ev.xkey, 0);
			if (ks == NoSymbol || IsModifierKey(ks)) {
				continue;
			}
			unsigned st = ev.xkey.state;
			if (ks == XK_Escape && !(st & all_mods)) {
				snprintf(why, whylen, "cancelled");
				goto done;
			}
			const char *name = XKeysymToString(ks);
			if (!name) {
				snprintf(why, whylen, "that key has no name");
				goto done;
			}
			snprintf(chord, n, "%s%s%s%s%s%s%s", st & ShiftMask ? "Shift+" : "", st & ControlMask ? "Ctrl+" : "",
			         st & Mod1Mask ? "Alt+" : "", st & Mod4Mask ? "Super+" : "", st & Mod3Mask ? "Mod3+" : "",
			         st & Mod5Mask ? "Mod5+" : "", name);
			ok = true;
			goto done;
		}
		if (left_ms <= 0) {
			goto done;
		}
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(ConnectionNumber(d), &fds);
		int slice = left_ms < 200 ? left_ms : 200;
		struct timeval tv = {slice / 1000, (slice % 1000) * 1000};
		select(ConnectionNumber(d) + 1, &fds, NULL, NULL, &tv);
		left_ms -= slice;
	}
done:
	XUngrabKeyboard(d, CurrentTime);
	XCloseDisplay(d);
	return ok;
}
