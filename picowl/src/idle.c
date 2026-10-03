/* idle.c - idle notifier (ext-idle-notify-v1) and idle inhibitors
 * (idle-inhibit-v1); dim/blank timing is in power.c. */
#include <stdlib.h>
#include "picowl.h"
#include "power.h"

/* Per-inhibitor state, found through wlr_idle_inhibitor_v1.data. wlroots
 * keeps the list; NULL data means ignored or already dying. */
struct pw_inhibitor {
	struct pw_server *server;
	struct wlr_idle_inhibitor_v1 *wlr;
	struct wl_listener destroy;      /* wlr inhibitor events.destroy */
	struct wl_listener map, unmap;   /* on the inhibitor's own surface */
};

/* Does an inhibitor on surf count? Only a surface the user can see: the
 * focused toplevel (all toplevels are maximized, so the rest are covered), or
 * a layer surface whose scene node is shown. Popups and subsurfaces count
 * with their owner. *who gets a name of the owner for the log. */
static bool inhibitor_counts(struct pw_server *s, struct wlr_surface *surf,
	const char **who)
{
	for (int depth = 0; surf && depth < 8; depth++) {
		struct wlr_surface *root = wlr_surface_get_root_surface(surf);
		if (!root->mapped)
			return false;
		struct wlr_xdg_popup *pp = wlr_xdg_popup_try_from_wlr_surface(root);
		if (pp) {
			surf = pp->parent;
			continue;
		}
		struct wlr_xdg_toplevel *tl = wlr_xdg_toplevel_try_from_wlr_surface(root);
		if (tl) {
			if (!s->focused_view || s->focused_view->xdg_toplevel != tl)
				return false;
			*who = tl->app_id;
			return true;
		}
		struct wlr_layer_surface_v1 *l = wlr_layer_surface_v1_try_from_wlr_surface(root);
		struct pw_layer_surface *ls = l ? l->data : NULL;
		if (!ls)
			return false;
		int x, y;
		if (l->current.layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
			/* false while autohide has the panel hidden */
			if (!wlr_scene_node_coords(&ls->scene_tree->node, &x, &y))
				return false;
		} else if (s->focused_view) {
			return false; /* under a maximized app */
		}
		*who = l->namespace;
		return true;
	}
	return false;
}

void pw_idle_inhibit_update(struct pw_server *s)
{
	if (!s->idle_inhibit_mgr)
		return;
	const char *who = NULL;
	bool on = false;
	struct wlr_idle_inhibitor_v1 *wi;
	wl_list_for_each(wi, &s->idle_inhibit_mgr->inhibitors, link) {
		if (wi->data && inhibitor_counts(s, wi->surface, &who)) {
			on = true;
			break;
		}
	}
	if (on == s->idle_inhibited)
		return;
	s->idle_inhibited = on;
	if (on)
		pw_log(WLR_INFO, "idle inhibit on (app_id %s)", who ? who : "");
	else
		pw_log(WLR_INFO, "idle inhibit off");
	/* ext-idle-notify gets the raw result; power.c applies the profile. */
	if (s->idle_notifier)
		wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, on);
	pw_power_inhibit(s, PW_INHIBIT_CLIENT, on);
}

static void inhibitor_map(struct wl_listener *listener, void *data)
{
	struct pw_inhibitor *in = wl_container_of(listener, in, map);
	(void)data;
	pw_idle_inhibit_update(in->server);
}

static void inhibitor_unmap(struct wl_listener *listener, void *data)
{
	struct pw_inhibitor *in = wl_container_of(listener, in, unmap);
	(void)data;
	pw_idle_inhibit_update(in->server);
}

static void inhibitor_destroy(struct wl_listener *listener, void *data)
{
	struct pw_inhibitor *in = wl_container_of(listener, in, destroy);
	(void)data;
	/* wlroots unlinks the inhibitor only after this signal: hide it first. */
	in->wlr->data = NULL;
	pw_idle_inhibit_update(in->server);
	wl_list_remove(&in->destroy.link);
	wl_list_remove(&in->map.link);
	wl_list_remove(&in->unmap.link);
	free(in);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data)
{
	struct pw_server *server = wl_container_of(listener, server, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *wi = data;
	struct pw_inhibitor *in = calloc(1, sizeof(*in));
	if (!in) {
		pw_log(WLR_ERROR, "idle inhibitor: out of memory, ignoring it");
		return;
	}
	in->server = server;
	in->wlr = wi;
	wi->data = in;
	in->destroy.notify = inhibitor_destroy;
	wl_signal_add(&wi->events.destroy, &in->destroy);
	in->map.notify = inhibitor_map;
	wl_signal_add(&wi->surface->events.map, &in->map);
	in->unmap.notify = inhibitor_unmap;
	wl_signal_add(&wi->surface->events.unmap, &in->unmap);
	pw_idle_inhibit_update(server);
}

void pw_idle_init(struct pw_server *server)
{
	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
	if (!server->idle_notifier)
		pw_log(WLR_ERROR, "cannot create idle notifier");
	server->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server->display);
	if (!server->idle_inhibit_mgr) {
		pw_log(WLR_ERROR, "cannot create idle inhibit manager");
		return;
	}
	server->new_idle_inhibitor.notify = handle_new_inhibitor;
	wl_signal_add(&server->idle_inhibit_mgr->events.new_inhibitor,
		&server->new_idle_inhibitor);
}

/* Tell ext-idle-notify clients about user activity. */
void pw_idle_notify(struct pw_server *server)
{
	if (server->idle_notifier && server->seat)
		wlr_idle_notifier_v1_notify_activity(server->idle_notifier,
			server->seat);
}

/* Notify idle clients and drive the dim/blank state machine (power.c). */
void pw_idle_activity(struct pw_server *server)
{
	pw_idle_notify(server);
	(void)pw_power_activity(server);
}
