/* picowl-commit-loop - paced wl_shm commit loop for the rotation measurement
 * (doc/rotation-measurement.md, workloads W3 and W4).
 *   --fps N            target commit rate, 1..1000 (default 30)
 *   --damage full      W3: redraw and damage the whole surface (default)
 *   --damage WxH       W4: blink a WxH square and damage only that rectangle
 *   --damage square    same as 16x16
 *   --duration SECONDS run time (default 60)
 * Prints one CSV row on stdout at exit; diagnostics go to stderr. */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"

#define NBUF 2
#define SQUARE_DEFAULT 16
/* Two buffers must stay below INT32_MAX, the limit of wl_shm.create_pool. */
#define MAX_DIM 8192
#define STARTUP_TIMEOUT_S 10
#define DRAIN_NS 1000000000LL

struct buf {
	struct wl_buffer *wl;
	void *data;
	bool busy;
};

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_presentation *presentation;
static struct wl_surface *surface;
static struct buf bufs[NBUF];
static bool have_565, configured, frame_ready;
static volatile sig_atomic_t quit, closed;
static int32_t cfg_w, cfg_h;
static bool resize_warned;

static uint32_t shm_format = WL_SHM_FORMAT_XRGB8888;
static int bpp = 4;
static int width, height;
static size_t stride;

static bool damage_full = true;
static int dmg_w, dmg_h, dmg_x, dmg_y;

/* Statistics. The delta array is sized once at startup from fps and
 * duration, so the commit loop never allocates. */
static int64_t *deltas_ns;
static size_t n_deltas, cap_deltas;
static bool deltas_full_warned;
static uint64_t n_committed, n_presented, n_discarded;
static unsigned outstanding;
static int64_t last_present_ns;
static bool have_last_present;

static uint32_t pal[256];

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void die(const char *msg)
{
	fprintf(stderr, "picowl-commit-loop: %s\n", msg);
	exit(1);
}

