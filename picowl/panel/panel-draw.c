/* panel-draw.c - the look of picowl-panel. */
#include <math.h>
#include <string.h>
#include "panel-draw.h"

/* ---- assets ---- */

bool pl_assets_init(struct pl_assets *a, const char *font_path, int font_px,
	int bar_alpha, int popup_alpha)
{
	memset(a, 0, sizeof(*a));
	a->bar_alpha = pl_clamp_alpha(bar_alpha);
	a->popup_alpha = pl_clamp_alpha(popup_alpha);
	int small = font_px * 11 / 12;
	return pl_font_load(&a->font, font_path, font_px, small < 8 ? 8 : small);
}

static void masks_free(struct pl_assets *a)
{
	struct pl_mask *all[] = { &a->bat_outline, &a->bat_inner, &a->bolt, &a->hl,
		&a->ring, &a->disc, &a->track };

	for (int i = 0; i < 2; i++)
		pl_mask_free(&a->sun[i]);
	for (int v = 0; v < 3; v++)
		for (int i = 0; i < 2; i++)
			pl_mask_free(&a->speaker[v][i]);
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		pl_mask_free(all[i]);
	a->built = false;
}

void pl_assets_free(struct pl_assets *a)
{
	masks_free(a);
	pl_font_free(&a->font);
}

void pl_assets_metrics(const struct pl_assets *a, struct pl_metrics *m)
{
	m->clock_w = pl_font_text_w(&a->font, 0, "88:88");
	m->bat_text_w = pl_font_text_w(&a->font, 0, "100%");
	m->pct_w = pl_font_text_w(&a->font, 1, "100%");
}

/* All shapes are drawn in a box of 18 units and scaled to the icon size. */
#define U 18.0f

struct icon {
	float s;	/* edge in pixels */
	int waves;	/* speaker: 0..2 */
	bool slash;
};

static float sun_sd(const void *ctx, float x, float y)
{
	const struct icon *ic = ctx;
	float u = ic->s / U, px = x / u, py = y / u;
	float d = pl_sd_circle(px, py, 9, 9, 3.7f);

	for (int k = 0; k < 8; k++) {
		float an = (float)k * 0.78539816f, cs = cosf(an), sn = sinf(an);
		float r = pl_sd_capsule(px, py, 9 + 5.9f * cs, 9 + 5.9f * sn,
			9 + 8.1f * cs, 9 + 8.1f * sn, 0.75f);
		if (r < d)
			d = r;
	}
	return d * u;
}

static float speaker_sd(const void *ctx, float x, float y)
{
	static const float cone[4][2] = { { 4.6f, 6.4f }, { 8.6f, 2.8f },
		{ 8.6f, 15.2f }, { 4.6f, 11.6f } };
	const struct icon *ic = ctx;
	float u = ic->s / U, px = x / u, py = y / u;
	/* Without waves the speaker is centred. */
	float ox = ic->waves ? 0 : 4.3f;
	float qx = px - ox;
	float d = pl_sd_rrect(qx, py, 0.8f, 6.4f, 4.8f, 11.6f, 0.7f);
	float c = pl_sd_poly(qx, py, cone, 4);

	if (c < d)
		d = c;
	for (int i = 0; i < ic->waves; i++) {
		float w = pl_sd_arc(px, py, 8.4f, 9, i ? 7.6f : 4.3f, 1.7f, 42);
		if (w < d)
			d = w;
	}
	if (ic->slash) {
		float sl = pl_sd_capsule(px, py, 2.8f, 2.6f, 15.2f, 15.4f, 0.8f);
		/* A gap around the slash, so that it reads over the speaker. */
		float cut = sl - 1.3f;
		d = d > -cut ? d : -cut;
		if (sl < d)
			d = sl;
	}
	return d * u;
}

struct bat {
	float w, h;
};

static float bat_outline_sd(const void *ctx, float x, float y)
{
	const struct bat *b = ctx;
	float t = b->h * 0.12f < 1.2f ? 1.2f : b->h * 0.12f;
	float r = b->h * 0.25f;
	float o = pl_sd_rrect(x, y, 0, 0, b->w - 2, b->h, r);
	float d = fabsf(o + t / 2) - t / 2;
	float nub = pl_sd_rrect(x, y, b->w - 2.6f, b->h * 0.3f, b->w, b->h * 0.7f, 1.0f);

	return d < nub ? d : nub;
}

