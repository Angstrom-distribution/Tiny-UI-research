/* pw-tile-client - wl_shm + xdg-shell client for the tiled layout test.
 *   pw-tile-client landscape   output 1280x720, [layout] stack = tile-a, tile-b,
 *                              [app.tile-a] aspect = 1:2
 *   pw-tile-client portrait    the same output turned to 720x1280, no aspect
 *   pw-tile-client pan         portrait, with a bottom anchored layer surface
 *                              (namespace wvkbd, 100 rows high, no exclusive
 *                              zone, like the shipped wvkbd-ipaq) standing in
 *                              for the keyboard: the tiled pair is panned, not
 *                              resized, other windows are not touched
 *   pw-tile-client pan-zone    the same keyboard with an exclusive zone of 100:
 *                              the pair is panned, other windows shrink
 *   pw-tile-client nopan       the same with [layout] pan empty: all shrink
 *   pw-tile-client pan-landscape  the keyboard on the 1280x720 output: the side
 *                              by side pair shrinks
 *   pw-tile-client focus-keep  [layout] focus = tile-a: tile-a maps first, then
 *                              tile-b (and again after a restart); the
 *                              wl_keyboard focus stays with tile-a, alt+Tab
 *                              moves it, a third app takes it
 *   pw-tile-client focus-default  the same without the key: tile-b takes it
 *   pw-tile-client pixels DIR  portrait pair drawn red (tile-a) and blue
 *                              (tile-b), keyboard green; stops at each step for
 *                              tests/pan-e2e.sh, which takes a screenshot and
 *                              answers by creating a file in DIR
 *   pw-tile-client pixels-zone DIR  the same with the keyboard's exclusive zone
 * One connection, toplevels (app_ids tile-a and tile-b, and other for a window
 * outside the stack), and the configure sizes the compositor sends are checked
 * after every step. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include <xkbcommon/xkbcommon.h>

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
static struct wl_keyboard *keyboard;
static struct wl_surface *kfocus; /* surface which holds the wl_keyboard focus */
static bool paint_colours; /* pixels mode: solid colours instead of grey */

