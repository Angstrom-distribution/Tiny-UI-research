/*
 * picowl-panel - a tiny layer-shell panel for picowl on 240x320 handhelds:
 * clock, battery, a backlight slider and a volume slider, drawn with wl_shm.
 *
 * One thread, one poll() loop. Nothing wakes it up without a reason: the
 * Wayland socket, the minute timer of the clock, a 30 s timer for the battery,
 * the mixer's descriptors, and a short deadline only while a drag has a value
 * waiting to be written. A redraw happens when a value changes, and only the
 * changed rectangles are damaged. The pure parts are in panel-logic.c,
 * panel-draw.c and panel-sys.c; this file is the glue.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "panel-draw.h"
#include "panel-logic.h"
#include "panel-sys.h"

/* The battery is read this often; the only periodic wake-up besides the
 * clock. PICOWL_PANEL_BATTERY_POLL_S shortens it for tests. */
#define BATTERY_POLL_S 30
#define MAX_DAMAGE 8
#define MAX_INJECT 64

enum { W_CLOCK = 1, W_BATTERY = 2, W_BACKLIGHT = 4, W_VOLUME = 8 };

struct panel {
	/* options */
	int height;
	bool bottom;
	int scale;
	bool dump_state, watch, exit_after_frame;
	const char *inject;

	/* wayland */
	struct wl_display *dpy;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_pointer *pointer;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *ls;
	int n_outputs;
	bool shm_565;
	bool configured, closed, need_buffer;
	int cfg_w, cfg_h;

	/* the single buffer */
	struct wl_buffer *buffer;
	void *map;
	size_t map_size;
	struct pl_canvas canvas;
	struct pl_layout layout;

	/* damage collected since the last commit, and the widgets to redraw */
	unsigned dirty;
	struct pl_rect dmg[MAX_DAMAGE];
	int n_dmg;

	/* what is shown */
	struct pl_state st;
	struct pl_touch touch;
	int px, py;

	/* writes waiting for the throttle */
	bool pending[PL_SLIDERS];
	int pending_val[PL_SLIDERS];
	int64_t last_apply[PL_SLIDERS];

	/* system */
	struct pl_backlight bl;
	bool have_bl, bl_warned, vol_warned;
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	long vol_min, vol_max;
	struct pollfd mixer_fds[8];
	int n_mixer_fds;

	int clock_fd, battery_fd, sig_fd;
	bool quit;
	int exit_code;
};

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "picowl-panel: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void dump_state(const struct panel *p, const char *why);

/* ---- damage and redraw ---- */

static struct pl_rect rect_union(struct pl_rect a, struct pl_rect b)
{
	int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
	int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
	int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
	return (struct pl_rect){ x0, y0, x1 - x0, y1 - y0 };
}

static void add_damage(struct panel *p, struct pl_rect r)
{
	if (p->n_dmg == MAX_DAMAGE) {
		for (int i = 1; i < p->n_dmg; i++)
			p->dmg[0] = rect_union(p->dmg[0], p->dmg[i]);
		p->n_dmg = 1;
	}
	p->dmg[p->n_dmg++] = r;
}

static void commit(struct panel *p)
{
	if (!p->buffer)
		return;
	wl_surface_attach(p->surface, p->buffer, 0, 0);
	for (int i = 0; i < p->n_dmg; i++)
		wl_surface_damage_buffer(p->surface, p->dmg[i].x, p->dmg[i].y,
			p->dmg[i].w, p->dmg[i].h);
	p->n_dmg = 0;
	wl_surface_commit(p->surface);
}

/* Draw the widgets that changed into the buffer and send them. */
static void flush_redraw(struct panel *p)
{
	const struct pl_layout *l = &p->layout;

	if (!p->buffer || !p->dirty)
		return;
	if (p->dirty & W_CLOCK) {
		pl_render_clock(&p->canvas, l, &p->st);
		add_damage(p, l->clock);
	}
	if (p->dirty & W_BATTERY) {
		pl_render_battery(&p->canvas, l, &p->st);
		add_damage(p, l->battery);
	}
	if (p->dirty & W_BACKLIGHT) {
		pl_render_slider(&p->canvas, l, PL_SLIDER_BACKLIGHT, p->st.bl_pct);
		add_damage(p, l->slider[PL_SLIDER_BACKLIGHT].cell);
	}
	if (p->dirty & W_VOLUME) {
		pl_render_slider(&p->canvas, l, PL_SLIDER_VOLUME, p->st.vol_pct);
		add_damage(p, l->slider[PL_SLIDER_VOLUME].cell);
	}
	if (p->watch) {
		static const char *const names[] = { "clock", "battery", "backlight", "volume" };
		char why[48] = "";
		for (int i = 0; i < 4; i++)
			if (p->dirty & (1u << i))
				snprintf(why + strlen(why), sizeof(why) - strlen(why), "%s%s",
					*why ? "," : "", names[i]);
		p->dirty = 0;
		commit(p);
		dump_state(p, why);
		return;
	}
	p->dirty = 0;
	commit(p);
}

