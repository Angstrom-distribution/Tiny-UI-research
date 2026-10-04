/*
 * tablet.c - tablet-v2: drawing tablets, their tools and pads.
 *
 * Tablet coordinates reach us as wlr_cursor events: wlr_cursor has already
 * applied the output transform of the output the device is mapped to, exactly
 * as for touch (input.c maps tablets and touch devices the same way and
 * applies the libinput calibration matrix under hardware rotation). The
 * tool events are turned into layout coordinates by warping the cursor, so
 * the focused surface is found the way the pointer and touch code find it.
 *
 * Clients that did not bind tablet-v2 get nothing from a tablet; there is no
 * pointer emulation.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>

#include "picowl.h"

struct pw_tablet {
	struct wl_list link;           /* T.tablets */
	struct wlr_tablet *wlr;
	struct wlr_tablet_v2_tablet *v2;
	bool usable;                   /* the output mapping is right */
	struct wl_listener destroy;
};

struct pw_tablet_pad {
	struct wl_list link;           /* T.pads */
	struct wlr_tablet_pad *wlr;
	struct wlr_tablet_v2_tablet_pad *v2;
	struct pw_tablet *tablet;      /* paired by device path, or NULL */
	struct wlr_surface *focus;     /* surface the pad entered, or NULL */
	struct wl_listener focus_destroy;
	struct wl_listener button;
	struct wl_listener ring;
	struct wl_listener strip;
	struct wl_listener destroy;
};

struct pw_tool {
	struct wl_list link;           /* T.tools */
	struct wlr_tablet_v2_tablet_tool *v2;
	/* The tablet the tool last entered or is held on, or NULL out of
	 * proximity. A wlr_tablet_tool belongs to no tablet by itself, and a
	 * tablet going unusable must not touch the tools of the others. */
	struct pw_tablet *tablet;
	/* While a tip or button is held the surface that got the press keeps the
	 * tool, like the touch grab: motion is reported in its coordinates. */
	bool grabbed;
	double grab_ox, grab_oy;       /* layout origin of the grabbed surface */
	struct wl_listener destroy;
};

/* File-local: one tablet manager per process. */
static struct {
	struct pw_server *server;
	struct wlr_tablet_manager_v2 *mgr;
	struct wl_list tablets, pads, tools;
	struct wl_listener axis, proximity, tip, button;
	bool added;
} T;

/* The tool object is keyed by its wlr_tablet_tool through the destroy signal
 * of that tool, which is also what frees the tablet-v2 side. */
struct pw_tool_key {
	struct wlr_tablet_tool *wlr;
	struct pw_tool tool;
};

static struct pw_tablet *tablet_find(const struct wlr_tablet *wlr)
{
	struct pw_tablet *t;
	wl_list_for_each(t, &T.tablets, link)
		if (t->wlr == wlr)
			return t;
	return NULL;
}

static bool paths_overlap(const struct wl_array *a, const struct wl_array *b)
{
	char **pa, **pb;
	wl_array_for_each(pa, a)
		wl_array_for_each(pb, b)
			if (*pa && *pb && !strcmp(*pa, *pb))
				return true;
	return false;
}

/* A pad belongs to the tablet in the same libinput device group; wlroots
 * gives the sysfs paths of that group for both. */
static void pair_pad(struct pw_tablet_pad *pad)
{
	struct pw_tablet *t;
	wl_list_for_each(t, &T.tablets, link) {
		if (paths_overlap(&pad->wlr->paths, &t->wlr->paths)) {
			pad->tablet = t;
			return;
		}
	}
}

/* ---- pads ------------------------------------------------------------- */

static void pad_focus_gone(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_tablet_pad *pad = wl_container_of(l, pad, focus_destroy);
	wl_list_remove(&pad->focus_destroy.link);
	pad->focus = NULL;
}

