/*
 * pw-key-client - presses keys on picowl's seat through a virtual keyboard,
 * for tests that need a keybinding to fire in a headless run.
 *
 * usage: pw-key-client CODE...
 *
 * Each CODE is an evdev key code (397 is KEY_CALENDAR). Every key is pressed
 * and released, and the connection is synchronised after each one so that
 * picowl has handled it when this returns. Exits 0, or 1 with a message.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *mgr;

static void fail(const char *what)
{
	fprintf(stderr, "pw-key-client: FAIL: %s\n", what);
	exit(1);
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
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
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy)
		fail("cannot connect");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!seat || !mgr)
		fail("no wl_seat or virtual keyboard manager");

	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *map = ctx ? xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
	char *str = map ? xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
	if (!str)
		fail("cannot compile a keymap");
	size_t size = strlen(str) + 1;
	int fd = memfd_create("pw-key-client", 0);
	if (fd < 0 || ftruncate(fd, size) < 0)
		fail("memfd");
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		fail("mmap");
	memcpy(p, str, size);
	munmap(p, size);

	struct zwp_virtual_keyboard_v1 *vk =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(mgr, seat);
	zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	wl_display_roundtrip(dpy);

	for (int i = 1; i < argc; i++) {
		uint32_t code = (uint32_t)atoi(argv[i]);
		zwp_virtual_keyboard_v1_key(vk, ms(), code, WL_KEYBOARD_KEY_STATE_PRESSED);
		zwp_virtual_keyboard_v1_key(vk, ms(), code, WL_KEYBOARD_KEY_STATE_RELEASED);
		wl_display_roundtrip(dpy);
	}
	zwp_virtual_keyboard_v1_destroy(vk);
	wl_display_roundtrip(dpy);
	wl_display_disconnect(dpy);
	return 0;
}
