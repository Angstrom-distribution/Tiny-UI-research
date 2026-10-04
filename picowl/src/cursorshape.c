/* cursorshape.c - cursor-shape-v1 shape to picowl look mapping. */
#include "cursorshape.h"

bool pw_owner_touch_start(struct pw_cursor_owner *o)
{
	bool redraw = !o->running || o->client_busy;
	o->running = true;
	o->client_busy = false;
	return redraw;
}

bool pw_owner_stop(struct pw_cursor_owner *o)
{
	bool was = o->running;
	o->running = false;
	o->client_busy = false;
	return was;
}

enum pw_shape_action pw_owner_client_shape(struct pw_cursor_owner *o,
	enum pw_shape_look look)
{
	if (look == PW_SHAPE_BUSY) {
		if (o->running)
			return PW_SHAPE_ACT_NONE;
		o->running = true;
		o->client_busy = true;
		return PW_SHAPE_ACT_START;
	}
	if (o->running && o->client_busy) {
		o->running = false;
		o->client_busy = false;
		return PW_SHAPE_ACT_STOP;
	}
	return PW_SHAPE_ACT_NONE;
}

bool pw_owner_focus_change(struct pw_cursor_owner *o)
{
	if (!o->running || !o->client_busy)
		return false;
	o->running = false;
	o->client_busy = false;
	return true;
}

enum pw_shape_look pw_cursor_shape_look(enum wp_cursor_shape_device_v1_shape shape)
{
	switch (shape) {
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT:
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS:
		return PW_SHAPE_BUSY;
	default:
		return PW_SHAPE_DEFAULT;
	}
}