struct win {
	const char *name;
	struct wl_surface *surface;
	struct xdg_surface *xs;
	struct xdg_toplevel *tl;
	int cfg_w, cfg_h;	/* last toplevel configure */
	bool activated;		/* ... and whether it carried the activated state */
	int n_configures;
	bool configured;
	int n_kenter;		/* wl_keyboard.enter events for this window */
};
#define MAX_WINS 8
static struct win *wins[MAX_WINS]; /* open windows, to name the keyboard focus */

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fail(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "pw-tile-client: FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 4);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
		vk_mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* Attach a buffer of the configured size so that the surface maps and the
 * compositor sees a commit for every configure. */
static void commit_buffer(struct win *w)
{
	int bw = w->cfg_w > 0 ? w->cfg_w : 64, bh = w->cfg_h > 0 ? w->cfg_h : 64;
	size_t size = (size_t)bw * bh * 4;
	int fd = memfd_create("picowl-tile", MFD_CLOEXEC);

	if (fd < 0 || ftruncate(fd, size) < 0)
		fail("memfd");
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("mmap");
	if (paint_colours) {
		/* XRGB8888: red for tile-a, blue for tile-b, grey for the rest */
		uint32_t px = !strcmp(w->name, "tile-a") ? 0xffff0000 :
			!strcmp(w->name, "tile-b") ? 0xff0000ff : 0xff808080;
		for (size_t i = 0; i < (size_t)bw * bh; i++)
			((uint32_t *)map)[i] = px;
	} else {
		memset(map, w->name[strlen(w->name) - 1] == 'a' ? 0x40 : 0xc0, size);
	}
	munmap(map, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, bw, bh, bw * 4,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(w->surface, buf, 0, 0);
	wl_surface_damage_buffer(w->surface, 0, 0, bw, bh);
	wl_surface_commit(w->surface);
	wl_buffer_destroy(buf);
}

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	struct win *w = d;
	xdg_surface_ack_configure(xs, serial);
	w->configured = true;
	w->n_configures++;
	commit_buffer(w);
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t width, int32_t height,
	struct wl_array *states)
{
	struct win *w = d;
	uint32_t *s;
	(void)t;
	w->cfg_w = width;
	w->cfg_h = height;
	w->activated = false;
	wl_array_for_each(s, states)
		if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
			w->activated = true;
}
static void tl_close(void *d, struct xdg_toplevel *t)
{
	(void)d; (void)t;
}
static const struct xdg_toplevel_listener tl_listener = { tl_configure, tl_close, NULL, NULL };

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Dispatch whatever arrives within ms milliseconds; the callers loop on their
 * own condition. */
static void pump(int ms)
{
	struct pollfd pfd = { .fd = wl_display_get_fd(dpy), .events = POLLIN };

	while (wl_display_prepare_read(dpy) != 0)
		wl_display_dispatch_pending(dpy);
	if (wl_display_flush(dpy) < 0 && errno != EAGAIN)
		fail("flush");
	if (poll(&pfd, 1, ms) > 0)
		wl_display_read_events(dpy);
	else
		wl_display_cancel_read(dpy);
	wl_display_dispatch_pending(dpy);
}

static void open_win(struct win *w, const char *app_id)
{
	memset(w, 0, sizeof(*w));
	for (int i = 0; i < MAX_WINS; i++)
		if (!wins[i]) {
			wins[i] = w;
			break;
		}
	w->name = app_id;
	w->surface = wl_compositor_create_surface(compositor);
	w->xs = xdg_wm_base_get_xdg_surface(wm_base, w->surface);
	xdg_surface_add_listener(w->xs, &xs_listener, w);
	w->tl = xdg_surface_get_toplevel(w->xs);
	xdg_toplevel_add_listener(w->tl, &tl_listener, w);
	xdg_toplevel_set_title(w->tl, app_id);
	xdg_toplevel_set_app_id(w->tl, app_id);
	wl_surface_commit(w->surface);
}

static void close_win(struct win *w)
{
	for (int i = 0; i < MAX_WINS; i++)
		if (wins[i] == w)
			wins[i] = NULL;
	if (kfocus == w->surface)
		kfocus = NULL;
	xdg_toplevel_destroy(w->tl);
	xdg_surface_destroy(w->xs);
	wl_surface_destroy(w->surface);
	w->tl = NULL;
}

static void set_fixed(struct win *w, int fw, int fh)
{
	xdg_toplevel_set_min_size(w->tl, fw, fh);
	xdg_toplevel_set_max_size(w->tl, fw, fh);
	wl_surface_commit(w->surface);
}

/* Wait (up to 3 s) until w has been configured to width x height, then wait a
 * little more and check that nothing stale or extra changed it. */
static void expect_size(const char *step, struct win *w, int width, int height)
{
	int64_t end = now_ms() + 3000;

	while ((w->cfg_w != width || w->cfg_h != height) && now_ms() < end)
		pump(50);
	if (w->cfg_w != width || w->cfg_h != height)
		fail("%s: %s configured %dx%d, expected %dx%d", step, w->name,
			w->cfg_w, w->cfg_h, width, height);
	end = now_ms() + 150;
	while (now_ms() < end)
		pump(50);
	if (w->cfg_w != width || w->cfg_h != height)
		fail("%s: %s changed to %dx%d after %dx%d", step, w->name,
			w->cfg_w, w->cfg_h, width, height);
	printf("pw-tile-client: %s: %s %dx%d\n", step, w->name, width, height);
	fflush(stdout);
}

/* ---- the keyboard stand-in -------------------------------------------- */

/* A bottom anchored layer surface, in the namespace of wvkbd, with or without
 * an exclusive zone: the shipped wvkbd-ipaq asks for none, so that is the main
 * case, and the zone is the other. Like wvkbd it is hidden by attaching no buffer and shown again by
 * committing and attaching one after the configure that follows. */
static bool kb_zoned;	/* ask for an exclusive zone of the keyboard's height */
static struct {
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *ls;
	int zone;
	int w, h;		/* last configure */
	int n_configures;
	bool wanted;		/* attach a buffer at the next configure */
} kb;

static void kbd_attach(void)
{
	int bw = kb.w > 0 ? kb.w : 64, bh = kb.h > 0 ? kb.h : kb.zone;
	size_t size = (size_t)bw * bh * 4;
	int fd = memfd_create("picowl-kbd", MFD_CLOEXEC);

	if (fd < 0 || ftruncate(fd, size) < 0)
		fail("memfd");
	uint32_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("mmap");
	for (size_t i = 0; i < (size_t)bw * bh; i++)
		map[i] = 0xff00ff00;
	munmap(map, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, bw, bh, bw * 4,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(kb.surface, buf, 0, 0);
	wl_surface_damage_buffer(kb.surface, 0, 0, bw, bh);
	wl_surface_commit(kb.surface);
	wl_buffer_destroy(buf);
}

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
	uint32_t width, uint32_t height)
{
	(void)d;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	kb.w = width;
	kb.h = height;
	kb.n_configures++;
	if (kb.wanted)
		kbd_attach();
}
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls)
{
	(void)d; (void)ls;
	fail("keyboard layer surface was closed");
}
static const struct zwlr_layer_surface_v1_listener ls_listener = { ls_configure, ls_closed };

