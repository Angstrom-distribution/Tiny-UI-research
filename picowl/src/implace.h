/* implace.h - where an input method popup goes. Pure geometry, no wlroots,
 * so it can be unit-tested. */
#ifndef PICOWL_IMPLACE_H
#define PICOWL_IMPLACE_H

struct pw_im_rect {
	int x, y, w, h;
};

/* Place a popup of size w x h next to the text cursor rectangle cursor, in
 * layout coordinates, inside area (the usable area of the output). The popup
 * goes below the cursor, or above it when there is no room below, and is then
 * clamped into area (the top left corner wins if it is larger than area). */
struct pw_im_rect pw_im_place(const struct pw_im_rect *cursor, int w, int h,
	const struct pw_im_rect *area);

#endif
