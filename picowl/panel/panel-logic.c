/* panel-logic.c - the pure part of picowl-panel. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "panel-logic.h"

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* ---- value mapping ---- */

int pl_bl_clamp_pct(int pct)
{
	return clampi(pct, PL_BL_FLOOR_PCT, 100);
}

int pl_bl_pct_from_raw(int raw, int max)
{
	if (max <= 0)
		return 0;
	raw = clampi(raw, 0, max);
	return (int)(((long long)raw * 100 + max / 2) / max);
}

int pl_bl_raw_from_pct(int pct, int max)
{
	if (max <= 0)
		return 0;
	int raw = (int)(((long long)pl_bl_clamp_pct(pct) * max + 50) / 100);
	return clampi(raw, 1, max);
}

long pl_vol_raw_from_pct(int pct, long min, long max)
{
	long range = max - min;
	if (range <= 0)
		return min;
	return min + (range * clampi(pct, 0, 100) + 50) / 100;
}

int pl_vol_pct_from_raw(long raw, long min, long max)
{
	long range = max - min;
	if (range <= 0)
		return 0;
	if (raw <= min)
		return 0;
	if (raw >= max)
		return 100;
	return (int)(((raw - min) * 100 + range / 2) / range);
}

void pl_clock_text(char *buf, size_t len, int hour, int min)
{
	snprintf(buf, len, "%02d:%02d", clampi(hour, 0, 23), clampi(min, 0, 59));
}

void pl_battery_text(char *buf, size_t len, enum pl_bat_status st, int pct)
{
	switch (st) {
	case PL_BAT_DISCHARGING:
	case PL_BAT_CHARGING:
	case PL_BAT_FULL:
		snprintf(buf, len, "%d%%", clampi(pct, 0, 100));
		break;
	case PL_BAT_AC:
		snprintf(buf, len, "AC");
		break;
	default:
		snprintf(buf, len, "--");
		break;
	}
}

void pl_pct_text(char *buf, size_t len, int pct)
{
	snprintf(buf, len, "%d%%", clampi(pct, 0, 100));
}

static const char *const weekdays[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
	"Thursday", "Friday", "Saturday" };
static const char *const months[12] = { "January", "February", "March", "April", "May",
	"June", "July", "August", "September", "October", "November", "December" };

void pl_date_text(char *buf, size_t len, int year, int mon, int mday, int wday)
{
	snprintf(buf, len, "%s %d %s %d", weekdays[clampi(wday, 0, 6)], clampi(mday, 1, 31),
		months[clampi(mon, 0, 11)], clampi(year, 1, 9999));
}

enum pl_row_kind pl_row_kind_of(int open)
{
	if (open == PL_SLIDER_BACKLIGHT || open == PL_SLIDER_VOLUME)
		return PL_ROW_SLIDER;
	if (open == PL_BTN_CLOCK)
		return PL_ROW_DATE;
	if (open == PL_BTN_BATTERY)
		return PL_ROW_ESTIMATE;
	return PL_ROW_NONE;
}

int pl_fit_choose(const char *const *cand, int ncand, int nfaces, int avail,
	pl_width_fn width, void *ctx, int *face)
{
	for (int f = 0; f < nfaces; f++)
		for (int c = 0; c < ncand; c++)
			if (width(ctx, f, cand[c]) <= avail) {
				*face = f;
				return c;
			}
	*face = nfaces > 0 ? nfaces - 1 : 0;
	return ncand > 0 ? ncand - 1 : 0;
}

/* ---- layout ---- */

int pl_clamp_height(int h)
{
	return clampi(h, PL_HEIGHT_MIN, PL_HEIGHT_MAX);
}

int pl_clamp_alpha(int a)
{
	return clampi(a, 0, 255);
}

int pl_default_font_px(int bar_h)
{
	return clampi((pl_clamp_height(bar_h) * 12 + 10) / 20, 8, 40);
}

int pl_row_height(int bar_h)
{
	int h = pl_clamp_height(bar_h) + 8;
	return h < 36 ? 36 : h;
}

int pl_surface_height(int bar_h, bool row)
{
	bar_h = pl_clamp_height(bar_h);
	return bar_h + (row ? pl_row_height(bar_h) : 0);
}

