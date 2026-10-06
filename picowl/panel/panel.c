/*
 * picowl-panel - a tiny layer-shell panel for picowl on 240x320 handhelds: a
 * slim bar with the clock, the battery and a backlight and a volume button.
 * Tapping a button pops a row out under the bar: a slider for the backlight and
 * the volume, the date for the clock, the time left for the battery. Drawn with
 * wl_shm.
 *
 * One thread, one poll() loop. Nothing wakes it up without a reason: the
 * Wayland socket, the minute timer of the clock, a 30 s timer for the battery,
 * the mixer's descriptors, a timer that exists only while a row is open (it
 * closes the row 3 s after the last touch), and a short deadline only
 * while a drag has a value waiting to be written. A redraw happens when a value
 * changes, and only the changed rectangles are damaged. The pure parts are in
 * panel-logic.c, panel-gfx.c, panel-draw.c and panel-sys.c; this file is the
 * glue.
 *
 * The row is part of the same layer surface: opening it asks the compositor
 * for a taller surface (the exclusive zone stays the height of the bar, so no
 * window moves), and the row is drawn when the configure for the new size has
 * been acknowledged. Until then the old, smaller buffer stays on screen.
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
#define MAX_OUTPUTS 8

/* Widgets to redraw. W_ROW_VALUE is the track, thumb and value of the open
 * slider row, W_ROW the whole row. */
enum {
	W_CLOCK = 1, W_BATTERY = 2, W_BACKLIGHT = 4, W_VOLUME = 8,
	W_ROW_VALUE = 16, W_ROW = 32,
};

struct panel {
	/* options */
	int height;		/* the bar */
	bool bottom;
	const char *font_path;
	int font_px;
	int bar_alpha, popup_alpha;
	enum pl_subopt subopt;
	bool crisp;		/* --style crisp: pixel fonts and whole-pixel shapes */
	enum pl_crisp_font crisp_font;	/* --crisp-font */
	bool edge_top;		/* --edge top: the bar stays on a top edge when rotated */
	int battery_mah;	/* capacity given by the user, 0 unknown */
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
	/* what each output says about its stripes, and the one the bar is on */
	struct out_info {
		struct wl_output *wl;
		int subpixel, transform;
	} outs[MAX_OUTPUTS];
	struct wl_output *surf_out;
	enum pl_sub sub;
	bool assets_ready;
	bool shm_565;
	bool configured, closed, need_buffer, first;
	int cfg_w, cfg_h;
	int req_h;		/* the surface thickness (bar and row) last asked for */
	/* The strip: on an output rotated by 90 or 270 degrees the bar is on the
	 * short side, drawn in the panel's own orientation and turned by the
	 * buffer transform. strip is what the output calls for now, req_strip what
	 * the compositor was asked for and buf_strip the transform of the buffer
	 * that is attached, which changes only together with the buffer. */
	enum pl_strip strip, req_strip, buf_strip;

	/* the single buffer */
	struct wl_buffer *buffer;
	void *map;
	size_t map_size;
	struct pl_canvas canvas;
	struct pl_layout layout;
	struct pl_assets assets;
	bool font_ttf;

	/* damage collected since the last commit, and the widgets to redraw */
	unsigned dirty;
	struct pl_rect dmg[MAX_DAMAGE];
	int n_dmg;

	/* what is shown */
	struct pl_state st;
	struct pl_touch touch;
	struct pl_popup pop;
	int px, py;

	/* the battery's rate filter, and the text of the estimate as last shown,
	 * to see whether the open row has to be drawn again */
	struct pl_est filter;
	char est_text[48];

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

	int clock_fd, battery_fd, popup_fd, sig_fd;
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
	wl_surface_set_buffer_transform(p->surface, p->buf_strip);
	wl_surface_attach(p->surface, p->buffer, 0, 0);
	for (int i = 0; i < p->n_dmg; i++)
		wl_surface_damage_buffer(p->surface, p->dmg[i].x, p->dmg[i].y,
			p->dmg[i].w, p->dmg[i].h);
	p->n_dmg = 0;
	wl_surface_commit(p->surface);
}

static uint32_t anchor_for(enum pl_strip strip, bool bottom)
{
	switch (pl_edge_for(strip, bottom)) {
	case PL_EDGE_BOTTOM:
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	case PL_EDGE_LEFT:
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	case PL_EDGE_RIGHT:
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	default:
		return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	}
}

/* State for a surface of the given thickness (bar and row) in the current
 * mode: along the edge the compositor decides (size 0), so the bar spans the
 * short side of a rotated output. The anchor is sent only when the mode
 * changes, the exclusive zone never. */
static void request_surface(struct panel *p, int want)
{
	if (p->strip != p->req_strip || !p->req_h)
		zwlr_layer_surface_v1_set_anchor(p->ls, anchor_for(p->strip, p->bottom));
	p->req_h = want;
	p->req_strip = p->strip;
	if (p->strip != PL_STRIP_NONE)
		zwlr_layer_surface_v1_set_size(p->ls, want, 0);
	else
		zwlr_layer_surface_v1_set_size(p->ls, 0, want);
}

/* Ask for the surface the open or closed row needs, or for the other edge
 * when the output was rotated. The exclusive zone is never touched: the bar
 * keeps its strip, the row only covers windows. Done after a pending
 * configure has been answered with its buffer, so that a commit never carries
 * an acknowledged size and a buffer of another one. */
