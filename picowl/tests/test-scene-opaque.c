/*
 * Test for wlroots patch 0005: a scene buffer whose buffer swap changes the
 * opacity must update the nodes below it.
 *
 * It follows what a surface commit does to the scene node of a layer surface
 * that grows (surface_reconfigure): the opaque region, then the size of the
 * node, then the buffer. The panel's bar is an opaque RGB565 buffer of 18 rows,
 * its slider row an ARGB8888 buffer of 54 rows whose opaque region is the bar:
 * the node below must be visible under the row, so that the row can blend with
 * it, and not under the bar.
 */
#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

struct tbuf {
	struct wlr_buffer base;
	uint32_t format;
	void *data;
	size_t stride;
};

static void tbuf_destroy(struct wlr_buffer *b)
{
	struct tbuf *t = wl_container_of(b, t, base);
	wlr_buffer_finish(b);
	free(t->data);
	free(t);
}

static bool tbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
		uint32_t *format, size_t *stride)
{
	(void)flags;
	struct tbuf *t = wl_container_of(b, t, base);
	*data = t->data;
	*format = t->format;
	*stride = t->stride;
	return true;
}

static void tbuf_end(struct wlr_buffer *b)
{
	(void)b;
}

static const struct wlr_buffer_impl tbuf_impl = {
	.destroy = tbuf_destroy,
	.begin_data_ptr_access = tbuf_begin,
	.end_data_ptr_access = tbuf_end,
};

static struct tbuf *tbuf_create(int w, int h, uint32_t format, int bpp)
{
	struct tbuf *t = calloc(1, sizeof(*t));
	t->format = format;
	t->stride = (size_t)w * bpp;
	t->data = calloc((size_t)w * h, bpp);
	wlr_buffer_init(&t->base, &tbuf_impl, w, h);
	return t;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static bool visible_at(struct wlr_scene_node *node, int x, int y)
{
	/* No public accessor: the member is declared as WLR_PRIVATE, which only
	 * wlroots's own build defines away. The region is what the renderer
	 * draws of the node. */
	return pixman_region32_contains_point(&node->WLR_PRIVATE.visible, x, y, NULL);
}

/* The layer surface of the panel grows from the bar to the bar and the row,
 * and shrinks again. */
int main(void)
{
	struct wlr_scene *scene = wlr_scene_create();
	float ground[4] = { 1, 0, 1, 1 };
	struct wlr_scene_rect *below = wlr_scene_rect_create(&scene->tree, 240, 320, ground);

	struct wlr_scene_buffer *sb = wlr_scene_buffer_create(&scene->tree, NULL);
	struct wlr_scene_node *node = &below->node;

	pixman_region32_t opaque;
	pixman_region32_init_rect(&opaque, 0, 0, 240, 18);

	/* the bar alone: RGB565, opaque */
	struct tbuf *bar = tbuf_create(240, 18, DRM_FORMAT_RGB565, 2);
	wlr_scene_buffer_set_opaque_region(sb, &opaque);
	wlr_scene_buffer_set_dest_size(sb, 240, 18);
	wlr_scene_buffer_set_buffer(sb, &bar->base);
	wlr_buffer_drop(&bar->base);
	CHECK(!visible_at(node, 3, 5), "the node below shows through the opaque bar");
	CHECK(visible_at(node, 3, 30), "the node below is not visible under the bar's strip");

	/* the row opens: the node grows first, the ARGB8888 buffer comes after */
	struct tbuf *row = tbuf_create(240, 54, DRM_FORMAT_ARGB8888, 4);
	wlr_scene_buffer_set_opaque_region(sb, &opaque);
	wlr_scene_buffer_set_dest_size(sb, 240, 54);
	wlr_scene_buffer_set_buffer(sb, &row->base);
	wlr_buffer_drop(&row->base);
	CHECK(!visible_at(node, 3, 5), "the node below shows through the bar of the open row");
	CHECK(visible_at(node, 3, 21),
		"the node below is culled under the translucent row (visible region stale)");
	CHECK(visible_at(node, 200, 53), "the node below is culled at the end of the row");

	/* the row closes: the opaque buffer is back */
	bar = tbuf_create(240, 18, DRM_FORMAT_RGB565, 2);
	wlr_scene_buffer_set_opaque_region(sb, &opaque);
	wlr_scene_buffer_set_dest_size(sb, 240, 18);
	wlr_scene_buffer_set_buffer(sb, &bar->base);
	wlr_buffer_drop(&bar->base);
	CHECK(!visible_at(node, 3, 5), "the node below shows through the closed bar");
	CHECK(visible_at(node, 3, 21), "the node below is not visible where the row was");

	/* a same-size swap of a buffer of the same opacity changes nothing */
	row = tbuf_create(240, 54, DRM_FORMAT_ARGB8888, 4);
	wlr_scene_buffer_set_dest_size(sb, 240, 54);
	wlr_scene_buffer_set_buffer(sb, &row->base);
	wlr_buffer_drop(&row->base);
	row = tbuf_create(240, 54, DRM_FORMAT_ARGB8888, 4);
	wlr_scene_buffer_set_buffer(sb, &row->base);
	wlr_buffer_drop(&row->base);
	CHECK(visible_at(node, 3, 21) && !visible_at(node, 3, 5),
		"the second ARGB8888 buffer changed the visible region");

	pixman_region32_fini(&opaque);
	wlr_scene_node_destroy(&scene->tree.node);

	if (failures) {
		fprintf(stderr, "test-scene-opaque: %d failures\n", failures);
		return 1;
	}
	printf("test-scene-opaque: ok\n");
	return 0;
}