void pl_layout_compute(struct pl_layout *l, int w, int bar_h, bool row_shown,
	bool bottom, const struct pl_metrics *m)
{
	memset(l, 0, sizeof(*l));
	bar_h = pl_clamp_height(bar_h);
	if (w < 2)
		w = 2;
	l->w = w;
	l->bar_h = bar_h;
	l->row_h = pl_row_height(bar_h);
	l->row_shown = row_shown;
	l->bottom = bottom;
	l->h = bar_h + (row_shown ? l->row_h : 0);
	int dh = bar_h - 1;	/* a row of the bar is its line */
	/* The line is on the side of the windows: at the bottom of a top bar,
	 * at the top of a bottom one. */
	int by = bottom && row_shown ? l->row_h : 0;
	int iy = by + (bottom ? 1 : 0);
	l->bar_y = by;
	l->bar_line_y = bottom ? by : by + dh;

	l->clock = (struct pl_rect){ 0, iy, PL_MARGIN + m->clock_w + PL_GAP, dh };

	l->bat_icon_w = clampi(bar_h * 9 / 10, 16, 30);
	l->bat_icon_h = l->bat_icon_w / 2;
	int group_w = l->bat_icon_w + PL_GAP + m->bat_text_w;
	/* The battery is a button too, so its rectangle leaves room for the
	 * highlight around the icon and the text. */
	l->battery = (struct pl_rect){ w - PL_MARGIN - group_w - PL_BAT_PAD, iy,
		group_w + PL_MARGIN + PL_BAT_PAD, dh };

	/* A stylus needs 36 px, the bar is slimmer than that. */
	int bw = clampi(bar_h, 36, 44);
	int hh = dh - 2 < 32 ? dh - 2 : 32, hw = bw - 8;
	int x = l->battery.x - PL_GAP;
	for (int i = PL_SLIDERS - 1; i >= 0; i--) {
		x -= bw;
		l->button[i] = (struct pl_rect){ x, by, bw, bar_h };
		l->hl[i] = (struct pl_rect){ x + (bw - hw) / 2, iy + (dh - hh) / 2, hw, hh };
	}
	l->bar_icon = clampi(bar_h * 14 / 20, 12, 28);

	/* The clock and the battery: the touch target is the text widened to at
	 * least 36 px and as high as the bar; the highlight is a pill around the
	 * text with 4 px to spare, and for the battery around the widest it gets. */
	int cw = l->clock.w < 36 ? 36 : l->clock.w;
	l->button[PL_BTN_CLOCK] = (struct pl_rect){ 0, by, cw, bar_h };
	l->hl[PL_BTN_CLOCK] = (struct pl_rect){ PL_MARGIN - PL_HL_PAD, iy + (dh - hh) / 2,
		m->clock_w + 2 * PL_HL_PAD, hh };
	int bx = l->battery.x, bww = w - bx;
	if (bww < 36) {
		bx = w - 36;
		bww = 36;
	}
	l->button[PL_BTN_BATTERY] = (struct pl_rect){ bx, by, bww, bar_h };
	l->hl[PL_BTN_BATTERY] = (struct pl_rect){ l->battery.x + PL_BAT_PAD - PL_HL_PAD,
		iy + (dh - hh) / 2, group_w + 2 * PL_HL_PAD, hh };

	int ry = bottom ? 0 : bar_h;
	l->row = (struct pl_rect){ 0, ry, w, l->row_h };
	l->row_in = (struct pl_rect){ 0, ry + (bottom ? 1 : 0), w, l->row_h - 1 };
	l->row_line_y = bottom ? ry : ry + l->row_h - 1;
	int tw_ = w - 2 * PL_MARGIN;
	l->text = (struct pl_rect){ PL_MARGIN, l->row_in.y, tw_ < 1 ? 1 : tw_, l->row_h - 1 };
	int ri = clampi(l->row_h * 2 / 3, 20, 28);
	l->row_icon = (struct pl_rect){ PL_MARGIN, l->row_in.y + (l->row_h - 1 - ri) / 2, ri, ri };
	l->pct = (struct pl_rect){ w - PL_MARGIN - m->pct_w, l->row_in.y, m->pct_w + PL_MARGIN,
		l->row_h - 1 };

	struct pl_slider *s = &l->slider;
	int tx = PL_MARGIN + ri + 8;
	int tw = w - PL_MARGIN - m->pct_w - 8 - tx;
	if (tw < 4)
		tw = 4;
	s->thumb_d = clampi(l->row_h * 22 / 36, 16, 26);
	if (s->thumb_d > tw / 2)
		s->thumb_d = tw / 2 < 2 ? 2 : tw / 2;
	s->track_h = clampi(l->row_h / 6, 4, 8);
	if (m->crisp) {
		if (s->thumb_d > 1 && !(s->thumb_d & 1))
			s->thumb_d--;
		if (!(s->track_h & 1))
			s->track_h--;
	}
	s->track = (struct pl_rect){ tx, ry, tw, l->row_h };
	/* Wide enough for a stylus to land on, short of the icon and the text. */
	s->cell = (struct pl_rect){ tx - 6, ry, tw + 12, l->row_h };
}

