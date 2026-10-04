#include "implace.h"

struct pw_im_rect pw_im_place(const struct pw_im_rect *cursor, int w, int h,
	const struct pw_im_rect *area)
{
	struct pw_im_rect p = { cursor->x, cursor->y + cursor->h, w, h };

	/* Above only when below does not fit: covering the line being typed on
	 * is worse than covering the line below it. */
	if (p.y + h > area->y + area->h)
		p.y = cursor->y - h;
	if (p.x + w > area->x + area->w)
		p.x = area->x + area->w - w;
	if (p.y + h > area->y + area->h)
		p.y = area->y + area->h - h;
	if (p.x < area->x)
		p.x = area->x;
	if (p.y < area->y)
		p.y = area->y;
	return p;
}
