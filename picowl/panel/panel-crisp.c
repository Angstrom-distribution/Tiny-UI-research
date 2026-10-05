/* panel-crisp.c - the whole-pixel shapes of picowl-panel's crisp style. */
#include <string.h>
#include "panel-crisp.h"
#include "panel-draw.h"

/* ---- icons ---- */

/* Symmetric about the middle of the grid: the centre lies between two pixels,
 * so nothing is half a pixel off. */
static const char *const sun_rows[12] = {
	".....##.....",
	".#...##...#.",
	"..#......#..",
	"....####....",
	"...######...",
	"##.######.##",
	"##.######.##",
	"...######...",
	"....####....",
	"..#......#..",
	".#...##...#.",
	".....##.....",
};

/* The speaker is in the same place with and without waves, so that the icon
 * does not jump when the volume crosses a step. Muted has a cross where the
 * waves are. */
static const char *const speaker_muted_rows[12] = {
	"............",
	"......#.....",
	".....##.....",
	"....###.....",
	".######.#..#",
	".######..##.",
	".######..##.",
	".######.#..#",
	"....###.....",
	".....##.....",
	"......#.....",
	"............",
};

static const char *const speaker_one_rows[12] = {
	"............",
	"......#.....",
	".....##.....",
	"....###.....",
	".######.#...",
	".######..#..",
	".######..#..",
	".######.#...",
	"....###.....",
	".....##.....",
	"......#.....",
	"............",
};

static const char *const speaker_two_rows[12] = {
	"............",
	"......#..#..",
	".....##...#.",
	"....###....#",
	".######.#..#",
	".######..#.#",
	".######..#.#",
	".######.#..#",
	"....###....#",
	".....##...#.",
	"......#..#..",
	"............",
};

/* The bolt is drawn over the battery, where it must not touch the outline. */
static const char *const bolt_rows[6] = {
	"..##.",
	".##..",
	".###.",
	"..##.",
	".##..",
	"##...",
};

const struct pl_bitmap pl_ico_sun = { 12, 12, sun_rows };
const struct pl_bitmap pl_ico_speaker[3] = {
	{ 12, 12, speaker_muted_rows },
	{ 12, 12, speaker_one_rows },
	{ 12, 12, speaker_two_rows },
};
const struct pl_bitmap pl_ico_bolt = { 5, 6, bolt_rows };

int pl_crisp_icon_scale(int box)
{
	int s = box / PL_ICON_GRID;

	return s < 1 ? 1 : s;
}

void pl_crisp_bitmap(const struct pl_canvas *c, const struct pl_bitmap *b, int scale, int x,
	int y, uint32_t rgb)
{
	uint32_t px = pl_pixel(c, rgb, 255);

	for (int r = 0; r < b->h; r++)
		for (int col = 0; col < b->w;) {
			if (b->rows[r][col] != '#') {
				col++;
				continue;
			}
			int run = 1;
			while (col + run < b->w && b->rows[r][col + run] == '#')
				run++;
			pl_fill_px(c, (struct pl_rect){ x + col * scale, y + r * scale, run * scale,
				scale }, px);
			col += run;
		}
}

/* ---- rectangles and the thumb ---- */

void pl_crisp_pill(const struct pl_canvas *c, struct pl_rect r, uint32_t rgb, int alpha,
	int x_end)
{
	uint32_t px = pl_pixel(c, rgb, alpha);
	int x1 = r.x + r.w, e = x_end < x1 ? x_end : x1;

	/* Too small to have corners to cut. */
	if (r.w <= 2 || r.h <= 2) {
		pl_fill_px(c, (struct pl_rect){ r.x, r.y, e - r.x, r.h }, px);
		return;
	}
	/* The first and last row are one pixel shorter at each end; where the
	 * shape is cut short by x_end there is no corner and the end is straight. */
	int top_end = e == x1 ? x1 - 1 : e;

	pl_fill_px(c, (struct pl_rect){ r.x + 1, r.y, top_end - (r.x + 1), 1 }, px);
	pl_fill_px(c, (struct pl_rect){ r.x + 1, r.y + r.h - 1, top_end - (r.x + 1), 1 }, px);
	pl_fill_px(c, (struct pl_rect){ r.x, r.y + 1, e - r.x, r.h - 2 }, px);
}