/* ---- the buffer ---- */

static int make_shm_file(size_t size)
{
	int fd = memfd_create("picowl-panel", MFD_CLOEXEC);
	if (fd < 0) {
		const char *dir = getenv("XDG_RUNTIME_DIR");
		char path[PATH_MAX];
		if (!dir)
			return -1;
		snprintf(path, sizeof(path), "%s/picowl-panel-XXXXXX", dir);
		fd = mkostemp(path, O_CLOEXEC);
		if (fd < 0)
			return -1;
		unlink(path);
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Lay out for w x h, allocate a buffer of that size, draw everything and
 * commit. The old buffer is released after the new one is attached. */
static bool rebuild(struct panel *p, int w, int h)
{
	int bpp = p->shm_565 ? 2 : 4;
	size_t stride = (size_t)w * bpp, size = stride * h;
	struct wl_buffer *old = p->buffer;
	void *old_map = p->map;
	size_t old_size = p->map_size;

	if (w < 1 || h < 2 || w > 8192 || h > 8192)
		return false;
	if (!old || p->canvas.w != w || p->canvas.h != h) {
		int fd = make_shm_file(size);
		if (fd < 0) {
			say("cannot allocate the %dx%d buffer: %s", w, h, strerror(errno));
			return false;
		}
		void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED) {
			say("cannot map the buffer: %s", strerror(errno));
			close(fd);
			return false;
		}
		struct wl_shm_pool *pool = wl_shm_create_pool(p->shm, fd, (int32_t)size);
		p->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride,
			p->shm_565 ? WL_SHM_FORMAT_RGB565 : WL_SHM_FORMAT_XRGB8888);
		wl_shm_pool_destroy(pool);
		close(fd);
		p->map = map;
		p->map_size = size;
		p->canvas = (struct pl_canvas){ map, w, h, (int)stride, p->shm_565 };
	}
	pl_layout_compute(&p->layout, w, h, p->scale);
	pl_render_all(&p->canvas, &p->layout, &p->st);

	/* Everything is opaque: the compositor need not blend under it. */
	struct wl_region *rg = wl_compositor_create_region(p->compositor);
	wl_region_add(rg, 0, 0, w, h);
	wl_surface_set_opaque_region(p->surface, rg);
	wl_region_destroy(rg);

	p->dirty = 0;
	p->n_dmg = 0;
	add_damage(p, (struct pl_rect){ 0, 0, w, h });
	commit(p);

	if (old && old != p->buffer) {
		wl_buffer_destroy(old);
		munmap(old_map, old_size);
	}
	return true;
}

/* ---- backlight and volume ---- */

/* The thumb shows what the device holds: picowl dims, caps on the LOW profile
 * and the user's own tools write the brightness behind the panel's back, and
 * the panel itself only read it at start. Not while a write of ours is
 * waiting or the stylus is on the slider, where the thumb is the user's. */
static void backlight_refresh(struct panel *p)
{
	if (!p->have_bl || p->pending[PL_SLIDER_BACKLIGHT] ||
			(p->touch.down && p->touch.slider == PL_SLIDER_BACKLIGHT))
		return;
	int raw = pl_backlight_read(&p->bl);
	if (raw < 0)
		return;
	int v = pl_bl_pct_from_raw(raw, p->bl.max);
	if (v != p->st.bl_pct) {
		p->st.bl_pct = v;
		p->dirty |= W_BACKLIGHT;
	}
}

