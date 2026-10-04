/* test-tile.c - unit tests for the two-window tile layout. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../src/tile.h"

static const struct pw_tile_hint none[2];

static struct pw_tile_hint fixed(int w, int h)
{
	return (struct pw_tile_hint){ .fixed_w = w, .fixed_h = h };
}

static struct pw_tile_hint aspect(int w, int h)
{
	return (struct pw_tile_hint){ .aspect_w = w, .aspect_h = h };
}

static void expect(const struct pw_tile_box *b, int x, int y, int w, int h)
{
	if (b->x != x || b->y != y || b->w != w || b->h != h)
		fprintf(stderr, "got %d,%d %dx%d expected %d,%d %dx%d\n",
			b->x, b->y, b->w, b->h, x, y, w, h);
	assert(b->x == x && b->y == y && b->w == w && b->h == h);
}

/* The slots lie inside usable, do not overlap and add up to its area. */
static void check_cover(const struct pw_tile_box *u, const struct pw_tile_box o[2])
{
	for (int i = 0; i < 2; i++) {
		assert(o[i].w > 0 && o[i].h > 0);
		assert(o[i].x >= u->x && o[i].y >= u->y);
		assert((long long)o[i].x + o[i].w <= (long long)u->x + u->w);
		assert((long long)o[i].y + o[i].h <= (long long)u->y + u->h);
	}
	assert((long long)o[0].w * o[0].h + (long long)o[1].w * o[1].h
		== (long long)u->w * u->h);
	int disjoint = o[0].x + o[0].w <= o[1].x || o[1].x + o[1].w <= o[0].x
		|| o[0].y + o[0].h <= o[1].y || o[1].y + o[1].h <= o[0].y;
	assert(disjoint);
}

static void test_even_split(void)
{
	struct pw_tile_box o[2];
	struct pw_tile_box land = { 0, 0, 1280, 720 }, port = { 0, 0, 240, 320 };

	assert(pw_tile_layout(&land, none, o));
	expect(&o[0], 0, 0, 640, 720);
	expect(&o[1], 640, 0, 640, 720);

	assert(pw_tile_layout(&port, none, o));
	expect(&o[0], 0, 0, 240, 160);
	expect(&o[1], 0, 160, 240, 160);

	/* square counts as portrait */
	struct pw_tile_box sq = { 0, 0, 200, 200 };
	assert(pw_tile_layout(&sq, none, o));
	expect(&o[0], 0, 0, 200, 100);

	/* odd extent: the second window takes the spare pixel */
	struct pw_tile_box odd = { 0, 0, 241, 321 };
	assert(pw_tile_layout(&odd, none, o));
	expect(&o[0], 0, 0, 241, 160);
	expect(&o[1], 0, 160, 241, 161);
	check_cover(&odd, o);
}

static void test_fixed_hint(void)
{
	struct pw_tile_box o[2];
	struct pw_tile_hint h[2] = { fixed(320, 240), {0} };

	/* portrait: height = width * h / w */
	struct pw_tile_box port = { 0, 0, 240, 320 };
	assert(pw_tile_layout(&port, h, o));
	expect(&o[0], 0, 0, 240, 180);
	expect(&o[1], 0, 180, 240, 140);

	/* landscape: width = height * w / h, 720 * 300 / 300 */
	struct pw_tile_box land = { 0, 0, 1280, 720 };
	h[0] = fixed(300, 300);
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 720, 720);
	expect(&o[1], 720, 0, 560, 720);

	/* origin offset (panel on top, second output) */
	struct pw_tile_box off = { 10, 20, 240, 300 };
	h[0] = fixed(320, 240);
	assert(pw_tile_layout(&off, h, o));
	expect(&o[0], 10, 20, 240, 180);
	expect(&o[1], 10, 200, 240, 120);
	check_cover(&off, o);
}

