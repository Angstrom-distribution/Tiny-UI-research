/* cursorshape.h - what picowl can show for a cursor-shape-v1 request. */
#ifndef PW_CURSORSHAPE_H
#define PW_CURSORSHAPE_H

#include <stdbool.h>
#include "cursor-shape-v1-protocol.h"

/* picowl draws no pointer, only the wait animation, so two looks exist. */
enum pw_shape_look {
	PW_SHAPE_DEFAULT,  /* no image: the same as ignoring wl_pointer.set_cursor */
	PW_SHAPE_BUSY,     /* the hold animation */
};

/* Unknown (newer) shapes map to PW_SHAPE_DEFAULT. */
enum pw_shape_look pw_cursor_shape_look(enum wp_cursor_shape_device_v1_shape shape);

/* Who owns the animation on screen. A touch hold owns it for as long as the
 * touch state machine says; a client wait request only gets it when nothing
 * is showing. Kept apart from the wlroots calls so the rules can be tested. */
struct pw_cursor_owner {
	bool running;
	bool client_busy;   /* the running animation was asked for by a client */
};

enum pw_shape_action {
	PW_SHAPE_ACT_NONE,
	PW_SHAPE_ACT_START,
	PW_SHAPE_ACT_STOP,
};

/* The touch state machine starts the animation. Returns whether it has to be
 * drawn from frame 0: false only if a touch already owns the running one. A
 * client-owned animation is taken over, so a later client request or focus
 * change can no longer stop it. */
bool pw_owner_touch_start(struct pw_cursor_owner *o);

/* The touch state machine stops the animation. Returns whether one was
 * running and has to be taken off the screen. */
bool pw_owner_stop(struct pw_cursor_owner *o);

/* A client with pointer (or tablet tool) focus asked for a shape. */
enum pw_shape_action pw_owner_client_shape(struct pw_cursor_owner *o,
	enum pw_shape_look look);

/* The pointer focus moved. Returns whether a client's animation must stop. */
bool pw_owner_focus_change(struct pw_cursor_owner *o);

#endif