static void apply_backlight(struct panel *p, int pct)
{
	if (!p->have_bl)
		return;
	if (pl_backlight_write(&p->bl, pl_bl_raw_from_pct(pct, p->bl.max)))
		return;
	if (!p->bl_warned) {
		/* Once: a dragged slider would repeat it for every write. */
		say("cannot write %s/brightness: %s", p->bl.path, strerror(errno));
		p->bl_warned = true;
	}
	/* Not applied: do not leave the thumb at a level the screen is not at. */
	int raw = pl_backlight_read(&p->bl);
	if (raw >= 0) {
		p->st.bl_pct = pl_bl_pct_from_raw(raw, p->bl.max);
		p->dirty |= W_BACKLIGHT;
	}
}

static void silent_alsa_error(const char *file, int line, const char *func,
	int err, const char *fmt, ...)
{
	(void)file; (void)line; (void)func; (void)err; (void)fmt;
}

static void mixer_close(struct panel *p)
{
	if (p->mixer)
		snd_mixer_close(p->mixer);
	p->mixer = NULL;
	p->elem = NULL;
	p->n_mixer_fds = 0;
	p->st.vol_pct = -1;
}

/* Master, PCM, Headphone, Speaker, else the first element that has a playback
 * volume. */
static snd_mixer_elem_t *mixer_pick(snd_mixer_t *m)
{
	static const char *const names[] = { "Master", "PCM", "Headphone", "Speaker" };
	snd_mixer_selem_id_t *sid;

	snd_mixer_selem_id_alloca(&sid);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snd_mixer_selem_id_set_index(sid, 0);
		snd_mixer_selem_id_set_name(sid, names[i]);
		snd_mixer_elem_t *e = snd_mixer_find_selem(m, sid);
		if (e && snd_mixer_selem_has_playback_volume(e))
			return e;
	}
	for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e))
		if (snd_mixer_selem_is_active(e) && snd_mixer_selem_has_playback_volume(e))
			return e;
	return NULL;
}

static int mixer_read_pct(struct panel *p)
{
	long v;

	for (int ch = 0; ch <= SND_MIXER_SCHN_LAST; ch++) {
		if (!snd_mixer_selem_has_playback_channel(p->elem, ch))
			continue;
		if (snd_mixer_selem_get_playback_volume(p->elem, ch, &v) < 0)
			return -1;
		return pl_vol_pct_from_raw(v, p->vol_min, p->vol_max);
	}
	return -1;
}

static void mixer_open(struct panel *p)
{
	snd_mixer_t *m = NULL;
	int err;

	snd_lib_error_set_handler(silent_alsa_error);
	if ((err = snd_mixer_open(&m, 0)) < 0 ||
			(err = snd_mixer_attach(m, "default")) < 0 ||
			(err = snd_mixer_selem_register(m, NULL, NULL)) < 0 ||
			(err = snd_mixer_load(m)) < 0) {
		say("no mixer (%s), the volume slider is disabled", snd_strerror(err));
		if (m)
			snd_mixer_close(m);
		return;
	}
	snd_mixer_elem_t *e = mixer_pick(m);
	if (!e || snd_mixer_selem_get_playback_volume_range(e, &p->vol_min, &p->vol_max) < 0 ||
			p->vol_max <= p->vol_min) {
		say("no playback volume control, the volume slider is disabled");
		snd_mixer_close(m);
		return;
	}
	int n = snd_mixer_poll_descriptors_count(m);
	if (n < 1 || n > (int)(sizeof(p->mixer_fds) / sizeof(p->mixer_fds[0])) ||
			snd_mixer_poll_descriptors(m, p->mixer_fds, n) < 0) {
		say("cannot watch the mixer, the volume slider is disabled");
		snd_mixer_close(m);
		return;
	}
	p->mixer = m;
	p->elem = e;
	p->n_mixer_fds = n;
	p->st.vol_pct = mixer_read_pct(p);
	if (p->st.vol_pct < 0)
		mixer_close(p);
}

static void apply_volume(struct panel *p, int pct)
{
	if (!p->elem)
		return;
	int err = snd_mixer_selem_set_playback_volume_all(p->elem,
		pl_vol_raw_from_pct(pct, p->vol_min, p->vol_max));
	if (err >= 0 && pct > 0 && snd_mixer_selem_has_playback_switch(p->elem))
		err = snd_mixer_selem_set_playback_switch_all(p->elem, 1);
	if (err >= 0)
		return;
	if (!p->vol_warned) {
		say("cannot set the volume: %s", snd_strerror(err));
		p->vol_warned = true;
	}
	/* Not applied: show what the mixer holds. */
	int v = mixer_read_pct(p);
	if (v >= 0) {
		p->st.vol_pct = v;
		p->dirty |= W_VOLUME;
	}
}

