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
	struct wl_listener enable, commit, disable, destroy;
};

/* The one input method; a second one is refused. */
struct pw_im {
	struct wlr_input_method_v2 *wlr;
	struct wl_listener commit, grab_keyboard, destroy;
};

static struct {
	struct pw_server *server;
	struct wlr_text_input_manager_v3 *ti_mgr;
	struct wlr_input_method_manager_v2 *im_mgr;
	struct wl_list tis;        /* struct pw_ti */
	struct pw_im *im;          /* NULL: no input method connected */
	struct pw_ti *active;      /* the text input the input method works on */
	struct wl_listener new_text_input;
	struct wl_listener new_input_method;
	struct wl_listener focus_change;
	bool inited;
} R;

static struct wlr_surface *keyboard_focus(void)
{
	return R.server->seat->keyboard_state.focused_surface;
}

/* Only a text input whose surface holds the keyboard focus may drive the
 * input method. */
static bool ti_focused(const struct pw_ti *t)
{
	return t->wlr->focused_surface && t->wlr->focused_surface == keyboard_focus();
}

/* Send what the text input told us in its last commit. Features the client
 * did not use when it enabled are not sent, they would be made-up values. */
static void im_send_state(const struct pw_ti *t)
{
	const struct wlr_text_input_v3 *w = t->wlr;
	struct wlr_input_method_v2 *im = R.im->wlr;

	if (w->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
		wlr_input_method_v2_send_surrounding_text(im, w->current.surrounding.text,
			w->current.surrounding.cursor, w->current.surrounding.anchor);
	wlr_input_method_v2_send_text_change_cause(im, w->current.text_change_cause);
	if (w->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
		wlr_input_method_v2_send_content_type(im, w->current.content_type.hint,
			w->current.content_type.purpose);
	wlr_input_method_v2_send_done(im);
}

static void relay_deactivate(void)
{
	if (!R.active)
		return;
	if (R.im) {
		wlr_input_method_v2_send_deactivate(R.im->wlr);
		wlr_input_method_v2_send_done(R.im->wlr);
	}
	R.active = NULL;
}

/* At most one text input is active: taking over from another one is a
 * deactivate and done first, so the input method resets its state. R.active
 * is set before the events go out because send_done may run code that asks. */
static void relay_activate(struct pw_ti *t)
{
	if (!R.im)
		return;
	relay_deactivate();
	R.active = t;
	wlr_input_method_v2_send_activate(R.im->wlr);
	im_send_state(t);
}

/* Bring the input method in line with the focus and the enabled state of the
 * text inputs: drop an active one that lost the focus, then pick one that is
 * enabled and focused. wlroots keeps current_enabled across leave and enter,
 * so a text input that enabled before the input method connected or before
 * it got the focus is found here. */
static void relay_reeval(void)
{
	struct pw_ti *t;

	if (R.active && !ti_focused(R.active))
		relay_deactivate();
	if (R.active || !R.im)
		return;
	wl_list_for_each(t, &R.tis, link) {
		if (t->wlr->current_enabled && ti_focused(t)) {
			relay_activate(t);
			return;
		}
	}
}

static void ti_enable(struct wl_listener *l, void *data)
{
	struct pw_ti *t = wl_container_of(l, t, enable);
	(void)data;
	/* Without focus the enable stays on record and relay_reeval picks it up
	 * on enter. */
	if (ti_focused(t))
		relay_activate(t);
}

static void ti_commit(struct wl_listener *l, void *data)
{
	struct pw_ti *t = wl_container_of(l, t, commit);
	(void)data;
	if (R.active == t && R.im)
		im_send_state(t);
}

static void ti_disable(struct wl_listener *l, void *data)
{
	struct pw_ti *t = wl_container_of(l, t, disable);
	(void)data;
	if (R.active == t)
		relay_deactivate();
}

/* wlroots asserts that no listener is left on a text input when it is
 * destroyed, so everything added in handle_new_text_input goes here. */
static void ti_release(struct pw_ti *t)
{
	if (R.active == t)
		R.active = NULL;
	wl_list_remove(&t->enable.link);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->disable.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

static void ti_destroy(struct wl_listener *l, void *data)
{
	struct pw_ti *t = wl_container_of(l, t, destroy);
	(void)data;
	/* The application is gone or dropped the object: the input method must
	 * not keep a keyboard up for it. */
	if (R.active == t)
		relay_deactivate();
	ti_release(t);
	/* Another enabled text input of the same client may be waiting for its
	 * turn, and no focus change or commit is due to wake it up. */
	relay_reeval();
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
	relay_reeval();
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
	t->enable.notify = ti_enable;
	wl_signal_add(&wlr->events.enable, &t->enable);
	t->commit.notify = ti_commit;
	wl_signal_add(&wlr->events.commit, &t->commit);
	t->disable.notify = ti_disable;
	wl_signal_add(&wlr->events.disable, &t->disable);
	t->destroy.notify = ti_destroy;
	wl_signal_add(&wlr->events.destroy, &t->destroy);
	wl_list_insert(&R.tis, &t->link);

	/* A text input created after its client got the focus (toolkits create
	 * it lazily) must be entered now, there is no focus change to wait for. */
	if (focus && wl_resource_get_client(wlr->resource) == wl_resource_get_client(focus->resource))
		wlr_text_input_v3_send_enter(wlr, focus);
}

/* The input method answers with what to put into the application. Order of
 * the requests on the wire does not matter, text-input applies them in its
 * own order when done arrives. */
static void im_commit(struct wl_listener *l, void *data)
{
	struct wlr_input_method_v2 *im = data;
	const struct wlr_input_method_v2_state *s = &im->current;
	struct pw_ti *t = R.active;
	(void)l;

	/* A late commit for a text input that lost the focus is dropped, the
	 * application must never get text for a field it left. */
	if (!t || !ti_focused(t))
		return;
	if (s->delete.before_length || s->delete.after_length)
		wlr_text_input_v3_send_delete_surrounding_text(t->wlr,
			s->delete.before_length, s->delete.after_length);
	if (s->commit_text)
		wlr_text_input_v3_send_commit_string(t->wlr, s->commit_text);
	if (s->preedit.text)
		wlr_text_input_v3_send_preedit_string(t->wlr, s->preedit.text,
			s->preedit.cursor_begin, s->preedit.cursor_end);
	wlr_text_input_v3_send_done(t->wlr);
}

/* A key grab is not implemented (the on-screen keyboard injects keys through
 * virtual-keyboard). Leaving the object alive would make the client wait for
 * keys that never come, so it is destroyed and left inert. */
static void im_grab_keyboard(struct wl_listener *l, void *data)
{
	(void)l;
	pw_log(WLR_INFO, "input-method: keyboard grab is not supported, ignored");
	wlr_input_method_keyboard_grab_v2_destroy(data);
}

/* wlroots asserts that no listener is left on an input method when it is
 * destroyed, so everything added in handle_new_input_method goes here. */
static void im_release(struct pw_im *im)
{
	wl_list_remove(&im->commit.link);
	wl_list_remove(&im->grab_keyboard.link);
	wl_list_remove(&im->destroy.link);
	if (R.im == im)
		R.im = NULL;
	free(im);
}

static void im_destroy(struct wl_listener *l, void *data)
{
	struct pw_im *im = wl_container_of(l, im, destroy);
	(void)data;
	/* The client is gone, nothing to deactivate. The text inputs keep their
	 * enabled state and a new input method picks them up. */
	R.active = NULL;
	im_release(im);
}

static void handle_new_input_method(struct wl_listener *l, void *data)
{
	struct wlr_input_method_v2 *wlr = data;
	struct pw_im *im;
	(void)l;

	if (R.im) {
		/* One input method per seat; the protocol's answer to the second
		 * is unavailable. This destroys wlr. */
		pw_log(WLR_INFO, "input-method: already taken, refusing a second one");
		wlr_input_method_v2_send_unavailable(wlr);
		return;
	}
	im = calloc(1, sizeof(*im));
	if (!im) {
		pw_log(WLR_ERROR, "input-method: out of memory");
		wlr_input_method_v2_send_unavailable(wlr);
		return;
	}
	im->wlr = wlr;
	im->commit.notify = im_commit;
	wl_signal_add(&wlr->events.commit, &im->commit);
	im->grab_keyboard.notify = im_grab_keyboard;
	wl_signal_add(&wlr->events.grab_keyboard, &im->grab_keyboard);
	im->destroy.notify = im_destroy;
	wl_signal_add(&wlr->events.destroy, &im->destroy);
	R.im = im;
	relay_reeval();
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
	/* Normally the clients are gone and every object released itself; after
	 * a failed start they may not be. */
	if (R.im)
		im_release(R.im);
	while (!wl_list_empty(&R.tis)) {
		struct pw_ti *t = wl_container_of(R.tis.next, t, link);
		ti_release(t);
	}
	R.inited = false;
}