enum pl_strip pl_strip_for(int transform, bool force_top)
{
	if (force_top)
		return PL_STRIP_NONE;
	return transform == PL_STRIP_90 ? PL_STRIP_90 :
		transform == PL_STRIP_270 ? PL_STRIP_270 : PL_STRIP_NONE;
}

void pl_rot_resolve(const struct pl_rot_hint *h, int geo_transform, int geo_subpixel,
	int *transform, int *native_subpixel)
{
	bool hw = h && h->present && h->hardware;

	*transform = hw ? h->transform : geo_transform;
	*native_subpixel = hw ? h->subpixel : geo_subpixel;
}

enum pl_edge pl_edge_for(enum pl_strip strip, bool bottom)
{
	switch (strip) {
	case PL_STRIP_90:
		return bottom ? PL_EDGE_LEFT : PL_EDGE_RIGHT;
	case PL_STRIP_270:
		return bottom ? PL_EDGE_RIGHT : PL_EDGE_LEFT;
	default:
		return bottom ? PL_EDGE_BOTTOM : PL_EDGE_TOP;
	}
}

void pl_strip_buffer_size(enum pl_strip strip, int sw, int sh, int *bw, int *bh)
{
	bool swap = strip != PL_STRIP_NONE;

	*bw = swap ? sh : sw;
	*bh = swap ? sw : sh;
}

/* At 90 the compositor shows the buffer turned clockwise: its top edge is the
 * right edge of the surface and its left edge the top one, which is what puts
 * the physical top of a panel whose output is rotated by 90 on the right. 270
 * is the other way round. (Measured with the scanout of the headless output;
 * the wording of the protocol suggests the opposite direction.) */
struct pl_rect pl_rect_to_surface(enum pl_strip strip, struct pl_rect r, int bw, int bh)
{
	switch (strip) {
	case PL_STRIP_90:
		return (struct pl_rect){ bh - r.y - r.h, r.x, r.h, r.w };
	case PL_STRIP_270:
		return (struct pl_rect){ r.y, bw - r.x - r.w, r.h, r.w };
	default:
		return r;
	}
}

void pl_point_to_buffer(enum pl_strip strip, int sx, int sy, int bw, int bh, int *x, int *y)
{
	switch (strip) {
	case PL_STRIP_90:
		*x = sy;
		*y = bh - 1 - sx;
		break;
	case PL_STRIP_270:
		*x = bw - 1 - sy;
		*y = sx;
		break;
	default:
		*x = sx;
		*y = sy;
	}
}

struct pl_rect pl_input_rect(const struct pl_layout *l, int sw, int sh)
{
	int h = l->bar_h + (l->row_shown ? l->row_h : 0);
	return (struct pl_rect){ 0, 0, sw < l->w ? sw : l->w, sh < h ? sh : h };
}

int pl_opaque_rects(const struct pl_layout *l, int bar_alpha, int popup_alpha,
	struct pl_rect out[2])
{
	int n = 0;

	if (bar_alpha >= 255)
		out[n++] = (struct pl_rect){ 0, l->bar_y, l->w, l->bar_h };
	if (l->row_shown && popup_alpha >= 255)
		out[n++] = l->row;
	return n;
}