/* The mixer has something to say: another program changed the volume. */
static void mixer_events(struct panel *p, int fds_start, struct pollfd *pfd)
{
	unsigned short rev = 0;

	for (int i = 0; i < p->n_mixer_fds; i++)
		p->mixer_fds[i].revents = pfd[fds_start + i].revents;
	if (snd_mixer_poll_descriptors_revents(p->mixer, p->mixer_fds,
			p->n_mixer_fds, &rev) < 0 ||
			(rev & (POLLERR | POLLHUP | POLLNVAL)) ||
			snd_mixer_handle_events(p->mixer) < 0) {
		say("the mixer went away, the volume slider is disabled");
		mixer_close(p);
		p->dirty |= W_VOLUME;
		return;
	}
	/* Our own drag would come back quantized and make the thumb jump. */
	if (p->touch.down && p->touch.slider == PL_SLIDER_VOLUME)
		return;
	int v = mixer_read_pct(p);
	if (v >= 0 && v != p->st.vol_pct) {
		p->st.vol_pct = v;
		p->dirty |= W_VOLUME;
	}
}

/* ---- touch ---- */

static void set_slider(struct panel *p, int s, int v)
{
	int *cur = s == PL_SLIDER_BACKLIGHT ? &p->st.bl_pct : &p->st.vol_pct;

	if (*cur == v)
		return;
	*cur = v;
	p->dirty |= s == PL_SLIDER_BACKLIGHT ? W_BACKLIGHT : W_VOLUME;
	p->pending[s] = true;
	p->pending_val[s] = v;
}

/* Write the values that wait; at most every PL_APPLY_INTERVAL_MS per slider
 * unless force (the release of a drag, or the end of the program). */
static void apply_pending(struct panel *p, bool force)
{
	int64_t now = now_ms();

	for (int s = 0; s < PL_SLIDERS; s++) {
		if (!p->pending[s])
			continue;
		if (!force && pl_apply_wait_ms(now, p->last_apply[s]) > 0)
			continue;
		if (s == PL_SLIDER_BACKLIGHT)
			apply_backlight(p, p->pending_val[s]);
		else
			apply_volume(p, p->pending_val[s]);
		p->pending[s] = false;
		p->last_apply[s] = now;
	}
}

static int apply_timeout_ms(const struct panel *p)
{
	int64_t now = now_ms();
	int t = -1;

	for (int s = 0; s < PL_SLIDERS; s++) {
		if (!p->pending[s])
			continue;
		int w = pl_apply_wait_ms(now, p->last_apply[s]);
		if (t < 0 || w < t)
			t = w;
	}
	return t;
}

static void touch_press(struct panel *p)
{
	bool en[PL_SLIDERS] = { p->st.bl_pct >= 0, p->st.vol_pct >= 0 };

	/* set_slider ignores a value equal to the shown one, which must not be
	 * a stale one. */
	backlight_refresh(p);
	int v, s = pl_touch_press(&p->touch, &p->layout, en, p->px, p->py, &v);

	if (s != PL_SLIDER_NONE)
		set_slider(p, s, v);
}

static void touch_motion(struct panel *p)
{
	int v, s = pl_touch_motion(&p->touch, &p->layout, p->px, &v);

	if (s != PL_SLIDER_NONE)
		set_slider(p, s, v);
}

static void touch_release(struct panel *p)
{
	pl_touch_release(&p->touch);
	apply_pending(p, true);
}

/* ---- wl_pointer ---- */

static void ptr_enter(void *d, struct wl_pointer *w, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	struct panel *p = d;
	(void)w; (void)serial; (void)s;
	p->px = wl_fixed_to_int(x);
	p->py = wl_fixed_to_int(y);
	backlight_refresh(p);
}

static void ptr_leave(void *d, struct wl_pointer *w, uint32_t serial,
	struct wl_surface *s)
{
	struct panel *p = d;
	(void)w; (void)serial; (void)s;
	if (p->touch.down)
		touch_release(p);
}

static void ptr_motion(void *d, struct wl_pointer *w, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
	struct panel *p = d;
	(void)w; (void)time;
	p->px = wl_fixed_to_int(x);
	p->py = wl_fixed_to_int(y);
	touch_motion(p);
}

static void ptr_button(void *d, struct wl_pointer *w, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	struct panel *p = d;
	(void)w; (void)serial; (void)time;
	if (button != BTN_LEFT)
		return;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		touch_press(p);
	else if (p->touch.down)
		touch_release(p);
}

