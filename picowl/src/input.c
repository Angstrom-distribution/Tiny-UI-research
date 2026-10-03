/*
 * input.c - seat, keyboards, touch/pointer, keybindings.
 *
 * Touch is converted to pointer events for clients (single tracked touch id).
 * No xcursor manager is created and no cursor image is ever set: the devices
 * are stylus driven and the compositor draws no cursor.
 */
#include "picowl.h"

#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <xkbcommon/xkbcommon.h>

#define PW_MOD_MASK (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | \
                     WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)
#define PW_KEY_MAX 768
#define PW_NO_TOUCH (-1)

struct pw_input_state {
	struct pw_server *server;
	int32_t touch_id;              /* tracked touch point or PW_NO_TOUCH */
	bool touch_swallowed;          /* tracked touch only woke the display */
	int n_pointers, n_touch;
	uint8_t swallowed[PW_KEY_MAX / 8]; /* keycodes whose release is dropped */
	uint32_t btn_swallowed;        /* bit n: release of BTN_LEFT+n is dropped */
	struct xkb_keymap *keymap;     /* shared by all physical keyboards */
	bool frame_pending;            /* pointer events sent since last frame */
	double grab_ox, grab_oy;       /* layout origin of the touch-grabbed surface */

	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_abs;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener touch_down;
	struct wl_listener touch_up;
	struct wl_listener touch_motion;
	struct wl_listener touch_cancel;
	struct wl_listener touch_frame;
};

static struct pw_input_state st;

struct pw_pointer_dev { struct wl_listener destroy; };
struct pw_touch_dev { struct wl_listener destroy; };

/* ---- helpers ---------------------------------------------------------- */

static void swallow_set(uint32_t code, bool on)
{
	if (code >= PW_KEY_MAX)
		return;
	if (on)
		st.swallowed[code / 8] |= 1u << (code % 8);
	else
		st.swallowed[code / 8] &= ~(1u << (code % 8));
}

static bool swallow_get(uint32_t code)
{
	return code < PW_KEY_MAX && (st.swallowed[code / 8] & (1u << (code % 8)));
}

/* Report activity; returns true if the display was blanked and the event
 * must be dropped. */
static bool activity(struct pw_server *server)
{
	bool was_blanked = server->blanked;
	pw_idle_activity(server);
	return was_blanked;
}

static void update_capabilities(struct pw_server *server)
{
	uint32_t caps = 0;
	if (!wl_list_empty(&server->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	if (st.n_pointers > 0 || st.n_touch > 0)
		caps |= WL_SEAT_CAPABILITY_POINTER;
	wlr_seat_set_capabilities(server->seat, caps);
}

/* Surface under layout point; fills surface-local coords. */
static struct wlr_surface *surface_at(struct pw_server *server, double lx,
	double ly, double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
		wlr_scene_buffer_from_node(node));
	return ss ? ss->surface : NULL;
}

static void focus_surface(struct pw_server *server, struct wlr_surface *surface)
{
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);

	struct wlr_xdg_toplevel *top = wlr_xdg_toplevel_try_from_wlr_surface(root);
	if (top) {
		struct pw_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->xdg_toplevel == top) {
				if (view != server->focused_view)
					pw_view_focus(view);
				return;
			}
		}
		return;
	}

	struct wlr_layer_surface_v1 *layer = wlr_layer_surface_v1_try_from_wlr_surface(root);
	if (layer && layer->surface->mapped &&
	    layer->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		if (kb)
			wlr_seat_keyboard_notify_enter(server->seat, layer->surface,
				kb->keycodes, kb->num_keycodes, &kb->modifiers);
	}
}

