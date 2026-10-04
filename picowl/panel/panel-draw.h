/*
 * panel-draw.h - software drawing of the panel into a pixel buffer, RGB565 or
 * XRGB8888. Primitives only (rectangles, the 5x7 font, circles); no wayland,
 * no allocation.
 */
#ifndef PICOWL_PANEL_DRAW_H
#define PICOWL_PANEL_DRAW_H

#include "panel-logic.h"

#define PL_COL_BG 0x181a1e
#define PL_COL_FG 0xebeef2
#define PL_COL_SEP 0x30343c
#define PL_COL_TRACK 0x424854
#define PL_COL_FILL 0x3c8ce6	/* (60,140,230) */
#define PL_COL_THUMB 0xf0f4fa
#define PL_COL_THUMB_EDGE 0x20242a
#define PL_COL_DISABLED 0x5a5e66
#define PL_COL_BAT_OK 0x50c864
#define PL_COL_BAT_LOW 0xe6a030
#define PL_COL_BAT_CRIT 0xe04848
#define PL_COL_BOLT 0xffdc3c

struct pl_canvas {
	uint8_t *data;
	int w, h;
	int stride;		/* bytes */
	bool rgb565;
};

/* Pixel value as stored for a 0xRRGGBB colour. */
uint32_t pl_pixel(const struct pl_canvas *c, uint32_t rgb);

/* All drawing is clipped to the canvas. */
void pl_fill(const struct pl_canvas *c, struct pl_rect r, uint32_t rgb);
void pl_draw_text(const struct pl_canvas *c, int x, int y, int scale,
	const char *s, uint32_t rgb);

/* Redraw one widget including its background. */
void pl_render_clock(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_state *st);
void pl_render_battery(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_state *st);
void pl_render_slider(const struct pl_canvas *c, const struct pl_layout *l,
	enum pl_slider_id id, int pct);
void pl_render_all(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_state *st);

#endif