static void kbd_show(int zone)
{
	int before = kb.n_configures;
	int64_t end = now_ms() + 3000;

	if (!layer_shell)
		fail("no zwlr_layer_shell_v1");
	kb.wanted = true;
	if (!kb.ls) {
		kb.zone = zone;
		kb.surface = wl_compositor_create_surface(compositor);
		kb.ls = zwlr_layer_shell_v1_get_layer_surface(layer_shell, kb.surface, NULL,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wvkbd");
		zwlr_layer_surface_v1_add_listener(kb.ls, &ls_listener, NULL);
		zwlr_layer_surface_v1_set_size(kb.ls, 0, zone);
		zwlr_layer_surface_v1_set_anchor(kb.ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		if (kb_zoned)
			zwlr_layer_surface_v1_set_exclusive_zone(kb.ls, zone);
	}
	wl_surface_commit(kb.surface);
	while (kb.n_configures == before && now_ms() < end)
		pump(50);
	if (kb.n_configures == before)
		fail("the keyboard surface was never configured");
	wl_display_roundtrip(dpy);
}

static void kbd_hide(void)
{
	kb.wanted = false;
	wl_surface_attach(kb.surface, NULL, 0, 0);
	wl_surface_commit(kb.surface);
	wl_display_roundtrip(dpy);
}

/* Let whatever the compositor wants to say arrive. */
static void settle(int ms)
{
	int64_t end = now_ms() + ms;

	while (now_ms() < end)
		pump(50);
}

/* w was configured width x height at its configure number n, and nothing came
 * since: a pan must neither resize a window nor send it any configure. */
static void expect_untouched(const char *step, struct win *w, int width, int height, int n)
{
	settle(300);
	if (w->n_configures != n)
		fail("%s: %s got %d new configure(s), now %dx%d", step, w->name,
			w->n_configures - n, w->cfg_w, w->cfg_h);
	if (w->cfg_w != width || w->cfg_h != height)
		fail("%s: %s is %dx%d, expected %dx%d", step, w->name, w->cfg_w, w->cfg_h,
			width, height);
	printf("pw-tile-client: %s: %s untouched at %dx%d\n", step, w->name, width, height);
	fflush(stdout);
}

static void expect_active(const char *step, struct win *w, bool active)
{
	if (w->activated != active)
		fail("%s: %s activated=%d, expected %d", step, w->name, w->activated, active);
}

/* ---- the keyboard focus ----------------------------------------------- */

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz)
{
	(void)d; (void)k; (void)f; (void)sz;
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
	struct wl_array *keys)
{
	(void)d; (void)k; (void)serial; (void)keys;
	kfocus = s;
	for (int i = 0; i < MAX_WINS; i++)
		if (wins[i] && wins[i]->surface == s)
			wins[i]->n_kenter++;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)k; (void)serial;
	if (kfocus == s)
		kfocus = NULL;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t a, uint32_t b, uint32_t c, uint32_t e)
{
	(void)d; (void)k; (void)a; (void)b; (void)c; (void)e;
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t a, uint32_t b, uint32_t c,
	uint32_t e, uint32_t f)
{
	(void)d; (void)k; (void)a; (void)b; (void)c; (void)e; (void)f;
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)d; (void)k; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener kb_listener = { kb_keymap, kb_enter, kb_leave,
	kb_key, kb_mods, kb_repeat };

