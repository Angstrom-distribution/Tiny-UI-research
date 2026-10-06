/*
 * pw-pointer-client - moves the pointer and presses its button on picowl's seat
 * through a virtual pointer, for tests that must hit a place of the output as a
 * user's stylus does. picowl offers the protocol only with
 * PICOWL_TEST_VIRTUAL_POINTER=1.
 *
 * usage: pw-pointer-client --size WxH ACTION...
 *
 * --size is the size of the output as clients see it (the logical size, 320x240
 * on an h2200 turned by 90 degrees); positions are pixels of it.
 * ACTION is one of
 *   move X,Y   put the pointer on the pixel X,Y (its centre)
 *   down       press the left button
 *   up         release it
 *   tap X,Y    move, press and release
 *   wait MS    let that much time pass
 * The connection is synchronised after each action, so picowl has handled it
 * when the next one starts. Exits 0, or 1 with a message.
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *mgr;

static void fail(const char *what)
{
	fprintf(stderr, "pw-pointer-client: FAIL: %s\n", what);
	exit(1);
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static uint32_t ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
	int w = 0, h = 0, i = 1;

	if (i + 1 < argc && !strcmp(argv[i], "--size")) {
		char x;
		if (sscanf(argv[i + 1], "%dx%d%c", &w, &h, &x) != 2 || w < 1 || h < 1)
			fail("--size wants WxH");
		i += 2;
	}
	if (!w)
		fail("usage: pw-pointer-client --size WxH ACTION...");

	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy)
		fail("cannot connect");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!seat || !mgr)
		fail("no zwlr_virtual_pointer_manager_v1 (PICOWL_TEST_VIRTUAL_POINTER=1 in picowl's environment)");
	struct zwlr_virtual_pointer_v1 *vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
	wl_display_roundtrip(dpy);

	for (; i < argc; i++) {
		int x, y;
		if (!strcmp(argv[i], "move") && i + 1 < argc && sscanf(argv[i + 1], "%d,%d", &x, &y) == 2) {
			i++;
			/* The pixel's centre: the extent is doubled so that no rounding
			 * of the position can land on the pixel next to it. */
			zwlr_virtual_pointer_v1_motion_absolute(vp, ms(), 2 * x + 1, 2 * y + 1,
				2 * w, 2 * h);
			zwlr_virtual_pointer_v1_frame(vp);
		} else if (!strcmp(argv[i], "tap") && i + 1 < argc && sscanf(argv[i + 1], "%d,%d", &x, &y) == 2) {
			i++;
			zwlr_virtual_pointer_v1_motion_absolute(vp, ms(), 2 * x + 1, 2 * y + 1,
				2 * w, 2 * h);
			zwlr_virtual_pointer_v1_frame(vp);
			wl_display_roundtrip(dpy);
			zwlr_virtual_pointer_v1_button(vp, ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
			zwlr_virtual_pointer_v1_frame(vp);
			wl_display_roundtrip(dpy);
			zwlr_virtual_pointer_v1_button(vp, ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
			zwlr_virtual_pointer_v1_frame(vp);
		} else if (!strcmp(argv[i], "down")) {
			zwlr_virtual_pointer_v1_button(vp, ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
			zwlr_virtual_pointer_v1_frame(vp);
		} else if (!strcmp(argv[i], "up")) {
			zwlr_virtual_pointer_v1_button(vp, ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
			zwlr_virtual_pointer_v1_frame(vp);
		} else if (!strcmp(argv[i], "wait") && i + 1 < argc && sscanf(argv[i + 1], "%d", &x) == 1) {
			i++;
			wl_display_roundtrip(dpy);
			usleep((useconds_t)x * 1000);
		} else {
			fail("bad action");
		}
		if (wl_display_roundtrip(dpy) < 0)
			fail("the connection broke");
	}
	zwlr_virtual_pointer_v1_destroy(vp);
	wl_display_roundtrip(dpy);
	return 0;
}
