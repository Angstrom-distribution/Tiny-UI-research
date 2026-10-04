/* test-implace.c - unit tests for the input method popup placement. */
#include <assert.h>
#include <stdio.h>
#include "../src/implace.h"

static void check(struct pw_im_rect r, int x, int y)
{
	if (r.x != x || r.y != y)
		fprintf(stderr, "got %d,%d expected %d,%d\n", r.x, r.y, x, y);
	assert(r.x == x && r.y == y);
}

int main(void)
{
	const struct pw_im_rect area = { 0, 0, 320, 240 };
	struct pw_im_rect cur = { 100, 50, 2, 16 };

	/* Below the cursor when it fits. */
	check(pw_im_place(&cur, 80, 40, &area), 100, 66);

	/* No room below: above the cursor, not over it. */
	cur = (struct pw_im_rect){ 100, 200, 2, 16 };
	check(pw_im_place(&cur, 80, 40, &area), 100, 160);

	/* Too far right: pushed back inside. */
	cur = (struct pw_im_rect){ 300, 50, 2, 16 };
	check(pw_im_place(&cur, 80, 40, &area), 240, 66);

	/* An area that does not start at the origin (panel on top, second output). */
	const struct pw_im_rect off = { 320, 20, 320, 200 };
	cur = (struct pw_im_rect){ 330, 25, 2, 16 };
	check(pw_im_place(&cur, 80, 40, &off), 330, 41);
	/* Cursor left of the area (scrolled out of view): clamped to its edge. */
	cur = (struct pw_im_rect){ 10, 25, 2, 16 };
	check(pw_im_place(&cur, 80, 40, &off), 320, 41);

	/* Neither above nor below fits (rotated, tiny area): top of the area. */
	const struct pw_im_rect small = { 0, 0, 240, 50 };
	cur = (struct pw_im_rect){ 10, 20, 2, 16 };
	check(pw_im_place(&cur, 80, 60, &small), 10, 0);

	/* Wider than the area: the left edge wins. */
	cur = (struct pw_im_rect){ 100, 50, 2, 16 };
	check(pw_im_place(&cur, 400, 40, &area), 0, 66);

	printf("ok pw_im_place\n");
	return 0;
}