static void seat_caps(void *d, struct wl_seat *st, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(st);
		wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *st, const char *n)
{
	(void)d; (void)st; (void)n;
}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static struct zwp_virtual_keyboard_v1 *vkbd;

/* The headless seat has no keyboard, so without one there is no wl_keyboard
 * to ask for: a virtual keyboard of this client gives the seat the capability
 * and sends the alt+Tab. */
static void keyboard_setup(void)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *map = ctx ? xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
	char *str = map ? xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;

	if (!seat || !vk_mgr || !str)
		fail("no wl_seat, virtual keyboard manager or keymap");
	size_t size = strlen(str) + 1;
	int fd = memfd_create("pw-tile-keymap", 0);
	if (fd < 0 || ftruncate(fd, size) < 0)
		fail("memfd");
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		fail("mmap");
	memcpy(p, str, size);
	munmap(p, size);
	wl_seat_add_listener(seat, &seat_listener, NULL);
	vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_mgr, seat);
	zwp_virtual_keyboard_v1_keymap(vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!keyboard)
		fail("the seat never offered a keyboard");
}

static void vk_key(uint32_t code, uint32_t state)
{
	zwp_virtual_keyboard_v1_key(vkbd, now_ms(), code, state);
	wl_display_roundtrip(dpy);
}

static void alt_tab(void)
{
	/* a virtual keyboard's modifier state is what the client sets (Mod1 is
	 * alt in the default keymap), key events do not change it */
	zwp_virtual_keyboard_v1_modifiers(vkbd, 8, 0, 0, 0);
	vk_key(15, WL_KEYBOARD_KEY_STATE_PRESSED);
	vk_key(15, WL_KEYBOARD_KEY_STATE_RELEASED);
	zwp_virtual_keyboard_v1_modifiers(vkbd, 0, 0, 0, 0);
	wl_display_roundtrip(dpy);
}

/* w (NULL: nobody) holds the keyboard focus, and the activated state agrees. */
static void expect_kfocus(const char *step, struct win *w)
{
	settle(300);
	if ((w ? w->surface : NULL) != kfocus)
		fail("%s: the keyboard focus is not on %s", step, w ? w->name : "nobody");
	for (int i = 0; i < MAX_WINS; i++)
		if (wins[i] && wins[i]->activated != (wins[i] == w))
			fail("%s: %s activated=%d", step, wins[i]->name, wins[i]->activated);
	printf("pw-tile-client: %s: keyboard focus on %s\n", step, w ? w->name : "nobody");
	fflush(stdout);
}

/* keep: [layout] focus = tile-a, so tile-b (the second app) never takes the
 * keyboard when it maps; without the key it does, as every window does. */
static void focus(bool keep)
{
	struct win a, b, o;

	keyboard_setup();
	open_win(&a, "tile-a");
	expect_kfocus("a alone", &a);
	open_win(&b, "tile-b");
	expect_size("pair", &a, 720, 640);
	expect_kfocus("b mapped", keep ? &a : &b);
	if (keep && b.n_kenter != 0)
		fail("b mapped: tile-b got the keyboard for a moment");

	/* the partner restarts, as the player does */
	close_win(&b);
	expect_kfocus("b closed", &a);
	open_win(&b, "tile-b");
	expect_kfocus("b restarted", keep ? &a : &b);
	if (keep && b.n_kenter != 0)
		fail("b restarted: tile-b got the keyboard for a moment");

	/* alt+Tab still moves it between the pair, both ways */
	alt_tab();
	expect_kfocus("alt+Tab", keep ? &b : &a);
	alt_tab();
	expect_kfocus("alt+Tab back", keep ? &a : &b);

	/* a window outside the stack takes it as ever */
	open_win(&o, "other");
	expect_kfocus("other mapped", &o);

	close_win(&o);
	close_win(&b);
	close_win(&a);
	wl_display_roundtrip(dpy);
}