enum pl_fmt pl_pick_format(int bar_alpha, int popup_alpha, bool row_shown,
	bool have_rgb565)
{
	if (bar_alpha < 255 || (row_shown && popup_alpha < 255))
		return PL_FMT_ARGB8888;
	return have_rgb565 ? PL_FMT_RGB565 : PL_FMT_XRGB8888;
}

static int travel(const struct pl_slider *s)
{
	int t = s->track.w - s->thumb_d;
	return t < 1 ? 1 : t;
}

struct pl_rect pl_slider_thumb(const struct pl_layout *l, int pct)
{
	const struct pl_slider *s = &l->slider;

	pct = clampi(pct, 0, 100);
	return (struct pl_rect){ s->track.x + (travel(s) * pct + 50) / 100,
		l->row_in.y + (l->row_h - 1 - s->thumb_d) / 2, s->thumb_d, s->thumb_d };
}

int pl_slider_value(const struct pl_slider *s, int x)
{
	int t = travel(s);
	int rel = x - s->track.x - s->thumb_d / 2;
	if (rel <= 0)
		return 0;
	if (rel >= t)
		return 100;
	return (rel * 100 + t / 2) / t;
}

static bool contains(const struct pl_rect *r, int x, int y)
{
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

struct pl_hit pl_hit_test(const struct pl_layout *l, int x, int y)
{
	for (int i = 0; i < PL_BUTTONS; i++)
		if (contains(&l->button[i], x, y))
			return (struct pl_hit){ PL_HIT_BUTTON, i };
	if (l->row_shown && contains(&l->slider.cell, x, y))
		return (struct pl_hit){ PL_HIT_TRACK, PL_SLIDER_NONE };
	return (struct pl_hit){ PL_HIT_NONE, PL_SLIDER_NONE };
}

/* ---- touch ---- */

static int slider_value(const struct pl_layout *l, int slider, int x)
{
	int v = pl_slider_value(&l->slider, x);
	return slider == PL_SLIDER_BACKLIGHT ? pl_bl_clamp_pct(v) : v;
}

void pl_touch_press(struct pl_touch *t, const struct pl_layout *l, int open,
	const bool enabled[PL_BUTTONS], int x, int y, struct pl_touch_out *out)
{
	t->down = true;
	t->slider = PL_SLIDER_NONE;
	*out = (struct pl_touch_out){ PL_SLIDER_NONE, PL_SLIDER_NONE, 0 };
	struct pl_hit h = pl_hit_test(l, x, y);

	if (h.kind == PL_HIT_BUTTON) {
		/* A button never sets a value: a stray tap on the sun would set
		 * the floor (a dark screen) and one on the speaker would mute. */
		if (enabled[h.slider])
			out->tap = h.slider;
	} else if (h.kind == PL_HIT_TRACK && pl_row_kind_of(open) == PL_ROW_SLIDER &&
			enabled[open]) {
		t->slider = open;
		out->slider = open;
		out->value = slider_value(l, open, x);
	}
}

void pl_touch_motion(struct pl_touch *t, const struct pl_layout *l, int x,
	struct pl_touch_out *out)
{
	*out = (struct pl_touch_out){ PL_SLIDER_NONE, PL_SLIDER_NONE, 0 };
	if (!t->down || t->slider == PL_SLIDER_NONE)
		return;
	out->slider = t->slider;
	out->value = slider_value(l, t->slider, x);
}

int pl_touch_release(struct pl_touch *t)
{
	int s = t->down ? t->slider : PL_SLIDER_NONE;
	t->down = false;
	t->slider = PL_SLIDER_NONE;
	return s;
}

int pl_apply_wait_ms(int64_t now_ms, int64_t last_ms)
{
	if (last_ms < 0)
		return 0;
	int64_t d = now_ms - last_ms;
	if (d < 0 || d >= PL_APPLY_INTERVAL_MS)
		return 0;
	return (int)(PL_APPLY_INTERVAL_MS - d);
}

/* ---- the pop-out row ---- */

int pl_popup_tap(struct pl_popup *p, int button, int64_t now_ms)
{
	p->open = p->open == button ? PL_SLIDER_NONE : button;
	p->last_ms = now_ms;
	return p->open;
}

void pl_popup_touch(struct pl_popup *p, int64_t now_ms)
{
	p->last_ms = now_ms;
}

int pl_popup_wait_ms(const struct pl_popup *p, int64_t now_ms, bool touching)
{
	if (p->open == PL_SLIDER_NONE)
		return -1;
	if (touching)
		return PL_POPUP_MS;
	int64_t left = p->last_ms + PL_POPUP_MS - now_ms;
	if (left <= 0)
		return 0;
	/* A clock that went back must not keep it open for ever. */
	return left > PL_POPUP_MS ? PL_POPUP_MS : (int)left;
}

bool pl_popup_expire(struct pl_popup *p, int64_t now_ms, bool touching)
{
	if (pl_popup_wait_ms(p, now_ms, touching) != 0)
		return false;
	p->open = PL_SLIDER_NONE;
	return true;
}

/* ---- the battery's time estimate ---- */

void pl_batt_raw_clear(struct pl_batt_raw *r)
{
	*r = (struct pl_batt_raw){ .st = PL_BAT_NONE, .pct = -1, .charger = -1,
		.charge_now = PL_ABSENT, .charge_full = PL_ABSENT,
		.charge_full_design = PL_ABSENT, .charge_empty = PL_ABSENT,
		.energy_now = PL_ABSENT, .energy_full = PL_ABSENT,
		.energy_full_design = PL_ABSENT, .current_now = PL_ABSENT,
		.current_avg = PL_ABSENT, .power_now = PL_ABSENT,
		.time_to_empty_now = PL_ABSENT };
}

enum { RATE_NONE, RATE_CURRENT, RATE_POWER };

static int64_t abs64(int64_t v)
{
	return v < 0 ? -v : v;
}

/* The rate attribute to use, in the order of preference. */
static int rate_of(const struct pl_batt_raw *r, int64_t *v)
{
	if (r->current_avg != PL_ABSENT) {
		*v = r->current_avg;
		return RATE_CURRENT;
	}
	if (r->current_now != PL_ABSENT) {
		*v = r->current_now;
		return RATE_CURRENT;
	}
	if (r->power_now != PL_ABSENT) {
		*v = r->power_now;
		return RATE_POWER;
	}
	return RATE_NONE;
}

enum pl_dir pl_batt_dir(const struct pl_batt_raw *r, int64_t smoothed)
{
	if (r->st == PL_BAT_NONE || r->st == PL_BAT_AC)
		return PL_DIR_NONE;
	if (r->st == PL_BAT_FULL)
		return PL_DIR_FULL;
	if (r->not_charging)
		return PL_DIR_NOTCHARGING;
	bool chg = r->st == PL_BAT_CHARGING;

	if (r->charger == 0)
		return PL_DIR_DISCHARGING;
	if (r->charger == 1) {
		if (smoothed != PL_ABSENT && smoothed > PL_EST_CHG_ON_UA)
			return PL_DIR_CHARGING;
		if (smoothed != PL_ABSENT && smoothed < -PL_EST_NOTCHG_UA)
			return PL_DIR_NOTCHARGING;
		return chg ? PL_DIR_CHARGING : PL_DIR_NOTCHARGING;
	}
	return chg ? PL_DIR_CHARGING : PL_DIR_DISCHARGING;
}

void pl_est_init(struct pl_est *e)
{
	memset(e, 0, sizeof(*e));
}

static void est_reset(struct pl_est *e)
{
	e->ema = 0;
	e->n = 0;
	e->skip = 0;
	e->dir = PL_DIR_NONE;
}

void pl_est_add(struct pl_est *e, const struct pl_batt_raw *r, int64_t now_ms)
{
	int64_t v = 0;

	if (e->have_last && (now_ms - e->last_ms > PL_EST_GAP_MS || now_ms < e->last_ms)) {
		est_reset(e);
		e->have_v = false;
	}
	e->last_ms = now_ms;
	e->have_last = true;
	if (r->st != PL_BAT_DISCHARGING && r->st != PL_BAT_CHARGING && r->st != PL_BAT_FULL)
		return;
	int kind = rate_of(r, &v);
	if (kind == RATE_NONE)
		return;
	if (e->kind != kind) {
		est_reset(e);
		e->have_v = false;
		e->kind = kind;
	}
	int64_t mag = abs64(v);
	int64_t idle = kind == RATE_CURRENT ? PL_EST_IDLE_UA : PL_EST_IDLE_UW;
	int64_t max = kind == RATE_CURRENT ? PL_EST_MAX_UA : PL_EST_MAX_UW;

	/* Idle: the old average stays, and the sample is not counted. */
	if (mag < idle || mag > max)
		return;
	/* What the direction is judged by when nothing is averaged yet, or the
	 * sample is of a direction that is not averaged (not charging). */
	e->last_v = v;
	e->have_v = true;
	enum pl_dir d = pl_batt_dir(r, kind == RATE_CURRENT ? v : PL_ABSENT);
	if (d != PL_DIR_DISCHARGING && d != PL_DIR_CHARGING)
		return;
	bool changed = e->dir != PL_DIR_NONE && d != e->dir;
	if (changed)
		est_reset(e);
	e->dir = d;
	if (e->skip > 0) {
		e->skip--;
		return;
	}
	int k = e->n + 1 < PL_EST_EMA_K ? e->n + 1 : PL_EST_EMA_K;
	e->ema += (v - e->ema) / k;
	e->n++;
	if (changed)
		e->skip = PL_EST_SKIP;
}

/* Usable charge (uAh) or energy (uWh) left, in the unit of the rate. */
static int64_t remaining(const struct pl_batt_raw *r, int kind, int mah)
{
	int pct = r->pct;

	if (kind == RATE_CURRENT) {
		/* charge_now counts the reserve below charge_empty too. */
		if (r->charge_now != PL_ABSENT) {
			int64_t empty = r->charge_empty != PL_ABSENT && r->charge_empty > 0 ?
				r->charge_empty : 0;
			return r->charge_now > empty ? r->charge_now - empty : 0;
		}
		if (pct < 0)
			return PL_ABSENT;
		if (r->charge_full != PL_ABSENT)
			return r->charge_full * pct / 100;
		if (r->charge_full_design != PL_ABSENT)
			return r->charge_full_design * pct / 100;
		if (mah > 0)
			return (int64_t)mah * 1000 * pct / 100;
		return PL_ABSENT;
	}
	if (r->energy_now != PL_ABSENT)
		return r->energy_now;
	if (pct < 0)
		return PL_ABSENT;
	if (r->energy_full != PL_ABSENT)
		return r->energy_full * pct / 100;
	if (r->energy_full_design != PL_ABSENT)
		return r->energy_full_design * pct / 100;
	return PL_ABSENT;
}

/* Charge or energy still to go to full. */
static int64_t to_full(const struct pl_batt_raw *r, int kind, int mah)
{
	int pct = r->pct;
	int64_t full, now;

	if (kind == RATE_CURRENT) {
		full = r->charge_full != PL_ABSENT ? r->charge_full :
			r->charge_full_design != PL_ABSENT ? r->charge_full_design :
			mah > 0 ? (int64_t)mah * 1000 : PL_ABSENT;
		now = r->charge_now;
	} else {
		full = r->energy_full != PL_ABSENT ? r->energy_full : r->energy_full_design;
		now = r->energy_now;
	}
	if (full == PL_ABSENT)
		return PL_ABSENT;
	if (now == PL_ABSENT) {
		if (pct < 0)
			return PL_ABSENT;
		return full * (100 - pct) / 100;
	}
	return full > now ? full - now : 0;
}

static struct pl_estimate unsure(const struct pl_batt_raw *r, enum pl_dir d)
{
	struct pl_estimate out = { PL_EST_ESTIMATING, 0, false };

	if (d == PL_DIR_DISCHARGING && r->time_to_empty_now != PL_ABSENT &&
			r->time_to_empty_now > 0 && r->time_to_empty_now < PL_EST_HINT_MAX_S &&
			r->time_to_empty_now >= 60)
		out = (struct pl_estimate){ PL_EST_LEFT, (int)(r->time_to_empty_now / 60), true };
	return out;
}

struct pl_estimate pl_est_compute(const struct pl_est *e, const struct pl_batt_raw *r,
	int battery_mah)
{
	struct pl_estimate out = { PL_EST_NONE, 0, false };
	int64_t v = 0;
	int kind = rate_of(r, &v);

	if (r->st == PL_BAT_AC) {
		out.kind = PL_EST_AC;
		return out;
	}
	if (r->st == PL_BAT_NONE)
		return out;
	int64_t smoothed = e->kind != RATE_CURRENT ? PL_ABSENT : e->n > 0 ? e->ema :
		e->have_v ? e->last_v : PL_ABSENT;
	enum pl_dir d = pl_batt_dir(r, smoothed);

	if (d == PL_DIR_FULL) {
		out.kind = PL_EST_FULL;
		return out;
	}
	if (d == PL_DIR_NOTCHARGING) {
		out.kind = PL_EST_NOTCHARGING;
		return out;
	}
	if (d == PL_DIR_CHARGING && r->pct >= PL_EST_TAPER_PCT) {
		out.kind = PL_EST_CHARGING;
		return out;
	}
	/* Warm, and the run is the one that is going on now. */
	if (e->n < PL_EST_WARM || e->dir != d || e->kind != kind || kind == RATE_NONE)
		return unsure(r, d);
	int64_t rate = abs64(e->ema);
	int64_t amount = d == PL_DIR_CHARGING ? to_full(r, kind, battery_mah) :
		remaining(r, kind, battery_mah);

	if (rate <= 0 || amount == PL_ABSENT)
		return unsure(r, d);
	int64_t minutes = amount * 60 / rate;

	if (minutes < 1)
		return (struct pl_estimate){ PL_EST_ESTIMATING, 0, false };
	if (minutes > PL_EST_OVER_MIN) {
		out.kind = d == PL_DIR_CHARGING ? PL_EST_OVER_FULL : PL_EST_OVER_LEFT;
		return out;
	}
	out.kind = d == PL_DIR_CHARGING ? PL_EST_TOFULL : PL_EST_LEFT;
	out.minutes = (int)minutes;
	return out;
}

int pl_est_round_min(int m)
{
	if (m <= 0)
		return 0;
	if (m < PL_EST_RND1_MIN)
		return m;
	if (m < PL_EST_RND2_MIN)
		return (m + PL_EST_STEP1 / 2) / PL_EST_STEP1 * PL_EST_STEP1;
	return (m + PL_EST_STEP2 / 2) / PL_EST_STEP2 * PL_EST_STEP2;
}

int pl_est_hysteresis(int shown, int fresh)
{
	if (shown <= 0)
		return fresh;
	int limit = shown / PL_EST_HYST_DIV;
	int d = fresh > shown ? fresh - shown : shown - fresh;

	if (shown < PL_EST_HYST_SMALL)
		limit = 1;
	else if (limit < PL_EST_HYST_MIN)
		limit = PL_EST_HYST_MIN;
	return d > limit ? fresh : shown;
}

void pl_est_text(char *buf, size_t len, const struct pl_estimate *e, int pct, int variant)
{
	char dur[24] = "", body[40];
	bool nopct = variant & 1, brief = variant & 2;
	const char *tail = "";
	const char *tilde = e->hint ? "~" : "";

	if (e->kind == PL_EST_LEFT || e->kind == PL_EST_TOFULL) {
		int m = pl_est_round_min(e->minutes);
		if (m >= 60)
			snprintf(dur, sizeof(dur), "%d h %02d min", m / 60, m % 60);
		else
			snprintf(dur, sizeof(dur), "%d min", m);
	}
	switch (e->kind) {
	case PL_EST_LEFT:
		tail = brief ? "" : " left";
		snprintf(body, sizeof(body), "%s%s%s", tilde, dur, tail);
		break;
	case PL_EST_TOFULL:
		tail = brief ? "" : " to full";
		snprintf(body, sizeof(body), "%s%s", dur, tail);
		break;
	case PL_EST_OVER_LEFT:
		snprintf(body, sizeof(body), "> 24 h%s", brief ? "" : " left");
		break;
	case PL_EST_OVER_FULL:
		snprintf(body, sizeof(body), "> 24 h%s", brief ? "" : " to full");
		break;
	case PL_EST_FULL:
		snprintf(body, sizeof(body), "Fully charged");
		break;
	case PL_EST_NOTCHARGING:
		snprintf(body, sizeof(body), "Not charging");
		break;
	case PL_EST_CHARGING:
		snprintf(body, sizeof(body), "Charging");
		break;
	case PL_EST_ESTIMATING:
		snprintf(body, sizeof(body), "Estimating...");
		break;
	case PL_EST_AC:
		snprintf(body, sizeof(body), "On AC power");
		break;
	default:
		snprintf(body, sizeof(body), "--");
		break;
	}
	bool show_pct = !nopct && pct >= 0 && e->kind != PL_EST_NONE && e->kind != PL_EST_AC;
	if (show_pct)
		snprintf(buf, len, "%d%%  %s", clampi(pct, 0, 100), body);
	else
		snprintf(buf, len, "%s", body);
}

/* ---- font ---- */

static const struct {
	char c;
	uint8_t rows[7];
} glyphs[] = {
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
	{ '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
	{ '3', { 0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e } },
	{ '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
	{ '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
	{ '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
	{ '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
	{ '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
	{ ':', { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 } },
	{ '%', { 0x19, 0x1a, 0x02, 0x04, 0x08, 0x0b, 0x13 } },
	{ '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
	{ 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
	{ 'D', { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } },
	{ 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
	{ 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
	{ 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
	{ 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
	{ 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
	{ 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
	{ 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
	{ 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } },
	{ 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
	{ 'Y', { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 } },
	{ 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
	{ ',', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08 } },
	{ '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
	{ '>', { 0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10 } },
	{ '~', { 0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00 } },
};

const uint8_t *pl_font_glyph(char c)
{
	for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
		if (glyphs[i].c == c)
			return glyphs[i].rows;
	return NULL;
}

int pl_text_width(const char *s, int scale)
{
	int n = (int)strlen(s);
	return n ? (n * 6 - 1) * scale : 0;
}

/* ---- the look ---- */

int pl_density_ppi(int mode_w, int mode_h, int mm_w, int mm_h)
{
	if (mode_w <= 0 || mode_h <= 0 || mm_w <= 0 || mm_h <= 0)
		return 0;
	double px = sqrt((double)mode_w * mode_w + (double)mode_h * mode_h);
	double mm = sqrt((double)mm_w * mm_w + (double)mm_h * mm_h);

	return (int)(px / (mm / 25.4) + 0.5);
}

int pl_height_for_ppi(int ppi)
{
	int h = (18 * ppi + PL_DPI_FALLBACK_QVGA / 2) / PL_DPI_FALLBACK_QVGA;

	if (h & 1)
		h++;
	return pl_clamp_height(h);
}

void pl_look_resolve(const struct pl_look_in *in, struct pl_look *out)
{
	int ppi = in->dpi_opt > 0 ? clampi(in->dpi_opt, PL_DPI_MIN, PL_DPI_MAX) : 0;

	if (ppi) {
		out->src = PL_DPI_OVERRIDE;
	} else {
		int rep = pl_density_ppi(in->mode_w, in->mode_h, in->mm_w, in->mm_h);

		if (rep >= PL_DPI_PLAUSIBLE_MIN && rep <= PL_DPI_PLAUSIBLE_MAX) {
			ppi = rep;
			out->src = PL_DPI_REPORTED;
		} else {
			int lng = in->mode_w > in->mode_h ? in->mode_w : in->mode_h;

			ppi = lng >= PL_VGA_LONG_SIDE ? PL_DPI_FALLBACK_VGA : PL_DPI_FALLBACK_QVGA;
			out->src = rep > 0 ? PL_DPI_IMPLAUSIBLE : PL_DPI_FALLBACK;
		}
	}
	out->ppi = ppi;
	out->vga = ppi >= PL_DPI_SMOOTH_MIN;
	out->crisp = in->style_opt == PL_STYLE_AUTO ? !out->vga : in->style_opt == PL_STYLE_CRISP;
	out->height = in->height_opt > 0 ? pl_clamp_height(in->height_opt) :
		out->crisp ? PL_HEIGHT_DEFAULT : pl_height_for_ppi(ppi);
}

bool pl_look_same(const struct pl_look *a, const struct pl_look *b)
{
	return a->ppi == b->ppi && a->src == b->src && a->vga == b->vga &&
		a->crisp == b->crisp && a->height == b->height;
}

const char *pl_dpi_src_name(enum pl_dpi_src s)
{
	return s == PL_DPI_REPORTED ? "reported" : s == PL_DPI_FALLBACK ? "fallback" :
		s == PL_DPI_IMPLAUSIBLE ? "implausible, using mode class" : "override";
}