static void pad_set_focus(struct pw_tablet_pad *pad, struct wlr_surface *surface)
{
	if (pad->focus == surface)
		return;
	if (pad->focus) {
		wlr_tablet_v2_tablet_pad_notify_leave(pad->v2, pad->focus);
		wl_list_remove(&pad->focus_destroy.link);
		pad->focus = NULL;
	}
	if (surface && pad->tablet) {
		wlr_tablet_v2_tablet_pad_notify_enter(pad->v2, pad->tablet->v2, surface);
		pad->focus = surface;
		wl_signal_add(&surface->events.destroy, &pad->focus_destroy);
	}
}

static void tablet_pads_focus(struct pw_tablet *tab, struct wlr_surface *surface)
{
	struct pw_tablet_pad *pad;
	wl_list_for_each(pad, &T.pads, link)
		if (pad->tablet == tab)
			pad_set_focus(pad, surface);
}

/* Pad events follow the tool rules: dropped while leased and as the waking
 * event of a blanked display. A button release always passes, or the client
 * would be left with a pressed pad button. */
static bool pad_blocked(void)
{
	return pw_lease_active(T.server) || pw_input_activity(T.server);
}

static void pad_handle_button(struct wl_listener *l, void *data)
{
	struct pw_tablet_pad *pad = wl_container_of(l, pad, button);
	struct wlr_tablet_pad_button_event *ev = data;
	if (ev->state != WLR_BUTTON_RELEASED && pad_blocked())
		return;
	if (ev->group < pad->v2->group_count)
		wlr_tablet_v2_tablet_pad_notify_mode(pad->v2, ev->group, ev->mode,
			ev->time_msec);
	wlr_tablet_v2_tablet_pad_notify_button(pad->v2, ev->button, ev->time_msec,
		(enum zwp_tablet_pad_v2_button_state)ev->state);
}

static void pad_handle_ring(struct wl_listener *l, void *data)
{
	struct pw_tablet_pad *pad = wl_container_of(l, pad, ring);
	struct wlr_tablet_pad_ring_event *ev = data;
	if (pad_blocked())
		return;
	wlr_tablet_v2_tablet_pad_notify_ring(pad->v2, ev->ring, ev->position,
		ev->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, ev->time_msec);
}

static void pad_handle_strip(struct wl_listener *l, void *data)
{
	struct pw_tablet_pad *pad = wl_container_of(l, pad, strip);
	struct wlr_tablet_pad_strip_event *ev = data;
	if (pad_blocked())
		return;
	wlr_tablet_v2_tablet_pad_notify_strip(pad->v2, ev->strip, ev->position,
		ev->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, ev->time_msec);
}

static void pad_handle_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_tablet_pad *pad = wl_container_of(l, pad, destroy);
	if (pad->focus)
		wl_list_remove(&pad->focus_destroy.link);
	wl_list_remove(&pad->button.link);
	wl_list_remove(&pad->ring.link);
	wl_list_remove(&pad->strip.link);
	wl_list_remove(&pad->destroy.link);
	wl_list_remove(&pad->link);
	free(pad);
}

bool pw_tablet_pad_add(struct pw_server *server, struct wlr_input_device *dev)
{
	(void)server;
	if (!T.mgr)
		return false;
	struct pw_tablet_pad *pad = calloc(1, sizeof(*pad));
	if (!pad)
		return false;
	pad->wlr = wlr_tablet_pad_from_input_device(dev);
	pad->v2 = wlr_tablet_pad_create(T.mgr, T.server->seat, dev);
	if (!pad->v2) {
		free(pad);
		return false;
	}
	pad->focus_destroy.notify = pad_focus_gone;
	pad->button.notify = pad_handle_button;
	wl_signal_add(&pad->wlr->events.button, &pad->button);
	pad->ring.notify = pad_handle_ring;
	wl_signal_add(&pad->wlr->events.ring, &pad->ring);
	pad->strip.notify = pad_handle_strip;
	wl_signal_add(&pad->wlr->events.strip, &pad->strip);
	pad->destroy.notify = pad_handle_destroy;
	wl_signal_add(&dev->events.destroy, &pad->destroy);
	wl_list_insert(&T.pads, &pad->link);
	pair_pad(pad);
	if (!pad->tablet)
		pw_log(WLR_INFO, "tablet pad '%s': no tablet found, it gets no focus", dev->name);
	return true;
}

