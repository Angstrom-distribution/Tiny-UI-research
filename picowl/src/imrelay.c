/* imrelay.c - relay between zwp_text_input_v3 (applications) and
 * zwp_input_method_v2 (the on-screen keyboard or input method engine). */
#include <stdlib.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>
#include "picowl.h"

static struct {
	struct pw_server *server;
	struct wlr_text_input_manager_v3 *ti_mgr;
	struct wlr_input_method_manager_v2 *im_mgr;
	struct wl_listener new_text_input;
	struct wl_listener new_input_method;
	bool inited;
} R;

static void handle_new_text_input(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
	pw_log(WLR_DEBUG, "text-input: new object");
}

static void handle_new_input_method(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
	pw_log(WLR_DEBUG, "input-method: new object");
}

void pw_im_init(struct pw_server *server)
{
	R.server = server;
	R.ti_mgr = wlr_text_input_manager_v3_create(server->display);
	R.im_mgr = wlr_input_method_manager_v2_create(server->display);
	if (!R.ti_mgr || !R.im_mgr) {
		/* A manager without our listeners is harmless to wlroots, and
		 * clients just see fewer globals. */
		pw_log(WLR_ERROR, "cannot create the text-input or input-method manager");
		R.ti_mgr = NULL;
		R.im_mgr = NULL;
		return;
	}
	R.new_text_input.notify = handle_new_text_input;
	wl_signal_add(&R.ti_mgr->events.text_input, &R.new_text_input);
	R.new_input_method.notify = handle_new_input_method;
	wl_signal_add(&R.im_mgr->events.input_method, &R.new_input_method);
	R.inited = true;
}

void pw_im_finish(struct pw_server *server)
{
	(void)server;
	if (!R.inited)
		return;
	/* wlroots asserts at display destroy that these lists are empty. */
	wl_list_remove(&R.new_text_input.link);
	wl_list_remove(&R.new_input_method.link);
	R.inited = false;
}
