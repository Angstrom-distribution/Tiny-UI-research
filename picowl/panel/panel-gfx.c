/* panel-gfx.c - canvases, blending and coverage masks of picowl-panel. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "panel-gfx.h"

/* ---- colour ---- */

/* Blending in linear light keeps light text on a dark ground at its true
 * weight; blending the gamma-encoded values would thin it out. The tables are
 * built with integer code from 256 pow() calls, once. */
#define LIN_MAX 4095

static uint16_t srgb_lin[256];
static uint8_t lin_srgb[LIN_MAX + 1];
static bool tables_ready;

static void init_tables(void)
{
	if (tables_ready)
		return;
	for (int i = 0; i < 256; i++) {
		double v = i / 255.0;
		double l = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
		srgb_lin[i] = (uint16_t)(l * LIN_MAX + 0.5);
	}
	int k = 0;
	for (int i = 0; i <= LIN_MAX; i++) {
		while (k < 255 && 2 * i > srgb_lin[k] + srgb_lin[k + 1])
			k++;
		lin_srgb[i] = (uint8_t)k;
	}
	tables_ready = true;
}

static int mul255(int v, int a)
{
	return (v * a + 127) / 255;
}

uint32_t pl_premul(uint32_t rgb, int alpha)
{
	if (alpha < 0)
		alpha = 0;
	if (alpha > 255)
		alpha = 255;
	return ((uint32_t)alpha << 24) |
		((uint32_t)mul255((rgb >> 16) & 0xff, alpha) << 16) |
		((uint32_t)mul255((rgb >> 8) & 0xff, alpha) << 8) |
		(uint32_t)mul255(rgb & 0xff, alpha);
}

uint32_t pl_over(uint32_t dst, uint32_t src, int a)
{
	if (a >= 255)
		return 0xff000000u | (src & 0xffffff);
	if (a <= 0)
		return dst;
	init_tables();
	int da = (int)(dst >> 24);
	if (da == 0)
		return pl_premul(src, a);

	/* Alpha-weighted: what is already there counts for its opacity. */
	int wd = mul255(da, 255 - a);
	int ao = a + wd;
	uint32_t out = (uint32_t)ao << 24;
	for (int sh = 16; sh >= 0; sh -= 8) {
		int s = (int)((src >> sh) & 0xff);
		int d = (int)((dst >> sh) & 0xff);
		if (da < 255) {
			d = (d * 255 + da / 2) / da;
			if (d > 255)
				d = 255;
		}
		int lin = (srgb_lin[s] * a + srgb_lin[d] * wd + ao / 2) / ao;
		out |= (uint32_t)mul255(lin_srgb[lin], ao) << sh;
	}
	return out;
}

