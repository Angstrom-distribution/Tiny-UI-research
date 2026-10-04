/* panel-draw.c - software drawing of the panel. */
#include <string.h>
#include "panel-draw.h"

#define TEXT_PAD 6

uint32_t pl_pixel(const struct pl_canvas *c, uint32_t rgb)
{
	if (!c->rgb565)
		return rgb & 0xffffff;
	uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

void pl_fill(const struct pl_canvas *c, struct pl_rect r, uint32_t rgb)
{
	int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
	int x1 = r.x + r.w > c->w ? c->w : r.x + r.w;
	int y1 = r.y + r.h > c->h ? c->h : r.y + r.h;
	uint32_t px = pl_pixel(c, rgb);

	for (int y = y0; y < y1; y++) {
		uint8_t *row = c->data + (size_t)y * c->stride;
		if (c->rgb565) {
			uint16_t *p = (uint16_t *)row + x0;
			for (int x = x0; x < x1; x++)
				*p++ = (uint16_t)px;
		} else {
			uint32_t *p = (uint32_t *)row + x0;
			for (int x = x0; x < x1; x++)
				*p++ = px;
		}
	}
}

static void fill(const struct pl_canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
	pl_fill(c, (struct pl_rect){ x, y, w, h }, rgb);
}

void pl_draw_text(const struct pl_canvas *c, int x, int y, int scale,
	const char *s, uint32_t rgb)
{
	for (; *s; s++, x += 6 * scale) {
		const uint8_t *g = pl_font_glyph(*s);
		if (!g)
			continue;
		for (int row = 0; row < 7; row++)
			for (int col = 0; col < 5; col++)
				if (g[row] & (0x10 >> col))
					fill(c, x + col * scale, y + row * scale,
						scale, scale, rgb);
	}
}

/* ---- icons ---- */

static void disc(const struct pl_canvas *c, int cx, int cy, int r, uint32_t rgb)
{
	for (int dy = -r; dy <= r; dy++) {
		int half = 0;
		while ((half + 1) * (half + 1) + dy * dy <= r * r + r)
			half++;
		if (dy * dy <= r * r + r)
			fill(c, cx - half, cy + dy, 2 * half + 1, 1, rgb);
	}
}

static void sun(const struct pl_canvas *c, struct pl_rect b, uint32_t rgb)
{
	int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
	int cr = b.w / 5, t = b.w / 10 < 1 ? 1 : b.w / 10;
	int from = cr + 2, to = b.w / 2 - 1;

	disc(c, cx, cy, cr, rgb);
	if (to < from)
		return;
	fill(c, cx + from, cy - t / 2, to - from + 1, t, rgb);
	fill(c, cx - to, cy - t / 2, to - from + 1, t, rgb);
	fill(c, cx - t / 2, cy + from, t, to - from + 1, rgb);
	fill(c, cx - t / 2, cy - to, t, to - from + 1, rgb);
	for (int d = from * 7 / 10; d <= to * 7 / 10; d++) {
		fill(c, cx + d, cy + d, t, t, rgb);
		fill(c, cx - d - t + 1, cy + d, t, t, rgb);
		fill(c, cx + d, cy - d - t + 1, t, t, rgb);
		fill(c, cx - d - t + 1, cy - d - t + 1, t, t, rgb);
	}
}

static void speaker(const struct pl_canvas *c, struct pl_rect b, uint32_t rgb)
{
	int cy = b.y + b.h / 2;
	int x0 = b.x + b.w / 10, bw = b.w / 4, bh = b.w / 3;
	int cw = b.w * 3 / 10, half_end = b.w * 7 / 20;

	fill(c, x0, cy - bh / 2, bw, bh, rgb);
	for (int i = 0; i < cw; i++) {
		int half = bh / 2 + i * (half_end - bh / 2) / cw;
		fill(c, x0 + bw + i, cy - half, 1, 2 * half + 1, rgb);
	}

	/* Two arcs of sound, +-45 degrees around the edge of the cone. */
	int wx = x0 + bw + cw - 1, t = b.w / 10 < 1 ? 1 : b.w / 10;
	int radii[2] = { b.w * 3 / 20, b.w * 3 / 10 };
	for (int k = 0; k < 2; k++) {
		int lo = radii[k] * radii[k], hi = (radii[k] + t) * (radii[k] + t);
		for (int dy = -radii[k] - t; dy <= radii[k] + t; dy++)
			for (int dx = 1; dx <= radii[k] + t; dx++) {
				int d2 = dx * dx + dy * dy;
				int ady = dy < 0 ? -dy : dy;
				if (ady <= dx && d2 >= lo && d2 < hi &&
						wx + dx < b.x + b.w)
					fill(c, wx + dx, cy + dy, 1, 1, rgb);
			}
	}
}

static const char *const bolt[7] = {
	"....#",
	"...##",
	"..##.",
	".####",
	"..##.",
	".##..",
	"##...",
};

static void battery_icon(const struct pl_canvas *c, int x, int y, int bw, int bh,
	int pct, bool charging)
{
	int t = bh >= 10 ? 2 : 1;
	int nub = bh / 5 < 1 ? 1 : bh / 5;

	fill(c, x, y, bw, bh, PL_COL_FG);
	fill(c, x + bw, y + bh / 4, nub, bh - 2 * (bh / 4), PL_COL_FG);
	int ix = x + t, iy = y + t, iw = bw - 2 * t, ih = bh - 2 * t;
	fill(c, ix, iy, iw, ih, PL_COL_BG);
	int fw = iw * pct / 100;
	if (fw < 1 && pct > 0)
		fw = 1;
	uint32_t col = pct > 30 ? PL_COL_BAT_OK : pct > 15 ? PL_COL_BAT_LOW : PL_COL_BAT_CRIT;
	fill(c, ix, iy, fw, ih, col);

	if (charging) {
		int s = ih / 7 < 1 ? 1 : ih / 7;
		int bx = ix + (iw - 5 * s) / 2, by = iy + (ih - 7 * s) / 2;
		for (int r = 0; r < 7; r++)
			for (int q = 0; q < 5; q++)
				if (bolt[r][q] == '#')
					fill(c, bx + q * s, by + r * s, s, s, PL_COL_BOLT);
	}
}

/* ---- widgets ---- */

static int text_y(const struct pl_layout *l, const struct pl_rect *r)
{
	return r->y + (r->h - 7 * l->text_scale) / 2;
}

void pl_render_clock(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_state *st)
{
	char buf[8];

	pl_fill(c, l->clock, PL_COL_BG);
	pl_clock_text(buf, sizeof(buf), st->hour, st->min);
	pl_draw_text(c, l->clock.x + TEXT_PAD, text_y(l, &l->clock), l->text_scale,
		buf, PL_COL_FG);
}

void pl_render_battery(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_state *st)
{
	char buf[8];
	bool icon = st->bat == PL_BAT_DISCHARGING || st->bat == PL_BAT_CHARGING ||
		st->bat == PL_BAT_FULL;
	int bw = l->row_h * 4 / 5, bh = bw / 2, nub = bh / 5 < 1 ? 1 : bh / 5;

	pl_fill(c, l->battery, PL_COL_BG);
	pl_battery_text(buf, sizeof(buf), st->bat, st->bat_pct);
	int tw = pl_text_width(buf, l->text_scale);
	int total = tw + (icon ? bw + nub + TEXT_PAD : 0);
	int x = l->battery.x + l->battery.w - TEXT_PAD - total;
	if (icon) {
		battery_icon(c, x, l->battery.y + (l->battery.h - bh) / 2, bw, bh,
			st->bat_pct < 0 ? 0 : st->bat_pct > 100 ? 100 : st->bat_pct,
			st->bat == PL_BAT_CHARGING);
		x += bw + nub + TEXT_PAD;
	}
	pl_draw_text(c, x, text_y(l, &l->battery), l->text_scale, buf, PL_COL_FG);
}

void pl_render_slider(const struct pl_canvas *c, const struct pl_layout *l,
	enum pl_slider_id id, int pct)
{
	const struct pl_slider *s = &l->slider[id];
	bool on = pct >= 0;
	uint32_t ink = on ? PL_COL_FG : PL_COL_DISABLED;

	pl_fill(c, s->cell, PL_COL_BG);
	if (id == PL_SLIDER_VOLUME)
		fill(c, s->cell.x, s->cell.y + 2, 1, s->cell.h - 4, PL_COL_SEP);
	if (id == PL_SLIDER_BACKLIGHT)
		sun(c, s->icon, ink);
	else
		speaker(c, s->icon, ink);

	int bar_h = s->cell.h / 4 < 4 ? 4 : s->cell.h / 4;
	int bar_y = s->cell.y + (s->cell.h - bar_h) / 2;
	if (!on) {
		fill(c, s->track.x, bar_y, s->track.w, bar_h, PL_COL_TRACK);
		return;
	}
	struct pl_rect th = pl_slider_thumb(s, pct);
	int cx = th.x + th.w / 2;
	fill(c, s->track.x, bar_y, cx - s->track.x, bar_h, PL_COL_FILL);
	fill(c, cx, bar_y, s->track.x + s->track.w - cx, bar_h, PL_COL_TRACK);
	pl_fill(c, th, PL_COL_THUMB_EDGE);
	fill(c, th.x + 1, th.y + 1, th.w - 2, th.h - 2, PL_COL_THUMB);
	fill(c, cx - 1, th.y + 4, 2, th.h - 8, PL_COL_FILL);
}

void pl_render_all(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_state *st)
{
	pl_fill(c, (struct pl_rect){ 0, 0, c->w, c->h }, PL_COL_BG);
	pl_render_clock(c, l, st);
	pl_render_battery(c, l, st);
	pl_render_slider(c, l, PL_SLIDER_BACKLIGHT, st->bl_pct);
	pl_render_slider(c, l, PL_SLIDER_VOLUME, st->vol_pct);
}
