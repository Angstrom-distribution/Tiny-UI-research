/* test-cursorshape.c - unit tests for the cursor-shape-v1 mapping. */
#include <assert.h>
#include <stdio.h>
#include "../src/cursorshape.h"

int main(void)
{
	assert(pw_cursor_shape_look(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT) == PW_SHAPE_BUSY);
	assert(pw_cursor_shape_look(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS) == PW_SHAPE_BUSY);

	assert(pw_cursor_shape_look(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT) == PW_SHAPE_DEFAULT);
	assert(pw_cursor_shape_look(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT) == PW_SHAPE_DEFAULT);
	assert(pw_cursor_shape_look(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER) == PW_SHAPE_DEFAULT);
	assert(pw_cursor_shape_look(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED) == PW_SHAPE_DEFAULT);
	assert(pw_cursor_shape_look(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING) == PW_SHAPE_DEFAULT);

	/* a shape newer than this build knows about, and zero (not a valid shape) */
	assert(pw_cursor_shape_look((enum wp_cursor_shape_device_v1_shape)9999) == PW_SHAPE_DEFAULT);
	assert(pw_cursor_shape_look((enum wp_cursor_shape_device_v1_shape)0) == PW_SHAPE_DEFAULT);
	printf("ok pw_cursor_shape_look\n");

	/* a client wait request starts the animation only when none is shown,
	 * and only that client-owned animation ends on another shape or focus */
	struct pw_cursor_owner o = {0};
	assert(pw_owner_client_shape(&o, PW_SHAPE_BUSY) == PW_SHAPE_ACT_START);
	assert(o.running && o.client_busy);
	assert(pw_owner_client_shape(&o, PW_SHAPE_BUSY) == PW_SHAPE_ACT_NONE);
	assert(pw_owner_client_shape(&o, PW_SHAPE_DEFAULT) == PW_SHAPE_ACT_STOP);
	assert(!o.running && !o.client_busy);
	assert(pw_owner_client_shape(&o, PW_SHAPE_DEFAULT) == PW_SHAPE_ACT_NONE);
	assert(!pw_owner_focus_change(&o));
	assert(pw_owner_client_shape(&o, PW_SHAPE_BUSY) == PW_SHAPE_ACT_START);
	assert(pw_owner_focus_change(&o));
	assert(!o.running && !o.client_busy);

	/* a touch hold animation is never stopped by a client */
	assert(pw_owner_touch_start(&o));
	assert(o.running && !o.client_busy);
	assert(pw_owner_client_shape(&o, PW_SHAPE_BUSY) == PW_SHAPE_ACT_NONE);
	assert(pw_owner_client_shape(&o, PW_SHAPE_DEFAULT) == PW_SHAPE_ACT_NONE);
	assert(!pw_owner_focus_change(&o));
	assert(!pw_owner_touch_start(&o));   /* idempotent, no restart */
	assert(o.running);
	assert(pw_owner_stop(&o));
	assert(!pw_owner_stop(&o));

	/* the touch hold fires while a client wait animation runs: the touch
	 * takes it over, so the client can no longer end it */
	assert(pw_owner_client_shape(&o, PW_SHAPE_BUSY) == PW_SHAPE_ACT_START);
	assert(pw_owner_touch_start(&o));
	assert(o.running && !o.client_busy);
	assert(pw_owner_client_shape(&o, PW_SHAPE_DEFAULT) == PW_SHAPE_ACT_NONE);
	assert(!pw_owner_focus_change(&o));
	assert(o.running);
	printf("ok pw_cursor_owner\n");
	return 0;
}
