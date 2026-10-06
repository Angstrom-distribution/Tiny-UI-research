/* test-scenehit.c - hit testing in a panned stack.
 *
 * picowl pans a tiled stack by moving the scene nodes of the two windows, and
 * input finds the surface under a point with wlr_scene_node_at() (src/input.c,
 * surface_at), whose surface-local coordinates come from the node positions.
 * This builds the scene the way view_arrange() leaves it for a 240x320 output
 * with a 100 row keyboard, using the slots and the pan of tile.c, and checks
 * what is under the points that matter. It tests the contract picowl relies on
 * (wlroots computes the local coordinates from the moved node), not a picowl
 * function: there is no way to inject a pointer or a touch into a headless
 * picowl, so no client sees a click in the end to end tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include "../src/tile.h"

static void buf_destroy(struct wlr_buffer *b)
{
	free(b);
}

static const struct wlr_buffer_impl buf_impl = { .destroy = buf_destroy };

/* A tree at (x, y) holding one buffer node of w x h. */
static struct wlr_scene_tree *window(struct wlr_scene_tree *parent, int x, int y, int w, int h)
{
	struct wlr_buffer *b = calloc(1, sizeof(*b));
	struct wlr_scene_tree *t = wlr_scene_tree_create(parent);

	assert(b && t);
	wlr_buffer_init(b, &buf_impl, w, h);
	assert(wlr_scene_buffer_create(t, b));
	wlr_buffer_drop(b);
	wlr_scene_node_set_position(&t->node, x, y);
	return t;
}

static struct wlr_scene_node *at(struct wlr_scene *scene, double x, double y,
	double *sx, double *sy)
{
	struct wlr_scene_node *n = wlr_scene_node_at(&scene->tree.node, x, y, sx, sy);

	return n && n->type == WLR_SCENE_NODE_BUFFER ? n : NULL;
}

int main(void)
{
	const int W = 240, H = 320, zone = 100;
	struct pw_tile_box area = { 0, 0, W, H }, slot[2];
	struct pw_tile_hint hints[2] = {{0}, {0}};

	assert(pw_tile_layout(&area, hints, PW_TILE_SECOND_MIN_DEFAULT, slot));
	int pan = pw_tile_pan(zone, H, slot[1].y);
	assert(pan == zone);

	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_tree *apps = wlr_scene_tree_create(&scene->tree);
	struct wlr_scene_tree *overlay = wlr_scene_tree_create(&scene->tree);
	struct wlr_scene_tree *a = window(apps, slot[0].x, slot[0].y - pan, slot[0].w, slot[0].h);
	struct wlr_scene_tree *b = window(apps, slot[1].x, slot[1].y - pan, slot[1].w, slot[1].h);
	struct wlr_scene_tree *kbd = window(overlay, 0, H - zone, W, zone);
	struct wlr_scene_node *n;
	double sx, sy;

	/* the keyboard answers where it is drawn, over the lower window */
	n = at(scene, 120, 250, &sx, &sy);
	assert(n && n->parent == kbd && sx == 120 && sy == 30);
	n = at(scene, 120, H - zone, &sx, &sy);
	assert(n && n->parent == kbd && sy == 0);

	/* the lower window ends where the keyboard starts: its last row is 219,
	 * and the touch at 200 lands at row 140 of the window (200 - (160 - 100)) */
	n = at(scene, 30, 219, &sx, &sy);
	assert(n && n->parent == b && sx == 30 && sy == 159);
	n = at(scene, 120, 200, &sx, &sy);
	assert(n && n->parent == b && sx == 120 && sy == 140);
	/* its first row is on screen at 60, not at its unpanned 160 */
	n = at(scene, 120, 60, &sx, &sy);
	assert(n && n->parent == b && sy == 0);

	/* the upper window shows its lower 60 rows: screen row 0 is its row 100,
	 * row 59 its row 159 */
	n = at(scene, 5, 0, &sx, &sy);
	assert(n && n->parent == a && sx == 5 && sy == 100);
	n = at(scene, 5, 59, &sx, &sy);
	assert(n && n->parent == a && sy == 159);

	/* the points of the unpanned layout no longer mean what they did: 100 is
	 * the lower window now, it was the upper one */
	n = at(scene, 120, 100, &sx, &sy);
	assert(n && n->parent == b && sy == 40);

	/* back where it was: the keyboard is gone and both nodes return */
	wlr_scene_node_set_position(&a->node, slot[0].x, slot[0].y);
	wlr_scene_node_set_position(&b->node, slot[1].x, slot[1].y);
	wlr_scene_node_set_enabled(&kbd->node, false);
	n = at(scene, 120, 100, &sx, &sy);
	assert(n && n->parent == a && sy == 100);
	n = at(scene, 120, 250, &sx, &sy);
	assert(n && n->parent == b && sy == 90);

	wlr_scene_node_destroy(&scene->tree.node);
	printf("test-scenehit: ok\n");
	return 0;
}
