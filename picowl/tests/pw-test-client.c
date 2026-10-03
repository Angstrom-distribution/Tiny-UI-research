/* pw-test-client - minimal wl_shm + xdg-shell client used by the smoke test. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define FMT_RGB565 WL_SHM_FORMAT_RGB565

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct wl_callback *frame_cb;
static bool have_565, configured, done;
static uint32_t format = WL_SHM_FORMAT_XRGB8888;
static int32_t cfg_w, cfg_h;

static void shm_format(void *d, struct wl_shm *s, uint32_t f)
{
	(void)d; (void)s;
	if (f == FMT_RGB565)
		have_565 = true;
}
static const struct wl_shm_listener shm_listener = { shm_format };

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
	else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
		wl_shm_add_listener(shm, &shm_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
	(void)d; (void)t;
	wl_callback_destroy(cb);
	frame_cb = NULL;
	done = true;
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	configured = true;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
	struct wl_array *states)
{
	(void)d; (void)t; (void)states;
	cfg_w = w;
	cfg_h = h;
}
static void tl_close(void *d, struct xdg_toplevel *t)
{
	(void)d; (void)t;
	exit(1);
}
static const struct xdg_toplevel_listener tl_listener = { tl_configure, tl_close, NULL, NULL };

static int timeout_exit(void)
{
	fprintf(stderr, "picowl-test-client: timeout\n");
	return 1;
}

int main(void)
{
	alarm(5);
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "picowl-test-client: cannot connect\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy); /* shm formats */
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "picowl-test-client: missing globals\n");
		return 1;
	}
	if (have_565)
		format = FMT_RGB565;

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_title(tl, "picowl-test");
	xdg_toplevel_set_app_id(tl, "picowl-test-client");
	wl_surface_commit(surface);

	while (!configured)
		if (wl_display_dispatch(dpy) < 0)
			return timeout_exit();

	int w = cfg_w > 0 ? cfg_w : 64, h = cfg_h > 0 ? cfg_h : 64;
	int bpp = format == FMT_RGB565 ? 2 : 4;
	int stride = w * bpp;
	size_t size = (size_t)stride * h;
	int fd = memfd_create("picowl-test", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("memfd");
		return 1;
	}
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	if (bpp == 2) {
		uint16_t *p = map;
		for (size_t i = 0; i < size / 2; i++)
			p[i] = 0x07e0; /* green */
	} else {
		uint32_t *p = map;
		for (size_t i = 0; i < size / 4; i++)
			p[i] = 0xff00ff00;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);

	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, w, h);
	frame_cb = wl_surface_frame(surface);
	wl_callback_add_listener(frame_cb, &frame_listener, NULL);
	wl_surface_commit(surface);

	while (!done)
		if (wl_display_dispatch(dpy) < 0)
			return timeout_exit();

	printf("picowl-test-client: mapped %dx%d format %u\n", w, h, format);
	fflush(stdout);
	return 0;
}
