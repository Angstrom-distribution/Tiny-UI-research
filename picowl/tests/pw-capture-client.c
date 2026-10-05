/*
 * pw-capture-client - minimal wlr-screencopy client for the smoke test.
 *
 * Captures the first output into a wl_shm buffer and prints
 *   pw-capture-client: captured WxH format=F identical=yes|no pixel=XXXXXXXX
 * where F is the wl_shm format code and pixel the first pixel (16 bits wide
 * for the 16 bpp formats). Prints "pw-capture-client: no screencopy" and exits
 * 0 when the compositor does not advertise zwlr_screencopy_manager_v1.
 * --at X,Y also prints "pw-capture-client: at X,Y rgb=RRGGBB" (the pixel
 * converted to 8 bits per channel), --distinct X,Y,W,H prints
 * "pw-capture-client: distinct N" for the rectangle. --palette X,Y,W,H prints
 * "pw-capture-client: palette N" and one "pw-capture-client: colour RRGGBB COUNT"
 * line for each colour of the rectangle (up to 4096; more is "palette 4097"),
 * --dump X,Y,W,H one "pw-capture-client: row Y RRGGBB RRGGBB ..." line for each
 * row of it. --spread X,Y,W,H with
 * --bg RRGGBB and --fg RRGGBB prints "pw-capture-client: spread max=F ink=N":
 * for each pixel of the rectangle that is not the ground, the coverage of the
 * text colour is worked out per channel in linear light, and F is the largest
 * difference between the channels of one pixel (0 for grayscale text, whose
 * channels all have the same coverage; subpixel text has up to 1) over N
 * pixels. Exit status 1 on a
 * failed capture, a timeout or a missing output/shm.
 */
#include <math.h>
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

/* A pixel as 0xRRGGBB. RGB565 and the 32 bit formats are the ones picowl has. */
static uint32_t rgb_at(int x, int y, size_t bpp)
{
	const uint8_t *p = (const uint8_t *)pixels + (size_t)y * stride + (size_t)x * bpp;
	uint32_t v = 0;

	memcpy(&v, p, bpp);
	if (bpp == 2) {
		uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
		return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
	}
	return v & 0xffffff;
}

static double lin(uint32_t v)
{
	double c = v / 255.0;

	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

int main(int argc, char **argv)
{
	int at_x = -1, at_y = -1, dx = -1, dy = 0, dw = 0, dh = 0;
	int sx = -1, sy = 0, sw = 0, sh = 0;
	int px0 = -1, py0 = 0, pw0 = 0, ph0 = 0;
	int ux = -1, uy = 0, uw = 0, uh = 0;
	uint32_t bg = 0, fg = 0xffffff;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--at") && i + 1 < argc)
			sscanf(argv[++i], "%d,%d", &at_x, &at_y);
		else if (!strcmp(argv[i], "--distinct") && i + 1 < argc)
			sscanf(argv[++i], "%d,%d,%d,%d", &dx, &dy, &dw, &dh);
		else if (!strcmp(argv[i], "--palette") && i + 1 < argc)
			sscanf(argv[++i], "%d,%d,%d,%d", &px0, &py0, &pw0, &ph0);
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
			sscanf(argv[++i], "%d,%d,%d,%d", &ux, &uy, &uw, &uh);
		else if (!strcmp(argv[i], "--spread") && i + 1 < argc)
			sscanf(argv[++i], "%d,%d,%d,%d", &sx, &sy, &sw, &sh);
		else if (!strcmp(argv[i], "--bg") && i + 1 < argc)
			bg = (uint32_t)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--fg") && i + 1 < argc)
			fg = (uint32_t)strtoul(argv[++i], NULL, 16);
	}
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
	if (at_x >= 0 && (uint32_t)at_x < width && (uint32_t)at_y < height)
		printf("pw-capture-client: at %d,%d rgb=%06x\n", at_x, at_y, rgb_at(at_x, at_y, bpp));
	if (dx >= 0) {
		static uint32_t seen[64];
		int n = 0;
		for (int y = dy; y < dy + dh && (uint32_t)y < height; y++)
			for (int x = dx; x < dx + dw && (uint32_t)x < width; x++) {
				uint32_t v = rgb_at(x, y, bpp);
				int k = 0;
				while (k < n && seen[k] != v)
					k++;
				if (k == n && n < 64)
					seen[n++] = v;
			}
		printf("pw-capture-client: distinct %d\n", n);
	}
	if (px0 >= 0) {
		static uint32_t col[4096];
		static int cnt[4096];
		int n = 0;
		for (int y = py0; y < py0 + ph0 && (uint32_t)y < height; y++)
			for (int x = px0; x < px0 + pw0 && (uint32_t)x < width; x++) {
				uint32_t v = rgb_at(x, y, bpp);
				int k = 0;
				while (k < n && col[k] != v)
					k++;
				if (k == n && n < 4096) {
					col[n] = v;
					cnt[n++] = 0;
				} else if (k == n) {
					n = 4097;
					goto palette_done;
				}
				cnt[k]++;
			}
palette_done:
		printf("pw-capture-client: palette %d\n", n);
		for (int k = 0; k < n && k < 4096; k++)
			printf("pw-capture-client: colour %06x %d\n", col[k], cnt[k]);
	}
	if (ux >= 0)
		for (int y = uy; y < uy + uh && (uint32_t)y < height; y++) {
			printf("pw-capture-client: row %d", y);
			for (int x = ux; x < ux + uw && (uint32_t)x < width; x++)
				printf(" %06x", rgb_at(x, y, bpp));
			printf("\n");
		}
	if (sx >= 0) {
		double max = 0;
		int ink = 0;
		for (int y = sy; y < sy + sh && (uint32_t)y < height; y++)
			for (int x = sx; x < sx + sw && (uint32_t)x < width; x++) {
				uint32_t v = rgb_at(x, y, bpp);
				double t[3], lo = 2, hi = -1;
				bool text = false;
				for (int k = 0; k < 3; k++) {
					int sh8 = 16 - 8 * k;
					double b = lin((bg >> sh8) & 0xff), f = lin((fg >> sh8) & 0xff);
					t[k] = (lin((v >> sh8) & 0xff) - b) / (f - b);
					if (t[k] < lo)
						lo = t[k];
					if (t[k] > hi)
						hi = t[k];
					if (t[k] > 0.1 || t[k] < -0.1)
						text = true;
				}
				if (!text)
					continue;
				ink++;
				if (hi - lo > max)
					max = hi - lo;
			}
		printf("pw-capture-client: spread max=%.3f ink=%d\n", max, ink);
	}
	fflush(stdout);
	return 0;
}
