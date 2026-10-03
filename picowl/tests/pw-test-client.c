/* pw-test-client - minimal wl_shm + xdg-shell client used by the smoke test.
 *   --zerocopy            commit dmabuf frames from picowl-buffer-v1
 *   --zerocopy-count N    request N buffers, print how many were granted
 *   --app-id ID           xdg_toplevel app_id (default picowl-test-client)
 *   --bind-version N      bind picowl_buffer_manager_v1 at min(advertised, N)
 *                         (default and maximum 2; 1 behaves as a v1 client)
 *   --probe               print what the picowl-buffer global announced and exit
 *   --readback            with --zerocopy: time 32-bit reads of buffer 0 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "picowl-buffer-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

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
static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct picowl_buffer_manager_v1 *pbm;
static bool pb_fmt_565;
static int pb_copy_type = -1;
static int pb_caching = -1;     /* -1: not received (v1 compositor or client) */
static uint32_t pb_version, pb_bind_max = 2;
static struct zwp_idle_inhibit_manager_v1 *inhibit_mgr;
static const char *no_global;      /* --expect-no-global NAME */
static bool saw_no_global;         /* ... and the registry advertised it */

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

static void pbm_format(void *d, struct picowl_buffer_manager_v1 *m, uint32_t f)
{
	(void)d; (void)m;
	if (f == 0x36314752) /* DRM_FORMAT_RGB565 */
		pb_fmt_565 = true;
}
static void pbm_copy_type(void *d, struct picowl_buffer_manager_v1 *m, uint32_t t)
{
	(void)d; (void)m;
	pb_copy_type = (int)t;
}
static void pbm_caching(void *d, struct picowl_buffer_manager_v1 *m, uint32_t c)
{
	(void)d; (void)m;
	pb_caching = (int)c;
}
/* The caching member must be set: libwayland aborts on a NULL one when the
 * manager is bound at version 2. */
