/*
 * view.c - xdg-shell toplevels and popups, focus, stacking, foreign-toplevel.
 *
 * PDA window policy: every toplevel is maximized to the usable area of the
 * first output; move/resize/minimize requests are acknowledged and ignored.
 * The one exception is the pair named by [layout] stack: while both are
 * mapped they share the usable area (see tile.h).
 *
 * Extra per-view state (the ext-foreign-toplevel handle) lives in a private
 * wrapper around struct pw_view so that picowl.h needs no change.
 */
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>

#include "picowl.h"
#include "tile.h"

struct view_impl {
	struct pw_view base;	/* must be first */
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;
	int hint_w, hint_h;	/* fixed size the client announced at its last commit, 0 = none */
};

struct popup {
	struct wlr_xdg_popup *xdg_popup;
	struct pw_server *server;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/* File-local state: one server per process. */
static struct wlr_ext_foreign_toplevel_list_v1 *ext_list;
static struct wl_listener new_xdg_popup_listener;
static struct pw_server *the_server;

static struct view_impl *impl_of(struct pw_view *view)
{
	return (struct view_impl *)view;
}

static struct pw_output *first_output(struct pw_server *server)
{
	if (wl_list_empty(&server->outputs))
		return NULL;
	return wl_container_of(server->outputs.next, (struct pw_output *)NULL, link);
}

/* ---- geometry --------------------------------------------------------- */

/* Position of app_id in [layout] stack, or -1. */
static int stack_index(struct pw_server *server, const char *app_id)
{
	struct pw_config *c = server->config;

	if (!c || c->n_stack != 2 || !app_id)
		return -1;
	for (int i = 0; i < 2; i++)
		if (strcmp(c->stack[i], app_id) == 0)
			return i;
	return -1;
}

static int view_stack_index(struct pw_view *view)
{
	return stack_index(view->server, view->xdg_toplevel->app_id);
}

/* The fixed size of a client is the only hint wlroots can carry: min_size
 * equal to max_size, both axes. The values are double-buffered state, so
 * current is what the client committed. */
static void view_fixed_size(struct pw_view *view, int *w, int *h)
{
	struct wlr_xdg_toplevel *tl = view->xdg_toplevel;

	*w = *h = 0;
	if (tl->current.min_width > 0 && tl->current.min_width == tl->current.max_width &&
			tl->current.min_height > 0 &&
			tl->current.min_height == tl->current.max_height) {
		*w = tl->current.min_width;
		*h = tl->current.min_height;
	}
}

/* Remember the hint of the last commit; true if it differs from the one
 * before. A client changing it (a clip with another aspect) must re-layout. */
static bool view_hint_changed(struct pw_view *view)
{
	struct view_impl *impl = impl_of(view);
	int w, h;

	view_fixed_size(view, &w, &h);
	if (w == impl->hint_w && h == impl->hint_h)
		return false;
	impl->hint_w = w;
	impl->hint_h = h;
	return true;
}

/* The window which takes the keyboard when view maps. With [layout] focus
 * naming a stack app, the other stack app appearing must not take it from
 * there: on the demo device the terminal maps first and the player maps
 * later, and every restart of the player would steal the on-screen
 * keyboard's keys. Other apps are not covered, they take the focus as ever. */
static struct pw_view *map_focus_target(struct pw_server *server, struct pw_view *view)
{
	struct pw_view *v;
	const char *name = server->config ? server->config->focus : NULL;