/* Send pointer enter/motion for the current cursor position. */
static void pointer_motion_notify(struct pw_server *server, uint32_t time)
{
	double sx, sy;
	struct wlr_surface *s = surface_at(server, server->cursor->x,
		server->cursor->y, &sx, &sy);
	if (s) {
		wlr_seat_pointer_notify_enter(server->seat, s, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

/* ---- actions ---------------------------------------------------------- */

static void rotate_first_output(struct pw_server *server)
{
	if (wl_list_empty(&server->outputs))
		return;
	struct pw_output *out = wl_container_of(server->outputs.next, out, link);
	enum wl_output_transform t = out->wlr_output->transform;
	enum wl_output_transform next = t < WL_OUTPUT_TRANSFORM_270 ?
		t + 1 : WL_OUTPUT_TRANSFORM_NORMAL;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_transform(&state, next);
	if (!wlr_output_commit_state(out->wlr_output, &state))
		pw_log(WLR_ERROR, "rotate: commit failed");
	else
		pw_output_update_geometry(out);
	wlr_output_state_finish(&state);
}

void pw_input_run_action(struct pw_server *server, const struct pw_keybinding *binding)
{
	switch (binding->action) {
	case PW_ACTION_SPAWN:
		if (binding->command && pw_spawn(binding->command) < 0)
			pw_log(WLR_ERROR, "spawn failed: %s", binding->command);
		break;
	case PW_ACTION_CYCLE_VIEWS:
		pw_view_cycle(server);
		break;
	case PW_ACTION_CLOSE_VIEW:
		pw_view_close_focused(server);
		break;
	case PW_ACTION_TOGGLE_BLANK:
		pw_output_blank(server, !server->blanked);
		break;
	case PW_ACTION_ROTATE:
		rotate_first_output(server);
		break;
	case PW_ACTION_QUIT:
		wl_display_terminate(server->display);
		break;
	}
}

/* ---- keyboard --------------------------------------------------------- */

static bool handle_keybinding(struct pw_server *server,
	struct wlr_keyboard *kb, uint32_t keycode)
{
	if (!server->config)
		return false;
	uint32_t mods = wlr_keyboard_get_modifiers(kb) & PW_MOD_MASK;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->xkb_state, keycode + 8, &syms);

	struct pw_keybinding *b;
	wl_list_for_each(b, &server->config->keybindings, link) {
		if ((b->modifiers & PW_MOD_MASK) != mods)
			continue;
		bool match = false;
		if (b->key_name) {
			if (!b->keysym_cached) {
				b->keysym = xkb_keysym_from_name(b->key_name,
					XKB_KEYSYM_CASE_INSENSITIVE);
				b->keysym_cached = true;
			}
			xkb_keysym_t want = b->keysym;
			if (want == XKB_KEY_NoSymbol)
				continue;
			for (int i = 0; i < nsyms; i++)
				if (syms[i] == want || xkb_keysym_to_lower(syms[i]) ==
				    xkb_keysym_to_lower(want))
					match = true;
		} else {
			match = b->keycode == keycode;
		}
		if (match) {
			pw_input_run_action(server, b);
			return true;
		}
	}
	return false;
}

static void keyboard_handle_modifiers(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_keyboard *k = wl_container_of(l, k, modifiers);
	wlr_seat_set_keyboard(k->server->seat, k->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *l, void *data)
{
	struct pw_keyboard *k = wl_container_of(l, k, key);
	struct pw_server *server = k->server;
	struct wlr_keyboard_key_event *ev = data;
	bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;

	/* Releases never wake the display. */
	if (!pressed) {
		if (swallow_get(ev->keycode)) {
			swallow_set(ev->keycode, false);
			return;
		}
		if (server->blanked)
			return;
	}
	bool was_blanked = activity(server);

	if (pressed && was_blanked) {
		swallow_set(ev->keycode, true);
		return;
	}
	if (pressed && handle_keybinding(server, k->wlr_keyboard, ev->keycode)) {
		swallow_set(ev->keycode, true);
		return;
	}

	wlr_seat_set_keyboard(server->seat, k->wlr_keyboard);
	wlr_seat_keyboard_notify_key(server->seat, ev->time_msec, ev->keycode, ev->state);
}

static void keyboard_handle_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_keyboard *k = wl_container_of(l, k, destroy);
	struct pw_server *server = k->server;
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	free(k);
	update_capabilities(server);
}

static void keyboard_add(struct pw_server *server, struct wlr_keyboard *wlr_kb,
	bool virtual)
{
	struct pw_keyboard *k = calloc(1, sizeof(*k));
	if (!k)
		return;
	k->server = server;
	k->wlr_keyboard = wlr_kb;

	/* Virtual keyboards get their keymap from the client. */
	if (!virtual && st.keymap)
		wlr_keyboard_set_keymap(wlr_kb, st.keymap);
	wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

	k->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_kb->events.modifiers, &k->modifiers);
	k->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_kb->events.key, &k->key);
	k->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&wlr_kb->base.events.destroy, &k->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_kb);
	wl_list_insert(&server->keyboards, &k->link);
	update_capabilities(server);
}

