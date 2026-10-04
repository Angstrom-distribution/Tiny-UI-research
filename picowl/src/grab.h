#ifndef PW_GRAB_H
#define PW_GRAB_H

#include <wayland-server-core.h>

struct wlr_scene_node;

/* The surface which keeps a touch or a tablet tool after the press, and where
 * its origin is. The origin is read from the scene node every time, because
 * the stack pans and windows are re-arranged while the finger is down, and a
 * client told cursor - (origin at press time) would see the point jump. */
struct pw_grab {
	struct wlr_scene_node *node;	/* NULL: no node, or it was destroyed */
	double ox, oy;			/* layout origin, the last one known */
	struct wl_listener destroy;
};

/* node is the scene node the press hit (NULL when there is none, as under a
 * lease); ox, oy is its layout origin at the press. */
void pw_grab_set(struct pw_grab *g, struct wlr_scene_node *node, double ox, double oy);
void pw_grab_clear(struct pw_grab *g);

/* The current layout origin of the grabbed surface. */
void pw_grab_origin(struct pw_grab *g, double *ox, double *oy);

#endif
