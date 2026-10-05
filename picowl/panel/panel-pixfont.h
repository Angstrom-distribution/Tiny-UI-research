/*
 * panel-pixfont.h - the pixel fonts of picowl-panel's crisp style: X11
 * misc-fixed bitmap fonts, compiled in as const data by
 * panel/fonts/gen-pixfont.py (panel-pixfont-data.c).
 *
 * A font is a grid of equal cells, w x h pixels, for the characters 0x20 to
 * 0x7e. Each glyph is h rows of bpr bytes (1 for w <= 8, else 2, big endian),
 * the leftmost pixel in the top bit of the first byte. The baseline is
 * `ascent` rows below the top of the cell.
 */
#ifndef PICOWL_PANEL_PIXFONT_H
#define PICOWL_PANEL_PIXFONT_H

#include <stdint.h>

#define PL_PIX_FIRST 0x20
#define PL_PIX_LAST 0x7e

struct pl_pixfont {
	const char *name;
	uint8_t w, h, ascent, bpr;
	const uint8_t *rows;
};

extern const struct pl_pixfont pl_pixfont_6x10;
extern const struct pl_pixfont pl_pixfont_7x13B;
extern const struct pl_pixfont pl_pixfont_7x14B;
extern const struct pl_pixfont pl_pixfont_9x15B;
extern const struct pl_pixfont pl_pixfont_10x20;

/* The h rows of ch, or NULL for a character the font does not have. */
static inline const uint8_t *pl_pixfont_glyph(const struct pl_pixfont *f, int ch)
{
	if (ch < PL_PIX_FIRST || ch > PL_PIX_LAST)
		return 0;
	return f->rows + (unsigned)(ch - PL_PIX_FIRST) * f->h * f->bpr;
}

/* Pixel x (0 is leftmost) of row r of glyph g. */
static inline int pl_pixfont_bit(const struct pl_pixfont *f, const uint8_t *g, int r, int x)
{
	const uint8_t *row = g + r * f->bpr;
	unsigned v = f->bpr == 1 ? row[0] : (unsigned)(row[0] << 8 | row[1]);

	return (v >> ((f->bpr * 8 - 1) - x)) & 1;
}

#endif
