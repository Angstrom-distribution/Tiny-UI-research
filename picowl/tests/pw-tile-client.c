/* pw-tile-client - wl_shm + xdg-shell client for the tiled layout test.
 *   pw-tile-client landscape   output 1280x720, [layout] stack = tile-a, tile-b,
 *                              [app.tile-a] aspect = 1:2
 *   pw-tile-client portrait    the same output turned to 720x1280, no aspect
 *   pw-tile-client nopan       the portrait run with a bottom anchored layer
 *                              surface (namespace wvkbd, exclusive zone 100)
 *                              standing in for the keyboard and [layout] pan
 *                              empty: it shrinks the usable area, so the pair
 *                              is laid out again, and hiding it restores them
 * One connection, two toplevels (app_ids tile-a and tile-b), and the configure
 * sizes the compositor sends are checked after every step. */
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

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;

struct win {
	const char *name;
	struct wl_surface *surface;
	struct xdg_surface *xs;
	struct xdg_toplevel *tl;
	int cfg_w, cfg_h;	/* last toplevel configure */
	bool activated;		/* ... and whether it carried the activated state */
	int n_configures;
	bool configured;
};

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
	memset(map, w->name[strlen(w->name) - 1] == 'a' ? 0x40 : 0xc0, size);
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

/* A bottom anchored layer surface with an exclusive zone, in the namespace of
 * wvkbd. Like wvkbd it is hidden by attaching no buffer and shown again by
 * committing and attaching one after the configure that follows. */
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

static void expect_active(const char *step, struct win *w, bool active)
{
	if (w->activated != active)
		fail("%s: %s activated=%d, expected %d", step, w->name, w->activated, active);
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

/* [layout] pan empty: the keyboard shrinks the usable area for everyone. */
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

int main(int argc, char **argv)
{
	static const char *const modes[] = { "landscape", "portrait", "nopan" };
	bool known = false;

	for (unsigned i = 0; argc >= 2 && i < sizeof(modes) / sizeof(modes[0]); i++)
		known |= !strcmp(argv[1], modes[i]);
	if (!known || argc != 2) {
		fprintf(stderr, "usage: pw-tile-client landscape|portrait|nopan\n");
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

	if (!strcmp(argv[1], "landscape"))
		landscape();
	else if (!strcmp(argv[1], "portrait"))
		portrait();
	else
		nopan();
	printf("pw-tile-client: ok %s\n", argv[1]);
	return 0;
}
