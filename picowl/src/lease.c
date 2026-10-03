/*
 * lease.c - wp_drm_lease_device_v1. The media player (--vo drm:lease) drives
 * KMS directly on the leased output while picowl keeps its session, its input
 * and its power policy. wlroots does the protocol and the kernel lease; this
 * file decides who may lease (leasepolicy.c), and keeps picowl's state right
 * while the lease lasts. See doc/design/drm-lease.md.
 *
 * Granting destroys the wlr_output, so picowl runs with no outputs until the
 * lease ends: then wlroots emits new_output again and output.c adopts it.
 */
#include <stdlib.h>
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include "picowl.h"
#include "lease.h"
#include "leasepolicy.h"
#include "power.h"

struct pw_lease {
	struct pw_server *server;
	struct wlr_drm_lease_v1_manager *mgr;
	struct wlr_drm_lease_v1 *active;       /* NULL when not leased */
	struct pw_view *view;                  /* lessee's focused view at the grant */
	struct wl_listener request;
	struct wl_listener drm_lease_destroy;  /* active->drm_lease->events.destroy */
	struct wl_listener session_active;
	bool session_listening;
};

static struct pw_lease *lease_of(struct pw_server *server)
{
	return server->lease;
}

static struct pw_output *output_of(struct pw_server *server, struct wlr_output *wo)
{
	struct pw_output *o;
	wl_list_for_each(o, &server->outputs, link)
		if (o->wlr_output == wo)
			return o;
	return NULL;
}

/* The mode size before any hardware rotation: the frame the lessee renders
 * and the touch panel reports in. wlroots swaps the modes for 90/270. */
static void native_size(const struct pw_output *o, int *w, int *h)
{
	struct wlr_output_mode *m = wlr_output_preferred_mode(o->wlr_output);
	*w = m ? m->width : o->wlr_output->width;
	*h = m ? m->height : o->wlr_output->height;
	if (o->hw_rotation)
		pw_rot_logical_size(o->rotation, *w, *h, w, h);
}

/* The lease is over: wlroots' own listener on the same signal ran first and
 * freed its wlr_drm_lease_v1, and wlroots asserts that no listener is left
 * when this returns. It rescans the connectors right after. */
static void handle_drm_lease_destroy(struct wl_listener *listener, void *data)
{
	struct pw_lease *l = wl_container_of(listener, l, drm_lease_destroy);
	struct pw_server *server = l->server;
	(void)data;

	wl_list_remove(&l->drm_lease_destroy.link);
	l->active = NULL;
	l->view = NULL;
	pw_log(WLR_INFO, "lease: ended, taking the output back");
	pw_input_lease_touch(server, NULL);
	pw_power_inhibit(server, PW_INHIBIT_LEASE, false);
}

static void handle_request(struct wl_listener *listener, void *data)
{
	struct pw_lease *l = wl_container_of(listener, l, request);
	struct pw_server *server = l->server;
	struct wlr_drm_lease_request_v1 *req = data;
	struct pw_view *view = server->focused_view;

	struct pw_lease_facts facts = {
		.enabled = server->config->lease_enable,
		.leased = l->active != NULL,
		.session_active = !server->session || server->session->active,
		.exclusive_focus = pw_layer_has_exclusive_focus(server),
		.requester_focused = view && wl_resource_get_client(
			view->xdg_toplevel->resource) == wl_resource_get_client(req->resource),
		.app_id = view ? view->xdg_toplevel->app_id : NULL,
	};
	enum pw_lease_verdict v = pw_lease_decide(&facts, server->config->lease_allow);
	if (v != PW_LEASE_GRANT) {
		pw_log(WLR_INFO, "lease: rejected (%s)", pw_lease_verdict_name(v));
		wlr_drm_lease_request_v1_reject(req);
		return;
	}
	/* One output, one lease. */
	struct pw_output *po = req->n_connectors == 1 ?
		output_of(server, req->connectors[0]->output) : NULL;
	if (!po) {
		pw_log(WLR_INFO, "lease: rejected (the request must name one active output)");
		wlr_drm_lease_request_v1_reject(req);
		return;
	}

	/* The output is destroyed during the grant. */
	struct wlr_box box = { 0 };
	native_size(po, &box.width, &box.height);
	pw_log(WLR_INFO, "lease: granting %s (%dx%d) to '%s'", po->wlr_output->name,
		box.width, box.height,
		view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "");

	/* The lessee modesets a display which picowl must not think is blank. */
	if (server->blanked)
		pw_power_set_blanked(server, false);

	/* On failure wlroots has sent `finished` already, and the request is not
	 * rejected a second time here. picowl keeps the output. */
	struct wlr_drm_lease_v1 *lease = wlr_drm_lease_request_v1_grant(req);
	if (!lease) {
		pw_log(WLR_ERROR, "lease: grant failed, keeping the output");
		return;
	}
	l->active = lease;
	l->view = view;
	l->drm_lease_destroy.notify = handle_drm_lease_destroy;
	wl_signal_add(&lease->drm_lease->events.destroy, &l->drm_lease_destroy);

	pw_power_inhibit(server, PW_INHIBIT_LEASE, true);
	pw_input_lease_touch(server, &box);
}