static void ptr_axis(void *d, struct wl_pointer *w, uint32_t t, uint32_t a, wl_fixed_t v)
{
	(void)d; (void)w; (void)t; (void)a; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *w) { (void)d; (void)w; }
static void ptr_axis_source(void *d, struct wl_pointer *w, uint32_t s)
{
	(void)d; (void)w; (void)s;
}
static void ptr_axis_stop(void *d, struct wl_pointer *w, uint32_t t, uint32_t a)
{
	(void)d; (void)w; (void)t; (void)a;
}
static void ptr_axis_discrete(void *d, struct wl_pointer *w, uint32_t a, int32_t v)
{
	(void)d; (void)w; (void)a; (void)v;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
	.button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
	.axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
	struct panel *p = d;
	(void)seat;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !p->pointer) {
		p->pointer = wl_seat_get_pointer(p->seat);
		wl_pointer_add_listener(p->pointer, &pointer_listener, p);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && p->pointer) {
		wl_pointer_destroy(p->pointer);
		p->pointer = NULL;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{
	(void)d; (void)s; (void)n;
}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* ---- registry, shm, layer surface ---- */

static void shm_format(void *d, struct wl_shm *shm, uint32_t format)
{
	struct panel *p = d;
	(void)shm;
	if (format == WL_SHM_FORMAT_RGB565)
		p->shm_565 = true;
}
static const struct wl_shm_listener shm_listener = { shm_format };

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	struct panel *p = d;

	if (!strcmp(iface, wl_compositor_interface.name)) {
		/* damage_buffer needs version 4; an older one is reported in main */
		if (ver >= 4)
			p->compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		p->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
		wl_shm_add_listener(p->shm, &shm_listener, p);
	} else if (!strcmp(iface, wl_seat_interface.name) && !p->seat) {
		p->seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
		wl_seat_add_listener(p->seat, &seat_listener, p);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		p->layer_shell = wl_registry_bind(r, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		p->n_outputs++;
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r; (void)name;
}
static const struct wl_registry_listener registry_listener = { reg_global, reg_remove };

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
	uint32_t w, uint32_t h)
{
	struct panel *p = d;

	zwlr_layer_surface_v1_ack_configure(ls, serial);
	p->cfg_w = (int)w;
	p->cfg_h = (int)h;
	p->configured = true;
	p->need_buffer = true;
}

static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls)
{
	struct panel *p = d;
	(void)ls;
	p->closed = true;
}
static const struct zwlr_layer_surface_v1_listener ls_listener = { ls_configure, ls_closed };

/* ---- clock, battery ---- */

/* Arm the clock for the next full minute. Absolute and cancel-on-set, so a
 * change of the system time is noticed at once and the next wake-up is
 * recomputed. */
static void clock_arm(struct panel *p)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	struct itimerspec its = { .it_value = { .tv_sec = (now.tv_sec / 60 + 1) * 60 } };
	timerfd_settime(p->clock_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL);
}

static void clock_update(struct panel *p)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	if (tm.tm_hour != p->st.hour || tm.tm_min != p->st.min) {
		p->st.hour = tm.tm_hour;
		p->st.min = tm.tm_min;
		p->dirty |= W_CLOCK;
	}
}

static void battery_update(struct panel *p)
{
	enum pl_bat_status st;
	int pct;

	pl_battery_read(NULL, &st, &pct);
	if (st != p->st.bat || pct != p->st.bat_pct) {
		p->st.bat = st;
		p->st.bat_pct = pct;
		p->dirty |= W_BATTERY;
	}
}

/* ---- test hooks ---- */

/* "p X,Y" press, "m X,Y" motion, "e X,Y" pointer enter, "r" release,
 * separated by ; or space: the same handlers as the wl_pointer events, for
 * tests without a pointer. "b RAW" writes the backlight as another process
 * would. */
