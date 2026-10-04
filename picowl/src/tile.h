/* tile.h - the two-window tiled layout. Pure geometry, no wlroots, so it can
 * be unit-tested. */
#ifndef PICOWL_TILE_H
#define PICOWL_TILE_H

#include <stdbool.h>

struct pw_tile_box {
	int x, y, w, h;
};

/* What is known about one window's natural shape. Values <= 0 mean unknown. */
struct pw_tile_hint {
	int fixed_w, fixed_h;   /* client min_size == max_size, both used or none */
	int aspect_w, aspect_h; /* configured [app.<id>] aspect */
};

/* Minimum share of the split axis each window keeps, in percent. */
#define PW_TILE_MIN_PERCENT 25

/* Split usable into a first and a second slot. A portrait area (height >=
 * width) stacks first above second at full width, a landscape one puts first
 * left of second at full height. The first window's extent along the split
 * axis follows the aspect ratio of its fixed size hint, else of its configured
 * aspect, else it is half; it is clamped so that both windows keep at least
 * PW_TILE_MIN_PERCENT of the extent. The second window gets the rest, so the
 * slots cover usable exactly. hints[1] is accepted for symmetry and does not
 * influence the result. Returns false (out untouched) if usable is too small
 * to give both windows one pixel. */
bool pw_tile_layout(const struct pw_tile_box *usable,
	const struct pw_tile_hint hints[2], struct pw_tile_box out[2]);

#endif