int pl_crisp_disc_hw(int r, int dy)
{
	int lim = r * r + r - dy * dy, hw = r;

	if (r < 0 || lim < 0 || dy > r || dy < -r)
		return -1;
	while (hw * hw > lim)
		hw--;
	return hw;
}

void pl_crisp_thumb(const struct pl_canvas *c, int x, int y, int d, uint32_t ring,
	uint32_t fill)
{
	int r = d / 2, cx = x + r;
	uint32_t ring_px = pl_pixel(c, ring, 255), fill_px = pl_pixel(c, fill, 255);

	for (int dy = -r; dy <= r; dy++) {
		int ow = pl_crisp_disc_hw(r, dy), iw = pl_crisp_disc_hw(r - 1, dy);

		if (ow < 0)
			continue;
		if (iw < 0) {
			pl_fill_px(c, (struct pl_rect){ cx - ow, y + r + dy, 2 * ow + 1, 1 }, ring_px);
			continue;
		}
		/* The ring on both sides of the row, the fill between. */
		pl_fill_px(c, (struct pl_rect){ cx - ow, y + r + dy, ow - iw, 1 }, ring_px);
		pl_fill_px(c, (struct pl_rect){ cx + iw + 1, y + r + dy, ow - iw, 1 }, ring_px);
		pl_fill_px(c, (struct pl_rect){ cx - iw, y + r + dy, 2 * iw + 1, 1 }, fill_px);
	}
}

/* ---- the battery ---- */

/* The outline is as thick as the smooth style's (12 percent of the height,
 * rounded, at least 1 px) and so is the interior: a gap of 1 px between them. */
static int bat_outline(int h)
{
	int t = (h * 12 + 50) / 100;

	return t < 1 ? 1 : t;
}

static int bat_nub(int w)
{
	int n = w / 8;

	return n < 1 ? 1 : n;
}

struct pl_rect pl_crisp_battery_inner(int w, int h)
{
	int ins = bat_outline(h) + 1, bw = w - bat_nub(w);

	return (struct pl_rect){ ins, ins, bw - 2 * ins, h - 2 * ins };
}

int pl_crisp_battery_fill(int inner_w, int pct)
{
	if (pct <= 0)
		return 0;
	if (pct > 100)
		pct = 100;
	int fw = (inner_w * pct + 50) / 100;

	/* Any charge shows. */
	return fw < 1 ? 1 : fw;
}

void pl_crisp_battery(const struct pl_canvas *c, int x, int y, int w, int h, int pct,
	uint32_t fill_rgb, bool charging)
{
	int ol = bat_outline(h), nub = bat_nub(w), bw = w - nub;
	int ny0 = (h * 3 + 5) / 10;
	uint32_t fg = pl_pixel(c, PL_COL_FG, 255);
	struct pl_rect in = pl_crisp_battery_inner(w, h);

	/* The outline without its four corner pixels, and the nub. */
	pl_fill_px(c, (struct pl_rect){ x + 1, y, bw - 2, ol }, fg);
	pl_fill_px(c, (struct pl_rect){ x + 1, y + h - ol, bw - 2, ol }, fg);
	pl_fill_px(c, (struct pl_rect){ x, y + 1, ol, h - 2 }, fg);
	pl_fill_px(c, (struct pl_rect){ x + bw - ol, y + 1, ol, h - 2 }, fg);
	pl_fill_px(c, (struct pl_rect){ x + bw, y + ny0, nub, h - 2 * ny0 }, fg);

	pl_fill(c, (struct pl_rect){ x + in.x, y + in.y, in.w, in.h }, PL_COL_BAT_EMPTY, 255);
	pl_fill(c, (struct pl_rect){ x + in.x, y + in.y, pl_crisp_battery_fill(in.w, pct), in.h },
		fill_rgb, 255);
	if (charging) {
		const struct pl_bitmap *b = &pl_ico_bolt;

		pl_crisp_bitmap(c, b, 1, x + (bw - b->w) / 2, y + (h - b->h) / 2, PL_COL_BOLT);
	}
}
