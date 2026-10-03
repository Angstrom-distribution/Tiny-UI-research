/*
 * view.c - xdg-shell toplevels and popups, focus, stacking, foreign-toplevel.
 *
 * PDA window policy: every toplevel is maximized to the usable area of the
 * first output; move/resize/minimize requests are acknowledged and ignored.
 *
 * Extra per-view state (the ext-foreign-toplevel handle) lives in a private
 * wrapper around struct pw_view so that picowl.h needs no change.
 */
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_compositor.h>

#include "picowl.h"

struct view_impl {
	struct pw_view base;	/* must be first */
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;
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

static void view_arrange(struct pw_view *view)
{
	struct wlr_xdg_toplevel *tl = view->xdg_toplevel;
	struct pw_output *output = first_output(view->server);
	struct wlr_box box;

	if (!output || !tl->base->initialized)
		return;

	pw_output_usable_area(output, &box);
	if (box.width <= 0 || box.height <= 0)
		return;

	wlr_scene_node_set_position(&view->scene_tree->node, box.x, box.y);

	/* The setters skip redundant configures, so this is cheap to repeat. */
	wlr_xdg_toplevel_set_maximized(tl, true);
	wlr_xdg_toplevel_set_fullscreen(tl, tl->requested.fullscreen);
	wlr_xdg_toplevel_set_size(tl, box.width, box.height);
}

void pw_view_arrange_all(struct pw_server *server)
{
	struct pw_view *view;

	wl_list_for_each(view, &server->views, link)
		view_arrange(view);
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

void pw_view_focus(struct pw_view *view)
{
	struct pw_server *server;
	struct pw_view *prev;

	if (!view || !view->mapped)
		return;
	server = view->server;
	prev = server->focused_view;

	if (prev && prev != view)
		view_set_activated(prev, false);

	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	server->focused_view = view;

	view_set_activated(view, true);
	focus_keyboard(server, view);
}

void pw_view_cycle(struct pw_server *server)
{
	struct pw_view *front, *next;

	if (wl_list_empty(&server->views) || server->views.next->next == &server->views)
		return;

	/* Rotate: front goes to the back (list and scene order), next is raised. */
	front = wl_container_of(server->views.next, front, link);
	next = wl_container_of(front->link.next, next, link);
	wl_list_remove(&front->link);
	wl_list_insert(server->views.prev, &front->link);
	wlr_scene_node_lower_to_bottom(&front->scene_tree->node);
	pw_view_focus(next);
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
	wl_list_insert(&view->server->views, &view->link);
	view_create_handles(view);
	view_arrange(view);
	pw_view_focus(view);
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

	if (server->focused_view == view) {
		server->focused_view = NULL;
		if (!wl_list_empty(&server->views)) {
			struct pw_view *next = wl_container_of(server->views.next, next, link);
			pw_view_focus(next);
		} else if (server->seat) {
			wlr_seat_keyboard_notify_clear_focus(server->seat);
		}
	}
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
}

static void view_destroy(struct wl_listener *l, void *data)
{
	struct pw_view *view = wl_container_of(l, view, destroy);
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

void pw_view_init(struct pw_server *server)
{
	the_server = server;
	wl_list_init(&server->views);
	server->focused_view = NULL;

	server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
	server->foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(server->display);
	ext_list = wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);

	server->new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	new_xdg_popup_listener.notify = new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &new_xdg_popup_listener);
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