static void die_errno(const char *what)
{
	fprintf(stderr, "picowl-commit-loop: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void die_display(const char *what)
{
	int err = wl_display_get_error(dpy);
	if (err)
		fprintf(stderr, "picowl-commit-loop: %s: display error %d (%s)\n", what, err,
			err == EPROTO ? "protocol error" : strerror(err));
	else
		fprintf(stderr, "picowl-commit-loop: %s: connection lost\n", what);
	exit(1);
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

/* A compositor that never configures us would otherwise hang the run with
 * no output; SIGALRM is only armed until the first configure. */
static void on_alarm(int sig)
{
	(void)sig;
	static const char m[] = "picowl-commit-loop: timeout waiting for the first configure\n";
	if (write(2, m, sizeof(m) - 1)) {}
	_exit(1);
}

static void shm_format_cb(void *d, struct wl_shm *s, uint32_t f)
{
	(void)d; (void)s;
	if (f == WL_SHM_FORMAT_RGB565)
		have_565 = true;
}
static const struct wl_shm_listener shm_listener = { shm_format_cb };

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
		{
		/* damage_buffer, which exact damage reporting needs, arrived in version 4. */
		if (ver < 4)
			die("compositor is older than wl_compositor version 4");
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	}
	else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
		if (shm)
			wl_shm_add_listener(shm, &shm_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
		if (wm_base)
			xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	} else if (!strcmp(iface, wp_presentation_interface.name))
		presentation = wl_registry_bind(r, name, &wp_presentation_interface,
			ver < 2 ? ver : 2);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	if (!configured) {
		width = cfg_w;
		height = cfg_h;
		configured = true;
	} else if ((cfg_w != width || cfg_h != height) && cfg_w > 0 && cfg_h > 0 &&
		   !resize_warned) {
		/* Buffers are fixed at the first size so that every run commits the
		 * same pixel count; a changed size would invalidate the comparison. */
		fprintf(stderr, "picowl-commit-loop: ignoring resize to %dx%d\n", cfg_w, cfg_h);
		resize_warned = true;
	}
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
	closed = 1;
	quit = 1;
}
static const struct xdg_toplevel_listener tl_listener = { tl_configure, tl_close, NULL, NULL };

static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
	(void)d; (void)t;
	wl_callback_destroy(cb);
	frame_ready = true;
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void buf_release(void *d, struct wl_buffer *b)
{
	(void)b;
	((struct buf *)d)->busy = false;
}
static const struct wl_buffer_listener buf_listener = { buf_release };

static void fb_sync_output(void *d, struct wp_presentation_feedback *fb, struct wl_output *o)
{
	(void)d; (void)fb; (void)o;
}

static void fb_presented(void *d, struct wp_presentation_feedback *fb,
	uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec, uint32_t refresh,
	uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
	(void)d; (void)refresh; (void)seq_hi; (void)seq_lo; (void)flags;
	int64_t t = (int64_t)(((uint64_t)sec_hi << 32) | sec_lo) * 1000000000LL + nsec;
	if (have_last_present) {
		if (n_deltas < cap_deltas)
			deltas_ns[n_deltas++] = t - last_present_ns;
		else if (!deltas_full_warned) {
			fprintf(stderr, "picowl-commit-loop: delta buffer full, further frames not timed\n");
			deltas_full_warned = true;
		}
	}
	last_present_ns = t;
	have_last_present = true;
	n_presented++;
	outstanding--;
	wl_proxy_destroy((struct wl_proxy *)fb);
}

static void fb_discarded(void *d, struct wp_presentation_feedback *fb)
{
	(void)d;
	n_discarded++;
	outstanding--;
	wl_proxy_destroy((struct wl_proxy *)fb);
}
static const struct wp_presentation_feedback_listener fb_listener = {
	fb_sync_output, fb_presented, fb_discarded
};

static uint32_t pack_rgb(unsigned r, unsigned g, unsigned b)
{
	if (bpp == 2)
		return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void store_px(void *base, size_t row_off, int x, uint32_t v)
{
	char *row = (char *)base + row_off;
	if (bpp == 2)
		((uint16_t *)row)[x] = (uint16_t)v;
	else
		((uint32_t *)row)[x] = v;
}

static unsigned tri(unsigned i)
{
	i &= 255;
	return i < 128 ? i * 2 : (255 - i) * 2;
}

/* Every pixel changes on every frame, so a stale-copy or partial-damage bug
 * shows up as a visible tear instead of passing unnoticed. */
static void draw_pattern(void *base, unsigned frame)
{
	unsigned off = frame * 4;
	for (int y = 0; y < height; y++) {
		size_t ro = (size_t)y * stride;
		if (bpp == 2) {
			uint16_t *row = (uint16_t *)((char *)base + ro);
			for (int x = 0; x < width; x++)
				row[x] = (uint16_t)pal[((unsigned)(x + y) + off) & 255];
		} else {
			uint32_t *row = (uint32_t *)((char *)base + ro);
			for (int x = 0; x < width; x++)
				row[x] = pal[((unsigned)(x + y) + off) & 255];
		}
	}
}

static void fill_rect(void *base, int x0, int y0, int w, int h, uint32_t v)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			store_px(base, (size_t)y * stride, x, v);
}

static void init_palette(void)
{
	for (unsigned i = 0; i < 256; i++)
		pal[i] = pack_rgb(tri(i), tri(i + 85), tri(i + 170));
}

static struct buf *free_buf(void)
{
	for (int i = 0; i < NBUF; i++)
		if (!bufs[i].busy)
			return &bufs[i];
	return NULL;
}

static void request_frame(void)
{
	struct wl_callback *cb = wl_surface_frame(surface);
	if (!cb)
		die("wl_surface.frame failed");
	wl_callback_add_listener(cb, &frame_listener, NULL);
}

/* The first commit has to carry the whole surface because the compositor has
 * no earlier content to keep, even for the small-damage workload. It is not
 * counted and gets no feedback, so every counted commit damages exactly what
 * --damage says. */
static void prime(void)
{
	struct buf *b = &bufs[0];
	if (damage_full)
		draw_pattern(b->data, 0);
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	request_frame();
	wl_surface_commit(surface);
	b->busy = true;
}

static void commit_frame(struct buf *b)
{
	unsigned frame = (unsigned)n_committed;
	int x = 0, y = 0, w = width, h = height;

	if (damage_full) {
		draw_pattern(b->data, frame);
	} else {
		/* Only the square differs between the buffers, so repainting it
		 * alone keeps whichever buffer is free consistent. */
		x = dmg_x; y = dmg_y; w = dmg_w; h = dmg_h;
		fill_rect(b->data, x, y, w, h, (frame & 1) ? pack_rgb(0, 0, 0) : pack_rgb(255, 255, 255));
	}
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, x, y, w, h);
	request_frame();
	struct wp_presentation_feedback *fb = wp_presentation_feedback(presentation, surface);
	if (!fb)
		die("wp_presentation.feedback failed");
	wp_presentation_feedback_add_listener(fb, &fb_listener, NULL);
	outstanding++;
	wl_surface_commit(surface);
	b->busy = true;
	n_committed++;
}

/* Dispatch events, waiting at most timeout_ns (negative: until an event). */
static void pump(int64_t timeout_ns)
{
	while (wl_display_prepare_read(dpy) != 0)
		if (wl_display_dispatch_pending(dpy) < 0)
			die_display("dispatch");

	struct pollfd pfd = { .fd = wl_display_get_fd(dpy), .events = POLLIN };
	if (wl_display_flush(dpy) < 0) {
		if (errno != EAGAIN) {
			wl_display_cancel_read(dpy);
			die_display("flush");
		}
		pfd.events |= POLLOUT;
	}

	struct timespec ts, *tp = NULL;
	if (timeout_ns >= 0) {
		ts.tv_sec = (time_t)(timeout_ns / 1000000000LL);
		ts.tv_nsec = (long)(timeout_ns % 1000000000LL);
		tp = &ts;
	}
	int r = ppoll(&pfd, 1, tp, NULL);
	if (r < 0 && errno != EINTR) {
		wl_display_cancel_read(dpy);
		die_errno("poll");
	}
	if (r > 0 && (pfd.revents & POLLIN)) {
		if (wl_display_read_events(dpy) < 0)
			die_display("read");
	} else {
		wl_display_cancel_read(dpy);
		if (r > 0 && (pfd.revents & (POLLHUP | POLLERR)))
			die("compositor connection closed");
	}
	if (wl_display_dispatch_pending(dpy) < 0)
		die_display("dispatch");
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

static void setup_buffers(void)
{
	stride = (size_t)width * bpp;
	size_t one = stride * height;
	size_t total = one * NBUF;

	int fd = memfd_create("picowl-commit-loop", MFD_CLOEXEC);
	if (fd < 0)
		die_errno("memfd_create");
	if (ftruncate(fd, (off_t)total) < 0)
		die_errno("ftruncate");
	void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		die_errno("mmap");
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)total);
	if (!pool)
		die("wl_shm.create_pool failed");
	close(fd);
	for (int i = 0; i < NBUF; i++) {
		bufs[i].data = (char *)map + one * i;
		bufs[i].wl = wl_shm_pool_create_buffer(pool, (int32_t)(one * i), width, height,
			(int32_t)stride, shm_format);
		if (!bufs[i].wl)
			die("wl_shm_pool.create_buffer failed");
		wl_buffer_add_listener(bufs[i].wl, &buf_listener, &bufs[i]);
		/* Both buffers start with the same background so the blink workload
		 * never has to repaint more than the square. */
		if (!damage_full)
			fill_rect(bufs[i].data, 0, 0, width, height, pack_rgb(128, 128, 128));
	}
	wl_shm_pool_destroy(pool);
}

