/*
 * panel-crisp.h - the shapes of picowl-panel's crisp style: icons as small
 * 1-bit bitmaps, a battery and a slider thumb made of whole-pixel rectangles
 * and rows, highlights with cut corners. Nothing in it is anti-aliased: every
 * pixel it writes is exactly one flat colour, and every edge is on a pixel
 * boundary. The pixel fonts are in panel-pixfont.h and panel-font.c.
 *
 * Drawing is a handful of rectangle fills per shape, without a mask, a blend
 * or an allocation.
 */
#ifndef PICOWL_PANEL_CRISP_H
#define PICOWL_PANEL_CRISP_H

#include "panel-gfx.h"

/* A bitmap drawn from strings, one per row: '#' is a set pixel. */
struct pl_bitmap {
	int w, h;
	const char *const *rows;
};

/* The icons are designed on a grid of PL_ICON_GRID pixels and drawn at a whole
 * multiple of it. */
#define PL_ICON_GRID 12

extern const struct pl_bitmap pl_ico_sun;
extern const struct pl_bitmap pl_ico_speaker[3];	/* muted, one wave, two */
extern const struct pl_bitmap pl_ico_bolt;

/* The scale an icon is drawn at in a box of box pixels: the largest whole
 * number of grids that fits, at least 1. */
int pl_crisp_icon_scale(int box);

/* Draws the set pixels of b at a whole-number scale, top left at x, y. */
void pl_crisp_bitmap(const struct pl_canvas *c, const struct pl_bitmap *b, int scale, int x,
	int y, uint32_t rgb);

/* A flat rectangle without its four corner pixels (the highlight of a button,
 * the track of a slider), in rgb with alpha. Only the part left of x_end is
 * drawn, which is how the filled part of a track is made from the same shape. */
void pl_crisp_pill(const struct pl_canvas *c, struct pl_rect r, uint32_t rgb, int alpha,
	int x_end);

/* The slider's thumb: a disc of odd diameter d (a centre pixel) with a ring of
 * 1 px, top left at x, y. */
void pl_crisp_thumb(const struct pl_canvas *c, int x, int y, int d, uint32_t ring,
	uint32_t fill);

/* Half the width of the row dy rows from the centre of a disc of radius r,
 * minus the centre pixel: the disc covers the pixels x - hw .. x + hw. -1 if
 * the row is outside. */
int pl_crisp_disc_hw(int r, int dy);

/* The battery in a box of w x h at x, y: an outline of whole pixels with a nub,
 * a gap, and the fill in whole pixels proportional to pct, in fill_rgb. */
void pl_crisp_battery(const struct pl_canvas *c, int x, int y, int w, int h, int pct,
	uint32_t fill_rgb, bool charging);

/* How many pixels of the interior the fill has at pct, and where the interior
 * of a battery of w x h starts and how wide it is. */
int pl_crisp_battery_fill(int inner_w, int pct);
struct pl_rect pl_crisp_battery_inner(int w, int h);

#endif