static float bat_inner_sd(const void *ctx, float x, float y)
{
	const struct bat *b = ctx;
	float t = b->h * 0.12f < 1.2f ? 1.2f : b->h * 0.12f;
	float ins = t + 0.8f;
	float r = b->h * 0.25f - ins;

	return pl_sd_rrect(x, y, ins, ins, b->w - 2 - ins, b->h - ins, r < 0.8f ? 0.8f : r);
}

static float bolt_sd(const void *ctx, float x, float y)
{
	const struct bat *b = ctx;	/* the size of the bolt */
	float u = x / b->w, v = y / b->h;
	const float up[3][2] = { { 0.68f, 0 }, { 0.10f, 0.56f }, { 0.52f, 0.56f } };
	const float lo[3][2] = { { 0.48f, 0.44f }, { 0.90f, 0.44f }, { 0.32f, 1.0f } };
	float d1 = pl_sd_poly(u, v, up, 3), d2 = pl_sd_poly(u, v, lo, 3);
	float sc = b->w < b->h ? b->w : b->h;

	return (d1 < d2 ? d1 : d2) * sc;
}

struct disc {
	float cx, cy, r;
};

static float disc_sd(const void *ctx, float x, float y)
{
	const struct disc *d = ctx;

	return pl_sd_circle(x, y, d->cx, d->cy, d->r);
}

struct rr {
	float w, h, r;
};

static float rr_sd(const void *ctx, float x, float y)
{
	const struct rr *r = ctx;

	return pl_sd_rrect(x, y, 0, 0, r->w, r->h, r->r);
}

bool pl_assets_prepare(struct pl_assets *a, const struct pl_layout *l)
{
	int key[9] = { l->bar_icon, l->row_icon.w, l->slider.thumb_d, l->slider.track.w,
		l->slider.track_h, l->hl[0].w, l->hl[0].h, l->bat_icon_w, l->bat_icon_h };

	if (a->built && !memcmp(key, a->key, sizeof(key)))
		return true;
	masks_free(a);
	memcpy(a->key, key, sizeof(key));

	int sz[2] = { l->bar_icon, l->row_icon.w };
	for (int i = 0; i < 2; i++) {
		struct icon ic = { (float)sz[i], 0, false };
		a->sun[i] = pl_mask_from_sdf(sz[i], sz[i], sun_sd, &ic);
		for (int v = 0; v < 3; v++) {
			ic.waves = v;
			ic.slash = v == 0;
			a->speaker[v][i] = pl_mask_from_sdf(sz[i], sz[i], speaker_sd, &ic);
		}
	}
	struct bat b = { (float)l->bat_icon_w, (float)l->bat_icon_h };
	a->bat_outline = pl_mask_from_sdf(l->bat_icon_w, l->bat_icon_h, bat_outline_sd, &b);
	a->bat_inner = pl_mask_from_sdf(l->bat_icon_w, l->bat_icon_h, bat_inner_sd, &b);
	int bh = l->bat_icon_h - 3 < 3 ? 3 : l->bat_icon_h - 3;
	int bw = bh * 5 / 7 < 3 ? 3 : bh * 5 / 7;
	struct bat bo = { (float)bw, (float)bh };
	a->bolt = pl_mask_from_sdf(bw, bh, bolt_sd, &bo);

	struct rr hl = { (float)l->hl[0].w, (float)l->hl[0].h, l->hl[0].h / 3.0f };
	a->hl = pl_mask_from_sdf(l->hl[0].w, l->hl[0].h, rr_sd, &hl);
	int td = l->slider.thumb_d;
	struct disc ring = { td / 2.0f, td / 2.0f, td / 2.0f };
	struct disc inner = { td / 2.0f, td / 2.0f, td / 2.0f - 1.3f };
	a->ring = pl_mask_from_sdf(td, td, disc_sd, &ring);
	a->disc = pl_mask_from_sdf(td, td, disc_sd, &inner);
	struct rr tr = { (float)l->slider.track.w, (float)l->slider.track_h,
		l->slider.track_h / 2.0f };
	a->track = pl_mask_from_sdf(l->slider.track.w, l->slider.track_h, rr_sd, &tr);

	struct pl_mask *all[] = { &a->bat_outline, &a->bat_inner, &a->bolt, &a->hl,
		&a->ring, &a->disc, &a->track, &a->sun[0], &a->sun[1] };
	bool ok = true;
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		ok &= all[i]->a != NULL;
	for (int v = 0; v < 3; v++)
		for (int i = 0; i < 2; i++)
			ok &= a->speaker[v][i].a != NULL;
	if (!ok) {
		masks_free(a);
		return false;
	}
	a->built = true;
	return true;
}