	if (!name || view_stack_index(view) < 0)
		return view;
	wl_list_for_each(v, &server->views, link) {
		const char *id = v->xdg_toplevel->app_id;

		if (v->mapped && id && strcmp(id, name) == 0 && view_stack_index(v) >= 0)
			return v;
	}
	return view;
}

/* Both stacked apps mapped: fill first and second with them and slots[] with
 * their boxes. Several mapped views with one app_id: the most recently
 * focused (list front) is the one which tiles.
 *
 * While a [layout] pan surface (the keyboard) is shown and the stack is split
 * top and bottom, the slots are those of the area without the surface and
 * *pan is how far the stack moves up (see pw_tile_pan), so the windows keep
 * their size. Otherwise the slots lie in the usable area, which has every
 * exclusive zone taken off, and *pan is 0. pan may be NULL when the caller
 * only asks whether the pair tiles. */
static bool tile_active(struct pw_server *server, struct pw_view **first,
		struct pw_view **second, struct wlr_box slots[2], int *pan)
{
	struct pw_output *output = first_output(server);
	struct pw_view *v, *pair[2] = { NULL, NULL };
	struct pw_tile_box usable, out[2];
	struct pw_tile_hint hints[2] = {{0}, {0}};
	struct wlr_box area;

	if (!output || !server->config || server->config->n_stack != 2)
		return false;
	wl_list_for_each(v, &server->views, link) {
		int i = v->mapped ? view_stack_index(v) : -1;
		if (i >= 0 && !pair[i])
			pair[i] = v;
	}
	if (!pair[0] || !pair[1])
		return false;

	pw_output_usable_area(output, &area);
	/* A side by side split cannot be panned: both windows span the height, so
	 * the top of each would be cut off. It shrinks like any other window. */
	bool panned = output->pan_zone > 0 &&
		output->tile_area.height >= output->tile_area.width;
	if (panned)
		area = output->tile_area;
	usable = (struct pw_tile_box){ area.x, area.y, area.width, area.height };
	for (int i = 0; i < 2; i++) {
		const struct pw_app_rule *rule = pw_config_app(server->config,
			pair[i]->xdg_toplevel->app_id);
		view_fixed_size(pair[i], &hints[i].fixed_w, &hints[i].fixed_h);
		if (rule) {
			hints[i].aspect_w = rule->aspect_w;
			hints[i].aspect_h = rule->aspect_h;
		}
	}
	if (!pw_tile_layout(&usable, hints, out))
		return false;
	if (pan)
		*pan = panned ? pw_tile_pan(output->pan_zone, output->full_area.height,
			out[1].y - output->full_area.y) : 0;

	for (int i = 0; i < 2; i++)
		slots[i] = (struct wlr_box){ out[i].x, out[i].y, out[i].w, out[i].h };
	*first = pair[0];
	*second = pair[1];
	return true;
}

/* The scene needs nothing here for direct scanout: it only scans out a
 * surface which covers the whole output alone, and two windows are two
 * entries in its render list. */
static void view_arrange(struct pw_view *view)
{
	struct wlr_xdg_toplevel *tl = view->xdg_toplevel;
	struct pw_output *output = first_output(view->server);
	struct pw_view *first, *second;
	struct wlr_box box, slots[2];
	bool tiled = false;
	int pan = 0;

	if (!output || !tl->base->initialized)
		return;

	pw_output_usable_area(output, &box);
	if (box.width <= 0 || box.height <= 0)
		return;

	if (tile_active(view->server, &first, &second, slots, &pan)) {
		if (view == first) {
			box = slots[0];
			tiled = true;
		} else if (view == second) {
			box = slots[1];
			tiled = true;
		}
	}

	/* Only the scene node moves for a pan: the client is told nothing, and
	 * hit testing and popup placement read the node position. */
	wlr_scene_node_set_position(&view->scene_tree->node, box.x,
		tiled ? box.y - pan : box.y);

	/* Every wlroots setter schedules a configure even when the value is the
	 * one already scheduled, and this runs for every window on every change
	 * of the layout: a client whose slot did not change (a window under a
	 * panned stack) must not hear anything, a video player would restart its
	 * scaler on each one. */
	/* A fullscreen request cannot be honoured inside a slot. */
	bool fullscreen = !tiled && tl->requested.fullscreen;
	if (!tl->scheduled.maximized)
		wlr_xdg_toplevel_set_maximized(tl, true);
	if (tl->scheduled.fullscreen != fullscreen)
		wlr_xdg_toplevel_set_fullscreen(tl, fullscreen);
	if (tl->scheduled.width != box.width || tl->scheduled.height != box.height)
		wlr_xdg_toplevel_set_size(tl, box.width, box.height);
}

void pw_view_arrange_all(struct pw_server *server)
{
	struct pw_view *view;

	wl_list_for_each(view, &server->views, link)
		view_arrange(view);
	pw_im_arrange(server);
}

/* ---- focus ------------------------------------------------------------ */

static void view_set_activated(struct pw_view *view, bool activated)
{
	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
	if (view->foreign_handle)
		wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_handle, activated);
}

