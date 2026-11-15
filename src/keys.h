// Key name lookup shared by the plugin and the tool.

#pragma once

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <string.h>
#include <strings.h>

// Finds the keysym for a key name. X key names are case-sensitive ("F1", "Return", "XF86Launch8"),
// so when the exact name is unknown this looks through the keyboard's own keys for a name that
// matches without regard to case, which accepts "f1", "return", and "xf86launch8".
static inline KeySym sq_keysym(Display *d, const char *name) {
	KeySym ks = XStringToKeysym(name);
	if (ks != NoSymbol) {
		return ks;
	}
	int lo, hi;
	XDisplayKeycodes(d, &lo, &hi);
	for (int kc = lo; kc <= hi; kc++) {
		for (int level = 0; level < 4; level++) {
			KeySym s = XkbKeycodeToKeysym(d, (KeyCode)kc, 0, level);
			if (s == NoSymbol) {
				continue;
			}
			const char *n = XKeysymToString(s);
			if (n && !strcasecmp(n, name)) {
				return s;
			}
		}
	}
	return NoSymbol;
}
