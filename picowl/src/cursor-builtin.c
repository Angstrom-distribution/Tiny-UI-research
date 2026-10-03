/* cursor-builtin.c - built-in hold animation: Pocket PC 2003 style.
 * 8 dots on a ring, 32x32, 8 frames; lead dot rotates with a short trail.
 * Integer arithmetic only (precomputed sin/cos table for 8 positions).
 */
#include <stdint.h>
#include <stdlib.h>
#include "cursor-builtin.h"

/* Precomputed sin/cos table for 8 positions (360/8 = 45 degrees).
 * For radius 11 pixels around centre (16,16) in a 32x32 image:
 * Using fixed-point Q16 representation then scaled by 11.
 * sin(k*45°) and cos(k*45°) for k=0..7. */

/* Ring positions (relative offsets from centre 16,16):
 * Position k: (11*cos(k*45°), 11*sin(k*45°))
 * Using integer approximations:
 * cos/sin values at 6 bits precision, then scaled by 11.
 */
static const struct {
	int dx, dy;  /* offset from centre (16,16) */
} ring_pos[8] = {
	{11,  0},   /* 0°   */
	{ 8,  8},   /* 45°  */
	{ 0, 11},   /* 90°  */
	{-8,  8},   /* 135° */
	{-11, 0},   /* 180° */
	{-8, -8},   /* 225° */
	{ 0,-11},   /* 270° */
	{ 8, -8},   /* 315° */
};

#define CENTRE 16

/* Draw a filled disc at (cx, cy) with radius r in the image.
 * Colour should be 0xRRGGBB (will be set with alpha 0xff).
 * Uses the integer test x*x + y*y <= r*r + r, which rounds the disc edge
 * to the nearest pixel (a plain r*r test turns small discs into plus signs). */
static void draw_disc(struct pw_image *img, int cx, int cy, int r, uint32_t colour)
{
	uint32_t argb = 0xff000000 | colour;
	int r_sq = r * r + r;

	for (int y = -r; y <= r; y++) {
		for (int x = -r; x <= r; x++) {
			if (x*x + y*y <= r_sq) {
				int px = cx + x;
				int py = cy + y;
				if (px >= 0 && px < PW_BUILTIN_SIZE &&
				    py >= 0 && py < PW_BUILTIN_SIZE) {
					img->pixels[py * PW_BUILTIN_SIZE + px] = argb;
				}
			}
		}
	}
}

/* Render frame k with the given fill and outline colours.
 * Lead dot (at position k): radius 4 with 1-pixel outline
 * Trail (k-1, k-2, i.e. behind the lead as it advances): radius 3 with
 * 1-pixel outline
 * Rest: radius 2, fill only
 * Strategy: draw outer ring in outline colour, then inner in fill colour. */
static void render_frame(struct pw_image *img, int k,
	uint32_t fill, uint32_t outline)
{
	/* Clear to transparent. */
	for (int i = 0; i < PW_BUILTIN_SIZE * PW_BUILTIN_SIZE; i++)
		img->pixels[i] = 0x00000000;

	/* Draw all dots. */
	for (int i = 0; i < 8; i++) {
		int pos = (k + i) % 8;
		int outer_r, inner_r;

		if (i == 0) {
			/* Lead dot: outer radius 4, inner radius 3 */
			outer_r = 4;
			inner_r = 3;
		} else if (i == 7 || i == 6) {
			/* Trail, behind the lead: outer radius 3, inner radius 2 */
			outer_r = 3;
			inner_r = 2;
		} else {
			/* Rest: radius 2, fill only */
			outer_r = 2;
			inner_r = 0;
		}

		int cx = CENTRE + ring_pos[pos].dx;
		int cy = CENTRE + ring_pos[pos].dy;

		/* Draw outer ring in outline colour. */
		draw_disc(img, cx, cy, outer_r, outline);

		/* Draw inner fill in fill colour (creates 1-pixel outline). */
		if (inner_r > 0) {
			draw_disc(img, cx, cy, inner_r, fill);
		} else {
			/* For dots without inner fill, use fill colour for the whole dot. */
			draw_disc(img, cx, cy, outer_r, fill);
		}
	}
}

bool pw_cursor_builtin_frames(uint32_t fill, uint32_t outline,
	struct pw_image frames[PW_BUILTIN_FRAMES])
{
	/* Allocate and render each frame. */
	for (int k = 0; k < PW_BUILTIN_FRAMES; k++) {
		if (!pw_image_alloc(&frames[k], PW_BUILTIN_SIZE, PW_BUILTIN_SIZE)) {
			/* Allocation failed; free what we have. */
			for (int i = 0; i < k; i++)
				pw_image_free(&frames[i]);
			return false;
		}

		render_frame(&frames[k], k, fill, outline);

		/* Check compliance. */
		char why[256];
		if (!pw_cursorfit_check(&frames[k], why, sizeof(why))) {
			/* Non-compliant frame; free all and fail. */
			for (int i = 0; i <= k; i++)
				pw_image_free(&frames[i]);
			return false;
		}
	}

	return true;
}
