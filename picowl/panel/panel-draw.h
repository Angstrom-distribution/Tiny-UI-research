/*
 * panel-draw.h - the look of picowl-panel: the vector icons, the slider and
 * the widgets, drawn into a canvas.
 *
 * The icons and the parts of the slider are coverage masks that are built when
 * the panel starts and again when its layout changes (pl_assets_prepare).
 * Drawing a widget only blends those masks and glyphs: nothing is rasterized
 * or allocated while the panel redraws.
 */
#ifndef PICOWL_PANEL_DRAW_H
#define PICOWL_PANEL_DRAW_H

#include "panel-gfx.h"

#define PL_COL_BG 0x1c1f24
#define PL_COL_LINE 0x363b44
#define PL_COL_ROW 0x252930
#define PL_COL_ROW_LINE 0x3d434d
#define PL_COL_FG 0xe8eaed
#define PL_COL_SECONDARY 0x9aa0a6
#define PL_COL_ACCENT 0x4c8dff
#define PL_COL_HL 0x2b3a57
#define PL_COL_TRACK 0x454b55
#define PL_COL_THUMB 0xffffff
#define PL_COL_THUMB_RING 0xaeb4be
#define PL_COL_DISABLED 0x5a5f66
#define PL_COL_BAT_FILL 0xc9cdd3
#define PL_COL_BAT_EMPTY 0x30353c
#define PL_COL_BAT_LOW 0xe5484d
#define PL_COL_BAT_CHARGING 0x3fb950
#define PL_COL_BOLT 0xffffff

/* Battery at or below this percent is drawn red. */
#define PL_BAT_LOW_PCT 15

struct pl_assets {
	struct pl_font font;
	int bar_alpha, popup_alpha;
	/* Order of the colour stripes under the text, where its ground is opaque. */
	enum pl_sub sub;

	/* What the masks were built for. */
	int key[9];
	bool built;

	struct pl_mask sun[2];		/* in a button, in the row */
	struct pl_mask speaker[3][2];	/* muted, one wave, two waves */
	struct pl_mask bat_outline, bat_inner, bolt;
	struct pl_mask hl, ring, disc, track;
};

/* Loads the font (font_path NULL: the default search) and keeps the alphas.
 * Returns false if the built-in bitmap font is used instead of a font file. */
bool pl_assets_init(struct pl_assets *a, const char *font_path, int font_px,
	int bar_alpha, int popup_alpha);
void pl_assets_free(struct pl_assets *a);

/* Text widths for the layout. */
void pl_assets_metrics(const struct pl_assets *a, struct pl_metrics *m);

/* Builds the masks for the sizes in l, unless they exist already. False if an
 * allocation failed. */
bool pl_assets_prepare(struct pl_assets *a, const struct pl_layout *l);

/* Variant of the speaker icon for a volume: 0 muted, 1 one wave, 2 two. */
int pl_speaker_variant(int vol_pct);

/* Rectangles that a widget redraw covers, for the damage. */
struct pl_rect pl_button_rect(const struct pl_layout *l, int slider);
struct pl_rect pl_row_value_rect(const struct pl_layout *l);

/* Redraw one widget including its background. open is the slider in the row,
 * or PL_SLIDER_NONE. */
void pl_render_clock(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st);
void pl_render_battery(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st);
void pl_render_button(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st, int open, int slider);
/* The row without its icon: track, thumb, value. For a drag. */
void pl_render_row_value(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st, int open);
/* The whole row. */
void pl_render_row(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st, int open);
void pl_render_all(const struct pl_canvas *c, const struct pl_layout *l,
	const struct pl_assets *a, const struct pl_state *st, int open);

#endif