static void sync_size(struct panel *p)
{
	int want = pl_surface_height(p->height, p->pop.open != PL_SLIDER_NONE);

	if ((want == p->req_h && p->strip == p->req_strip) || !p->configured ||
			p->need_buffer)
		return;
	request_surface(p, want);
	wl_surface_commit(p->surface);
}

/* Draw the widgets that changed into the buffer and send them. */
static void flush_redraw(struct panel *p)
{
	const struct pl_layout *l = &p->layout;
	const struct pl_canvas *c = &p->canvas;
	int open = p->pop.open;

	sync_size(p);
	if (!p->buffer || !p->dirty)
		return;
	if (p->dirty & W_CLOCK) {
		pl_render_clock(c, l, &p->assets, &p->st, open == PL_BTN_CLOCK);
		add_damage(p, l->clock);
	}
	if (p->dirty & W_BATTERY) {
		pl_render_battery(c, l, &p->assets, &p->st, open == PL_BTN_BATTERY);
		add_damage(p, l->battery);
	}
	for (int s = 0; s < PL_SLIDERS; s++)
		if (p->dirty & (W_BACKLIGHT << s)) {
			pl_render_button(c, l, &p->assets, &p->st, open, s);
			add_damage(p, pl_button_rect(l, s));
		}
	/* A row that is on its way out is left as it is until the smaller
	 * surface arrives. */
	if (l->row_shown && open != PL_SLIDER_NONE) {
		if (p->dirty & W_ROW) {
			pl_render_row(c, l, &p->assets, &p->st, open);
			add_damage(p, l->row);
		} else if (p->dirty & W_ROW_VALUE) {
			pl_render_row_value(c, l, &p->assets, &p->st, open);
			add_damage(p, pl_row_value_rect(l));
		}
	}
	if (p->watch) {
		static const char *const names[] = { "clock", "battery", "backlight", "volume", "row" };
		unsigned d = p->dirty;
		char why[48] = "";
		if (d & W_ROW)
			d &= ~(unsigned)W_ROW_VALUE;
		for (int i = 0; i < 6; i++)
			if (d & (1u << i))
				snprintf(why + strlen(why), sizeof(why) - strlen(why), "%s%s",
					*why ? "," : "", names[i > 4 ? 4 : i]);
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

static void add_rects(struct wl_region *rg, const struct pl_rect *r, int n)
{
	for (int i = 0; i < n; i++)
		wl_region_add(rg, r[i].x, r[i].y, r[i].w, r[i].h);
}

/* Touches land on the bar and the row only, whatever size the compositor made
 * the surface; what is opaque is left to the compositor not to blend under. */
static void set_regions(struct panel *p, int w, int h)
{
	const struct pl_layout *l = &p->layout;
	struct pl_rect in = pl_input_rect(l, w, h), opaque[2];
	int n = 0;

	if (p->canvas.fmt != PL_FMT_ARGB8888)
		opaque[n++] = (struct pl_rect){ 0, 0, w, h };
	else
		n = pl_opaque_rects(l, p->bar_alpha, p->popup_alpha, opaque);
	/* Regions are in surface coordinates, the layout is in the buffer's. */
	in = pl_rect_to_surface(p->buf_strip, in, w, h);
	for (int i = 0; i < n; i++)
		opaque[i] = pl_rect_to_surface(p->buf_strip, opaque[i], w, h);

	struct wl_region *rg = wl_compositor_create_region(p->compositor);
	add_rects(rg, opaque, n);
	wl_surface_set_opaque_region(p->surface, rg);
	wl_region_destroy(rg);
	rg = wl_compositor_create_region(p->compositor);
	add_rects(rg, &in, 1);
	wl_surface_set_input_region(p->surface, rg);
	wl_region_destroy(rg);
}

/* Lay out for w x h, allocate a buffer of that size and format, draw
 * everything and commit. The old buffer is released after the new one is
 * attached. */
static bool rebuild(struct panel *p, int w, int h)
{
	bool row_shown = h >= pl_surface_height(p->height, true);
	enum pl_fmt fmt = pl_pick_format(p->bar_alpha, p->popup_alpha, row_shown, p->shm_565);
	int bpp = fmt == PL_FMT_RGB565 ? 2 : 4;
	size_t stride = (size_t)w * bpp, size = stride * h;
	struct wl_buffer *old = p->buffer;
	void *old_map = p->map;
	size_t old_size = p->map_size;

	if (w < 1 || h < 2 || w > 8192 || h > 8192)
		return false;
	if (!old || p->canvas.w != w || p->canvas.h != h || p->canvas.fmt != fmt) {
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
			fmt == PL_FMT_RGB565 ? WL_SHM_FORMAT_RGB565 :
			fmt == PL_FMT_ARGB8888 ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
		wl_shm_pool_destroy(pool);
		close(fd);
		p->map = map;
		p->map_size = size;
		p->canvas = (struct pl_canvas){ map, w, h, (int)stride, fmt };
	}
	struct pl_metrics m;
	pl_assets_metrics(&p->assets, &m);
	pl_layout_compute(&p->layout, w, p->height, row_shown, p->bottom, &m);
	if (!pl_assets_prepare(&p->assets, &p->layout)) {
		say("cannot allocate the icons");
		return false;
	}
	pl_render_all(&p->canvas, &p->layout, &p->assets, &p->st, p->pop.open);
	set_regions(p, w, h);

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

static void popup_close(struct panel *p);

/* The value of slider s changed on screen: its button (the volume icon shows
 * the level) and, if it is the open one, its row. */
static void mark_value(struct panel *p, int s)
{
	if (s == PL_SLIDER_VOLUME)
		p->dirty |= W_VOLUME;
	if (p->pop.open == s)
		p->dirty |= W_ROW_VALUE;
}

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
		mark_value(p, PL_SLIDER_BACKLIGHT);
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
		mark_value(p, PL_SLIDER_BACKLIGHT);
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
		mark_value(p, PL_SLIDER_VOLUME);
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
		/* A row for a slider that is gone has nothing to show. */
		if (p->pop.open == PL_SLIDER_VOLUME)
			popup_close(p);
		return;
	}
	/* Our own drag would come back quantized and make the thumb jump. */
	if (p->touch.down && p->touch.slider == PL_SLIDER_VOLUME)
		return;
	int v = mixer_read_pct(p);
	if (v >= 0 && v != p->st.vol_pct) {
		p->st.vol_pct = v;
		mark_value(p, PL_SLIDER_VOLUME);
	}
}

/* ---- touch ---- */

static void set_slider(struct panel *p, int s, int v)
{
	int *cur = s == PL_SLIDER_BACKLIGHT ? &p->st.bl_pct : &p->st.vol_pct;

	if (*cur == v)
		return;
	*cur = v;
	mark_value(p, s);
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

/* ---- the pop-out row ---- */

/* The timer exists to close the row: armed while it is open only. A touch
 * moves the deadline in pl_popup and not the timer; when the timer fires it is
 * set again for what is left. */
static void popup_arm(struct panel *p, int ms)
{
	struct itimerspec its = { .it_value = { ms / 1000, (ms % 1000) * 1000000L } };

	if (!its.it_value.tv_sec && !its.it_value.tv_nsec)
		its.it_value.tv_nsec = 1;	/* all zero would disarm it */
	timerfd_settime(p->popup_fd, 0, &its, NULL);
}

static void popup_disarm(struct panel *p)
{
	struct itimerspec off = { { 0, 0 }, { 0, 0 } };

	timerfd_settime(p->popup_fd, 0, &off, NULL);
}

static void clock_update(struct panel *p);
static void battery_update(struct panel *p);

/* The widget a button is drawn by. */
static unsigned button_widget(int button)
{
	return button == PL_BTN_CLOCK ? W_CLOCK : button == PL_BTN_BATTERY ? W_BATTERY :
		button >= 0 && button < PL_SLIDERS ? W_BACKLIGHT << button : 0;
}

/* The open row went from prev to p->pop.open: the buttons and the row change,
 * and the surface is asked for its new size on the next flush. */
static void popup_changed(struct panel *p, int prev)
{
	int now = p->pop.open;

	p->dirty |= button_widget(prev) | button_widget(now);
	if (now == PL_BTN_CLOCK)
		clock_update(p);
	/* The row opens on a fresh sample, so that it does not show what was
	 * true up to 30 s ago, and the sample counts for the rate. */
	if (now == PL_BTN_BATTERY)
		battery_update(p);
	if (now != PL_SLIDER_NONE)
		p->dirty |= W_ROW;
	if (now == PL_SLIDER_NONE)
		popup_disarm(p);
	else if (prev == PL_SLIDER_NONE)
		popup_arm(p, PL_POPUP_MS);
}

static void popup_close(struct panel *p)
{
	int prev = p->pop.open;

	if (prev == PL_SLIDER_NONE)
		return;
	p->pop.open = PL_SLIDER_NONE;
	popup_changed(p, prev);
}

static void popup_timer(struct panel *p)
{
	int prev = p->pop.open;
	int64_t now = now_ms();

	if (pl_popup_expire(&p->pop, now, p->touch.down)) {
		popup_changed(p, prev);
		return;
	}
	int w = pl_popup_wait_ms(&p->pop, now, p->touch.down);
	if (w > 0)
		popup_arm(p, w);
}

static void touch_press(struct panel *p)
{
	bool en[PL_BUTTONS] = { p->st.bl_pct >= 0, p->st.vol_pct >= 0, true, true };
	struct pl_touch_out o;

	/* set_slider ignores a value equal to the shown one, which must not be
	 * a stale one. */
	backlight_refresh(p);
	pl_touch_press(&p->touch, &p->layout, p->pop.open, en, p->px, p->py, &o);
	pl_popup_touch(&p->pop, now_ms());
	if (o.tap != PL_SLIDER_NONE) {
		int prev = p->pop.open;
		pl_popup_tap(&p->pop, o.tap, now_ms());
		popup_changed(p, prev);
	}
	if (o.slider != PL_SLIDER_NONE)
		set_slider(p, o.slider, o.value);
}

static void touch_motion(struct panel *p)
{
	struct pl_touch_out o;

	pl_touch_motion(&p->touch, &p->layout, p->px, &o);
	if (o.slider == PL_SLIDER_NONE)
		return;
	pl_popup_touch(&p->pop, now_ms());
	set_slider(p, o.slider, o.value);
}

static void touch_release(struct panel *p)
{
	pl_touch_release(&p->touch);
	pl_popup_touch(&p->pop, now_ms());
	apply_pending(p, true);
}

/* ---- wl_pointer ---- */

/* The surface reports positions in its own coordinates, the layout works in
 * the buffer's: the same unless the bar is a strip. */
static void set_pointer(struct panel *p, wl_fixed_t x, wl_fixed_t y)
{
	pl_point_to_buffer(p->buf_strip, wl_fixed_to_int(x), wl_fixed_to_int(y),
		p->canvas.w, p->canvas.h, &p->px, &p->py);
}

static void ptr_enter(void *d, struct wl_pointer *w, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	struct panel *p = d;
	(void)w; (void)serial; (void)s;
	set_pointer(p, x, y);
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
	set_pointer(p, x, y);
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

/* The output the bar is on; without a surface enter (yet) the first one
 * stands in. */
static const struct out_info *bar_output(const struct panel *p)
{
	const struct out_info *oi = NULL;

	for (int i = 0; i < p->n_outputs && i < MAX_OUTPUTS; i++)
		if (p->outs[i].wl && (!oi || p->outs[i].wl == p->surf_out))
			oi = &p->outs[i];
	return oi;
}

/* The text mode follows the output the bar is on: its subpixel layout and
 * transform come in wl_output.geometry, which is sent again when the output is
 * rotated. So does the edge: the strip on a short side is for the outputs the
 * compositor says are rotated by 90 or 270 degrees. An output with hardware
 * rotation says it is not (picowl sends the transform normal then, the
 * display does the turning), so there the bar stays on the top. A change of
 * the edge is asked for from the loop, after the pending configure. */
static void sub_update(struct panel *p)
{
	const struct out_info *oi = bar_output(p);

	p->strip = oi ? pl_strip_for(oi->transform, p->edge_top) : PL_STRIP_NONE;
	/* A strip is drawn in the panel's own orientation, so its stripes run along
	 * the bar whatever the output's transform is. */
	enum pl_sub s = oi ? pl_subpixel_resolve(p->subopt, oi->subpixel,
			p->strip != PL_STRIP_NONE ? 0 : oi->transform) :
		pl_subpixel_resolve(p->subopt, 0, 0);

	if (s == p->sub)
		return;
	p->sub = s;
	/* Crisp text has no colour fringes whatever the output says. */
	if (p->assets_ready && !p->crisp) {
		p->assets.sub = s;
		p->dirty |= W_CLOCK | W_BATTERY | W_ROW;
	}
}

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
	int32_t ph, int32_t subpixel, const char *make, const char *model, int32_t transform)
{
	struct panel *p = d;
	(void)x; (void)y; (void)pw; (void)ph; (void)make; (void)model;

	for (int i = 0; i < p->n_outputs && i < MAX_OUTPUTS; i++)
		if (p->outs[i].wl == o) {
			p->outs[i].subpixel = subpixel;
			p->outs[i].transform = transform;
		}
	sub_update(p);
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{
	(void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }
static const struct wl_output_listener out_listener = {
	.geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale,
};

static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o)
{
	struct panel *p = d;
	(void)s;
	p->surf_out = o;
	sub_update(p);
}
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o)
{
	(void)d; (void)s; (void)o;
}
static void surf_noop2(void *d, struct wl_surface *s, int32_t v) { (void)d; (void)s; (void)v; }
static void surf_noop3(void *d, struct wl_surface *s, uint32_t v) { (void)d; (void)s; (void)v; }
static const struct wl_surface_listener surf_listener = {
	.enter = surf_enter, .leave = surf_leave,
	.preferred_buffer_scale = surf_noop2, .preferred_buffer_transform = surf_noop3,
};

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
		if (p->n_outputs < MAX_OUTPUTS) {
			struct wl_output *o = wl_registry_bind(r, name, &wl_output_interface,
				ver < 2 ? ver : 2);
			p->outs[p->n_outputs].wl = o;
			wl_output_add_listener(o, &out_listener, p);
		}
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
	/* The date is kept up to date by the same tick, so that an open date row
	 * follows midnight; with the row closed it is just a few integers. */
	if (tm.tm_year + 1900 != p->st.year || tm.tm_mon != p->st.mon ||
			tm.tm_mday != p->st.mday || tm.tm_wday != p->st.wday) {
		p->st.year = tm.tm_year + 1900;
		p->st.mon = tm.tm_mon;
		p->st.mday = tm.tm_mday;
		p->st.wday = tm.tm_wday;
		if (p->pop.open == PL_BTN_CLOCK)
			p->dirty |= W_ROW;
	}
}

static void battery_update(struct panel *p)
{
	struct pl_batt_raw raw;

	pl_battery_read_raw(NULL, &raw);
	pl_est_add(&p->filter, &raw, now_ms());
	struct pl_estimate e = pl_est_compute(&p->filter, &raw, p->battery_mah);

	/* While discharging the minutes move only when the estimate has really
	 * moved, so that a row that is open does not flicker. */
	if (e.kind == PL_EST_LEFT && !e.hint && p->st.est.kind == PL_EST_LEFT &&
			!p->st.est.hint)
		e.minutes = pl_est_hysteresis(p->st.est.minutes, e.minutes);
	if (raw.st != p->st.bat || raw.pct != p->st.bat_pct) {
		p->st.bat = raw.st;
		p->st.bat_pct = raw.pct;
		p->dirty |= W_BATTERY;
	}
	p->st.est = e;
	char text[48];
	pl_est_text(text, sizeof(text), &e, raw.pct, 0);
	if (strcmp(text, p->est_text)) {
		snprintf(p->est_text, sizeof(p->est_text), "%s", text);
		if (p->pop.open == PL_BTN_BATTERY)
			p->dirty |= W_ROW;
	}
}

/* ---- test hooks ---- */

static bool loop_once(struct panel *p, int max_ms);
static bool handle_buffer(struct panel *p);

/* A request for a new surface size is answered by a configure, and the row is
 * drawn when the buffer for it is: let that happen before the next event. */
static void settle(struct panel *p)
{
	for (int i = 0; i < 4 && !p->quit; i++) {
		if (wl_display_roundtrip(p->dpy) < 0) {
			p->quit = true;
			return;
		}
		if (!p->need_buffer)
			return;
		if (!handle_buffer(p))
			return;
		flush_redraw(p);
	}
}

static void wait_ms(struct panel *p, int ms)
{
	int64_t end = now_ms() + ms;

	while (!p->quit) {
		int64_t left = end - now_ms();
		if (left <= 0 || !loop_once(p, (int)left))
			break;
	}
}

static void tap_button(struct panel *p, int button)
{
	const struct pl_rect *b = &p->layout.button[button];

	p->px = b->x + b->w / 2;
	p->py = b->y + b->h / 2;
	touch_press(p);
	flush_redraw(p);
	if (p->touch.down)
		touch_release(p);
}

/* "p X,Y" press, "m X,Y" motion, "e X,Y" pointer enter, "r" release,
 * "ibl", "ivol", "icl" and "ibat" tap the backlight, volume, clock or battery
 * button, "w MS" run the
 * event loop for MS milliseconds (the auto-close, for one), separated by ; or
 * space: the same handlers as the wl_pointer events, for tests without a
 * pointer. "b RAW" writes the backlight as another process would. */
static void run_inject(struct panel *p)
{
	char buf[MAX_INJECT * 12];
	char *save = NULL;

	snprintf(buf, sizeof(buf), "%s", p->inject);
	for (char *tok = strtok_r(buf, "; ", &save); tok && !p->quit;
			tok = strtok_r(NULL, "; ", &save)) {
		int x, y;
		if (tok[0] == 'e' && sscanf(tok + 1, "%d,%d", &x, &y) == 2) {
			p->px = x;
			p->py = y;
			backlight_refresh(p);
		} else if (tok[0] == 'b' && sscanf(tok + 1, "%d", &x) == 1) {
			/* Another process sets the backlight. */
			pl_backlight_write(&p->bl, x);
		} else if (!strcmp(tok, "ibl")) {
			tap_button(p, PL_SLIDER_BACKLIGHT);
		} else if (!strcmp(tok, "ivol")) {
			tap_button(p, PL_SLIDER_VOLUME);
		} else if (!strcmp(tok, "icl")) {
			tap_button(p, PL_BTN_CLOCK);
		} else if (!strcmp(tok, "ibat")) {
			tap_button(p, PL_BTN_BATTERY);
		} else if (tok[0] == 'w' && sscanf(tok + 1, "%d", &x) == 1) {
			flush_redraw(p);
			wait_ms(p, x);
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
		settle(p);
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

static const char *slider_name(int s)
{
	return s == PL_SLIDER_BACKLIGHT ? "backlight" : s == PL_SLIDER_VOLUME ? "volume" :
		s == PL_BTN_CLOCK ? "clock" : s == PL_BTN_BATTERY ? "battery" : "none";
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
	printf("panel width=%d height=%d bar=%d row=%d format=%s anchor=%s popup=%s\n",
		l->w, l->h, l->bar_h, l->row_shown ? l->row_h : 0,
		p->canvas.fmt == PL_FMT_RGB565 ? "RGB565" :
		p->canvas.fmt == PL_FMT_ARGB8888 ? "ARGB8888" : "XRGB8888",
		p->bottom ? "bottom" : "top", slider_name(p->pop.open));
	printf("style %s=%s%s%s size=%d small=%d bar_alpha=%d popup_alpha=%d%s\n",
		p->crisp ? "crisp font" : "font",
		p->crisp ? "pixel" : p->font_ttf ? "ttf" : "bitmap", p->font_ttf ? ":" : "",
		p->font_ttf ? p->assets.font.path : "",
		pl_font_face_px(&p->assets.font, 0), pl_font_face_px(&p->assets.font, 1),
		p->bar_alpha, p->popup_alpha,
		!p->crisp ? "" : p->crisp_font == PL_CRISP_DEJAVU ? " crisp_font=dejavu" :
		" crisp_font=fixed");
	printf("text subpixel=%s\n", p->assets.sub == PL_SUB_RGB ? "rgb" :
		p->assets.sub == PL_SUB_BGR ? "bgr" : "none");
	/* What the compositor is told: the exclusive zone is the bar and never
	 * the row; the input region is the bar and the row. */
	struct pl_rect in = pl_input_rect(l, p->canvas.w, p->canvas.h);
	printf("surface exclusive=%d", l->bar_h);
	print_rect("input", in);
	printf("\n");
	/* Where the surface is for the compositor: the edge of the view, the buffer
	 * transform, and the surface and the input region in surface coordinates
	 * (the other lines are in the buffer's own frame). */
	static const char *const edges[] = { "top", "bottom", "left", "right" };
	int sw = p->canvas.w, sh = p->canvas.h;
	if (p->buf_strip != PL_STRIP_NONE) {
		sw = p->canvas.h;
		sh = p->canvas.w;
	}
	printf("placement edge=%s transform=%d surface=%dx%d",
		edges[pl_edge_for(p->buf_strip, p->bottom)], (int)p->buf_strip, sw, sh);
	print_rect("input", pl_rect_to_surface(p->buf_strip, in, p->canvas.w, p->canvas.h));
	printf("\n");
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
		printf("%s available=%d value=%d", slider_name(s), v >= 0, v);
		if (s == PL_SLIDER_BACKLIGHT && p->have_bl)
			printf(" raw=%d max=%d", pl_backlight_read(&p->bl), p->bl.max);
		print_rect("button", l->button[s]);
		printf("\n");
	}
	static const char *const est_names[] = { "none", "ac", "full", "notcharging", "charging",
		"estimating", "left", "tofull", "over_left", "over_full" };
	printf("estimate kind=%s minutes=%d hint=%d samples=%d\n", est_names[p->st.est.kind],
		p->st.est.minutes, p->st.est.hint, p->filter.n);
	/* The touch targets and the highlights of the open buttons. */
	for (int i = 0; i < PL_BUTTONS; i++) {
		printf("target %s", slider_name(i));
		print_rect("rect", l->button[i]);
		print_rect("hl", l->hl[i]);
		printf(" open=%d\n", p->pop.open == i);
	}
	if (l->row_shown && pl_row_kind_of(p->pop.open) == PL_ROW_SLIDER) {
		int v = p->pop.open == PL_SLIDER_BACKLIGHT ? p->st.bl_pct : p->st.vol_pct;
		printf("row slider=%s", slider_name(p->pop.open));
		print_rect("rect", l->row);
		print_rect("cell", l->slider.cell);
		print_rect("track", l->slider.track);
		print_rect("thumb", pl_slider_thumb(l, v));
		printf(" kind=slider\n");
	} else if (l->row_shown && p->pop.open != PL_SLIDER_NONE) {
		/* A line of text: what is drawn, in which size, and the room it
		 * has, so that a test can see that it fits. */
		struct pl_rowtext t;
		pl_row_text(l, &p->assets, &p->st, p->pop.open, &t);
		printf("row kind=%s", pl_row_kind_of(p->pop.open) == PL_ROW_DATE ? "date" : "estimate");
		print_rect("rect", l->row);
		printf(" size=%d width=%d avail=%d text=\"%s\"\n",
			pl_font_face_px(&p->assets.font, t.face), t.w, l->text.w, t.text);
	}
	fflush(stdout);
}

/* ---- main ---- */

static void usage(FILE *out)
{
	fprintf(out,
		"usage: picowl-panel [options]\n"
		"  --height N          height of the bar in pixels, %d..%d (default %d)\n"
		"  --bottom            anchor at the bottom edge (default: top)\n"
		"  --edge MODE         auto (default) or top. On an output that is rotated by 90\n"
		"                      or 270 degrees auto puts the bar on the short side, the\n"
		"                      edge that is the physical top of a portrait panel (the\n"
		"                      physical bottom with --bottom), drawn as in portrait and\n"
		"                      turned by the buffer transform; top keeps it on the top\n"
		"                      (bottom) edge of the rotated view. Other transforms are\n"
		"                      not affected\n"
		"  --style STYLE       smooth (default) or crisp: crisp draws nothing\n"
		"                      anti-aliased, with built-in pixel fonts and icons on\n"
		"                      whole pixels, and ignores --font, --font-size and\n"
		"                      --subpixel\n"
		"  --crisp-font F      fixed (default) or dejavu: the pixel font of the crisp\n"
		"                      style. fixed is the monospaced X11 misc-fixed, dejavu\n"
		"                      is DejaVu Sans hinted to bi-level, proportional\n"
		"  --font PATH         TrueType font file (default: the first of the\n"
		"                      Liberation Sans and DejaVu Sans files that exist);\n"
		"                      without a usable one the built-in bitmap font is used\n"
		"  --font-size PX      size of the text in pixels, 8..48 (default: %d at the\n"
		"                      default height)\n"
		"  --bar-alpha N       opacity of the bar, 0..255 (default %d)\n"
		"  --popup-alpha N     opacity of the pop-out row, 0..255 (default %d);\n"
		"                      below 255 the surface needs ARGB8888 while the row is open\n"
		"  --subpixel MODE     auto, rgb, bgr or none (default auto): the order of the\n"
		"                      colour stripes for subpixel text. auto uses what the\n"
		"                      compositor says about the output, and only when the\n"
		"                      stripes run across the bar; rgb and bgr force one. Text\n"
		"                      on a translucent ground is always grayscale\n"
		"  --battery-mah N     capacity of the battery in mAh, 0..100000 (default 0, unknown),\n"
		"                      the last resort for the time estimate when the battery\n"
		"                      reports no charge_full or charge_full_design\n"
		"  --dump-state        print the widget values and geometry after the first\n"
		"                      frame and exit (for tests)\n"
		"  --watch             test only: like --dump-state, and print the state again\n"
		"                      after every redraw, naming the widgets, without exiting\n"
		"  --exit-after-frame  exit after the first frame\n"
		"  --inject SPEC       test only: feed pointer events after the first frame\n"
		"                      (p X,Y press; m X,Y motion; e X,Y enter; r release;\n"
		"                      ibl, ivol, icl, ibat tap the backlight, volume, clock\n"
		"                      or battery button; w MS wait; b RAW sets the\n"
		"                      backlight), before the dump\n"
		"  --help              this text\n",
		PL_HEIGHT_MIN, PL_HEIGHT_MAX, PL_HEIGHT_DEFAULT,
		pl_default_font_px(PL_HEIGHT_DEFAULT), PL_ALPHA_BAR_DEFAULT,
		PL_ALPHA_POPUP_DEFAULT);
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
		} else if ((!strcmp(a, "--height") || !strcmp(a, "--font-size") ||
				!strcmp(a, "--bar-alpha") || !strcmp(a, "--popup-alpha")) &&
				i + 1 < argc) {
			if (!parse_int(argv[++i], &v)) {
				say("%s: '%s' is not a number", a, argv[i]);
				return -1;
			}
			if (!strcmp(a, "--height"))
				p->height = pl_clamp_height(v);
			else if (!strcmp(a, "--font-size"))
				p->font_px = v < 8 ? 8 : v > 48 ? 48 : v;
			else if (!strcmp(a, "--bar-alpha"))
				p->bar_alpha = pl_clamp_alpha(v);
			else
				p->popup_alpha = pl_clamp_alpha(v);
		} else if (!strcmp(a, "--battery-mah") && i + 1 < argc) {
			if (!parse_int(argv[++i], &v)) {
				say("%s: '%s' is not a number", a, argv[i]);
				return -1;
			}
			p->battery_mah = v < 0 ? 0 : v > 100000 ? 100000 : v;
		} else if (!strcmp(a, "--subpixel") && i + 1 < argc) {
			const char *v = argv[++i];
			if (!strcmp(v, "auto"))
				p->subopt = PL_SUBOPT_AUTO;
			else if (!strcmp(v, "rgb"))
				p->subopt = PL_SUBOPT_RGB;
			else if (!strcmp(v, "bgr"))
				p->subopt = PL_SUBOPT_BGR;
			else if (!strcmp(v, "none"))
				p->subopt = PL_SUBOPT_NONE;
			else {
				say("--subpixel: '%s' is not auto, rgb, bgr or none", v);
				return -1;
			}
		} else if (!strcmp(a, "--edge") && i + 1 < argc) {
			const char *v = argv[++i];
			if (!strcmp(v, "auto")) {
				p->edge_top = false;
			} else if (!strcmp(v, "top")) {
				p->edge_top = true;
			} else {
				say("--edge: '%s' is not auto or top", v);
				return -1;
			}
		} else if (!strcmp(a, "--style") && i + 1 < argc) {
			const char *v = argv[++i];
			if (!strcmp(v, "smooth")) {
				p->crisp = false;
			} else if (!strcmp(v, "crisp")) {
				p->crisp = true;
			} else {
				say("--style: '%s' is not smooth or crisp", v);
				return -1;
			}
		} else if (!strcmp(a, "--crisp-font") && i + 1 < argc) {
			const char *v = argv[++i];
			if (!strcmp(v, "fixed")) {
				p->crisp_font = PL_CRISP_FIXED;
			} else if (!strcmp(v, "dejavu")) {
				p->crisp_font = PL_CRISP_DEJAVU;
			} else {
				say("--crisp-font: '%s' is not fixed or dejavu", v);
				return -1;
			}
		} else if (!strcmp(a, "--font") && i + 1 < argc) {
			p->font_path = argv[++i];
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

/* A configure has been answered: draw for its size. */
static bool handle_buffer(struct panel *p)
{
	p->need_buffer = false;
	/* A configure that was sent before the compositor saw the new edge has
	 * the other orientation: it is answered, not drawn for, and the one for
	 * the new request follows. */
	if (p->cfg_w > 0 && p->cfg_h > 0 &&
			(p->strip != PL_STRIP_NONE ? p->cfg_w > p->cfg_h : p->cfg_w < p->cfg_h) &&
			p->strip != p->buf_strip)
		return true;
	int sw = p->cfg_w > 0 ? p->cfg_w :
		p->strip != PL_STRIP_NONE ? pl_surface_height(p->height, false) : 240;
	int sh = p->cfg_h > 0 ? p->cfg_h :
		p->strip != PL_STRIP_NONE ? 240 : pl_surface_height(p->height, false);
	int w, h;

	pl_strip_buffer_size(p->strip, sw, sh, &w, &h);
	bool turned = p->buf_strip != p->strip;
	p->buf_strip = p->strip;
	if (p->buffer && w == p->canvas.w && h == p->canvas.h) {
		/* Same size again: nothing to draw, but the configure is
		 * answered by a commit. A bar that moved to another edge has the
		 * same buffer and other regions: they are in surface coordinates. */
		if (turned) {
			set_regions(p, w, h);
			if (p->watch)
				dump_state(p, "edge");
		}
		commit(p);
		return true;
	}
	if (!rebuild(p, w, h)) {
		p->exit_code = 1;
		p->quit = true;
		return false;
	}
	if (p->first) {
		p->first = false;
		first_frame(p);
	} else if (p->watch) {
		dump_state(p, "resize");
	}
	return true;
}

/* One turn of the loop, waiting at most max_ms (-1: as long as it takes).
 * False when the loop is over. */
static bool loop_once(struct panel *p, int max_ms)
{
	struct pollfd pfd[5 + 8];

	while (wl_display_prepare_read(p->dpy) != 0)
		if (wl_display_dispatch_pending(p->dpy) < 0) {
			display_gone(p);
			return false;
		}
	int fl = wl_display_flush(p->dpy);
	if (fl < 0 && errno != EAGAIN) {
		wl_display_cancel_read(p->dpy);
		display_gone(p);
		return false;
	}

	int n = 0;
	pfd[n++] = (struct pollfd){ wl_display_get_fd(p->dpy),
		POLLIN | (fl < 0 ? POLLOUT : 0), 0 };
	pfd[n++] = (struct pollfd){ p->sig_fd, POLLIN, 0 };
	pfd[n++] = (struct pollfd){ p->clock_fd, POLLIN, 0 };
	pfd[n++] = (struct pollfd){ p->battery_fd, POLLIN, 0 };
	pfd[n++] = (struct pollfd){ p->popup_fd, POLLIN, 0 };
	int mixer_at = n;
	for (int i = 0; i < p->n_mixer_fds; i++)
		pfd[n++] = (struct pollfd){ p->mixer_fds[i].fd, p->mixer_fds[i].events, 0 };

	int timeout = apply_timeout_ms(p);
	if (max_ms >= 0 && (timeout < 0 || max_ms < timeout))
		timeout = max_ms;
	int r = poll(pfd, n, timeout);
	if (r < 0) {
		wl_display_cancel_read(p->dpy);
		if (errno == EINTR)
			return true;
		say("poll: %s", strerror(errno));
		p->exit_code = 1;
		return false;
	}
	if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
		if (wl_display_read_events(p->dpy) < 0) {
			display_gone(p);
			return false;
		}
	} else {
		wl_display_cancel_read(p->dpy);
	}
	if (wl_display_dispatch_pending(p->dpy) < 0) {
		display_gone(p);
		return false;
	}

	if (pfd[1].revents & POLLIN) {
		struct signalfd_siginfo si;
		if (read(p->sig_fd, &si, sizeof(si)) > 0) {
			p->quit = true;
			return false;
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
	if (pfd[4].revents & POLLIN) {
		uint64_t exp;
		if (read(p->popup_fd, &exp, sizeof(exp)) > 0)
			popup_timer(p);
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
		return false;
	}
	if (p->need_buffer && !handle_buffer(p))
		return false;
	apply_pending(p, false);
	flush_redraw(p);
	return !p->quit;
}

static void run_loop(struct panel *p)
{
	while (!p->quit && loop_once(p, -1))
		;
}

static void run(struct panel *p)
{
	run_loop(p);
	/* Every way out, not only a signal: a drag cut short still leaves the
	 * last value applied. */
	apply_pending(p, true);
}

int main(int argc, char **argv)
{
	struct panel p = {
		.height = PL_HEIGHT_DEFAULT, .first = true,
		.bar_alpha = PL_ALPHA_BAR_DEFAULT, .popup_alpha = PL_ALPHA_POPUP_DEFAULT,
		.last_apply = { -1, -1 }, .touch = { .slider = PL_SLIDER_NONE },
		.pop = { .open = PL_SLIDER_NONE },
		.st = { .bat = PL_BAT_NONE, .bat_pct = -1, .bl_pct = -1, .vol_pct = -1 },
		.dirty = 0, .clock_fd = -1, .battery_fd = -1, .popup_fd = -1, .sig_fd = -1,
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
	p.popup_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (p.sig_fd < 0 || p.clock_fd < 0 || p.battery_fd < 0 || p.popup_fd < 0) {
		say("cannot create timers: %s", strerror(errno));
		return 1;
	}

	/* The font is read once, here: glyphs are rasterized into memory and the
	 * file is let go. Without one the panel still works, in the bitmap font. */
	if (p.crisp) {
		pl_assets_init_crisp(&p.assets, p.height, p.bar_alpha, p.popup_alpha, p.crisp_font);
	} else {
		int px = p.font_px ? p.font_px : pl_default_font_px(p.height);
		p.font_ttf = pl_assets_init(&p.assets, p.font_path, px, p.bar_alpha, p.popup_alpha);
		if (!p.font_ttf && p.font_path)
			say("cannot use the font '%s', using the built-in bitmap font", p.font_path);
		else if (!p.font_ttf)
			say("no usable font file found, using the built-in bitmap font");
		p.assets.sub = p.sub;
	}
	p.assets_ready = true;

	p.st.hour = p.st.min = -1;
	p.st.year = p.st.mon = p.st.mday = p.st.wday = -1;
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
	wl_surface_add_listener(p.surface, &surf_listener, &p);
	zwlr_layer_surface_v1_add_listener(p.ls, &ls_listener, &p);
	request_surface(&p, p.height);
	/* The bar's strip only: the slider row is taller than that and covers
	 * the windows instead of pushing them. */
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
	pl_assets_free(&p.assets);
	wl_display_disconnect(p.dpy);
	return p.exit_code;
}
