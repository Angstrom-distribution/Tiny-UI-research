/* panel-logic.c - the pure part of picowl-panel. */
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
	l->battery = (struct pl_rect){ w - PL_MARGIN - group_w, iy, group_w + PL_MARGIN, dh };

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

	int ry = bottom ? 0 : bar_h;
	l->row = (struct pl_rect){ 0, ry, w, l->row_h };
	l->row_in = (struct pl_rect){ 0, ry + (bottom ? 1 : 0), w, l->row_h - 1 };
	l->row_line_y = bottom ? ry : ry + l->row_h - 1;
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
	s->track = (struct pl_rect){ tx, ry, tw, l->row_h };
	/* Wide enough for a stylus to land on, short of the icon and the text. */
	s->cell = (struct pl_rect){ tx - 6, ry, tw + 12, l->row_h };
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
	for (int i = 0; i < PL_SLIDERS; i++)
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
	const bool enabled[PL_SLIDERS], int x, int y, struct pl_touch_out *out)
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
	} else if (h.kind == PL_HIT_TRACK && open != PL_SLIDER_NONE && enabled[open]) {
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

int pl_popup_tap(struct pl_popup *p, int slider, int64_t now_ms)
{
	p->open = p->open == slider ? PL_SLIDER_NONE : slider;
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
