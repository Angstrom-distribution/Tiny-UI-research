/* grab.c - the layout origin of a surface which holds a touch or a tool. */
#include <wlr/types/wlr_scene.h>

#include "grab.h"

static void grab_node_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_grab *g = wl_container_of(l, g, destroy);

	/* the window went away under the finger: keep reporting from where it was */
	wl_list_remove(&g->destroy.link);
	g->node = NULL;
}

void pw_grab_clear(struct pw_grab *g)
{
	if (g->node)
		wl_list_remove(&g->destroy.link);
	g->node = NULL;
}

void pw_grab_set(struct pw_grab *g, struct wlr_scene_node *node, double ox, double oy)
{
	pw_grab_clear(g);
	g->ox = ox;
	g->oy = oy;
	if (!node)
		return;
	g->node = node;
	g->destroy.notify = grab_node_destroy;
	wl_signal_add(&node->events.destroy, &g->destroy);
}

void pw_grab_origin(struct pw_grab *g, double *ox, double *oy)
{
	int x, y;

	if (g->node && wlr_scene_node_coords(g->node, &x, &y)) {
		g->ox = x;
		g->oy = y;
	}
	*ox = g->ox;
	*oy = g->oy;
}
