#include "rotationproto.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include "picowl.h"
#include "picowl-rotation-v1-protocol.h"

struct pw_rot_res {
	struct wl_list link;
	struct wl_resource *res;
	struct pw_output *output;   /* NULL once the output is gone */
	bool sent;
	uint32_t transform, hardware, subpixel; /* what was sent last */
};

static struct {
	struct wl_global *global;
	struct pw_server *server;
	struct wl_list res;
} st;

/* Not on a repeat: a layout change calls this too. */
static void rot_send(struct pw_rot_res *r)
{
	struct pw_output *o = r->output;
	if (!o)
		return;
	uint32_t t = (uint32_t)o->rotation, h = o->hw_rotation ? 1 : 0;
	uint32_t s = (uint32_t)o->native_subpixel;
	if (r->sent && r->transform == t && r->hardware == h && r->subpixel == s)
		return;
	r->sent = true;
	r->transform = t;
	r->hardware = h;
	r->subpixel = s;
	picowl_rotation_v1_send_rotation(r->res, t, h, s);
}

static void rot_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static const struct picowl_rotation_v1_interface rot_impl = {
	.destroy = rot_destroy_req,
};

static void rot_resource_destroy(struct wl_resource *r)
{
	struct pw_rot_res *rr = wl_resource_get_user_data(r);
	wl_list_remove(&rr->link);
	free(rr);
}

static void mgr_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void mgr_get_rotation(struct wl_client *client, struct wl_resource *mgr,
	uint32_t id, struct wl_resource *output_res)
{
	struct pw_rot_res *rr = calloc(1, sizeof(*rr));
	if (!rr) {
		wl_client_post_no_memory(client);
		return;
	}
	rr->res = wl_resource_create(client, &picowl_rotation_v1_interface,
		wl_resource_get_version(mgr), id);
	if (!rr->res) {
		free(rr);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(rr->res, &rot_impl, rr, rot_resource_destroy);
	wl_list_insert(&st.res, &rr->link);

	struct wlr_output *wo = wlr_output_from_resource(output_res);
	struct pw_output *o;
	wl_list_for_each(o, &st.server->outputs, link)
		if (o->wlr_output == wo)
			rr->output = o;
	rot_send(rr);
}

static const struct picowl_rotation_manager_v1_interface mgr_impl = {
	.destroy = mgr_destroy_req,
	.get_rotation = mgr_get_rotation,
};

static void mgr_bind(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(client,
		&picowl_rotation_manager_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}

bool pw_rotationproto_init(struct pw_server *s)
{
	st.server = s;
	wl_list_init(&st.res);
	/* A test fixture: a compositor that does not tell, for the panel's
	 * fallback. */
	const char *no = getenv("PICOWL_TEST_NO_ROTATION_HINT");
	if (no && !strcmp(no, "1"))
		return true;
	st.global = wl_global_create(s->display,
		&picowl_rotation_manager_v1_interface, 1, NULL, mgr_bind);
	if (!st.global)
		pw_log(WLR_ERROR, "cannot create the picowl-rotation-v1 global");
	/* A missing global only costs the panel its hint. */
	return true;
}

void pw_rotationproto_finish(struct pw_server *s)
{
	(void)s;
	if (st.global)
		wl_global_destroy(st.global);
	st.global = NULL;
}

void pw_rotationproto_output_changed(struct pw_output *o)
{
	struct pw_rot_res *r;
	wl_list_for_each(r, &st.res, link)
		if (r->output == o)
			rot_send(r);
}

void pw_rotationproto_output_removed(struct pw_output *o)
{
	struct pw_rot_res *r;
	wl_list_for_each(r, &st.res, link)
		if (r->output == o)
			r->output = NULL;
}