static void focus_keyboard(struct pw_server *server, struct pw_view *view)
{
	struct wlr_seat *seat = server->seat;
	struct wlr_keyboard *kb;

	if (!seat || pw_layer_has_exclusive_focus(server))
		return;
	kb = wlr_seat_get_keyboard(seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(seat, view->xdg_toplevel->base->surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(seat, view->xdg_toplevel->base->surface,
			NULL, 0, NULL);
}

/* restack false: only the keyboard focus and activation move (tiled windows
 * do not overlap, so there is nothing to raise). */
static void view_focus(struct pw_view *view, bool restack, const char *reason)
{
	struct pw_server *server;
	struct pw_view *prev;

	if (!view || !view->mapped)
		return;
	server = view->server;
	if (pw_lease_blocks_focus(server, view))
		return; /* a pop-up must not end the lessee's playback */
	prev = server->focused_view;

	if (prev && prev != view)
		view_set_activated(prev, false);

	if (restack) {
		struct pw_view *first, *second, *partner = NULL;
		struct wlr_box slots[2];

		/* A tiled window brings its partner along, just below it, or a
		 * maximized third app raised in between would hide one of them. */
		if (tile_active(server, &first, &second, slots, NULL)) {
			if (view == first)
				partner = second;
			else if (view == second)
				partner = first;
		}
		if (partner)
			wlr_scene_node_raise_to_top(&partner->scene_tree->node);
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
		wl_list_remove(&view->link);
		wl_list_insert(&server->views, &view->link);
		if (partner) {
			wl_list_remove(&partner->link);
			wl_list_insert(&view->link, &partner->link);
		}
	}
	server->focused_view = view;
	if (prev != view) {
		server->panel_forced_visible = false;
		/* one line per change, to tell from the journal which window took
		 * the keyboard and why */
		pw_log(WLR_INFO, "focus: app_id=%s (%s)",
			view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "", reason);
	}

	view_set_activated(view, true);
	focus_keyboard(server, view);
	pw_panel_update(server);
}

/* The callers are a touch (or a request from a foreign-toplevel client). */
void pw_view_focus(struct pw_view *view)
{
	view_focus(view, true, "touch");
}

void pw_view_cycle(struct pw_server *server)
{
	struct pw_view *front, *next, *first, *second;
	struct wlr_box slots[2];

	/* With both tiled apps on screen, alt+Tab moves the keyboard between
	 * them; both stay where they are. */
	if (server->focused_view && tile_active(server, &first, &second, slots, NULL) &&
			(server->focused_view == first || server->focused_view == second)) {
		view_focus(server->focused_view == first ? second : first, false, "cycle");
		return;
	}

	if (wl_list_empty(&server->views) || server->views.next->next == &server->views)
		return;

	/* Rotate: front goes to the back (list and scene order), next is raised. */
	front = wl_container_of(server->views.next, front, link);
	next = wl_container_of(front->link.next, next, link);
	wl_list_remove(&front->link);
	wl_list_insert(server->views.prev, &front->link);
	wlr_scene_node_lower_to_bottom(&front->scene_tree->node);
	view_focus(next, true, "cycle");
}

void pw_view_close_focused(struct pw_server *server)
{
	if (server->focused_view)
		wlr_xdg_toplevel_send_close(server->focused_view->xdg_toplevel);
}

/* ---- toplevel listeners ------------------------------------------------ */

static void view_update_title(struct pw_view *view)
{
	struct view_impl *impl = impl_of(view);
	const char *title = view->xdg_toplevel->title ? view->xdg_toplevel->title : "";
	const char *app_id = view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "";

	if (view->foreign_handle) {
		wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, title);
		wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, app_id);
	}
	if (impl->ext_handle) {
		struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = title, .app_id = app_id,
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(impl->ext_handle, &st);
	}
}

