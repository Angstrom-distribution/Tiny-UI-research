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

/* ---- layout ---- */

int pl_clamp_height(int h)
{
	return clampi(h, PL_HEIGHT_MIN, PL_HEIGHT_MAX);
}

#define PAD 4
#define ICON_BASE 20
#define THUMB_BASE 20

void pl_layout_compute(struct pl_layout *l, int w, int h, int scale)
{
	memset(l, 0, sizeof(*l));
	scale = clampi(scale, 1, PL_SCALE_MAX);
	if (w < 2)
		w = 2;
	if (h < 2)
		h = 2;
	l->w = w;
	l->h = h;
	l->scale = scale;
	l->row_h = h / 2;
	int row2_h = h - l->row_h;

	/* The font is drawn at twice the 5x7 size by default: 7 pixel text is
	 * unreadable on a 3.5 inch 240x320 panel. */
	int ts = 2 * scale;
	int max_ts = (l->row_h - 4) / 7;
	l->text_scale = clampi(ts > max_ts ? max_ts : ts, 1, 2 * PL_SCALE_MAX);

	l->clock = (struct pl_rect){ 0, 0, w / 2, l->row_h };
	l->battery = (struct pl_rect){ w / 2, 0, w - w / 2, l->row_h };

	for (int i = 0; i < PL_SLIDERS; i++) {
		struct pl_slider *s = &l->slider[i];
		s->cell = (struct pl_rect){ i ? w / 2 : 0, l->row_h,
			i ? w - w / 2 : w / 2, row2_h };
		int isz = row2_h - 2 * PAD;
		if (isz > ICON_BASE * scale)
			isz = ICON_BASE * scale;
		if (isz < 4)
			isz = 4;
		s->icon = (struct pl_rect){ s->cell.x + PAD,
			s->cell.y + (row2_h - isz) / 2, isz, isz };
		int tx = s->icon.x + isz + PAD;
		int tw = s->cell.x + s->cell.w - PAD - tx;
		if (tw < 2)
			tw = 2;
		s->track = (struct pl_rect){ tx, s->cell.y, tw, row2_h };
		s->thumb_w = THUMB_BASE * scale;
		if (s->thumb_w > tw * 2 / 3)
			s->thumb_w = tw * 2 / 3;
		if (s->thumb_w < 1)
			s->thumb_w = 1;
		s->thumb_h = row2_h - 6;
		if (s->thumb_h < 2)
			s->thumb_h = 2;
	}
}

static int travel(const struct pl_slider *s)
{
	int t = s->track.w - s->thumb_w;
	return t < 1 ? 1 : t;
}

struct pl_rect pl_slider_thumb(const struct pl_slider *s, int pct)
{
	pct = clampi(pct, 0, 100);
	return (struct pl_rect){ s->track.x + (travel(s) * pct + 50) / 100,
		s->cell.y + (s->cell.h - s->thumb_h) / 2, s->thumb_w, s->thumb_h };
}

int pl_slider_value(const struct pl_slider *s, int x)
{
	int t = travel(s);
	int rel = x - s->track.x - s->thumb_w / 2;
	if (rel <= 0)
		return 0;
	if (rel >= t)
		return 100;
	return (rel * 100 + t / 2) / t;
}

int pl_slider_at(const struct pl_layout *l, int x, int y)
{
	for (int i = 0; i < PL_SLIDERS; i++) {
		const struct pl_rect *c = &l->slider[i].cell;
		if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h)
			return i;
	}
	return PL_SLIDER_NONE;
}

/* ---- touch ---- */

static int slider_value(const struct pl_layout *l, int slider, int x)
{
	int v = pl_slider_value(&l->slider[slider], x);
	return slider == PL_SLIDER_BACKLIGHT ? pl_bl_clamp_pct(v) : v;
}

int pl_touch_press(struct pl_touch *t, const struct pl_layout *l,
	const bool enabled[PL_SLIDERS], int x, int y, int *value)
{
	t->down = true;
	t->slider = PL_SLIDER_NONE;
	int s = pl_slider_at(l, x, y);
	if (s == PL_SLIDER_NONE || !enabled[s])
		return PL_SLIDER_NONE;
	/* The icon strip is inert: a stray tap on the sun would set the floor
	 * (a dark screen) and one on the speaker would mute. */
	if (x < l->slider[s].track.x)
		return PL_SLIDER_NONE;
	t->slider = s;
	*value = slider_value(l, s, x);
	return s;
}

int pl_touch_motion(struct pl_touch *t, const struct pl_layout *l, int x, int *value)
{
	if (!t->down || t->slider == PL_SLIDER_NONE)
		return PL_SLIDER_NONE;
	*value = slider_value(l, t->slider, x);
	return t->slider;
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