static bool parse_damage(const char *s)
{
	if (!strcmp(s, "full")) {
		damage_full = true;
		return true;
	}
	damage_full = false;
	if (!strcmp(s, "square")) {
		dmg_w = dmg_h = SQUARE_DEFAULT;
		return true;
	}
	char *end;
	errno = 0;
	long w = strtol(s, &end, 10);
	if (errno || end == s || (*end != 'x' && *end != 'X') || w <= 0 || w > MAX_DIM)
		return false;
	const char *p = end + 1;
	long h = strtol(p, &end, 10);
	if (errno || end == p || *end || h <= 0 || h > MAX_DIM)
		return false;
	dmg_w = (int)w;
	dmg_h = (int)h;
	return true;
}

static void usage(void)
{
	fprintf(stderr, "usage: picowl-commit-loop [--fps N] [--damage full|WxH|square] "
		"[--duration SECONDS]\n");
}

int main(int argc, char **argv)
{
	long fps = 30;
	double duration = 60;

	damage_full = true;
	for (int i = 1; i < argc; i++) {
		char *end;
		if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
			errno = 0;
			fps = strtol(argv[++i], &end, 10);
			if (errno || *end || fps < 1 || fps > 1000) {
				usage();
				die("--fps needs an integer from 1 to 1000");
			}
		} else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
			errno = 0;
			duration = strtod(argv[++i], &end);
			if (errno || *end || !(duration > 0) || duration > 86400) {
				usage();
				die("--duration needs a positive number of seconds, at most 86400");
			}
		} else if (!strcmp(argv[i], "--damage") && i + 1 < argc) {
			if (!parse_damage(argv[++i])) {
				usage();
				die("--damage needs full, square or WxH");
			}
		} else {
			usage();
			return 2;
		}
	}

	cap_deltas = (size_t)(duration * (double)fps) + (size_t)fps + 64;
	deltas_ns = calloc(cap_deltas, sizeof(*deltas_ns));
	if (!deltas_ns)
		die("out of memory for the delta buffer");

	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGALRM, on_alarm);
	alarm(STARTUP_TIMEOUT_S);

	dpy = wl_display_connect(NULL);
	if (!dpy)
		die("cannot connect to the Wayland display");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	if (!reg)
		die("wl_display.get_registry failed");
	wl_registry_add_listener(reg, &reg_listener, NULL);
	if (wl_display_roundtrip(dpy) < 0 || wl_display_roundtrip(dpy) < 0) /* 2nd: shm formats */
		die_display("registry roundtrip");
	if (!compositor)
		die("compositor does not advertise wl_compositor");
	if (!shm)
		die("compositor does not advertise wl_shm");
	if (!wm_base)
		die("compositor does not advertise xdg_wm_base");
	if (!presentation)
		die("compositor does not advertise wp_presentation");
	if (have_565) {
		shm_format = WL_SHM_FORMAT_RGB565;
		bpp = 2;
	}

	surface = wl_compositor_create_surface(compositor);
	if (!surface)
		die("wl_compositor.create_surface failed");
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	if (!xs)
		die("xdg_wm_base.get_xdg_surface failed");
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	if (!tl)
		die("xdg_surface.get_toplevel failed");
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_title(tl, "picowl-commit-loop");
	xdg_toplevel_set_app_id(tl, "picowl-commit-loop");
	wl_surface_commit(surface);

	while (!configured && !quit)
		pump(-1);
	if (!configured)
		die(closed ? "toplevel closed before the first configure" : "interrupted");
	alarm(0);

	if (width <= 0 || height <= 0) {
		fprintf(stderr, "picowl-commit-loop: compositor sent no size, using 64x64\n");
		width = height = 64;
	}
	if (width > MAX_DIM || height > MAX_DIM)
		die("configured size is too large");

	if (!damage_full) {
		if (dmg_w > width || dmg_h > height) {
			fprintf(stderr, "picowl-commit-loop: clamping %dx%d square to the %dx%d surface\n",
				dmg_w, dmg_h, width, height);
			if (dmg_w > width)
				dmg_w = width;
			if (dmg_h > height)
				dmg_h = height;
		}
		dmg_x = (width - dmg_w) / 2;
		dmg_y = (height - dmg_h) / 2;
	}

	init_palette();
	setup_buffers();

	if (damage_full)
		fprintf(stderr, "picowl-commit-loop: damage 0,0 %dx%d (full surface, %zu bytes)\n",
			width, height, stride * height);
	else
		fprintf(stderr, "picowl-commit-loop: damage %d,%d %dx%d (%zu bytes of %zu)\n",
			dmg_x, dmg_y, dmg_w, dmg_h, (size_t)dmg_w * dmg_h * bpp, stride * height);
	fprintf(stderr, "picowl-commit-loop: %s %dx%d, %ld fps for %.1f s\n",
		bpp == 2 ? "RGB565" : "XRGB8888", width, height, fps, duration);

	const int64_t period = 1000000000LL / fps;
	prime();
	int64_t start = now_ns();
	int64_t end = start + (int64_t)(duration * 1e9);
	int64_t next = start;

	while (!quit) {
		int64_t t = now_ns();
		if (t >= end)
			break;
		struct buf *b = free_buf();
		int64_t wait = end - t;
		if (frame_ready && b) {
			if (t >= next) {
				frame_ready = false;
				commit_frame(b);
				/* Falling behind must not bank credit: a burst of catch-up
				 * commits would break the fps cap. */
				next += period;
				if (next < t)
					next = t;
				continue;
			}
			if (next - t < wait)
				wait = next - t;
		}
		pump(wait);
	}

	int64_t drain_end = now_ns() + DRAIN_NS;
	while (outstanding && !closed) {
		int64_t left = drain_end - now_ns();
		if (left <= 0)
			break;
		pump(left);
	}
	if (outstanding)
		fprintf(stderr, "picowl-commit-loop: %u frames never got feedback\n", outstanding);
	if (closed)
		die("toplevel closed by the compositor");

	qsort(deltas_ns, n_deltas, sizeof(*deltas_ns), cmp_i64);
	double sum = 0, mean = NAN, p95 = NAN, max = NAN;
	uint64_t late = 0;
	const double late_ns = 1.5 * (double)period;
	for (size_t i = 0; i < n_deltas; i++) {
		sum += (double)deltas_ns[i];
		if ((double)deltas_ns[i] > late_ns)
			late++;
	}
	if (n_deltas) {
		mean = sum / (double)n_deltas / 1e6;
		size_t idx = (n_deltas * 95 + 99) / 100;
		p95 = (double)deltas_ns[idx ? idx - 1 : 0] / 1e6;
		max = (double)deltas_ns[n_deltas - 1] / 1e6;
	} else {
		fprintf(stderr, "picowl-commit-loop: fewer than two presented frames, no deltas\n");
	}

	printf("frames_committed,presented,discarded,late,mean_delta_ms,p95_delta_ms,"
	       "max_delta_ms,format,width,height\n");
	printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.3f,%s,%d,%d\n",
		n_committed, n_presented, n_discarded, late, mean, p95, max,
		bpp == 2 ? "RGB565" : "XRGB8888", width, height);
	if (fflush(stdout) || ferror(stdout))
		die("writing the CSV row failed");

	wl_display_disconnect(dpy);
	free(deltas_ns);
	return 0;
}