/* ---- tablets ---------------------------------------------------------- */

static void tablet_handle_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_tablet *tab = wl_container_of(l, tab, destroy);
	struct pw_tablet_pad *pad;
	wl_list_for_each(pad, &T.pads, link) {
		if (pad->tablet == tab) {
			pad_set_focus(pad, NULL);
			pad->tablet = NULL;
		}
	}
	struct pw_tool *tool;
	wl_list_for_each(tool, &T.tools, link)
		if (tool->tablet == tab)
			tool->tablet = NULL;
	wl_list_remove(&tab->destroy.link);
	wl_list_remove(&tab->link);
	free(tab);
}

bool pw_tablet_add(struct pw_server *server, struct wlr_input_device *dev)
{
	(void)server;
	if (!T.mgr)
		return false;
	struct pw_tablet *tab = calloc(1, sizeof(*tab));
	if (!tab)
		return false;
	tab->wlr = wlr_tablet_from_input_device(dev);
	tab->v2 = wlr_tablet_create(T.mgr, T.server->seat, dev);
	if (!tab->v2) {
		free(tab);
		return false;
	}
	tab->usable = true;
	tab->destroy.notify = tablet_handle_destroy;
	wl_signal_add(&dev->events.destroy, &tab->destroy);
	wl_list_insert(&T.tablets, &tab->link);

	/* The pad may have appeared first. */
	struct pw_tablet_pad *pad;
	wl_list_for_each(pad, &T.pads, link)
		if (!pad->tablet && paths_overlap(&pad->wlr->paths, &tab->wlr->paths))
			pad->tablet = tab;
	return true;
}

void pw_tablet_set_usable(struct wlr_input_device *dev, bool usable)
{
	struct pw_tablet *tab;
	wl_list_for_each(tab, &T.tablets, link) {
		if (&tab->wlr->base != dev || tab->usable == usable)
			continue;
		tab->usable = usable;
		if (!usable) {
			struct pw_tool *tool;
			wl_list_for_each(tool, &T.tools, link) {
				if (tool->tablet != tab)
					continue;
				wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
				tool->grabbed = false;
				tool->tablet = NULL;
			}
			tablet_pads_focus(tab, NULL);
		}
	}
}

/* ---- tools ------------------------------------------------------------ */

static void tool_handle_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_tool_key *k = wl_container_of(l, k, tool.destroy);
	wl_list_remove(&k->tool.destroy.link);
	wl_list_remove(&k->tool.link);
	free(k);
}

static struct pw_tool *tool_get(struct wlr_tablet_tool *wlr)
{
	struct pw_tool *t;
	wl_list_for_each(t, &T.tools, link) {
		struct pw_tool_key *k = wl_container_of(t, k, tool);
		if (k->wlr == wlr)
			return t;
	}
	/* wlroots aborts on a totem, which tablet-v2 has no type for. */
	if (wlr->type == WLR_TABLET_TOOL_TYPE_TOTEM)
		return NULL;
	struct pw_tool_key *k = calloc(1, sizeof(*k));
	if (!k)
		return NULL;
	k->wlr = wlr;
	k->tool.v2 = wlr_tablet_tool_create(T.mgr, T.server->seat, wlr);
	if (!k->tool.v2) {
		free(k);
		return NULL;
	}
	k->tool.destroy.notify = tool_handle_destroy;
	wl_signal_add(&wlr->events.destroy, &k->tool.destroy);
	wl_list_insert(&T.tools, &k->tool.link);
	return &k->tool;
}

/* Hand the tool to the surface under the cursor if that client bound
 * tablet-v2, else take it away from the previous one. */