static void foreign_request_activate(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, foreign_request_activate);
	(void)data;
	pw_view_focus(view);
}

static void foreign_request_close(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, foreign_request_close);
	(void)data;
	wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

static void view_create_handles(struct pw_view *view)
{
	struct view_impl *impl = impl_of(view);
	struct pw_server *server = view->server;
	struct pw_output *output = first_output(server);
	const char *title = view->xdg_toplevel->title ? view->xdg_toplevel->title : "";
	const char *app_id = view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "";

	if (server->foreign_toplevel_mgr) {
		view->foreign_handle = wlr_foreign_toplevel_handle_v1_create(
			server->foreign_toplevel_mgr);
		if (view->foreign_handle) {
			view->foreign_request_activate.notify = foreign_request_activate;
			wl_signal_add(&view->foreign_handle->events.request_activate,
				&view->foreign_request_activate);
			view->foreign_request_close.notify = foreign_request_close;
			wl_signal_add(&view->foreign_handle->events.request_close,
				&view->foreign_request_close);
			wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign_handle, true);
			if (output)
				wlr_foreign_toplevel_handle_v1_output_enter(
					view->foreign_handle, output->wlr_output);
		}
	}
	if (ext_list) {
		struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = title, .app_id = app_id,
		};
		impl->ext_handle = wlr_ext_foreign_toplevel_handle_v1_create(ext_list, &st);
	}
	view_update_title(view);
}

static void view_destroy_handles(struct pw_view *view)
{
	struct view_impl *impl = impl_of(view);

	if (view->foreign_handle) {
		wl_list_remove(&view->foreign_request_activate.link);
		wl_list_remove(&view->foreign_request_close.link);
		wlr_foreign_toplevel_handle_v1_destroy(view->foreign_handle);
		view->foreign_handle = NULL;
	}
	if (impl->ext_handle) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(impl->ext_handle);
		impl->ext_handle = NULL;
	}
}

static void view_map(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, map);
	(void)data;

	view->mapped = true;
	if (pw_lease_active(view->server)) {
		/* behind the lessee, which keeps the focus */
		wl_list_insert(view->server->views.prev, &view->link);
		wlr_scene_node_lower_to_bottom(&view->scene_tree->node);
	} else {
		wl_list_insert(&view->server->views, &view->link);
	}
	view_create_handles(view);
	/* A stacked app appearing changes its partner's slot too. */
	if (view_stack_index(view) >= 0)
		pw_view_arrange_all(view->server);
	else
		view_arrange(view);
	{
		struct pw_view *target = map_focus_target(view->server, view);
		struct pw_view *first, *second;
		struct wlr_box slots[2];
		const char *reason = "map";

		if (target != view)
			reason = "rule";
		else if (view_stack_index(view) >= 0 &&
				tile_active(view->server, &first, &second, slots, NULL))
			reason = "pair";
		view_focus(target, true, reason);
	}

	/* The hint is double-buffered state, so it is current once the surface
	 * maps; later changes are not tracked until something acts on them. */
	if (view->server->content_type_mgr) {
		enum wp_content_type_v1_type ct = wlr_surface_get_content_type_v1(
			view->server->content_type_mgr, view->xdg_toplevel->base->surface);
		if (ct != WP_CONTENT_TYPE_V1_TYPE_NONE)
			pw_log(WLR_DEBUG, "view '%s': content type hint %d",
				view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "",
				(int)ct);
	}
}