int pl_speaker_variant(int vol_pct)
{
	return vol_pct <= 0 ? 0 : vol_pct <= 50 ? 1 : 2;
}

/* ---- widgets ---- */

struct pl_rect pl_button_rect(const struct pl_layout *l, int slider)
{
	struct pl_rect r = l->button[slider];

	r.y = l->clock.y;
	r.h = l->bar_h - 1;
	return r;
}

struct pl_rect pl_row_value_rect(const struct pl_layout *l)
{
	return (struct pl_rect){ l->slider.cell.x, l->row_in.y, l->w - l->slider.cell.x,
		l->row_h - 1 };
}

void pl_render_clock(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st)
{
	char buf[8];

	pl_fill(c, l->clock, PL_COL_BG, a->bar_alpha);
	pl_clock_text(buf, sizeof(buf), st->hour, st->min);
	pl_font_draw(c, &a->font, 0, PL_MARGIN, l->clock.y, l->clock.h, buf, PL_COL_FG,
		pl_text_sub(a->sub, a->bar_alpha));
}

void pl_render_battery(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st)
{
	char buf[8];
	bool icon = st->bat == PL_BAT_DISCHARGING || st->bat == PL_BAT_CHARGING ||
		st->bat == PL_BAT_FULL;
	int dh = l->battery.h;

	pl_fill(c, l->battery, PL_COL_BG, a->bar_alpha);
	pl_battery_text(buf, sizeof(buf), st->bat, st->bat_pct);
	int tw = pl_font_text_w(&a->font, 0, buf);
	int tx = l->battery.x + l->battery.w - PL_MARGIN - tw;
	pl_font_draw(c, &a->font, 0, tx, l->battery.y, dh, buf, PL_COL_FG,
		pl_text_sub(a->sub, a->bar_alpha));
	if (!icon)
		return;

	int pct = st->bat_pct < 0 ? 0 : st->bat_pct > 100 ? 100 : st->bat_pct;
	bool charging = st->bat == PL_BAT_CHARGING;
	int iw = l->bat_icon_w, ih = l->bat_icon_h;
	int ix = tx - PL_GAP - iw, iy = l->battery.y + (dh - ih) / 2;
	/* The interior is inset by the outline and a gap, a little over 2 px. */
	int ins = 2, inner_w = iw - 2 - 2 * ins;
	int fw = inner_w * pct / 100;
	/* A sliver of 1 px is lost in the outline's anti-aliasing. */
	if (fw < 2 && pct > 0)
		fw = 2;
	/* Low wins over charging: the bolt says it is charging, the red that it
	 * is nearly empty. */
	uint32_t col = pct <= PL_BAT_LOW_PCT ? PL_COL_BAT_LOW :
		charging ? PL_COL_BAT_CHARGING : PL_COL_BAT_FILL;
	struct pl_rect clip = { ix, iy, ins + fw, ih };

	pl_blit(c, &a->bat_outline, ix, iy, PL_COL_FG, 255, NULL);
	pl_blit(c, &a->bat_inner, ix, iy, PL_COL_BAT_EMPTY, 255, NULL);
	pl_blit(c, &a->bat_inner, ix, iy, col, 255, &clip);
	if (charging)
		pl_blit(c, &a->bolt, ix + (iw - 2 - a->bolt.w) / 2,
			iy + (ih - a->bolt.h) / 2, PL_COL_BOLT, 255, NULL);
}