static struct wlr_surface *tool_focus(struct pw_tablet *tab, struct pw_tool *tool,
	double *sx, double *sy)
{
	struct wlr_cursor *cursor = T.server->cursor;
	struct wlr_surface *s = pw_input_surface_at(T.server, cursor->x, cursor->y, sx, sy);
	if (s && !wlr_surface_accepts_tablet_v2(s, tab->v2))
		s = NULL;
	if (!s) {
		wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
		tool->tablet = NULL;
		tablet_pads_focus(tab, NULL);
		return NULL;
	}
	wlr_tablet_v2_tablet_tool_notify_proximity_in(tool->v2, tab->v2, s);
	tool->tablet = tab;
	tablet_pads_focus(tab, s);
	return s;
}

/* Report the cursor position to the client holding the tool. press marks a
 * tip or button going down: that surface keeps the tool until the release.
 * Returns false if no client holds the tool, in which case nothing was sent
 * and a press must not be reported either: wlroots would mark the tool down
 * with no client, and the later release would then be lost. */
static bool tool_motion(struct pw_tablet *tab, struct pw_tool *tool, bool press)
{
	struct wlr_cursor *cursor = T.server->cursor;
	double sx, sy;
	if (tool->grabbed) {
		sx = cursor->x - tool->grab_ox;
		sy = cursor->y - tool->grab_oy;
	} else {
		struct wlr_surface *s = tool_focus(tab, tool, &sx, &sy);
		if (!s)
			return false;
		if (press) {
			tool->grabbed = true;
			tool->grab_ox = cursor->x - sx;
			tool->grab_oy = cursor->y - sy;
			pw_input_focus_surface(T.server, s);
		}
	}
	wlr_tablet_v2_tablet_tool_notify_motion(tool->v2, sx, sy);
	return true;
}

static void tool_release_check(struct pw_tool *tool)
{
	if (!tool->v2->is_down && tool->v2->num_buttons == 0)
		tool->grabbed = false;
}

/* Tablet events are dropped while the output mapping is wrong, while the
 * display is leased (the scene no longer matches the screen), and as the
 * waking event of a blanked display. Releases and proximity out always pass,
 * or the client would be left with a pressed tool. */
static bool tool_blocked(const struct pw_tablet *tab)
{
	return !tab->usable || pw_lease_active(T.server);
}

static void handle_proximity(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_tablet_tool_proximity_event *ev = data;
	struct pw_tablet *tab = tablet_find(ev->tablet);
	struct pw_tool *tool = tab ? tool_get(ev->tool) : NULL;
	if (!tool)
		return;
	if (ev->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
		wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
		tool->grabbed = false;
		tool->tablet = NULL;
		tablet_pads_focus(tab, NULL);
		return;
	}
	if (tool_blocked(tab) || pw_input_activity(T.server))
		return;
	wlr_cursor_warp_absolute(T.server->cursor, &ev->tablet->base, ev->x, ev->y);
	tool_motion(tab, tool, false);
}

static void handle_axis(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_tablet_tool_axis_event *ev = data;
	struct pw_tablet *tab = tablet_find(ev->tablet);
	struct pw_tool *tool = tab ? tool_get(ev->tool) : NULL;
	if (!tool || tool_blocked(tab) || pw_input_activity(T.server))
		return;

	if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y)) {
		/* NAN keeps the axis that did not change, as in wlroots */
		wlr_cursor_warp_absolute(T.server->cursor, &ev->tablet->base,
			ev->updated_axes & WLR_TABLET_TOOL_AXIS_X ? ev->x : NAN,
			ev->updated_axes & WLR_TABLET_TOOL_AXIS_Y ? ev->y : NAN);
		tool_motion(tab, tool, false);
	}
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE)
		wlr_tablet_v2_tablet_tool_notify_pressure(tool->v2, ev->pressure);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE)
		wlr_tablet_v2_tablet_tool_notify_distance(tool->v2, ev->distance);
	if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
		wlr_tablet_v2_tablet_tool_notify_tilt(tool->v2, ev->tilt_x, ev->tilt_y);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION)
		wlr_tablet_v2_tablet_tool_notify_rotation(tool->v2, ev->rotation);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER)
		wlr_tablet_v2_tablet_tool_notify_slider(tool->v2, ev->slider);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL)
		wlr_tablet_v2_tablet_tool_notify_wheel(tool->v2, ev->wheel_delta, 0);
}

