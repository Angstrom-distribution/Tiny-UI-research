/*
 * panel-gfx.h - the pixel layer of picowl-panel: canvases in RGB565, XRGB8888
 * and premultiplied ARGB8888, gamma-aware blending, anti-aliased coverage
 * masks built from signed distance functions, and text from a TrueType font
 * (stb_truetype) with the 5x7 bitmap font as the fallback.
 *
 * Everything that rasterizes (masks, glyphs) allocates and runs when the panel
 * starts or its layout changes. Drawing (fill, blit, text) only blends masks
 * that exist already: no allocation, no float, no stb.
 */
#ifndef PICOWL_PANEL_GFX_H
#define PICOWL_PANEL_GFX_H

#include "panel-logic.h"

struct pl_canvas {
	uint8_t *data;
	int w, h;
	int stride;		/* bytes */
	enum pl_fmt fmt;
};

/* ---- colour ---- */

/* 0xAARRGGBB premultiplied: what ARGB8888 holds for a colour and an alpha. */
uint32_t pl_premul(uint32_t rgb, int alpha);

/* src (0xRRGGBB, straight) with coverage a (0..255) over dst (premultiplied
 * 0xAARRGGBB), blended in linear light, as premultiplied 0xAARRGGBB. */
uint32_t pl_over(uint32_t dst, uint32_t src_rgb, int a);

/* Pixel value as stored in the canvas for a colour and an alpha (the alpha is
 * ignored by the formats without one). */
uint32_t pl_pixel(const struct pl_canvas *c, uint32_t rgb, int alpha);

/* ---- drawing, clipped to the canvas ---- */

/* Replaces the pixels, alpha included. */
void pl_fill(const struct pl_canvas *c, struct pl_rect r, uint32_t rgb, int alpha);

/* A coverage mask: 8 bits per pixel, 255 is inside. */
struct pl_mask {
	int w, h;
	uint8_t *a;
};

void pl_mask_free(struct pl_mask *m);

/* Blends rgb with the mask scaled by alpha over the canvas, at x, y. clip
 * limits where it draws (NULL: the canvas only). */
void pl_blit(const struct pl_canvas *c, const struct pl_mask *m, int x, int y,
	uint32_t rgb, int alpha, const struct pl_rect *clip);

/* ---- masks ---- */

/* Signed distance to a shape at a point, in pixels: negative inside. The mask
 * of a shape is the coverage of each pixel, 0.5 - d clamped, which is an
 * anti-aliased edge one pixel wide. */
typedef float (*pl_sdf_fn)(const void *ctx, float x, float y);

/* Allocates; a mask with a NULL .a is the result of a failure. */
struct pl_mask pl_mask_from_sdf(int w, int h, pl_sdf_fn fn, const void *ctx);

float pl_sd_circle(float px, float py, float cx, float cy, float r);
float pl_sd_rrect(float px, float py, float x0, float y0, float x1, float y1, float r);
float pl_sd_capsule(float px, float py, float ax, float ay, float bx, float by, float r);
/* Convex polygon, n vertices in either order. */
float pl_sd_poly(float px, float py, const float (*v)[2], int n);
/* An arc of the ring of radius r and thickness t around c, within +-half
 * degrees of the +x axis, with round ends. */
float pl_sd_arc(float px, float py, float cx, float cy, float r, float t, float half_deg);

/* ---- text ---- */

#define PL_FACES 2		/* 0: bar text, 1: the value in the slider row */

struct pl_glyph {
	struct pl_mask m;
	int xoff, yoff;		/* of the mask from the pen, baseline at yoff 0 */
	int adv;
};

#define PL_FONT_CHARS "0123456789:%-AC "
#define PL_FONT_NCHARS 16

struct pl_face {
	int px;
	struct pl_glyph g[PL_FONT_NCHARS];
	int digit_h;		/* height of a digit, and its top above the baseline */
	int digit_top;
};

struct pl_font {
	bool ttf;
	char path[300];		/* of the font file, if ttf */
	struct pl_face face[PL_FACES];
	int scale[PL_FACES];	/* of the bitmap font */
};

/* Default font files, tried in this order; NULL ends the list. */
extern const char *const pl_font_search[];

/* Loads path, or the first of pl_font_search when path is NULL, and rasterizes
 * the glyphs of the panel at px_bar and px_small. Returns false and uses the
 * built-in bitmap font when there is no usable font file. */
bool pl_font_load(struct pl_font *f, const char *path, int px_bar, int px_small);
void pl_font_free(struct pl_font *f);

int pl_font_text_w(const struct pl_font *f, int face, const char *s);

/* Draws s with its digits centred vertically in the band y..y+h. */
void pl_font_draw(const struct pl_canvas *c, const struct pl_font *f, int face,
	int x, int y, int h, const char *s, uint32_t rgb);

#endif