static void test_priority(void)
{
	struct pw_tile_box o[2];
	struct pw_tile_box land = { 0, 0, 1000, 500 };

	/* the client's hint beats the config */
	struct pw_tile_hint h[2] = { fixed(1, 1), {0} };
	h[0].aspect_w = 1;
	h[0].aspect_h = 2;
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 500, 500);

	/* only one of the two numbers: the hint is incomplete, config wins */
	h[0] = (struct pw_tile_hint){ .fixed_w = 100, .fixed_h = 0,
		.aspect_w = 1, .aspect_h = 2 };
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 250, 500);

	/* config alone */
	h[0] = aspect(3, 4);
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 375, 500);

	/* the second window's hint changes nothing */
	h[1] = fixed(1, 1);
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 375, 500);
	expect(&o[1], 375, 0, 625, 500);

	/* non-positive values count as unknown: an even split of 1000 */
	h[0] = (struct pw_tile_hint){ -5, -5, -1, 2 };
	h[1] = (struct pw_tile_hint){0};
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 500, 500);
	h[0] = (struct pw_tile_hint){ 0, 0, 0, 0 };
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 500, 500);
}

static void test_clamp(void)
{
	struct pw_tile_box o[2];
	struct pw_tile_box land = { 0, 0, 1280, 720 }, port = { 0, 0, 240, 320 };

	/* 2:1 would want 1440 of 1280: the second keeps 25 percent */
	struct pw_tile_hint h[2] = { fixed(400, 200), {0} };
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 960, 720);
	expect(&o[1], 960, 0, 320, 720);

	/* a very tall clip in landscape: the first keeps 25 percent */
	h[0] = fixed(1, 100);
	assert(pw_tile_layout(&land, h, o));
	expect(&o[0], 0, 0, 320, 720);
	expect(&o[1], 320, 0, 960, 720);

	/* portrait, wide clip: 16:9 at 240 wide is 135 high, inside the limits */
	h[0] = fixed(16, 9);
	assert(pw_tile_layout(&port, h, o));
	expect(&o[0], 0, 0, 240, 135);
	/* portrait, tall clip wants 1280 of 320 */
	h[0] = fixed(3, 16);
	assert(pw_tile_layout(&port, h, o));
	expect(&o[0], 0, 0, 240, 240);
	expect(&o[1], 0, 240, 240, 80);

	/* the minimum is rounded up: a quarter of 10 is 3 */
	struct pw_tile_box small = { 0, 0, 5, 10 };
	h[0] = fixed(100, 1);
	assert(pw_tile_layout(&small, h, o));
	expect(&o[0], 0, 0, 5, 3);
	expect(&o[1], 0, 3, 5, 7);
	h[0] = fixed(1, 100);
	assert(pw_tile_layout(&small, h, o));
	expect(&o[0], 0, 0, 5, 7);
	expect(&o[1], 0, 7, 5, 3);
}

static void test_degenerate(void)
{
	struct pw_tile_box o[2], mark = { 7, 7, 7, 7 };
	struct pw_tile_box u;

	u = (struct pw_tile_box){ 0, 0, 0, 100 };
	o[0] = o[1] = mark;
	assert(!pw_tile_layout(&u, none, o));
	assert(!memcmp(&o[0], &mark, sizeof(mark)));
	assert(!memcmp(&o[1], &mark, sizeof(mark)));
	u = (struct pw_tile_box){ 0, 0, 100, 0 };
	assert(!pw_tile_layout(&u, none, o));
	u = (struct pw_tile_box){ 0, 0, -5, 100 };
	assert(!pw_tile_layout(&u, none, o));
	u = (struct pw_tile_box){ 0, 0, 1, 1 };
	assert(!pw_tile_layout(&u, none, o));
	assert(!pw_tile_layout(NULL, none, o));
	assert(!pw_tile_layout(&u, NULL, o));

	/* two pixels is the smallest area which fits two windows */
	u = (struct pw_tile_box){ 0, 0, 1, 2 };
	assert(pw_tile_layout(&u, none, o));
	expect(&o[0], 0, 0, 1, 1);
	expect(&o[1], 0, 1, 1, 1);
	u = (struct pw_tile_box){ 0, 0, 3, 2 };
	assert(pw_tile_layout(&u, none, o));
	check_cover(&u, o);

	/* huge hints and areas: no overflow, still a valid cover */
	struct pw_tile_hint h[2] = { fixed(INT_MAX, 1), {0} };
	u = (struct pw_tile_box){ 0, 0, INT_MAX, INT_MAX };
	assert(pw_tile_layout(&u, h, o));
	check_cover(&u, o);
	h[0] = fixed(1, INT_MAX);
	assert(pw_tile_layout(&u, h, o));
	check_cover(&u, o);
	h[0] = fixed(INT_MAX, INT_MAX);
	assert(pw_tile_layout(&u, h, o));
	check_cover(&u, o);
	u = (struct pw_tile_box){ 0, 0, 1000, 100 };
	h[0] = fixed(INT_MAX, 1);
	assert(pw_tile_layout(&u, h, o));
	expect(&o[0], 0, 0, 750, 100);
	h[0] = fixed(1, INT_MAX);
	assert(pw_tile_layout(&u, h, o));
	expect(&o[0], 0, 0, 250, 100);
}

