/* test-grab.c - the origin of a touch-grabbed surface follows its scene node.
 *
 * input.c reports touch motion in the coordinates of the surface which got the
 * press, as cursor - origin. A pan moves the node while the finger is down, so
 * the origin must come from the node on every motion, not from the press. What
 * is not covered here is the wiring in input.c and tablet.c (a headless picowl
 * cannot be given a touch): that the motion code calls pw_grab_origin() is
 * checked by reading. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include "../src/grab.h"

static void buf_destroy(struct wlr_buffer *b)
{
	free(b);
}

static const struct wlr_buffer_impl buf_impl = { .destroy = buf_destroy };

int main(void)
{
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_tree *t = wlr_scene_tree_create(&scene->tree);
	struct wlr_buffer *b = calloc(1, sizeof(*b));
	double ox, oy, sx, sy;

	assert(b);
	wlr_buffer_init(b, &buf_impl, 240, 160);
	struct wlr_scene_buffer *sb = wlr_scene_buffer_create(t, b);
	wlr_buffer_drop(b);
	wlr_scene_node_set_position(&t->node, 0, 160);

	/* press at (30, 200): the node is found there, local point (30, 40) */
	struct wlr_scene_node *n = wlr_scene_node_at(&scene->tree.node, 30, 200, &sx, &sy);
	assert(n == &sb->node && sx == 30 && sy == 40);
	struct pw_grab g = {0};
	pw_grab_set(&g, n, 30 - sx, 200 - sy);
	pw_grab_origin(&g, &ox, &oy);
	assert(ox == 0 && oy == 160);

	/* the stack pans 100 rows up with the finger down, the finger stays: the
	 * client must see local y 140, which a press-time origin would give as 40 */
	wlr_scene_node_set_position(&t->node, 0, 60);
	pw_grab_origin(&g, &ox, &oy);
	assert(ox == 0 && oy == 60);
	assert(200 - oy == 140);

	/* and back */
	wlr_scene_node_set_position(&t->node, 0, 160);
	pw_grab_origin(&g, &ox, &oy);
	assert(oy == 160);

	/* the window closes under the finger: the last origin is kept */
	wlr_scene_node_set_position(&t->node, 0, 60);
	pw_grab_origin(&g, &ox, &oy);
	wlr_scene_node_destroy(&t->node);
	assert(g.node == NULL);
	pw_grab_origin(&g, &ox, &oy);
	assert(ox == 0 && oy == 60);

	/* no node (lease): the stored origin */
	pw_grab_set(&g, NULL, 7, 9);
	pw_grab_origin(&g, &ox, &oy);
	assert(ox == 7 && oy == 9);
	pw_grab_clear(&g);

	/* a clear before the node dies detaches the listener */
	struct wlr_scene_tree *t2 = wlr_scene_tree_create(&scene->tree);
	pw_grab_set(&g, &t2->node, 0, 0);
	pw_grab_clear(&g);
	wlr_scene_node_destroy(&t2->node);

	wlr_scene_node_destroy(&scene->tree.node);
	printf("test-grab: ok\n");
	return 0;
}
