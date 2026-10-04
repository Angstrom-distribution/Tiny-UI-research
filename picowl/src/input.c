/*
 * input.c - seat, keyboards, touch/pointer, keybindings.
 *
 * Touch is converted to pointer events for clients (single tracked touch id).
 * No xcursor manager is created; the devices are stylus driven and the
 * compositor draws a cursor only during the tap-and-hold animation
 * (cursor.c). Tap-and-hold itself is the pure state machine in touchhold.c;
 * this file performs the actions it returns.
 */
#include "picowl.h"

#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_output_layout.h>
#include <xkbcommon/xkbcommon.h>
#ifdef PW_HAVE_LIBINPUT
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <libudev.h>
#include <wlr/backend/libinput.h>
#include <libinput.h>
#include "pointercal.h"
#endif

#include "power.h"
#include "touchhold.h"
#include "rotate.h"
#include "leasepolicy.h"

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
	struct pw_touchhold th;        /* tap-and-hold state machine */
	struct wl_event_source *th_timer;
	int down_x, down_y;            /* layout coords of the touch-down point */
	struct wlr_box lease_box;      /* touch region while leased, empty if not */

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
struct pw_touch_dev {
	struct wl_list link;           /* touch_devs */
	struct wlr_input_device *dev;
	struct wl_listener destroy;
	float base[6];                 /* calibration, composed with rotation */
	bool have_cal;                 /* base is valid */
	bool blocked;                  /* uncalibrated touch device: disabled and never
	                                * attached to the cursor */
	bool tablet;                   /* a tablet shares the mapping, not the touch state */
	bool tablet_mapped;            /* tablet: rotation can be applied (tablet.c) */
};

static struct wl_list touch_devs; /* initialised in pw_input_init */

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
	pw_idle_notify(server);
	/* Blanked: swallow the waking event. Dimmed: undim, deliver it. */
	return pw_power_activity(server);
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

static void focus_surface(struct pw_server *server, struct wlr_surface *surface);

struct wlr_surface *pw_input_surface_at(struct pw_server *server, double lx,
	double ly, double *sx, double *sy)
{
	return surface_at(server, lx, ly, sx, sy);
}

void pw_input_focus_surface(struct pw_server *server, struct wlr_surface *surface)
{
	focus_surface(server, surface);
}

bool pw_input_activity(struct pw_server *server)
{
	return activity(server);
}

static void focus_surface(struct pw_server *server, struct wlr_surface *surface)
{
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);

	struct wlr_xdg_toplevel *top = wlr_xdg_toplevel_try_from_wlr_surface(root);
	if (top) {
		struct pw_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->xdg_toplevel == top) {
				/* An on-demand layer surface may hold the keyboard
				 * while this view is still focused_view. */
				struct wlr_surface *kf =
					server->seat->keyboard_state.focused_surface;
				if (view != server->focused_view ||
				    (kf && wlr_layer_surface_v1_try_from_wlr_surface(kf)))
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
	/* wlr_output->transform stays NORMAL under hardware rotation. */
	enum wl_output_transform next = (out->rotation & 4) |
		((out->rotation + 1) & 3);
	pw_output_rotate(out, next);
}

void pw_input_run_action(struct pw_server *server, const struct pw_keybinding *binding)
{
	/* The lessee owns scanout: some actions end the lease, one is dropped. */
	if (pw_lease_active(server)) {
		switch (pw_lease_key_policy(binding->action)) {
		case PW_LEASE_KEY_DROP:
			pw_log(WLR_INFO, "lease: key action dropped while leased");
			return;
		case PW_LEASE_KEY_REVOKE_FIRST:
			pw_lease_revoke(server, "key action");
			break;
		case PW_LEASE_KEY_PASS:
			break;
		}
	}
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
		pw_power_set_blanked(server, !server->blanked);
		break;
	case PW_ACTION_ROTATE:
		rotate_first_output(server);
		break;
	case PW_ACTION_TOGGLE_PANEL:
		pw_panel_toggle(server);
		break;
	case PW_ACTION_QUIT:
		wl_display_terminate(server->display);
		break;
	}
}