static void landscape(void)
{
	struct win a, b;

	open_win(&a, "tile-a");
	/* alone: maximized, whatever its aspect rule says */
	expect_size("alone", &a, 1280, 720);

	open_win(&b, "tile-b");
	/* no hint: [app.tile-a] aspect = 1:2 gives 720 / 2 = 360 columns */
	expect_size("config aspect", &a, 360, 720);
	expect_size("config aspect", &b, 920, 720);
	expect_active("focus", &b, true);
	expect_active("focus", &a, false);

	/* the client's own fixed size beats the config */
	set_fixed(&a, 300, 300);
	expect_size("fixed 1:1", &a, 720, 720);
	expect_size("fixed 1:1", &b, 560, 720);

	/* a new clip: the hint changes at run time */
	set_fixed(&a, 200, 300);
	expect_size("fixed 2:3", &a, 480, 720);
	expect_size("fixed 2:3", &b, 800, 720);

	/* too wide to fit: the other app keeps a quarter */
	set_fixed(&a, 400, 200);
	expect_size("clamped", &a, 960, 720);
	expect_size("clamped", &b, 320, 720);

	/* hint dropped: back to the config */
	set_fixed(&a, 0, 0);
	expect_size("hint dropped", &a, 360, 720);
	expect_size("hint dropped", &b, 920, 720);

	/* the partner goes away: maximized again, no stale slot */
	close_win(&b);
	expect_size("partner closed", &a, 1280, 720);
	close_win(&a);
	wl_display_roundtrip(dpy);
}

static void portrait(void)
{
	struct win a, b;

	open_win(&b, "tile-b");
	expect_size("alone", &b, 720, 1280);

	/* the second app first: it is the lower one, the first appears above it */
	open_win(&a, "tile-a");
	expect_size("even split", &a, 720, 640);
	expect_size("even split", &b, 720, 640);
	expect_active("focus", &a, true);
	expect_active("focus", &b, false);

	set_fixed(&a, 300, 200);
	expect_size("fixed 3:2", &a, 720, 480);
	expect_size("fixed 3:2", &b, 720, 800);

	/* the first app goes away: the second gets everything */
	close_win(&a);
	expect_size("partner closed", &b, 720, 1280);
	close_win(&b);
	wl_display_roundtrip(dpy);
}

/* The keyboard takes 100 rows at the bottom of the 720x1280 portrait output. */
#define KBD_ZONE 100

/* The rows the keyboard takes off the area of a window which is not panned. */
static int kbd_shrink(void)
{
	return kb_zoned ? KBD_ZONE : 0;
}

static void pan(void)
{
	struct win a, b, o;

	open_win(&b, "tile-b");
	expect_size("alone", &b, 720, 1280);
	open_win(&a, "tile-a");
	expect_size("pair", &a, 720, 640);
	expect_size("pair", &b, 720, 640);
	int na = a.n_configures, nb = b.n_configures;

	/* the stack is moved, neither window hears about it */
	kbd_show(KBD_ZONE);
	expect_untouched("keyboard shown", &a, 720, 640, na);
	expect_untouched("keyboard shown", &b, 720, 640, nb);
	kbd_hide();
	expect_untouched("keyboard hidden", &a, 720, 640, na);
	expect_untouched("keyboard hidden", &b, 720, 640, nb);

	/* the pair breaks up while the keyboard is up: the pan ends and the
	 * survivor is maximized into the area the keyboard leaves */
	kbd_show(KBD_ZONE);
	expect_untouched("shown again", &a, 720, 640, na);
	close_win(&a);
	expect_size("partner closed", &b, 720, 1280 - kbd_shrink());
	open_win(&a, "tile-a");
	expect_size("partner back", &a, 720, 640);
	expect_size("partner back", &b, 720, 640);
	kbd_hide();
	na = a.n_configures;
	nb = b.n_configures;
	expect_untouched("hidden again", &a, 720, 640, na);
	expect_untouched("hidden again", &b, 720, 640, nb);

	/* a window outside the stack is shrunk as always, the pair stays put */
	open_win(&o, "other");
	expect_size("other alone", &o, 720, 1280);
	settle(300);
	na = a.n_configures;
	nb = b.n_configures;
	kbd_show(KBD_ZONE);
	expect_size("other shrunk", &o, 720, 1280 - kbd_shrink());
	expect_untouched("other shown", &a, 720, 640, na);
	expect_untouched("other shown", &b, 720, 640, nb);
	kbd_hide();
	expect_size("other restored", &o, 720, 1280);
	expect_untouched("other hidden", &a, 720, 640, na);
	expect_untouched("other hidden", &b, 720, 640, nb);

	close_win(&o);
	close_win(&a);
	close_win(&b);
	wl_display_roundtrip(dpy);
}