static uint32_t pack565(uint32_t argb)
{
	uint32_t r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
	return (((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) |
		((b * 31 + 127) / 255);
}

static uint32_t unpack565(uint32_t v)
{
	uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

uint32_t pl_pixel(const struct pl_canvas *c, uint32_t rgb, int alpha)
{
	switch (c->fmt) {
	case PL_FMT_RGB565:
		return pack565(rgb);
	case PL_FMT_ARGB8888:
		return pl_premul(rgb, alpha);
	default:
		return rgb & 0xffffff;
	}
}

static uint32_t px_load(const struct pl_canvas *c, const uint8_t *p)
{
	if (c->fmt == PL_FMT_RGB565)
		return unpack565(*(const uint16_t *)p);
	uint32_t v = *(const uint32_t *)p;
	return c->fmt == PL_FMT_ARGB8888 ? v : (v | 0xff000000u);
}

static void px_store(const struct pl_canvas *c, uint8_t *p, uint32_t argb)
{
	if (c->fmt == PL_FMT_RGB565)
		*(uint16_t *)p = (uint16_t)pack565(argb);
	else
		*(uint32_t *)p = c->fmt == PL_FMT_ARGB8888 ? argb : (argb & 0xffffff);
}

/* ---- drawing ---- */

static bool clip_rect(const struct pl_canvas *c, struct pl_rect r, const struct pl_rect *clip,
	int *x0, int *y0, int *x1, int *y1)
{
	*x0 = r.x < 0 ? 0 : r.x;
	*y0 = r.y < 0 ? 0 : r.y;
	*x1 = r.x + r.w > c->w ? c->w : r.x + r.w;
	*y1 = r.y + r.h > c->h ? c->h : r.y + r.h;
	if (clip) {
		if (clip->x > *x0)
			*x0 = clip->x;
		if (clip->y > *y0)
			*y0 = clip->y;
		if (clip->x + clip->w < *x1)
			*x1 = clip->x + clip->w;
		if (clip->y + clip->h < *y1)
			*y1 = clip->y + clip->h;
	}
	return *x0 < *x1 && *y0 < *y1;
}

void pl_fill(const struct pl_canvas *c, struct pl_rect r, uint32_t rgb, int alpha)
{
	int x0, y0, x1, y1;

	if (!clip_rect(c, r, NULL, &x0, &y0, &x1, &y1))
		return;
	uint32_t px = pl_pixel(c, rgb, alpha);
	for (int y = y0; y < y1; y++) {
		uint8_t *row = c->data + (size_t)y * c->stride;
		if (c->fmt == PL_FMT_RGB565) {
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

void pl_mask_free(struct pl_mask *m)
{
	free(m->a);
	*m = (struct pl_mask){ 0 };
}

void pl_blit(const struct pl_canvas *c, const struct pl_mask *m, int x, int y,
	uint32_t rgb, int alpha, const struct pl_rect *clip)
{
	int x0, y0, x1, y1;
	int bpp = c->fmt == PL_FMT_RGB565 ? 2 : 4;

	if (!m->a || alpha <= 0 ||
			!clip_rect(c, (struct pl_rect){ x, y, m->w, m->h }, clip, &x0, &y0, &x1, &y1))
		return;
	if (alpha > 255)
		alpha = 255;
	uint32_t solid = pl_pixel(c, rgb, 255);
	for (int py = y0; py < y1; py++) {
		const uint8_t *mr = m->a + (size_t)(py - y) * m->w;
		uint8_t *row = c->data + (size_t)py * c->stride;
		for (int px = x0; px < x1; px++) {
			int a = mr[px - x];
			if (!a)
				continue;
			if (alpha < 255)
				a = mul255(a, alpha);
			uint8_t *p = row + (size_t)px * bpp;
			if (a >= 255 && alpha >= 255) {
				if (bpp == 2)
					*(uint16_t *)p = (uint16_t)solid;
				else
					*(uint32_t *)p = solid | (c->fmt == PL_FMT_ARGB8888 ? 0xff000000u : 0);
				continue;
			}
			px_store(c, p, pl_over(px_load(c, p), rgb, a));
		}
	}
}

/* ---- masks ---- */

struct pl_mask pl_mask_from_sdf(int w, int h, pl_sdf_fn fn, const void *ctx)
{
	struct pl_mask m = { w, h, NULL };

	if (w < 1 || h < 1)
		return m;
	m.a = malloc((size_t)w * h);
	if (!m.a)
		return m;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float cov = 0.5f - fn(ctx, x + 0.5f, y + 0.5f);
			cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
			m.a[(size_t)y * w + x] = (uint8_t)(cov * 255.0f + 0.5f);
		}
	return m;
}

float pl_sd_circle(float px, float py, float cx, float cy, float r)
{
	return hypotf(px - cx, py - cy) - r;
}

float pl_sd_rrect(float px, float py, float x0, float y0, float x1, float y1, float r)
{
	float hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
	float qx = fabsf(px - (x0 + x1) / 2) - hx, qy = fabsf(py - (y0 + y1) / 2) - hy;
	float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
	float in = qx > qy ? qx : qy;

	return hypotf(mx, my) + (in < 0 ? in : 0) - r;
}

float pl_sd_capsule(float px, float py, float ax, float ay, float bx, float by, float r)
{
	float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
	float len2 = bax * bax + bay * bay;
	float t = len2 > 0 ? (pax * bax + pay * bay) / len2 : 0;

	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return hypotf(pax - bax * t, pay - bay * t) - r;
}

float pl_sd_poly(float px, float py, const float (*v)[2], int n)
{
	float area = 0, d = -1e9f;

	for (int i = 0; i < n; i++) {
		const float *a = v[i], *b = v[(i + 1) % n];
		area += a[0] * b[1] - b[0] * a[1];
	}
	for (int i = 0; i < n; i++) {
		const float *a = v[i], *b = v[(i + 1) % n];
		float ex = b[0] - a[0], ey = b[1] - a[1];
		float len = hypotf(ex, ey);
		if (len <= 0)
			continue;
		float nx = ey / len, ny = -ex / len;
		if (area < 0) {
			nx = -nx;
			ny = -ny;
		}
		float e = nx * (px - a[0]) + ny * (py - a[1]);
		if (e > d)
			d = e;
	}
	return d;
}

float pl_sd_arc(float px, float py, float cx, float cy, float r, float t, float half_deg)
{
	float h = half_deg * 3.14159265f / 180.0f;
	float qx = px - cx, qy = py - cy;

	if (qx > 0 && fabsf(qy) <= qx * tanf(h))
		return fabsf(hypotf(qx, qy) - r) - t / 2;
	float ex = r * cosf(h), ey = r * sinf(h);
	float d1 = hypotf(qx - ex, qy - ey), d2 = hypotf(qx - ex, qy + ey);
	return (d1 < d2 ? d1 : d2) - t / 2;
}