static void handle_new_virtual_keyboard(struct wl_listener *l, void *data)
{
	struct pw_server *server = wl_container_of(l, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vk = data;
	keyboard_add(server, &vk->keyboard, true);
}

/* ---- pointer ---------------------------------------------------------- */

static void cursor_handle_motion(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_pointer_motion_event *ev = data;
	struct pw_server *server = st.server;
	if (activity(server))
		return;
	wlr_cursor_move(server->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	pointer_motion_notify(server, ev->time_msec);
}

static void cursor_handle_motion_abs(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_pointer_motion_absolute_event *ev = data;
	struct pw_server *server = st.server;
	if (activity(server))
		return;
	wlr_cursor_warp_absolute(server->cursor, &ev->pointer->base, ev->x, ev->y);
	pointer_motion_notify(server, ev->time_msec);
}

static void cursor_handle_button(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_pointer_button_event *ev = data;
	struct pw_server *server = st.server;
	uint32_t bit = ev->button >= BTN_LEFT && ev->button < BTN_LEFT + 32 ?
		1u << (ev->button - BTN_LEFT) : 0;
	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (st.btn_swallowed & bit) {
			st.btn_swallowed &= ~bit;
			return;
		}
		if (server->blanked)
			return;
	}
	if (activity(server)) {
		st.btn_swallowed |= bit;
		return;
	}
	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy;
		struct wlr_surface *s = surface_at(server, server->cursor->x,
			server->cursor->y, &sx, &sy);
		if (s)
			focus_surface(server, s);
	}
	wlr_seat_pointer_notify_button(server->seat, ev->time_msec, ev->button, ev->state);
}

static void cursor_handle_axis(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_pointer_axis_event *ev = data;
	struct pw_server *server = st.server;
	if (activity(server))
		return;
	wlr_seat_pointer_notify_axis(server->seat, ev->time_msec, ev->orientation,
		ev->delta, ev->delta_discrete, ev->source, ev->relative_direction);
}

static void cursor_handle_frame(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
	wlr_seat_pointer_notify_frame(st.server->seat);
}

static void pointer_dev_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_pointer_dev *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->destroy.link);
	free(p);
	st.n_pointers--;
	update_capabilities(st.server);
}

/* ---- touch ------------------------------------------------------------ */

static void touch_handle_down(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_touch_down_event *ev = data;
	struct pw_server *server = st.server;

	bool blanked = activity(server);
	if (st.touch_id != PW_NO_TOUCH)
		return; /* ignore additional fingers */
	st.touch_id = ev->touch_id;
	st.touch_swallowed = blanked;
	if (blanked)
		return;

	wlr_cursor_warp_absolute(server->cursor, &ev->touch->base, ev->x, ev->y);
	double sx, sy;
	struct wlr_surface *s = surface_at(server, server->cursor->x,
		server->cursor->y, &sx, &sy);
	if (s) {
		focus_surface(server, s);
		st.grab_ox = server->cursor->x - sx;
		st.grab_oy = server->cursor->y - sy;
		st.frame_pending = true;
		wlr_seat_pointer_notify_enter(server->seat, s, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, ev->time_msec, sx, sy);
		wlr_seat_pointer_notify_button(server->seat, ev->time_msec, BTN_LEFT,
			WL_POINTER_BUTTON_STATE_PRESSED);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

static void touch_handle_motion(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_touch_motion_event *ev = data;
	struct pw_server *server = st.server;
	activity(server);
	if (ev->touch_id != st.touch_id || st.touch_swallowed)
		return;
	wlr_cursor_warp_absolute(server->cursor, &ev->touch->base, ev->x, ev->y);
	/* keep the implicit grab's focus: report motion in its coordinates */
	if (server->seat->pointer_state.focused_surface) {
		st.frame_pending = true;
		wlr_seat_pointer_notify_motion(server->seat, ev->time_msec,
			server->cursor->x - st.grab_ox, server->cursor->y - st.grab_oy);
	}
}

static void touch_release(uint32_t time_msec)
{
	if (!st.touch_swallowed) {
		st.frame_pending = true;
		wlr_seat_pointer_notify_button(st.server->seat, time_msec, BTN_LEFT,
			WL_POINTER_BUTTON_STATE_RELEASED);
	}
	st.touch_id = PW_NO_TOUCH;
	st.touch_swallowed = false;
}

static void touch_handle_up(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_touch_up_event *ev = data;
	activity(st.server);
	if (ev->touch_id != st.touch_id)
		return;
	touch_release(ev->time_msec);
}

static void touch_handle_cancel(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_touch_cancel_event *ev = data;
	if (ev->touch_id != st.touch_id)
		return;
	touch_release(ev->time_msec);
}

static void touch_handle_frame(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
	if (!st.frame_pending)
		return;
	st.frame_pending = false;
	wlr_seat_pointer_notify_frame(st.server->seat);
}

static void touch_dev_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_touch_dev *t = wl_container_of(l, t, destroy);
	wl_list_remove(&t->destroy.link);
	free(t);
	if (--st.n_touch == 0) {
		st.touch_id = PW_NO_TOUCH;
		st.touch_swallowed = false;
	}
	update_capabilities(st.server);
}

/* ---- devices / seat --------------------------------------------------- */

static void handle_new_input(struct wl_listener *l, void *data)
{
	struct pw_server *server = wl_container_of(l, server, new_input);
	struct wlr_input_device *dev = data;

	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		keyboard_add(server, wlr_keyboard_from_input_device(dev), false);
		break;
	case WLR_INPUT_DEVICE_POINTER: {
		struct pw_pointer_dev *p = calloc(1, sizeof(*p));
		if (!p)
			break;
		p->destroy.notify = pointer_dev_destroy;
		wl_signal_add(&dev->events.destroy, &p->destroy);
		wlr_cursor_attach_input_device(server->cursor, dev);
		st.n_pointers++;
		update_capabilities(server);
		break;
	}
	case WLR_INPUT_DEVICE_TOUCH: {
		struct pw_touch_dev *t = calloc(1, sizeof(*t));
		if (!t)
			break;
		t->destroy.notify = touch_dev_destroy;
		wl_signal_add(&dev->events.destroy, &t->destroy);
		wlr_cursor_attach_input_device(server->cursor, dev);
		st.n_touch++;
		update_capabilities(server);
		break;
	}
	default:
		break;
	}
}