static void run_inject(struct panel *p)
{
	char buf[MAX_INJECT * 12];
	char *save = NULL;

	snprintf(buf, sizeof(buf), "%s", p->inject);
	for (char *tok = strtok_r(buf, "; ", &save); tok; tok = strtok_r(NULL, "; ", &save)) {
		int x, y;
		if (tok[0] == 'e' && sscanf(tok + 1, "%d,%d", &x, &y) == 2) {
			p->px = x;
			p->py = y;
			backlight_refresh(p);
		} else if (tok[0] == 'b' && sscanf(tok + 1, "%d", &x) == 1) {
			/* Another process sets the backlight. */
			pl_backlight_write(&p->bl, x);
		} else if (tok[0] == 'r') {
			if (p->touch.down)
				touch_release(p);
		} else if ((tok[0] == 'p' || tok[0] == 'm') && sscanf(tok + 1, "%d,%d", &x, &y) == 2) {
			p->px = x;
			p->py = y;
			if (tok[0] == 'p')
				touch_press(p);
			else
				touch_motion(p);
		} else {
			say("bad --inject token '%s'", tok);
		}
		flush_redraw(p);
	}
}

static const char *bat_name(enum pl_bat_status st)
{
	switch (st) {
	case PL_BAT_DISCHARGING: return "discharging";
	case PL_BAT_CHARGING: return "charging";
	case PL_BAT_FULL: return "full";
	case PL_BAT_AC: return "ac";
	default: return "none";
	}
}

static void print_rect(const char *name, struct pl_rect r)
{
	printf(" %s=%d,%d,%d,%d", name, r.x, r.y, r.w, r.h);
}

/* One line per widget, key=value, for the tests. */
static void dump_state(const struct panel *p, const char *why)
{
	const struct pl_layout *l = &p->layout;
	char text[8];

	if (why)
		printf("redraw %s\n", why);
	printf("panel width=%d height=%d row=%d format=%s anchor=%s scale=%d text_scale=%d\n",
		l->w, l->h, l->row_h, p->shm_565 ? "RGB565" : "XRGB8888",
		p->bottom ? "bottom" : "top", l->scale, l->text_scale);
	pl_clock_text(text, sizeof(text), p->st.hour, p->st.min);
	printf("clock text=%s", text);
	print_rect("rect", l->clock);
	printf("\n");
	pl_battery_text(text, sizeof(text), p->st.bat, p->st.bat_pct);
	printf("battery text=%s status=%s percent=%d", text, bat_name(p->st.bat),
		p->st.bat_pct);
	print_rect("rect", l->battery);
	printf("\n");
	for (int s = 0; s < PL_SLIDERS; s++) {
		int v = s == PL_SLIDER_BACKLIGHT ? p->st.bl_pct : p->st.vol_pct;
		printf("%s available=%d value=%d", s == PL_SLIDER_BACKLIGHT ? "backlight" : "volume",
			v >= 0, v);
		if (s == PL_SLIDER_BACKLIGHT && p->have_bl)
			printf(" raw=%d max=%d", pl_backlight_read(&p->bl), p->bl.max);
		print_rect("cell", l->slider[s].cell);
		print_rect("track", l->slider[s].track);
		if (v >= 0)
			print_rect("thumb", pl_slider_thumb(&l->slider[s], v));
		printf("\n");
	}
	fflush(stdout);
}

/* ---- main ---- */

static void usage(FILE *out)
{
	fprintf(out,
		"usage: picowl-panel [options]\n"
		"  --height N          surface height in pixels, 40..120 (default %d)\n"
		"  --bottom            anchor at the bottom edge (default: top)\n"
		"  --scale N           integer UI scale of text and thumb, 1..%d (default 1)\n"
		"  --dump-state        print the widget values and geometry after the first\n"
		"                      frame and exit (for tests)\n"
		"  --watch             test only: like --dump-state, and print the state again\n"
		"                      after every redraw, naming the widgets, without exiting\n"
		"  --exit-after-frame  exit after the first frame\n"
		"  --inject SPEC       test only: feed pointer events after the first frame\n"
		"                      (p X,Y press; m X,Y motion; e X,Y enter; r release;\n"
		"                      b RAW sets the backlight), before the dump\n"
		"  --help              this text\n",
		PL_HEIGHT_DEFAULT, PL_SCALE_MAX);
}

static bool parse_int(const char *s, int *v)
{
	char *end;
	long n = strtol(s, &end, 10);

	if (end == s || *end || n < INT_MIN / 2 || n > INT_MAX / 2)
		return false;
	*v = (int)n;
	return true;
}