static const struct picowl_buffer_manager_v1_listener pbm_listener = {
	pbm_format, pbm_copy_type, pbm_caching
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d;
	if (no_global && !strcmp(iface, no_global))
		saw_no_global = true;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) && ver >= 3)
		dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
	else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name))
		inhibit_mgr = wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, 1);
	else if (!strcmp(iface, picowl_buffer_manager_v1_interface.name)) {
		pb_version = ver < pb_bind_max ? ver : pb_bind_max;
		pbm = wl_registry_bind(r, name, &picowl_buffer_manager_v1_interface,
			pb_version);
		picowl_buffer_manager_v1_add_listener(pbm, &pbm_listener, NULL);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
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

/* ---- zero-copy mode ---- */
#define ZC_FRAMES 5
#define ZC_DRM_RGB565 0x36314752u

struct zbuf {
	struct picowl_buffer_v1 *pb;
	struct wl_buffer *wl;
	void *map;
	size_t size;
	uint32_t stride, offset, mod_hi, mod_lo;
	int fd;
	bool have_dmabuf, failed, busy;
};

static uint32_t zc_serial_copied;
static uint32_t zc_commits;
static uint32_t zc_serial_retained;
static bool zc_got_copied, zc_got_retained;

static void zb_dmabuf(void *d, struct picowl_buffer_v1 *pb, int32_t fd, uint32_t stride,
	uint32_t offset, uint32_t hi, uint32_t lo)
{
	(void)pb;
	struct zbuf *z = d;
	z->fd = fd;
	z->stride = stride;
	z->offset = offset;
	z->mod_hi = hi;
	z->mod_lo = lo;
	z->have_dmabuf = true;
}
static void zb_done(void *d, struct picowl_buffer_v1 *pb) { (void)d; (void)pb; }
static void zb_failed(void *d, struct picowl_buffer_v1 *pb, uint32_t r)
{
	(void)pb; (void)r;
	((struct zbuf *)d)->failed = true;
}
static void zb_copied(void *d, struct picowl_buffer_v1 *pb, uint32_t serial)
{
	(void)d; (void)pb;
	zc_serial_copied = serial;
	zc_got_copied = true;
}
static void zb_retained(void *d, struct picowl_buffer_v1 *pb, uint32_t serial)
{
	(void)d; (void)pb;
	zc_serial_retained = serial;
	zc_got_retained = true;
}
static const struct picowl_buffer_v1_listener zb_listener = {
	zb_dmabuf, zb_done, zb_failed, zb_copied, zb_retained
};

static void zb_release(void *d, struct wl_buffer *b)
{
	(void)b;
	((struct zbuf *)d)->busy = false;
}
static const struct wl_buffer_listener zb_wl_listener = { zb_release };

static void zc_alarm(int sig)
{
	(void)sig;
	static const char m[] = "picowl-test-client: zerocopy timeout\n";
	if (write(2, m, sizeof(m) - 1)) {}
	_exit(1);
}

static void dmabuf_sync(int fd, uint64_t flags)
{
	struct dma_buf_sync s = { .flags = flags };
	int r;
	do
		r = ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
	while (r < 0 && errno == EINTR);
	/* ENOTTY and friends: not a real dmabuf (or no sync support), ignore. */
}

static void zc_draw(struct zbuf *z, int w, int h, int frame)
{
	dmabuf_sync(z->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
	for (int y = 0; y < h; y++) {
		uint16_t *row = (uint16_t *)((char *)z->map + z->offset + (size_t)y * z->stride);
		for (int x = 0; x < w; x++) {
			unsigned r = (unsigned)((x * 31) / (w > 1 ? w - 1 : 1));
			unsigned g = (unsigned)((y * 63) / (h > 1 ? h - 1 : 1));
			unsigned b = (unsigned)((frame * 6) & 31);
			row[x] = (uint16_t)((r << 11) | (g << 5) | b);
		}
	}
	dmabuf_sync(z->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

static void zc_destroy(struct zbuf *z)
{
	if (z->wl)
		wl_buffer_destroy(z->wl);
	if (z->map && z->map != MAP_FAILED)
		munmap(z->map, z->size);
	if (z->fd >= 0)
		close(z->fd);
	if (z->pb)
		picowl_buffer_v1_destroy(z->pb);
	memset(z, 0, sizeof(*z));
	z->fd = -1;
}

/* Allocate and wrap one buffer. Returns false if unavailable. */
static bool zc_create(struct wl_display *dpy, struct zbuf *z, int w, int h)
{
	memset(z, 0, sizeof(*z));
	z->fd = -1;
	z->pb = picowl_buffer_manager_v1_create_buffer(pbm, w, h, ZC_DRM_RGB565);
	picowl_buffer_v1_add_listener(z->pb, &zb_listener, z);
	for (int i = 0; i < 2 && !z->have_dmabuf && !z->failed; i++)
		wl_display_roundtrip(dpy);
	if (!z->have_dmabuf || z->failed)
		return false;
	z->size = (size_t)z->stride * h;
	z->map = mmap(NULL, z->offset + z->size, PROT_READ | PROT_WRITE, MAP_SHARED, z->fd, 0);
	if (z->map == MAP_FAILED)
		return false;
	z->size += z->offset;
	struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dmabuf);
	zwp_linux_buffer_params_v1_add(p, z->fd, 0, z->offset, z->stride, z->mod_hi, z->mod_lo);
	z->wl = zwp_linux_buffer_params_v1_create_immed(p, w, h, ZC_DRM_RGB565, 0);
	zwp_linux_buffer_params_v1_destroy(p);
	wl_buffer_add_listener(z->wl, &zb_wl_listener, z);
	return true;
}

static void zc_commit(struct zbuf *z, int w, int h)
{
	wl_surface_attach(surface, z->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, w, h);
	wl_surface_commit(surface);
	zc_commits++;
}

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* One pass of 32-bit loads over the whole buffer, in microseconds. On
 * write-combined or uncached memory it is several times slower. */
static int64_t zc_readback_us(struct zbuf *z)
{
	const volatile uint32_t *p = (const volatile uint32_t *)z->map;
	size_t n = z->size / 4;
	uint32_t sum = 0;
	dmabuf_sync(z->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
	int64_t t0 = now_us();
	for (size_t i = 0; i < n; i++)
		sum += p[i];
	int64_t dt = now_us() - t0;
	dmabuf_sync(z->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
	(void)sum;
	return dt;
}

/* Returns 0 on success, -1 when zero-copy is unavailable (nothing committed). */
static int run_zerocopy(struct wl_display *dpy, int w, int h, bool readback)
{
	struct zbuf zb[2];
	int nbuf;

	if (!dmabuf || !pbm || !pb_fmt_565 || pb_copy_type < 0)
		return -1;
	nbuf = pb_copy_type == 1 ? 1 : 2;
	memset(zb, 0, sizeof(zb));
	zb[0].fd = zb[1].fd = -1;
	for (int i = 0; i < nbuf; i++) {
		if (!zc_create(dpy, &zb[i], w, h)) {
			for (int j = 0; j <= i; j++)
				zc_destroy(&zb[j]);
			return -1;
		}
	}

	signal(SIGALRM, zc_alarm);
	alarm(3);
	int frames = 0;
	if (pb_copy_type == 1) {
		/* One buffer until the compositor answers a commit with
		 * "retained" (composited, not direct scanout): then a second
		 * buffer is allocated and wl_buffer.release is used. */
		int cur = 0;
		picowl_buffer_v1_attach_surface(zb[0].pb, surface);
		zc_draw(&zb[0], w, h, 0);
		zb[0].busy = true;
		zc_commit(&zb[0], w, h);
		while (frames < ZC_FRAMES) {
			zc_got_copied = zc_got_retained = false;
			while (!(zc_got_copied && zc_serial_copied == zc_commits) &&
			       !(zc_got_retained && zc_serial_retained == zc_commits)) {
				if (wl_display_dispatch(dpy) < 0)
					return 1;
				if (zc_got_copied && zc_serial_copied != zc_commits)
					zc_got_copied = false;
				if (zc_got_retained && zc_serial_retained != zc_commits)
					zc_got_retained = false;
			}
			frames++;
			if (frames >= ZC_FRAMES)
				break;
			if (!(zc_got_copied && zc_serial_copied == zc_commits)) {
				int other = cur ^ 1;
				if (!zb[other].wl && !zc_create(dpy, &zb[other], w, h))
					return 1;
				while (zb[other].busy)
					if (wl_display_dispatch(dpy) < 0)
						return 1;
				cur = other;
			}
			zc_draw(&zb[cur], w, h, frames);
			zb[cur].busy = true;
			zc_commit(&zb[cur], w, h);
		}
	} else {
		for (int f = 0; f < ZC_FRAMES; f++) {
			int i = f & 1;
			while (zb[i].busy)
				if (wl_display_dispatch(dpy) < 0)
					return 1;
			zc_draw(&zb[i], w, h, f);
			zb[i].busy = true;
			zc_commit(&zb[i], w, h);
			wl_display_flush(dpy);
			frames++;
		}
		wl_display_roundtrip(dpy);
	}
	alarm(0);
	printf("picowl-test-client: zerocopy %d frames copy_type=%d caching=%d\n",
		frames, pb_copy_type, pb_caching);
	if (readback)
		printf("picowl-test-client: readback_us=%lld bytes=%zu caching=%d\n",
			(long long)zc_readback_us(&zb[0]), zb[0].size, pb_caching);
	fflush(stdout);
	return 0;
}

/* Request n buffers, stop at the first failure and report how many the
 * compositor granted (its per-client and pool limits). -1 = unavailable. */
static int run_zerocopy_count(struct wl_display *dpy, int w, int h, int n)
{
	if (!dmabuf || !pbm || !pb_fmt_565)
		return -1;
	struct zbuf *zb = calloc(n, sizeof(*zb));
	if (!zb)
		return -1;
	int granted = 0;
	for (int i = 0; i < n; i++) {
		if (!zc_create(dpy, &zb[i], w, h)) {
			zc_destroy(&zb[i]);
			break;
		}
		granted++;
	}
	printf("picowl-test-client: zerocopy-count requested %d granted %d\n", n, granted);
	fflush(stdout);
	for (int i = 0; i < granted; i++)
		zc_destroy(&zb[i]);
	free(zb);
	wl_display_roundtrip(dpy);
	return 0;
}

int main(int argc, char **argv)
{
	bool want_zc = getenv("PW_TEST_ZEROCOPY") && !strcmp(getenv("PW_TEST_ZEROCOPY"), "1");
	int zc_count = 0;
	bool probe = false, readback = false;
	bool want_inhibit = false;
	int linger = 0;
	const char *app_id = "picowl-test-client";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--zerocopy"))
			want_zc = true;
		else if (!strcmp(argv[i], "--zerocopy-count") && i + 1 < argc)
			zc_count = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bind-version") && i + 1 < argc) {
			int v = atoi(argv[++i]);
			pb_bind_max = v >= 1 && v <= 2 ? (uint32_t)v : 2;
		} else if (!strcmp(argv[i], "--probe"))
			probe = true;
		else if (!strcmp(argv[i], "--readback"))
			readback = true;
		else if (!strcmp(argv[i], "--app-id") && i + 1 < argc)
			app_id = argv[++i];
		else if (!strcmp(argv[i], "--inhibit"))
			want_inhibit = true;
		else if (!strcmp(argv[i], "--linger") && i + 1 < argc)
			linger = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--expect-no-global") && i + 1 < argc)
			no_global = argv[++i];
	}

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
	if (no_global) {
		if (saw_no_global) {
			fprintf(stderr, "picowl-test-client: unexpected global %s\n", no_global);
			return 1;
		}
		printf("picowl-test-client: no global %s\n", no_global);
		return 0;
	}
	if (probe) {
		if (pbm)
			printf("picowl-test-client: probe bufmgr version=%u format=%d "
				"copy_type=%d caching=%d\n", pb_version, pb_fmt_565,
				pb_copy_type, pb_caching);
		else
			printf("picowl-test-client: probe bufmgr none\n");
		fflush(stdout);
		return 0;
	}
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
	xdg_toplevel_set_app_id(tl, app_id);
	if (want_inhibit) {
		if (!inhibit_mgr) {
			fprintf(stderr, "picowl-test-client: no idle inhibit manager\n");
			return 1;
		}
		zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_mgr, surface);
	}
	wl_surface_commit(surface);

	while (!configured)
		if (wl_display_dispatch(dpy) < 0)
			return timeout_exit();

	int w = cfg_w > 0 ? cfg_w : 64, h = cfg_h > 0 ? cfg_h : 64;
	if (zc_count > 0) {
		if (run_zerocopy_count(dpy, w, h, zc_count) == 0)
			return 0;
		printf("picowl-test-client: zerocopy unavailable, using wl_shm\n");
		fflush(stdout);
	} else if (want_zc) {
		int rc = run_zerocopy(dpy, w, h, readback);
		if (rc > 0)
			return 1;
		if (rc == 0)
			return 0;
		printf("picowl-test-client: zerocopy unavailable, using wl_shm\n");
		fflush(stdout);
	}
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
	/* --linger S: stay connected (answering pings) for S s after the first frame. */
	alarm(linger + 5);
	for (int i = 0; i < linger * 10; i++) {
		usleep(100000);
		if (wl_display_roundtrip(dpy) < 0)
			return timeout_exit();
	}
	return 0;
}