/* The session is paused while picowl is still master (wlroots emits this
 * before it gives the device up): the protocol asks for the revoke. */
static void handle_session_active(struct wl_listener *listener, void *data)
{
	struct pw_lease *l = wl_container_of(listener, l, session_active);
	(void)data;
	if (!l->server->session->active)
		pw_lease_revoke(l->server, "session paused");
}

void pw_lease_init(struct pw_server *server)
{
	if (!server->config->lease_enable) {
		pw_log(WLR_INFO, "lease: disabled by config");
		return;
	}
	struct pw_lease *l = calloc(1, sizeof(*l));
	if (!l) {
		pw_log(WLR_ERROR, "lease: out of memory");
		return;
	}
	l->server = server;
	l->mgr = wlr_drm_lease_v1_manager_create(server->display, server->backend);
	if (!l->mgr) {
		/* Headless or nested backend, or the card cannot be opened as a
		 * non-master (the user is not in the video group). */
		pw_log(WLR_INFO, "lease: no DRM backend, disabled");
		free(l);
		return;
	}
	l->request.notify = handle_request;
	wl_signal_add(&l->mgr->events.request, &l->request);
	if (server->session) {
		l->session_active.notify = handle_session_active;
		wl_signal_add(&server->session->events.active, &l->session_active);
		l->session_listening = true;
	}
	server->lease = l;
	pw_log(WLR_INFO, "lease: wp_drm_lease_device_v1 offered, allow '%s'",
		server->config->lease_allow);
}

void pw_lease_offer(struct pw_output *output)
{
	struct pw_lease *l = lease_of(output->server);
	if (!l || !wlr_output_is_drm(output->wlr_output))
		return;
	if (wlr_drm_lease_v1_manager_offer_output(l->mgr, output->wlr_output))
		pw_log(WLR_INFO, "lease: offering %s", output->wlr_output->name);
}

bool pw_lease_active(struct pw_server *server)
{
	struct pw_lease *l = lease_of(server);
	return l && l->active;
}

void pw_lease_revoke(struct pw_server *server, const char *reason)
{
	struct pw_lease *l = lease_of(server);
	if (!l || !l->active)
		return;
	pw_log(WLR_INFO, "lease: revoking (%s)", reason);
	/* Frees l->active and clears it, see handle_drm_lease_destroy. */
	wlr_drm_lease_v1_revoke(l->active);
}

struct wlr_surface *pw_lease_touch_target(struct pw_server *server)
{
	struct pw_lease *l = lease_of(server);
	return l && l->active ? l->view->xdg_toplevel->base->surface : NULL;
}

bool pw_lease_blocks_focus(struct pw_server *server, const struct pw_view *view)
{
	struct pw_lease *l = lease_of(server);
	return l && l->active && l->view != view;
}

void pw_lease_view_gone(struct pw_server *server, struct pw_view *view)
{
	struct pw_lease *l = lease_of(server);
	if (l && l->active && l->view == view)
		pw_lease_revoke(server, "lessee unmapped");
}

void pw_lease_finish(struct pw_server *server)
{
	struct pw_lease *l = lease_of(server);
	if (!l)
		return;
	/* wlroots asserts at display destroy that no request listener is left. */
	wl_list_remove(&l->request.link);
	if (l->session_listening)
		wl_list_remove(&l->session_active.link);
	pw_lease_revoke(server, "shutting down");
	server->lease = NULL;
	free(l);
}