static void handle_request_set_selection(struct wl_listener *l, void *data)
{
	struct pw_server *server = wl_container_of(l, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(server->seat, ev->source, ev->serial);
}

/* Clients may ask for a cursor image; picowl never draws one. */
static void handle_request_set_cursor(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
}

void pw_input_init(struct pw_server *server)
{
	memset(&st, 0, sizeof(st));
	st.server = server;
	st.touch_id = PW_NO_TOUCH;

	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx) {
		st.keymap = xkb_keymap_new_from_names(ctx, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		xkb_context_unref(ctx);
	}
	if (!st.keymap)
		pw_log(WLR_ERROR, "failed to compile xkb keymap");

	wl_list_init(&server->keyboards);
	server->seat = wlr_seat_create(server->display, "seat0");
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

	server->virtual_keyboard_mgr =
		wlr_virtual_keyboard_manager_v1_create(server->display);
	server->new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&server->virtual_keyboard_mgr->events.new_virtual_keyboard,
		&server->new_virtual_keyboard);

	st.cursor_motion.notify = cursor_handle_motion;
	wl_signal_add(&server->cursor->events.motion, &st.cursor_motion);
	st.cursor_motion_abs.notify = cursor_handle_motion_abs;
	wl_signal_add(&server->cursor->events.motion_absolute, &st.cursor_motion_abs);
	st.cursor_button.notify = cursor_handle_button;
	wl_signal_add(&server->cursor->events.button, &st.cursor_button);
	st.cursor_axis.notify = cursor_handle_axis;
	wl_signal_add(&server->cursor->events.axis, &st.cursor_axis);
	st.cursor_frame.notify = cursor_handle_frame;
	wl_signal_add(&server->cursor->events.frame, &st.cursor_frame);

	st.touch_down.notify = touch_handle_down;
	wl_signal_add(&server->cursor->events.touch_down, &st.touch_down);
	st.touch_up.notify = touch_handle_up;
	wl_signal_add(&server->cursor->events.touch_up, &st.touch_up);
	st.touch_motion.notify = touch_handle_motion;
	wl_signal_add(&server->cursor->events.touch_motion, &st.touch_motion);
	st.touch_cancel.notify = touch_handle_cancel;
	wl_signal_add(&server->cursor->events.touch_cancel, &st.touch_cancel);
	st.touch_frame.notify = touch_handle_frame;
	wl_signal_add(&server->cursor->events.touch_frame, &st.touch_frame);

	server->new_input.notify = handle_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	server->request_set_cursor.notify = handle_request_set_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_set_cursor);
	server->request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);
}

void pw_input_finish(struct pw_server *server)
{
	if (st.keymap) {
		xkb_keymap_unref(st.keymap);
		st.keymap = NULL;
	}
	if (!server->cursor)
		return;
	wl_list_remove(&st.cursor_motion.link);
	wl_list_remove(&st.cursor_motion_abs.link);
	wl_list_remove(&st.cursor_button.link);
	wl_list_remove(&st.cursor_axis.link);
	wl_list_remove(&st.cursor_frame.link);
	wl_list_remove(&st.touch_down.link);
	wl_list_remove(&st.touch_up.link);
	wl_list_remove(&st.touch_motion.link);
	wl_list_remove(&st.touch_cancel.link);
	wl_list_remove(&st.touch_frame.link);
	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->new_virtual_keyboard.link);
	wl_list_remove(&server->request_set_cursor.link);
	wl_list_remove(&server->request_set_selection.link);
}