static void handle_tip(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_tablet_tool_tip_event *ev = data;
	struct pw_tablet *tab = tablet_find(ev->tablet);
	struct pw_tool *tool = tab ? tool_get(ev->tool) : NULL;
	if (!tool)
		return;

	if (ev->state == WLR_TABLET_TOOL_TIP_UP) {
		/* nothing to release if the press was dropped */
		if (!tool->v2->is_down)
			return;
		wlr_tablet_v2_tablet_tool_notify_up(tool->v2);
		tool_release_check(tool);
		return;
	}
	if (tool_blocked(tab) || pw_input_activity(T.server))
		return;
	wlr_cursor_warp_absolute(T.server->cursor, &ev->tablet->base, ev->x, ev->y);
	if (!tool_motion(tab, tool, true))
		return;
	wlr_tablet_v2_tablet_tool_notify_down(tool->v2);
	wlr_tablet_tool_v2_start_implicit_grab(tool->v2);
}

static bool tool_button_pressed(const struct pw_tool *tool, uint32_t button)
{
	for (size_t i = 0; i < tool->v2->num_buttons; i++)
		if (tool->v2->pressed_buttons[i] == button)
			return true;
	return false;
}

static void handle_button(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_tablet_tool_button_event *ev = data;
	struct pw_tablet *tab = tablet_find(ev->tablet);
	struct pw_tool *tool = tab ? tool_get(ev->tool) : NULL;
	if (!tool)
		return;

	if (ev->state == WLR_BUTTON_RELEASED) {
		if (!tool_button_pressed(tool, ev->button))
			return;
		wlr_tablet_v2_tablet_tool_notify_button(tool->v2, ev->button,
			ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
		tool_release_check(tool);
		return;
	}
	if (tool_blocked(tab) || pw_input_activity(T.server))
		return;
	if (!tool_motion(tab, tool, true))
		return;
	wlr_tablet_v2_tablet_tool_notify_button(tool->v2, ev->button,
		ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED);
	wlr_tablet_tool_v2_start_implicit_grab(tool->v2);
}

/* ---- setup ------------------------------------------------------------ */

void pw_tablet_init(struct pw_server *server)
{
	wl_list_init(&T.tablets);
	wl_list_init(&T.pads);
	wl_list_init(&T.tools);
	T.server = server;
	server->tablet_mgr = T.mgr = wlr_tablet_v2_create(server->display);
	if (!T.mgr) {
		pw_log(WLR_ERROR, "cannot create the tablet manager, tablets are ignored");
		return;
	}
	T.axis.notify = handle_axis;
	wl_signal_add(&server->cursor->events.tablet_tool_axis, &T.axis);
	T.proximity.notify = handle_proximity;
	wl_signal_add(&server->cursor->events.tablet_tool_proximity, &T.proximity);
	T.tip.notify = handle_tip;
	wl_signal_add(&server->cursor->events.tablet_tool_tip, &T.tip);
	T.button.notify = handle_button;
	wl_signal_add(&server->cursor->events.tablet_tool_button, &T.button);
	T.added = true;
}

void pw_tablet_finish(struct pw_server *server)
{
	(void)server;
	if (!T.added)
		return;
	wl_list_remove(&T.axis.link);
	wl_list_remove(&T.proximity.link);
	wl_list_remove(&T.tip.link);
	wl_list_remove(&T.button.link);
	T.added = false;
}