static void view_unmap(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, unmap);
	struct pw_server *server = view->server;
	(void)data;

	if (!view->mapped)
		return;
	view->mapped = false;
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	view_destroy_handles(view);
	pw_lease_view_gone(server, view);

	if (server->focused_view == view) {
		server->focused_view = NULL;
		if (!wl_list_empty(&server->views)) {
			struct pw_view *next = wl_container_of(server->views.next, next, link);
			view_focus(next, true, "unmap");
		} else if (server->seat) {
			wlr_seat_keyboard_notify_clear_focus(server->seat);
		}
		pw_panel_update(server);
	}
	/* The partner of a stacked app gets the whole area back. */
	if (view_stack_index(view) >= 0)
		pw_view_arrange_all(server);
}

static void view_commit(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, commit);
	(void)data;

	if (view->xdg_toplevel->base->initial_commit) {
		view_arrange(view);
		/* view_arrange() may bail out (no output / empty area): the
		 * client still needs its first configure. */
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	}
	/* min and max size take effect at commit; a stacked app which changes
	 * them at run time changes the split. */
	if (view_hint_changed(view) && view->mapped && view_stack_index(view) >= 0)
		pw_view_arrange_all(view->server);
}

static void view_destroy(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, destroy);
	struct pw_server *server = view->server;
	(void)data;

	if (view->mapped)
		view_unmap(&view->unmap, NULL);

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	view_destroy_handles(view);
	free(impl_of(view));
	pw_panel_update(server);
}

/* Requests: keep our policy, but always answer with a configure. */
static void view_request_maximize(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, request_maximize);
	(void)data;
	if (view->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	view_arrange(view);
}

static void view_request_fullscreen(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, request_fullscreen);
	(void)data;
	if (view->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	view_arrange(view);
	if (view->foreign_handle)
		wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign_handle,
			view->xdg_toplevel->requested.fullscreen);
}

static void view_request_minimize(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, request_minimize);
	(void)data;
	/* Acknowledge and ignore. */
	if (view->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void view_set_title(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, set_title);
	(void)data;
	view_update_title(view);
}

static void view_set_app_id(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, set_app_id);
	(void)data;
	view_update_title(view);
	/* the new app_id may join or leave the stack; arranging is idempotent,
	 * so views which are unaffected get no new configure */
	if (view->mapped)
		pw_view_arrange_all(view->server);
}

