/* tile.c - the two-window tiled layout, see tile.h. */
#include <stdint.h>

#include "tile.h"

bool pw_tile_layout(const struct pw_tile_box *usable,
	const struct pw_tile_hint hints[2], struct pw_tile_box out[2])
{
	if (!usable || !hints || !out || usable->w <= 0 || usable->h <= 0)
		return false;

	bool portrait = usable->h >= usable->w;
	int64_t extent = portrait ? usable->h : usable->w;
	if (extent < 2)
		return false;

	int64_t aw = 0, ah = 0;
	if (hints[0].fixed_w > 0 && hints[0].fixed_h > 0) {
		aw = hints[0].fixed_w;
		ah = hints[0].fixed_h;
	} else if (hints[0].aspect_w > 0 && hints[0].aspect_h > 0) {
		aw = hints[0].aspect_w;
		ah = hints[0].aspect_h;
	}

	int64_t size;
	if (aw > 0) {
		/* 64 bit: the product of two ints cannot overflow, and a client
		 * can announce any size. Rounded to nearest so that an odd
		 * extent does not always lose its pixel to the same side. */
		if (portrait)
			size = ((int64_t)usable->w * ah + aw / 2) / aw;
		else
			size = ((int64_t)usable->h * aw + ah / 2) / ah;
	} else {
		size = extent / 2;
	}

	/* A hint is only a preference: a wide clip must not squeeze the other
	 * app to nothing (nor a tall one the first), so each keeps a quarter.
	 * Rounded up, so the share is never below the percentage. */
	int64_t min = (extent * PW_TILE_MIN_PERCENT + 99) / 100;
	if (size < min)
		size = min;
	if (size > extent - min)
		size = extent - min;

	if (portrait) {
		out[0] = (struct pw_tile_box){ usable->x, usable->y, usable->w, (int)size };
		out[1] = (struct pw_tile_box){ usable->x, usable->y + (int)size,
			usable->w, usable->h - (int)size };
	} else {
		out[0] = (struct pw_tile_box){ usable->x, usable->y, (int)size, usable->h };
		out[1] = (struct pw_tile_box){ usable->x + (int)size, usable->y,
			usable->w - (int)size, usable->h };
	}
	return true;
}

int pw_tile_pan(int zone, int out_h, int lower_top)
{
	int limit = lower_top < out_h ? lower_top : out_h;

	if (zone <= 0 || limit <= 0)
		return 0;
	return zone < limit ? zone : limit;
}