static int parse_args(struct panel *p, int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int v;

		if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "--bottom")) {
			p->bottom = true;
		} else if (!strcmp(a, "--dump-state")) {
			p->dump_state = true;
		} else if (!strcmp(a, "--watch")) {
			p->watch = true;
		} else if (!strcmp(a, "--exit-after-frame")) {
			p->exit_after_frame = true;
		} else if ((!strcmp(a, "--height") || !strcmp(a, "--scale")) && i + 1 < argc) {
			if (!parse_int(argv[++i], &v)) {
				say("%s: '%s' is not a number", a, argv[i]);
				return -1;
			}
			if (!strcmp(a, "--height"))
				p->height = pl_clamp_height(v);
			else
				p->scale = v < 1 ? 1 : v > PL_SCALE_MAX ? PL_SCALE_MAX : v;
		} else if (!strcmp(a, "--inject") && i + 1 < argc) {
			p->inject = argv[++i];
		} else {
			say("unknown or incomplete option '%s'", a);
			usage(stderr);
			return -1;
		}
	}
	return 1;
}

/* Wayland connection failed or was closed. A protocol error is ours to
 * report; a plain disconnect (the compositor exited) is a normal end. */
static void display_gone(struct panel *p)
{
	int err = wl_display_get_error(p->dpy);

	if (err == EPROTO) {
		const struct wl_interface *iface;
		uint32_t id;
		int code = wl_display_get_protocol_error(p->dpy, &iface, &id);
		say("protocol error %d on %s", code, iface ? iface->name : "?");
		p->exit_code = 1;
	}
	p->quit = true;
}

static void first_frame(struct panel *p)
{
	wl_display_roundtrip(p->dpy);
	if (p->inject)
		run_inject(p);
	if (p->dump_state || p->watch)
		dump_state(p, p->watch ? "first" : NULL);
	if ((p->dump_state && !p->watch) || p->exit_after_frame)
		p->quit = true;
}

static void run(struct panel *p)
{
	bool first = true;
	struct pollfd pfd[4 + 8];

	while (!p->quit) {
		while (wl_display_prepare_read(p->dpy) != 0)
			if (wl_display_dispatch_pending(p->dpy) < 0) {
				display_gone(p);
				return;
			}
		int fl = wl_display_flush(p->dpy);
		if (fl < 0 && errno != EAGAIN) {
			wl_display_cancel_read(p->dpy);
			display_gone(p);
			return;
		}

		int n = 0;
		pfd[n++] = (struct pollfd){ wl_display_get_fd(p->dpy),
			POLLIN | (fl < 0 ? POLLOUT : 0), 0 };
		pfd[n++] = (struct pollfd){ p->sig_fd, POLLIN, 0 };
		pfd[n++] = (struct pollfd){ p->clock_fd, POLLIN, 0 };
		pfd[n++] = (struct pollfd){ p->battery_fd, POLLIN, 0 };
		int mixer_at = n;
		for (int i = 0; i < p->n_mixer_fds; i++)
			pfd[n++] = (struct pollfd){ p->mixer_fds[i].fd, p->mixer_fds[i].events, 0 };

		int r = poll(pfd, n, apply_timeout_ms(p));
		if (r < 0) {
			wl_display_cancel_read(p->dpy);
			if (errno == EINTR)
				continue;
			say("poll: %s", strerror(errno));
			p->exit_code = 1;
			return;
		}
		if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (wl_display_read_events(p->dpy) < 0) {
				display_gone(p);
				return;
			}
		} else {
			wl_display_cancel_read(p->dpy);
		}
		if (wl_display_dispatch_pending(p->dpy) < 0) {
			display_gone(p);
			return;
		}

		if (pfd[1].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(p->sig_fd, &si, sizeof(si)) > 0) {
				p->quit = true;
				break;
			}
		}
		if (pfd[2].revents & POLLIN) {
			uint64_t exp;
			/* ECANCELED: the system time was set. Either way, the next
			 * minute is the next wake-up. */
			if (read(p->clock_fd, &exp, sizeof(exp)) >= 0 || errno == ECANCELED) {
				/* Armed first: a change of the time after the arming
				 * cancels the timer, one before it is in the reading. */
				clock_arm(p);
				clock_update(p);
			}
		}
		if (pfd[3].revents & POLLIN) {
			uint64_t exp;
			if (read(p->battery_fd, &exp, sizeof(exp)) > 0)
				battery_update(p);
		}
		if (p->mixer && n > mixer_at) {
			bool ready = false;
			for (int i = mixer_at; i < n; i++)
				ready |= pfd[i].revents != 0;
			if (ready)
				mixer_events(p, mixer_at, pfd);
		}

		if (p->closed) {
			if (!p->configured) {
				say("the compositor closed the panel surface");
				p->exit_code = 1;
			}
			break;
		}
		if (p->need_buffer) {
			p->need_buffer = false;
			int w = p->cfg_w > 0 ? p->cfg_w : 240;
			int h = p->cfg_h > 0 ? p->cfg_h : p->height;
			if (p->buffer && w == p->canvas.w && h == p->canvas.h) {
				/* Same size again: nothing to draw, but the configure
				 * is answered by a commit. */
				commit(p);
			} else {
				if (!rebuild(p, w, h)) {
					p->exit_code = 1;
					return;
				}
				if (first) {
					first = false;
					first_frame(p);
				} else if (p->watch) {
					dump_state(p, "resize");
				}
			}
		}
		apply_pending(p, false);
		flush_redraw(p);
	}
	/* A drag cut short by a signal still leaves the last value applied. */
	apply_pending(p, true);
}