static void new_xdg_toplevel(struct wl_listener *l, void *data)
{
	struct pw_server *server = wl_container_of(l, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *tl = data;
	struct view_impl *impl = calloc(1, sizeof(*impl));
	struct pw_view *view;

	if (!impl) {
		wl_resource_post_no_memory(tl->resource);
		return;
	}
	view = &impl->base;
	view->server = server;
	view->xdg_toplevel = tl;
	wl_list_init(&view->link);
	view->scene_tree = wlr_scene_xdg_surface_create(server->layer_apps, tl->base);
	if (!view->scene_tree) {
		free(impl);
		wl_resource_post_no_memory(tl->resource);
		return;
	}
	view->scene_tree->node.data = view;
	tl->base->data = view->scene_tree;

	view->map.notify = view_map;
	wl_signal_add(&tl->base->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&tl->base->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&tl->base->surface->events.commit, &view->commit);
	view->destroy.notify = view_destroy;
	wl_signal_add(&tl->events.destroy, &view->destroy);
	view->request_maximize.notify = view_request_maximize;
	wl_signal_add(&tl->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&tl->events.request_fullscreen, &view->request_fullscreen);
	view->request_minimize.notify = view_request_minimize;
	wl_signal_add(&tl->events.request_minimize, &view->request_minimize);
	view->set_title.notify = view_set_title;
	wl_signal_add(&tl->events.set_title, &view->set_title);
	view->set_app_id.notify = view_set_app_id;
	wl_signal_add(&tl->events.set_app_id, &view->set_app_id);
}

/* ---- popups ----------------------------------------------------------- */

/* Find the root of a popup chain: the scene tree and geometry offset of the
 * toplevel or layer surface that ultimately owns it. */
static struct wlr_scene_tree *popup_root(struct wlr_xdg_popup *xp,
		int *gx, int *gy)
{
	struct wlr_surface *surf = xp->parent;

	*gx = *gy = 0;
	while (surf) {
		struct wlr_xdg_surface *xs = wlr_xdg_surface_try_from_wlr_surface(surf);
		if (xs) {
			if (xs->role == WLR_XDG_SURFACE_ROLE_POPUP && xs->popup) {
				surf = xs->popup->parent;
				continue;
			}
			*gx = xs->geometry.x;
			*gy = xs->geometry.y;
			return xs->data;
		}
		struct wlr_layer_surface_v1 *ls =
			wlr_layer_surface_v1_try_from_wlr_surface(surf);
		if (ls && ls->data)
			return ((struct pw_layer_surface *)ls->data)->scene_tree;
		break;
	}
	return NULL;
}

static void popup_commit(struct wl_listener *l, void *data)
{
	struct popup *popup = wl_container_of(l, popup, commit);
	struct wlr_xdg_surface *base = popup->xdg_popup->base;
	struct wlr_scene_tree *root_tree;
	struct pw_output *output;
	struct wlr_box usable, box;
	int lx, ly, gx, gy;
	(void)data;

	if (!base->initial_commit)
		return;

	output = first_output(popup->server);
	root_tree = popup_root(popup->xdg_popup, &gx, &gy);
	if (output && root_tree) {
		pw_output_usable_area(output, &usable);
		wlr_scene_node_coords(&root_tree->node, &lx, &ly);
		box.x = usable.x - lx - gx;
		box.y = usable.y - ly - gy;
		box.width = usable.width;
		box.height = usable.height;
		wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &box);
	}
	/* The initial configure must always be sent. */
	wlr_xdg_surface_schedule_configure(base);
}

static void popup_destroy(struct wl_listener *l, void *data)
{
	struct popup *popup = wl_container_of(l, popup, destroy);
	(void)data;
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

void pw_popup_create(struct pw_server *server, struct wlr_xdg_popup *xp,
		struct wlr_scene_tree *parent_tree)
{
	struct popup *popup = calloc(1, sizeof(*popup));

	if (!popup) {
		wl_resource_post_no_memory(xp->resource);
		return;
	}
	popup->xdg_popup = xp;
	popup->server = server;
	xp->base->data = wlr_scene_xdg_surface_create(parent_tree, xp->base);
	if (!xp->base->data) {
		free(popup);
		wl_resource_post_no_memory(xp->resource);
		return;
	}
	popup->commit.notify = popup_commit;
	wl_signal_add(&xp->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xp->events.destroy, &popup->destroy);
}

static void new_xdg_popup(struct wl_listener *l, void *data)
{
	struct wlr_xdg_popup *xp = data;
	struct wlr_xdg_surface *parent;
	(void)l;

	/* Popups of layer surfaces arrive here with no parent; layer.c creates
	 * them from the layer surface's own new_popup event. */
	if (!xp->parent)
		return;
	parent = wlr_xdg_surface_try_from_wlr_surface(xp->parent);
	if (!parent || !parent->data)
		return;
	pw_popup_create(the_server, xp, parent->data);
}

/* ---- init ------------------------------------------------------------- */

bool pw_view_init(struct pw_server *server)
{
	the_server = server;
	wl_list_init(&server->views);
	server->focused_view = NULL;

	server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
	if (!server->xdg_shell) {
		pw_log(WLR_ERROR, "cannot create xdg shell");
		return false;
	}
	server->foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(server->display);
	ext_list = wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);

	server->new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	new_xdg_popup_listener.notify = new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &new_xdg_popup_listener);
	return true;
}

void pw_view_finish(struct pw_server *server)
{
	(void)server;
	if (new_xdg_popup_listener.link.next) {
		wl_list_remove(&new_xdg_popup_listener.link);
		new_xdg_popup_listener.link.next = NULL;
		new_xdg_popup_listener.link.prev = NULL;
	}
}
