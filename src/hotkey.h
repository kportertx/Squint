// Global shortcut keys for the plugin: passive grabs on the X root window.
//
// The grabs go through the plugin's own XCB connection, never picom's. A grab another
// program already holds (an i3 binding, for example) comes back as an error reply that this
// code reads and reports, instead of reaching Xlib's error handler, which would exit picom.
// Key names are turned into keycodes with Xlib calls on a Display the caller passes in.
//
// Num Lock and Caps Lock never change what a chord matches: each chord is grabbed four times,
// with and without those two modifiers, and they are masked out when an event is compared.
// A held key repeats as a stream of presses, so a chord triggers at most once per 0.3 s.

#pragma once

#include <X11/Xlib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "config.h"
#include "keys.h"

#define HK_MAX 8
#define HK_DEBOUNCE 0.3

struct hk_bind {
	xcb_keycode_t code;
	uint16_t mods;  // without Num Lock and Caps Lock
	int action;
	double last;
};

struct hk {
	xcb_connection_t *c;
	xcb_window_t root;
	struct hk_bind b[HK_MAX];
	int n;
};

static const uint16_t hk_lock_variants[4] = {0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2,
                                             XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2};

static inline bool hk_open(struct hk *h) {
	memset(h, 0, sizeof *h);
	h->c = xcb_connect(NULL, NULL);
	if (!h->c || xcb_connection_has_error(h->c)) {
		if (h->c) {
			xcb_disconnect(h->c);
		}
		h->c = NULL;
		return false;
	}
	h->root = xcb_setup_roots_iterator(xcb_get_setup(h->c)).data->root;
	return true;
}

static inline int hk_fd(const struct hk *h) {
	return xcb_get_file_descriptor(h->c);
}

static inline void hk_ungrab_all(struct hk *h) {
	for (int i = 0; i < h->n; i++) {
		for (int v = 0; v < 4; v++) {
			xcb_ungrab_key(h->c, h->b[i].code, h->root, (uint16_t)(h->b[i].mods | hk_lock_variants[v]));
		}
	}
	h->n = 0;
	xcb_flush(h->c);
}

static inline void hk_close(struct hk *h) {
	if (h->c) {
		hk_ungrab_all(h);
		xcb_disconnect(h->c);
		h->c = NULL;
	}
}

// Grabs one chord and ties it to action. Returns true on success. On failure it grabs nothing
// and writes a reason into why. dpy is used only to turn the key name into a keycode.
static inline bool hk_grab(struct hk *h, Display *dpy, const char *chord, int action, char *why, size_t whylen) {
	unsigned mods;
	char key[SQ_STRLEN];
	if (!sq_parse_chord(chord, &mods, key, sizeof key) || !*key) {
		snprintf(why, whylen, "'%s' is not a chord", chord);
		return false;
	}
	if (h->n >= HK_MAX) {
		snprintf(why, whylen, "too many shortcut keys");
		return false;
	}
	KeySym ks = sq_keysym(dpy, key);
	xcb_keycode_t code = ks == NoSymbol ? 0 : (xcb_keycode_t)XKeysymToKeycode(dpy, ks);
	if (!code) {
		snprintf(why, whylen, "no key named '%s' on this keyboard", key);
		return false;
	}
	uint16_t m = (uint16_t)(mods & 0xff & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2));
	xcb_void_cookie_t ck[4];
	for (int v = 0; v < 4; v++) {
		ck[v] = xcb_grab_key_checked(h->c, 1, h->root, (uint16_t)(m | hk_lock_variants[v]), code,
		                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	}
	bool ok = true;
	for (int v = 0; v < 4; v++) {
		xcb_generic_error_t *e = xcb_request_check(h->c, ck[v]);
		if (e) {
			ok = false;
			free(e);
		}
	}
	if (!ok) {
		for (int v = 0; v < 4; v++) {
			xcb_ungrab_key(h->c, code, h->root, (uint16_t)(m | hk_lock_variants[v]));
		}
		xcb_flush(h->c);
		snprintf(why, whylen, "another program already uses '%s'", chord);
		return false;
	}
	h->b[h->n++] = (struct hk_bind){code, m, action, -1e9};
	return true;
}

// Drains the connection and writes the actions of the chords that were pressed into out.
// now is the time in seconds. Returns how many actions were written.
static inline int hk_poll(struct hk *h, double now, int *out, int max) {
	int n = 0;
	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_event(h->c))) {
		if ((ev->response_type & 0x7f) == XCB_KEY_PRESS) {
			xcb_key_press_event_t *k = (xcb_key_press_event_t *)ev;
			uint16_t st = (uint16_t)(k->state & 0xff & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2));
			for (int i = 0; i < h->n; i++) {
				if (h->b[i].code == k->detail && h->b[i].mods == st && now - h->b[i].last > HK_DEBOUNCE) {
					h->b[i].last = now;
					if (n < max) {
						out[n++] = h->b[i].action;
					}
				}
			}
		}
		free(ev);
	}
	return n;
}