int main(int argc, char **argv)
{
	struct panel p = {
		.height = PL_HEIGHT_DEFAULT, .scale = 1,
		.last_apply = { -1, -1 }, .touch = { .slider = PL_SLIDER_NONE },
		.st = { .bat = PL_BAT_NONE, .bat_pct = -1, .bl_pct = -1, .vol_pct = -1 },
		.dirty = 0, .clock_fd = -1, .battery_fd = -1, .sig_fd = -1,
	};
	int rc = parse_args(&p, argc, argv);

	if (rc <= 0)
		return rc < 0 ? 2 : 0;

	p.dpy = wl_display_connect(NULL);
	if (!p.dpy) {
		say("cannot connect to a Wayland compositor (WAYLAND_DISPLAY, XDG_RUNTIME_DIR)");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(p.dpy);
	wl_registry_add_listener(reg, &registry_listener, &p);
	wl_display_roundtrip(p.dpy);
	wl_display_roundtrip(p.dpy); /* shm formats, seat capabilities */

	if (!p.layer_shell) {
		say("the compositor has no zwlr_layer_shell_v1, cannot place a panel");
		return 1;
	}
	if (!p.compositor || !p.shm) {
		say("the compositor lacks wl_compositor version 4 or wl_shm");
		return 1;
	}
	if (p.n_outputs == 0) {
		say("the compositor has no output yet, nothing to show a panel on");
		return 1;
	}

	/* The signals must be blocked before the descriptor sees them. */
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	signal(SIGPIPE, SIG_IGN);
	p.sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	p.clock_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
	p.battery_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (p.sig_fd < 0 || p.clock_fd < 0 || p.battery_fd < 0) {
		say("cannot create timers: %s", strerror(errno));
		return 1;
	}

	p.st.hour = p.st.min = -1;
	clock_arm(&p);
	clock_update(&p);
	battery_update(&p);
	int poll_s = BATTERY_POLL_S;
	const char *e = getenv("PICOWL_PANEL_BATTERY_POLL_S");
	if (e && atoi(e) > 0)
		poll_s = atoi(e);
	struct itimerspec every = { .it_interval = { poll_s, 0 }, .it_value = { poll_s, 0 } };
	timerfd_settime(p.battery_fd, 0, &every, NULL);
	if (pl_backlight_find(&p.bl, NULL)) {
		int raw = pl_backlight_read(&p.bl);
		if (raw >= 0) {
			p.have_bl = true;
			p.st.bl_pct = pl_bl_pct_from_raw(raw, p.bl.max);
		}
	}
	if (!p.have_bl)
		say("no backlight, the brightness slider is disabled");
	mixer_open(&p);

	p.surface = wl_compositor_create_surface(p.compositor);
	p.ls = zwlr_layer_shell_v1_get_layer_surface(p.layer_shell, p.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "panel");
	zwlr_layer_surface_v1_add_listener(p.ls, &ls_listener, &p);
	zwlr_layer_surface_v1_set_size(p.ls, 0, p.height);
	zwlr_layer_surface_v1_set_anchor(p.ls,
		(p.bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(p.ls, p.height);
	zwlr_layer_surface_v1_set_keyboard_interactivity(p.ls,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(p.surface);

	run(&p);

	if (p.buffer)
		wl_buffer_destroy(p.buffer);
	if (p.map)
		munmap(p.map, p.map_size);
	zwlr_layer_surface_v1_destroy(p.ls);
	wl_surface_destroy(p.surface);
	mixer_close(&p);
	wl_display_disconnect(p.dpy);
	return p.exit_code;
}