/* [layout] pan empty: the keyboard, with a zone, shrinks the usable area for
 * everyone. */
static void nopan(void)
{
	struct win a, b;

	open_win(&b, "tile-b");
	open_win(&a, "tile-a");
	expect_size("pair", &a, 720, 640);
	expect_size("pair", &b, 720, 640);
	kbd_show(KBD_ZONE);
	expect_size("shrunk", &a, 720, 590);
	expect_size("shrunk", &b, 720, 590);
	kbd_hide();
	expect_size("restored", &a, 720, 640);
	expect_size("restored", &b, 720, 640);
	close_win(&a);
	close_win(&b);
	wl_display_roundtrip(dpy);
}

/* A side by side pair has no lower window to lift, so it shrinks. */
static void pan_landscape(void)
{
	struct win a, b;

	open_win(&b, "tile-b");
	open_win(&a, "tile-a");
	expect_size("pair", &a, 640, 720);
	expect_size("pair", &b, 640, 720);
	kbd_show(KBD_ZONE);
	expect_size("shrunk", &a, 640, 720 - KBD_ZONE);
	expect_size("shrunk", &b, 640, 720 - KBD_ZONE);
	kbd_hide();
	expect_size("restored", &a, 640, 720);
	expect_size("restored", &b, 640, 720);
	close_win(&a);
	close_win(&b);
	wl_display_roundtrip(dpy);
}

/* Stop until the script has looked at the screen: name.ready appears, the
 * script answers with name.go. */
static void sync_point(const char *dir, const char *name)
{
	char path[512];
	int64_t end = now_ms() + 20000;

	settle(400);
	snprintf(path, sizeof(path), "%s/%s.ready", dir, name);
	FILE *f = fopen(path, "w");
	if (!f)
		fail("cannot create %s", path);
	fclose(f);
	snprintf(path, sizeof(path), "%s/%s.go", dir, name);
	while (access(path, F_OK) != 0) {
		if (now_ms() > end)
			fail("no %s.go from the script", name);
		pump(50);
	}
}

static void pixels(const char *dir)
{
	struct win a, b;

	paint_colours = true;
	open_win(&b, "tile-b");
	open_win(&a, "tile-a");
	expect_size("pair", &a, 720, 640);
	expect_size("pair", &b, 720, 640);
	sync_point(dir, "before");
	kbd_show(KBD_ZONE);
	sync_point(dir, "shown");
	kbd_hide();
	sync_point(dir, "hidden");
	close_win(&a);
	close_win(&b);
	wl_display_roundtrip(dpy);
}

int main(int argc, char **argv)
{
	static const char *const modes[] = { "landscape", "portrait", "pan", "pan-zone",
		"nopan", "pan-landscape", "pixels", "pixels-zone", "focus-keep",
		"focus-default" };
	bool known = false;

	for (unsigned i = 0; argc >= 2 && i < sizeof(modes) / sizeof(modes[0]); i++)
		known |= !strcmp(argv[1], modes[i]);
	if (!known || argc != (!strncmp(argv[1], "pixels", 6) ? 3 : 2)) {
		fprintf(stderr, "usage: pw-tile-client landscape|portrait|pan|pan-zone|"
			"nopan|pan-landscape|focus-keep|focus-default|pixels DIR|pixels-zone DIR\n");
		return 2;
	}
	alarm(30);
	dpy = wl_display_connect(NULL);
	if (!dpy)
		fail("cannot connect");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm || !wm_base)
		fail("missing globals");

	kb_zoned = strcmp(argv[1], "pan") && strcmp(argv[1], "pixels");
	if (!strcmp(argv[1], "landscape"))
		landscape();
	else if (!strcmp(argv[1], "portrait"))
		portrait();
	else if (!strcmp(argv[1], "pan") || !strcmp(argv[1], "pan-zone"))
		pan();
	else if (!strcmp(argv[1], "focus-keep") || !strcmp(argv[1], "focus-default"))
		focus(!strcmp(argv[1], "focus-keep"));
	else if (!strcmp(argv[1], "nopan"))
		nopan();
	else if (!strcmp(argv[1], "pan-landscape"))
		pan_landscape();
	else
		pixels(argv[2]);
	printf("pw-tile-client: ok %s\n", argv[1]);
	return 0;
}