/* ---- keyboard --------------------------------------------------------- */

/* Binding matching a key press, or NULL. Without an xkb state (keymap failed
 * to compile) only keycode bindings (e.g. the power key) can match. */
static struct pw_keybinding *find_keybinding(struct pw_server *server,
	struct wlr_keyboard *kb, uint32_t keycode)
{
	if (!server->config)
		return NULL;
	uint32_t mods = wlr_keyboard_get_modifiers(kb) & PW_MOD_MASK;
	const xkb_keysym_t *syms = NULL;
	int nsyms = kb->xkb_state ?
		xkb_state_key_get_syms(kb->xkb_state, keycode + 8, &syms) : 0;

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
		if (match)
			return b;
	}
	return NULL;
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
	if (!pressed && swallow_get(ev->keycode)) {
		swallow_set(ev->keycode, false);
		return;
	}

	struct pw_keybinding *bind = pressed ?
		find_keybinding(server, k->wlr_keyboard, ev->keycode) : NULL;
	if (bind && bind->action == PW_ACTION_TOGGLE_BLANK && !server->blanked) {
		/* Blank straight from ACTIVE/DIMMED: activity() would first undim
		 * to full brightness, which then stays for the whole blank. */
		pw_idle_notify(server);
		swallow_set(ev->keycode, true);
		pw_input_run_action(server, bind);
		return;
	}

	/* A release whose press was delivered must reach the client even while
	 * blanked, or the key stays down there; it is not activity. */
	bool was_blanked = false;
	if (pressed || !server->blanked)
		was_blanked = activity(server);

	if (pressed && was_blanked) {
		swallow_set(ev->keycode, true);
		return;
	}
	if (bind) {
		swallow_set(ev->keycode, true);
		pw_input_run_action(server, bind);
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
	if (!virtual && st.keymap && !wlr_keyboard_set_keymap(wlr_kb, st.keymap))
		pw_log(WLR_ERROR, "failed to set keymap on keyboard");
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
	bool released = ev->state == WL_POINTER_BUTTON_STATE_RELEASED;
	if (released && (st.btn_swallowed & bit)) {
		st.btn_swallowed &= ~bit;
		return;
	}
	/* A release must reach the client even while blanked, or its implicit
	 * grab never ends; it is not activity and does not wake the display. */
	if (!(released && server->blanked) && activity(server)) {
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

#ifdef PW_HAVE_LIBINPUT
static bool abs_range(int fd, int code, int32_t *min, int32_t *max)
{
	struct input_absinfo ai;
	if (ioctl(fd, EVIOCGABS(code), &ai) < 0 || ai.maximum <= ai.minimum)
		return false;
	*min = ai.minimum;
	*max = ai.maximum;
	return true;
}

/* The ranges libinput normalizes with, read from the device node because the
 * public libinput API does not expose them: ABS_MT_POSITION_* on multitouch
 * panels, ABS_X/ABS_Y otherwise (the same choice as libinput's evdev.c). */
static bool touch_read_ranges(struct libinput_device *h, int32_t *minx,
	int32_t *maxx, int32_t *miny, int32_t *maxy)
{
	struct udev_device *ud = libinput_device_get_udev_device(h);
	if (!ud)
		return false;
	const char *node = udev_device_get_devnode(ud);
	int fd = node ? open(node, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
	bool ok = false;
	if (fd >= 0) {
		ok = (abs_range(fd, ABS_MT_POSITION_X, minx, maxx) &&
			abs_range(fd, ABS_MT_POSITION_Y, miny, maxy)) ||
			(abs_range(fd, ABS_X, minx, maxx) &&
			abs_range(fd, ABS_Y, miny, maxy));
		close(fd);
	}
	udev_device_unref(ud);
	return ok;
}

/* Raw axes of a resistive panel are wrong (inverted, offset), so a touch
 * device without calibration is switched off instead of used. libinput stops
 * delivering its events, and it is never attached to the cursor, so nothing
 * can reach a client even if disabling fails. */
static void touch_block(struct pw_touch_dev *t, struct libinput_device *h,
	const char *why)
{
	const struct pw_config *cfg = st.server->config;
	t->blocked = true;
	t->have_cal = false;
	pw_log(WLR_ERROR, "touch '%s': %s; the device is disabled because raw "
		"axes are never used. Fix: create %s with tslib's ts_calibrate, or "
		"set [touch] calibration = a b c d e f in picowl.ini (an explicit "
		"identity '1 0 0 0 1 0' if the panel is already calibrated), or "
		"provide LIBINPUT_CALIBRATION_MATRIX from udev", t->dev->name, why,
		cfg->pointercal && *cfg->pointercal ? cfg->pointercal : "/etc/pointercal");
	if (h && libinput_device_config_send_events_set_mode(h,
			LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) != LIBINPUT_CONFIG_STATUS_SUCCESS)
		pw_log(WLR_ERROR, "touch '%s': libinput could not disable it; it "
			"stays detached from the cursor", t->dev->name);
}

/* Find the base matrix of a libinput device. Touch devices: [touch]
 * calibration, then the tslib pointercal, then a non-identity libinput default
 * (udev LIBINPUT_CALIBRATION_MATRIX), else the device is blocked. A tablet is
 * not a resistive panel and never takes the pointercal or [touch] calibration
 * (they describe a touchscreen): its base is the libinput default, identity
 * included, as before. Not a libinput device (nested or headless backend):
 * there are no raw ADC axes, nothing to calibrate. */
static void touch_calibrate(struct pw_touch_dev *t)
{
	const struct pw_config *cfg = st.server->config;
	t->have_cal = false;
	if (!wlr_input_device_is_libinput(t->dev))
		return;
	struct libinput_device *h = wlr_libinput_get_device_handle(t->dev);
	bool has_matrix = h && libinput_device_config_calibration_has_matrix(h);
	if (!has_matrix) {
		if (!t->tablet)
			touch_block(t, h, "no absolute axes that can be calibrated");
		return;
	}

	const char *src = NULL;
	if (t->tablet) {
		/* Identity counts too: a tablet is right with raw axes, and rotation
		 * composes with the identity as well as with any other base. */
		libinput_device_config_calibration_get_default_matrix(h, t->base);
		src = "libinput default";
	}
	if (!src && cfg->have_calibration) {
		memcpy(t->base, cfg->calibration, sizeof(t->base));
		src = "[touch] calibration";
	}
	if (!src && cfg->pointercal && *cfg->pointercal) {
		const char *path = cfg->pointercal;
		struct pw_pointercal pc;
		char err[256];
		if (access(path, R_OK) < 0 && errno == ENOENT) {
			pw_log(WLR_DEBUG, "touch '%s': no pointercal %s", t->dev->name, path);
		} else if (!pw_pointercal_load(path, &pc, err, sizeof(err))) {
			pw_log(WLR_ERROR, "touch '%s': pointercal %s: %s", t->dev->name,
				path, err);
		} else {
			int32_t minx, maxx, miny, maxy;
			/* Without the ranges the normalization is unknown; do not fall
			 * back to another source behind the operator's pointercal. */
			if (!touch_read_ranges(h, &minx, &maxx, &miny, &maxy)) {
				touch_block(t, h, "cannot read the ABS ranges needed to apply "
					"the pointercal");
				return;
			}
			if (!pw_pointercal_to_matrix(&pc, minx, maxx, miny, maxy, t->base,
					err, sizeof(err)))
				pw_log(WLR_ERROR, "touch '%s': pointercal %s: %s", t->dev->name,
					path, err);
			else
				src = "pointercal";
		}
	}
	if (!src && libinput_device_config_calibration_get_default_matrix(h, t->base)) {
		/* returns non-zero only for a non-identity default; an identity one
		 * cannot be told from "none" */
		src = "libinput default (udev)";
	}
	if (!src) {
		touch_block(t, h, "no calibration found");
		return;
	}
	/* Apply it now so the first event is already right; rotation composes
	 * with it later. */
	if (libinput_device_config_calibration_set_matrix(h, t->base) !=
			LIBINPUT_CONFIG_STATUS_SUCCESS) {
		if (t->tablet)
			return;
		touch_block(t, h, "libinput refused the calibration matrix");
		return;
	}
	t->have_cal = true;
	pw_log(WLR_INFO, "%s '%s': calibration from %s: %f %f %f %f %f %f",
		t->tablet ? "tablet" : "touch", t->dev->name, src, t->base[0],
		t->base[1], t->base[2], t->base[3], t->base[4], t->base[5]);
}

static void apply_touch_matrix(struct pw_touch_dev *t, struct pw_output *o)
{
	if (!t->have_cal)
		return;
	struct libinput_device *h = wlr_libinput_get_device_handle(t->dev);
	if (!h)
		return;
	float m[6];
	if (o->hw_rotation) {
		float r[6];
		pw_rot_touch_matrix(o->rotation, r);
		pw_rot_matrix_mul(r, t->base, m);
	} else {
		/* software rotation is done by wlroots (device mapped to the
		 * output); restore the calibration exactly */
		memcpy(m, t->base, sizeof(m));
	}
	libinput_device_config_calibration_set_matrix(h, m);
}
#endif

/* A tablet under a hardware-rotated output is only right with a calibration
 * matrix; without one its coordinates would be rotated wrongly, so it is
 * ignored instead (tablet.c) until the output is back to normal. */
static void tablet_check_mapping(struct pw_touch_dev *t, struct pw_output *o)
{
	bool ok = !o->hw_rotation || o->rotation == WL_OUTPUT_TRANSFORM_NORMAL ||
		t->have_cal;
	if (ok != t->tablet_mapped)
		pw_log(ok ? WLR_INFO : WLR_ERROR, "tablet '%s': %s", t->dev->name,
			ok ? "mapped to the output" : "cannot follow the hardware "
			"rotation of the output (no calibration matrix), ignored");
	t->tablet_mapped = ok;
	pw_tablet_set_usable(t->dev, ok);
}

void pw_input_apply_rotation(struct pw_server *server, struct pw_output *o)
{
	struct pw_touch_dev *t;
	wl_list_for_each(t, &touch_devs, link) {
		if (t->blocked)
			continue; /* not attached to the cursor */
		/* wlr_cursor applies the output transform to touch and tablet
		 * coordinates of devices mapped to an output (software rotation) */
		wlr_cursor_map_input_to_output(server->cursor, t->dev, o->wlr_output);
#ifdef PW_HAVE_LIBINPUT
		apply_touch_matrix(t, o);
#endif
		if (t->tablet)
			tablet_check_mapping(t, o);
	}
#ifndef PW_HAVE_LIBINPUT
	static bool logged;
	if (!logged) {
		logged = true;
		pw_log(WLR_INFO, "built without libinput: no touch calibration");
	}
#endif
}

/* Map one touch device to the lease region (or clear it). A device's region
 * wins over its output, so the output mapping can stay as it is. */
static void touch_lease_apply(struct pw_touch_dev *t)
{
	if (t->blocked)
		return;
	wlr_cursor_map_input_to_region(st.server->cursor, t->dev,
		wlr_box_empty(&st.lease_box) ? NULL : &st.lease_box);
#ifdef PW_HAVE_LIBINPUT
	/* The lessee renders in the panel's native frame: no rotation matrix. */
	if (t->have_cal && !wlr_box_empty(&st.lease_box)) {
		struct libinput_device *h = wlr_libinput_get_device_handle(t->dev);
		if (h)
			libinput_device_config_calibration_set_matrix(h, t->base);
	}
#endif
}

void pw_input_lease_touch(struct pw_server *server, const struct wlr_box *box)
{
	st.lease_box = box ? *box : (struct wlr_box){ 0 };
	/* a gesture in progress began in layout coordinates */
	st.grab_ox = st.grab_oy = 0;
	if (!server->cursor)
		return;
	struct pw_touch_dev *t;
	wl_list_for_each(t, &touch_devs, link)
		touch_lease_apply(t);
}

/* wlr_cursor keeps a raw pointer to the output a touch device is mapped to:
 * unmap it before the output is freed and follow a surviving output. */
void pw_input_output_removed(struct pw_server *server, struct pw_output *gone)
{
	if (!server->cursor)
		return;
	struct pw_touch_dev *t;
	wl_list_for_each(t, &touch_devs, link)
		if (!t->blocked)
			wlr_cursor_map_input_to_output(server->cursor, t->dev, NULL);

	struct pw_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o != gone) {
			pw_input_apply_rotation(server, o);
			break;
		}
	}
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Arm (or disarm) the hold timer for the state machine's next deadline. */
static void th_rearm(void)
{
	if (!st.th_timer)
		return;
	int64_t dl = pw_touchhold_next_deadline(&st.th);
	if (dl < 0) {
		wl_event_source_timer_update(st.th_timer, 0);
		return;
	}
	int64_t d = dl - now_ms();
	if (d < 1)
		d = 1;
	if (d > 100000)
		d = 100000;
	wl_event_source_timer_update(st.th_timer, (int)d);
}

/* Perform state machine actions in the documented order. */
static void th_exec(unsigned a, uint32_t time)
{
	struct pw_server *server = st.server;
	struct wlr_seat *seat = server->seat;

	if (a & PW_TH_STOP_ANIMATION)
		pw_cursor_hold_stop(server);
	if (a & PW_TH_SEND_LEFT_PRESS) {
		st.frame_pending = true;
		wlr_seat_pointer_notify_button(seat, time, BTN_LEFT,
			WL_POINTER_BUTTON_STATE_PRESSED);
	}
	if ((a & PW_TH_SEND_MOTION) && seat->pointer_state.focused_surface) {
		/* keep the implicit grab's focus: report motion in its coordinates */
		st.frame_pending = true;
		wlr_seat_pointer_notify_motion(seat, time,
			server->cursor->x - st.grab_ox, server->cursor->y - st.grab_oy);
	}
	if (a & PW_TH_SEND_RIGHT_CLICK) {
		st.frame_pending = true;
		wlr_seat_pointer_notify_button(seat, time, BTN_RIGHT,
			WL_POINTER_BUTTON_STATE_PRESSED);
		wlr_seat_pointer_notify_button(seat, time, BTN_RIGHT,
			WL_POINTER_BUTTON_STATE_RELEASED);
	}
	if (a & PW_TH_SEND_LEFT_RELEASE) {
		st.frame_pending = true;
		wlr_seat_pointer_notify_button(seat, time, BTN_LEFT,
			WL_POINTER_BUTTON_STATE_RELEASED);
	}
	if (a & PW_TH_START_ANIMATION)
		pw_cursor_hold_start(server, st.down_x, st.down_y);
}

static int th_timer_cb(void *data)
{
	(void)data;
	int64_t now = now_ms();
	unsigned a = pw_touchhold_tick(&st.th, now);
	th_exec(a, (uint32_t)now);
	/* no touch frame will follow a timer-driven event: flush now */
	if (st.frame_pending) {
		st.frame_pending = false;
		wlr_seat_pointer_notify_frame(st.server->seat);
	}
	th_rearm();
	return 0;
}

/* Rule identity for a touched surface: toplevel app_id or layer namespace.
 * Walks subsurfaces and popup parents. Returns NULL if no app_id/namespace found
 * (lock surface, unknown role, etc). */
static const char *hold_identity(struct wlr_surface *s, enum pw_rule_kind *kind)
{
	if (!s)
		return NULL;

	struct wlr_surface *root = wlr_surface_get_root_surface(s);
	for (int depth = 0; depth < 16; depth++) {
		struct wlr_xdg_toplevel *tl = wlr_xdg_toplevel_try_from_wlr_surface(root);
		if (tl) {
			*kind = PW_RULE_APP;
			return tl->app_id;  /* may be NULL */
		}
		struct wlr_xdg_popup *pp = wlr_xdg_popup_try_from_wlr_surface(root);
		if (pp) {
			if (!pp->parent)
				return NULL;
			root = wlr_surface_get_root_surface(pp->parent);
			continue;
		}
		struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(root);
		if (ls) {
			*kind = PW_RULE_LAYER;
			return ls->namespace;
		}
		return NULL;  /* lock surface, unknown role */
	}
	return NULL;
}

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
	st.down_x = (int)server->cursor->x;
	st.down_y = (int)server->cursor->y;
	double sx, sy;
	/* Leased: the scene no longer matches the screen. The lessee gets the
	 * touch, in the coordinates of the lease region. */
	struct wlr_surface *s = pw_lease_touch_target(server);
	if (s) {
		sx = server->cursor->x;
		sy = server->cursor->y;
	} else {
		s = surface_at(server, server->cursor->x, server->cursor->y, &sx, &sy);
		if (s)
			focus_surface(server, s);
	}
	if (s) {
		st.grab_ox = server->cursor->x - sx;
		st.grab_oy = server->cursor->y - sy;
		st.frame_pending = true;
		wlr_seat_pointer_notify_enter(server->seat, s, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, ev->time_msec, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
	/* Set per-app/layer hold parameters */
	struct pw_hold_params hp;
	enum pw_rule_kind kind = PW_RULE_APP;
	const char *id = s ? hold_identity(s, &kind) : NULL;
	bool hit = pw_config_hold(server->config, kind, id, &hp);
	unsigned a = pw_touchhold_set_params(&st.th, (enum pw_th_hold_action)hp.action,
		hp.delay_ms, hp.hold_ms, hp.slop_px);
	th_exec(a, ev->time_msec);
	if (hit)
		pw_log(WLR_DEBUG, "hold: %s '%s' -> %s", kind == PW_RULE_APP ? "app" : "layer",
			id, hp.action == PW_HOLD_NONE ? "none" : "right-click");
	/* Now perform the touch down */
	a = pw_touchhold_down(&st.th, st.down_x, st.down_y, now_ms());
	th_exec(a, ev->time_msec);
	th_rearm();
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
	unsigned a = pw_touchhold_motion(&st.th, (int)server->cursor->x,
		(int)server->cursor->y, now_ms());
	if (!(a & PW_TH_SWALLOW))
		th_exec(a, ev->time_msec);
	th_rearm();
}

static void touch_end(uint32_t time_msec, bool cancel)
{
	if (!st.touch_swallowed) {
		unsigned a = cancel ? pw_touchhold_cancel(&st.th)
			: pw_touchhold_up(&st.th, now_ms());
		th_exec(a & ~(unsigned)PW_TH_SWALLOW, time_msec);
	}
	th_rearm();
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
	touch_end(ev->time_msec, false);
}

static void touch_handle_cancel(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_touch_cancel_event *ev = data;
	if (ev->touch_id != st.touch_id)
		return;
	touch_end(ev->time_msec, true);
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
	bool skip = t->tablet || t->blocked; /* not counted in n_touch */
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
	if (skip)
		return;
	if (--st.n_touch == 0) {
		if (st.touch_id != PW_NO_TOUCH && !st.touch_swallowed)
			th_exec(pw_touchhold_cancel(&st.th) & ~(unsigned)PW_TH_SWALLOW,
				(uint32_t)now_ms());
		th_rearm();
		st.touch_id = PW_NO_TOUCH;
		st.touch_swallowed = false;
	}
	update_capabilities(st.server);
}

/* ---- devices / seat --------------------------------------------------- */

/* Track a touch or tablet device for calibration and output mapping and attach
 * it to the cursor. */
static struct pw_touch_dev *absolute_dev_add(struct pw_server *server,
	struct wlr_input_device *dev, bool tablet)
{
	struct pw_touch_dev *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->dev = dev;
	t->tablet = tablet;
	t->tablet_mapped = true;
#ifdef PW_HAVE_LIBINPUT
	touch_calibrate(t);
#endif
	wl_list_insert(&touch_devs, &t->link);
	t->destroy.notify = touch_dev_destroy;
	wl_signal_add(&dev->events.destroy, &t->destroy);
	if (t->blocked)
		return t; /* never attached: no event can reach the cursor or a client */
	wlr_cursor_attach_input_device(server->cursor, dev);
	if (pw_lease_active(server))
		touch_lease_apply(t);
	/* Without an output the mapping follows when the first one appears. */
	if (!wl_list_empty(&server->outputs)) {
		struct pw_output *o = wl_container_of(server->outputs.next, o, link);
		pw_input_apply_rotation(server, o);
	}
	return t;
}

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
		struct pw_touch_dev *t = absolute_dev_add(server, dev, false);
		if (!t || t->blocked)
			break;
		st.n_touch++;
		update_capabilities(server);
		break;
	}
	case WLR_INPUT_DEVICE_TABLET:
		if (!pw_tablet_add(server, dev)) {
			pw_log(WLR_ERROR, "tablet '%s': not usable, ignored", dev->name);
			break;
		}
		if (!absolute_dev_add(server, dev, true)) {
			pw_log(WLR_ERROR, "tablet '%s': cannot map it, ignored", dev->name);
			pw_tablet_set_usable(dev, false);
		}
		break;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		if (!pw_tablet_pad_add(server, dev))
			pw_log(WLR_ERROR, "tablet pad '%s': not usable, ignored", dev->name);
		break;
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

static void handle_request_set_primary_selection(struct wl_listener *l, void *data)
{
	struct pw_server *server = wl_container_of(l, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(server->seat, ev->source, ev->serial);
}

/* Clients may ask for a cursor image; picowl never draws one. */
static void handle_request_set_cursor(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
}

bool pw_input_init(struct pw_server *server)
{
	memset(&st, 0, sizeof(st));
	wl_list_init(&touch_devs);
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
	server->virtual_keyboard_mgr =
		wlr_virtual_keyboard_manager_v1_create(server->display);
	if (!server->seat || !server->cursor || !server->virtual_keyboard_mgr) {
		pw_log(WLR_ERROR, "cannot create seat, cursor or virtual keyboard");
		/* no listeners yet: pw_input_finish must see no cursor */
		if (server->cursor)
			wlr_cursor_destroy(server->cursor);
		server->cursor = NULL;
		return false;
	}
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

	pw_im_init(server);
	pw_tablet_init(server);

	pw_touchhold_init(&st.th, (enum pw_th_hold_action)server->config->hold_action,
		server->config->hold_delay_ms, server->config->hold_ms,
		server->config->slop_px);
	st.th_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), th_timer_cb, NULL);

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
	server->request_set_primary_selection.notify = handle_request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
	return true;
}

void pw_input_finish(struct pw_server *server)
{
	if (st.th_timer) {
		wl_event_source_remove(st.th_timer);
		st.th_timer = NULL;
	}
	if (st.keymap) {
		xkb_keymap_unref(st.keymap);
		st.keymap = NULL;
	}
	pw_tablet_finish(server);
	pw_im_finish(server);
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
	wl_list_remove(&server->request_set_primary_selection.link);
}