/* Exact coverage over many shapes and hints. */
static void test_sweep(void)
{
	static const int sizes[] = { 2, 3, 4, 5, 7, 16, 100, 240, 241, 320, 480, 640, 641, 720, 1280 };
	static const int ratios[] = { 1, 2, 3, 4, 9, 16, 4096 };
	int n = sizeof(sizes) / sizeof(sizes[0]);
	int r = sizeof(ratios) / sizeof(ratios[0]);

	for (int i = 0; i < n; i++)
	for (int j = 0; j < n; j++)
	for (int a = -1; a < r; a++)
	for (int b = -1; b < r; b++)
	for (int k = 0; k < 2; k++) {
		struct pw_tile_box u = { 3, 5, sizes[i], sizes[j] }, o[2];
		struct pw_tile_hint h[2] = {{0}, {0}};
		if (a >= 0 && b >= 0) {
			if (k)
				h[0] = fixed(ratios[a], ratios[b]);
			else
				h[0] = aspect(ratios[a], ratios[b]);
		}
		assert(pw_tile_layout(&u, h, o));
		check_cover(&u, o);
		/* both windows keep a quarter of the split axis, and span the other */
		if (u.h >= u.w) {
			assert(o[0].w == u.w && o[1].w == u.w);
			assert(o[0].h * 4 >= u.h && o[1].h * 4 >= u.h);
		} else {
			assert(o[0].h == u.h && o[1].h == u.h);
			assert(o[0].w * 4 >= u.w && o[1].w * 4 >= u.w);
		}
	}
}

static void test_pan(void)
{
	/* 240x320 split evenly: the lower window starts at 160 */
	assert(pw_tile_pan(100, 320, 160) == 100);
	/* no zone, no pan */
	assert(pw_tile_pan(0, 320, 160) == 0);
	assert(pw_tile_pan(-5, 320, 160) == 0);
	/* the lower window stops at the top of the screen, never above it */
	assert(pw_tile_pan(250, 320, 160) == 160);
	assert(pw_tile_pan(160, 320, 160) == 160);
	assert(pw_tile_pan(161, 320, 160) == 160);
	assert(pw_tile_pan(159, 320, 160) == 159);
	/* a lower window already at the top (or an unknown position) cannot move */
	assert(pw_tile_pan(100, 320, 0) == 0);
	assert(pw_tile_pan(100, 320, -20) == 0);
	/* a zone bigger than the output counts as the output */
	assert(pw_tile_pan(5000, 320, 5000) == 320);
	/* a lower window which starts below the output is limited by the output */
	assert(pw_tile_pan(400, 320, 330) == 320);
	assert(pw_tile_pan(1, 1, 1) == 1);
	assert(pw_tile_pan(INT_MAX, INT_MAX, INT_MAX) == INT_MAX);
}

int main(void)
{
	test_even_split();
	test_fixed_hint();
	test_priority();
	test_clamp();
	test_degenerate();
	test_sweep();
	test_pan();
	printf("test-tile: ok\n");
	return 0;
}