static const struct pl_mask *icon_mask(const struct pl_assets *a, int slider, int vol_pct,
	int which)
{
	return slider == PL_SLIDER_BACKLIGHT ? &a->sun[which] :
		&a->speaker[pl_speaker_variant(vol_pct)][which];
}

void pl_render_button(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st, int open, int slider)
{
	struct pl_rect r = pl_button_rect(l, slider);
	int pct = slider == PL_SLIDER_BACKLIGHT ? st->bl_pct : st->vol_pct;
	uint32_t ink = pct < 0 ? PL_COL_DISABLED : open == slider ? PL_COL_ACCENT : PL_COL_FG;
	const struct pl_mask *m = icon_mask(a, slider, pct, 0);

	pl_fill(c, r, PL_COL_BG, a->bar_alpha);
	if (open == slider)
		pl_blit(c, &a->hl, l->hl[slider].x, l->hl[slider].y, PL_COL_HL, 255, NULL);
	pl_blit(c, m, r.x + (r.w - m->w) / 2, r.y + (r.h - m->h) / 2, ink, 255, NULL);
}

void pl_render_row_value(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st, int open)
{
	const struct pl_slider *s = &l->slider;
	int pct = open == PL_SLIDER_BACKLIGHT ? st->bl_pct : st->vol_pct;
	char buf[8];

	pl_fill(c, pl_row_value_rect(l), PL_COL_ROW, a->popup_alpha);
	if (open == PL_SLIDER_NONE || pct < 0)
		return;
	struct pl_rect th = pl_slider_thumb(l, pct);
	int ty = l->row_in.y + (l->row_h - 1 - s->track_h) / 2;
	int cx = th.x + th.w / 2;
	struct pl_rect left = { s->track.x, ty, cx - s->track.x, s->track_h };

	pl_blit(c, &a->track, s->track.x, ty, PL_COL_TRACK, 255, NULL);
	pl_blit(c, &a->track, s->track.x, ty, PL_COL_ACCENT, 255, &left);
	/* The ring is the part that lets the window behind show through. */
	pl_blit(c, &a->ring, th.x, th.y, PL_COL_THUMB_RING, a->popup_alpha, NULL);
	pl_blit(c, &a->disc, th.x, th.y, PL_COL_THUMB, 255, NULL);

	pl_pct_text(buf, sizeof(buf), pct);
	int tw = pl_font_text_w(&a->font, 1, buf);
	pl_font_draw(c, &a->font, 1, l->w - PL_MARGIN - tw, l->row_in.y, l->row_h - 1, buf,
		PL_COL_FG, pl_text_sub(a->sub, a->popup_alpha));
}

void pl_render_row(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st, int open)
{
	pl_fill(c, l->row_in, PL_COL_ROW, a->popup_alpha);
	pl_fill(c, (struct pl_rect){ 0, l->row_line_y, l->w, 1 }, PL_COL_ROW_LINE, a->popup_alpha);
	if (open == PL_SLIDER_NONE)
		return;
	int pct = open == PL_SLIDER_BACKLIGHT ? st->bl_pct : st->vol_pct;
	const struct pl_mask *m = icon_mask(a, open, pct, 1);
	pl_blit(c, m, l->row_icon.x, l->row_icon.y, pct < 0 ? PL_COL_DISABLED : PL_COL_FG, 255,
		NULL);
	pl_render_row_value(c, l, a, st, open);
}

void pl_render_all(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st, int open)
{
	pl_fill(c, (struct pl_rect){ 0, 0, c->w, c->h }, 0, 0);
	pl_fill(c, (struct pl_rect){ 0, l->clock.y, l->w, l->bar_h - 1 }, PL_COL_BG, a->bar_alpha);
	pl_fill(c, (struct pl_rect){ 0, l->bar_line_y, l->w, 1 }, PL_COL_LINE, a->bar_alpha);
	pl_render_clock(c, l, a, st);
	for (int i = 0; i < PL_SLIDERS; i++)
		pl_render_button(c, l, a, st, open, i);
	pl_render_battery(c, l, a, st);
	if (l->row_shown)
		pl_render_row(c, l, a, st, open);
}
