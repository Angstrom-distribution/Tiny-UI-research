/*
 * pw-capture-client - minimal wlr-screencopy client for the smoke test.
 *
 * Captures the first output into a wl_shm buffer and prints
 *   pw-capture-client: captured WxH format=F identical=yes|no pixel=XXXXXXXX
 * where F is the wl_shm format code and pixel the first pixel (16 bits wide
 * for the 16 bpp formats). Prints "pw-capture-client: no screencopy" and exits
 * 0 when the compositor does not advertise zwlr_screencopy_manager_v1.
 * Exit status 1 on a failed capture, a timeout or a missing output/shm.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm *shm;
static struct wl_output *output;
static struct zwlr_screencopy_manager_v1 *mgr;

static uint32_t fmt, width, height, stride;
static bool got_buffer, ready, failed;
static void *pixels = MAP_FAILED;
static size_t map_size;
static struct wl_buffer *wlbuf;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		/* Version 1 has no buffer_done: the copy goes out on buffer. */
		mgr = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void on_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
	uint32_t w, uint32_t h, uint32_t s)
{
	(void)d;
	fmt = format; width = w; height = h; stride = s;
	map_size = (size_t)s * h;

	const char *dir = getenv("XDG_RUNTIME_DIR");
	char path[512];
	snprintf(path, sizeof(path), "%s/pw-capture-XXXXXX", dir ? dir : "/tmp");
	int fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, (off_t)map_size) < 0) {
		fprintf(stderr, "pw-capture-client: cannot create the shm file\n");
		failed = true;
		return;
	}
	unlink(path);
	pixels = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		fprintf(stderr, "pw-capture-client: mmap failed\n");
		close(fd);
		failed = true;
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)map_size);
	wlbuf = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)s, format);
	wl_shm_pool_destroy(pool);
	close(fd);
	got_buffer = true;
	zwlr_screencopy_frame_v1_copy(f, wlbuf);
}
static void on_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
	(void)d; (void)f; (void)flags;
}
static void on_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t hi,
	uint32_t lo, uint32_t ns)
{
	(void)d; (void)f; (void)hi; (void)lo; (void)ns;
	ready = true;
}
static void on_failed(void *d, struct zwlr_screencopy_frame_v1 *f)
{
	(void)d; (void)f;
	failed = true;
}
static void on_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x,
	uint32_t y, uint32_t w, uint32_t h)
{
	(void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void on_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
	uint32_t w, uint32_t h)
{
	(void)d; (void)f; (void)format; (void)w; (void)h;
}
static void on_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f)
{
	(void)d; (void)f;
}
static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	on_buffer, on_flags, on_ready, on_failed, on_damage, on_dmabuf, on_buffer_done
};

int main(void)
{
	alarm(5);
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "pw-capture-client: cannot connect\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!mgr) {
		printf("pw-capture-client: no screencopy\n");
		return 0;
	}
	if (!shm || !output) {
		fprintf(stderr, "pw-capture-client: missing wl_shm or wl_output\n");
		return 1;
	}

	struct zwlr_screencopy_frame_v1 *frame =
		zwlr_screencopy_manager_v1_capture_output(mgr, 0, output);
	zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);
	while (!ready && !failed)
		if (wl_display_dispatch(dpy) < 0) {
			fprintf(stderr, "pw-capture-client: connection lost\n");
			return 1;
		}
	if (failed || !got_buffer) {
		fprintf(stderr, "pw-capture-client: capture failed\n");
		return 1;
	}

	/* Compare rows over the visible width only: the stride may pad. */
	size_t bpp = (stride / width) >= 4 ? 4 : 2;
	bool same = true;
	const uint8_t *first = pixels;
	for (uint32_t y = 0; y < height && same; y++) {
		const uint8_t *row = (const uint8_t *)pixels + (size_t)y * stride;
		for (uint32_t x = 0; x < width; x++)
			if (memcmp(row + (size_t)x * bpp, first, bpp)) {
				same = false;
				break;
			}
	}
	uint32_t px = 0;
	memcpy(&px, first, bpp);
	printf("pw-capture-client: captured %ux%u format=%u identical=%s pixel=%08x\n",
		width, height, fmt, same ? "yes" : "no", px);
	fflush(stdout);
	return 0;
}
