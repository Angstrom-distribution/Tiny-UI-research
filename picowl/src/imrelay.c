/* imrelay.c - relay between zwp_text_input_v3 (applications) and
 * zwp_input_method_v2 (the on-screen keyboard or input method engine). */
#include <stdlib.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>
#include "picowl.h"

/* One per zwp_text_input_v3 object; a client may create several. */
struct pw_ti {
	struct wl_list link;       /* R.tis */
	struct wlr_text_input_v3 *wlr;
	struct wl_listener destroy;
};

static struct {
	struct pw_server *server;
	struct wlr_text_input_manager_v3 *ti_mgr;
	struct wlr_input_method_manager_v2 *im_mgr;
	struct wl_list tis;        /* struct pw_ti */
	struct wl_listener new_text_input;
	struct wl_listener new_input_method;
	struct wl_listener focus_change;
	bool inited;
} R;

static struct wlr_surface *keyboard_focus(void)
{
	return R.server->seat->keyboard_state.focused_surface;
}

/* wlroots asserts that no listener is left on a text input when it is
 * destroyed, so everything added in handle_new_text_input goes here. */
static void ti_release(struct pw_ti *t)
{
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

static void ti_destroy(struct wl_listener *l, void *data)
{
	struct pw_ti *t = wl_container_of(l, t, destroy);
	(void)data;
	ti_release(t);
}

/* Text inputs follow the keyboard focus, per client. The state is read from
 * wlroots instead of the event: send_leave asserts a focused surface and
 * send_enter asserts none. When the focused surface is destroyed the seat's
 * destroy listener was added at keyboard enter, before the text input's own,
 * so it runs first and its focus_change(NULL) gets a leave out through here
 * while focused_surface is still set; send_leave unhooks wlroots' own
 * listener, which therefore never runs. */
static void relay_focus(struct wlr_surface *focus)
{
	struct pw_ti *t;
	struct wl_client *client = focus ? wl_resource_get_client(focus->resource) : NULL;

	wl_list_for_each(t, &R.tis, link) {
		if (t->wlr->focused_surface && t->wlr->focused_surface != focus)
			wlr_text_input_v3_send_leave(t->wlr);
	}
	wl_list_for_each(t, &R.tis, link) {
		if (client && !t->wlr->focused_surface &&
				wl_resource_get_client(t->wlr->resource) == client)
			wlr_text_input_v3_send_enter(t->wlr, focus);
	}
}

static void handle_focus_change(struct wl_listener *l, void *data)
{
	const struct wlr_seat_keyboard_focus_change_event *ev = data;
	(void)l;
	relay_focus(ev->new_surface);
}

static void handle_new_text_input(struct wl_listener *l, void *data)
{
	struct wlr_text_input_v3 *wlr = data;
	struct pw_ti *t = calloc(1, sizeof(*t));
	struct wlr_surface *focus = keyboard_focus();
	(void)l;

	if (!t) {
		/* No state, no relay; the client sees a text input that is never
		 * entered. */
		pw_log(WLR_ERROR, "text-input: out of memory");
		return;
	}
	t->wlr = wlr;
	t->destroy.notify = ti_destroy;
	wl_signal_add(&wlr->events.destroy, &t->destroy);
	wl_list_insert(&R.tis, &t->link);

	/* A text input created after its client got the focus (toolkits create
	 * it lazily) must be entered now, there is no focus change to wait for. */
	if (focus && wl_resource_get_client(wlr->resource) == wl_resource_get_client(focus->resource))
		wlr_text_input_v3_send_enter(wlr, focus);
}

static void handle_new_input_method(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
	pw_log(WLR_DEBUG, "input-method: new object");
}

void pw_im_init(struct pw_server *server)
{
	R.server = server;
	wl_list_init(&R.tis);
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
	R.focus_change.notify = handle_focus_change;
	wl_signal_add(&server->seat->keyboard_state.events.focus_change, &R.focus_change);
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
	wl_list_remove(&R.focus_change.link);
	/* Normally the clients are gone and every text input released itself;
	 * after a failed start they may not be. */
	while (!wl_list_empty(&R.tis)) {
		struct pw_ti *t = wl_container_of(R.tis.next, t, link);
		ti_release(t);
	}
	R.inited = false;
}
