/*
 * zerocopy.c - linux-dmabuf custom feedback and picowl_buffer_manager_v1.
 *
 * Only active with a DRM backend whose allocator can hand out dmabufs.
 * Commit, timer, frame and present paths do not allocate.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <drm_fourcc.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "picowl.h"
#include "picowl-buffer-v1-protocol.h"

/* ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT (an enum in the
 * generated linux-dmabuf protocol header, which picowl does not generate). */
#define PW_TRANCHE_FLAGS_SCANOUT 1u

#define PW_ZB_MAX_PER_CLIENT 3
#define PW_ZB_BUDGET (2u * 1024 * 1024)

struct pw_zbuf {
	struct wl_list link;
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_buffer *buffer;
	uint32_t bytes;
	struct pw_copyrel rel;
	struct wlr_scene_buffer *sbuf;  /* the surface's scene buffer, once found */
	struct wl_listener commit;
	struct wl_listener surface_destroy;
	struct wl_listener sample;      /* sbuf output_sample */
	struct wl_listener sbuf_destroy;
};

static struct {
	struct pw_server *server;
	struct wl_global *global;
	struct wl_list zbufs;   /* struct pw_zbuf */
	struct wl_list mgrs;    /* resources bound, via wl_resource_get_link */
	struct wl_event_source *idle_timer;
	struct wlr_drm_format_set fmtset;
	uint32_t total_bytes;
	bool active;
	bool copy_type;
} st;

static bool any_copy_type(void)
{
	struct pw_output *o;
	wl_list_for_each(o, &st.server->outputs, link)
		if (o->copy_type && o->wlr_output->enabled)
			return true;
	return false;
}

static void largest_output(int *mw, int *mh)
{
	int w = 0, h = 0;
	struct pw_output *o;
	wl_list_for_each(o, &st.server->outputs, link) {
		int ow = o->wlr_output->width, oh = o->wlr_output->height;
		if (ow > w) w = ow;
		if (oh > h) h = oh;
	}
	*mw = w; *mh = h;
}

static bool check_dmabuf(struct wlr_dmabuf_attributes *a, void *data)
{
	(void)data;
	if (a->n_planes != 1 || a->format != DRM_FORMAT_RGB565)
		return false;
	if (a->modifier != DRM_FORMAT_MOD_LINEAR &&
	    a->modifier != DRM_FORMAT_MOD_INVALID)
		return false;
	int mw, mh;
	largest_output(&mw, &mh);
	if (a->width <= 0 || a->height <= 0)
		return false;
	/* rows are read as width * 2 bytes at offset + y * stride (pixman wants
	 * 32-bit alignment): a smaller stride reads past the end of the buffer */
	if (a->stride[0] < (uint32_t)a->width * 2 || a->stride[0] % 4 ||
	    a->offset[0] % 4)
		return false;
	/* either orientation: clients may render for a rotated output */
	int big = mw > mh ? mw : mh;
	return a->width <= big && a->height <= big;
}

static void send_copy_type(struct wl_resource *r)
{
	picowl_buffer_manager_v1_send_copy_type(r, st.copy_type ? 1 : 0);
}

static void refresh_copy_type(void)
{
	bool v = any_copy_type();
	if (v == st.copy_type)
		return;
	st.copy_type = v;
	wlr_log(WLR_DEBUG, "zero-copy: copy_type now %d", v);
	struct wl_resource *r;
	wl_resource_for_each(r, &st.mgrs)
		send_copy_type(r);
}

/* ---- picowl_buffer_v1 ---------------------------------------------- */

static void zbuf_unbind_sbuf(struct pw_zbuf *z)
{
	if (!z->sbuf)
		return;
	wl_list_remove(&z->sample.link);
	wl_list_remove(&z->sbuf_destroy.link);
	z->sbuf = NULL;
}

static void zbuf_detach(struct pw_zbuf *z)
{
	if (!z->surface)
		return;
	zbuf_unbind_sbuf(z);
	wl_list_remove(&z->commit.link);
	wl_list_remove(&z->surface_destroy.link);
	z->surface = NULL;
}

static struct wlr_scene_buffer *find_sbuf(struct wlr_scene_tree *tree,
	struct wlr_surface *surface)
{
	struct wlr_scene_node *n;
	wl_list_for_each(n, &tree->children, link) {
		if (n->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *b = wlr_scene_buffer_from_node(n);
			struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(b);
			if (ss && ss->surface == surface)
				return b;
		} else if (n->type == WLR_SCENE_NODE_TREE) {
			struct wlr_scene_buffer *b =
				find_sbuf(wlr_scene_tree_from_node(n), surface);
			if (b)
				return b;
		}
	}
	return NULL;
}

static struct pw_output *output_of_scene_output(struct wlr_scene_output *so)
{
	struct pw_output *o;
	wl_list_for_each(o, &st.server->outputs, link)
		if (o->scene_output == so)
			return o;
	return NULL;
}

/* The scene sampled the surface's buffer: by direct scanout (the kernel
 * copies it during this commit on a copy-type output) or by compositing
 * (pixman reads it now and whenever the region is redrawn). Only the first
 * allows early release, everything else is "retained". */
static void zbuf_sample(struct wl_listener *l, void *data)
{
	const struct wlr_scene_output_sample_event *ev = data;
	struct pw_zbuf *z = wl_container_of(l, z, sample);
	struct pw_output *o = output_of_scene_output(ev->output);
	uint32_t serial;

	if (ev->direct_scanout && o && o->copy_type) {
		pw_copyrel_on_direct(&z->rel);
		return;
	}
	if (pw_copyrel_on_retain(&z->rel, false, &serial))
		picowl_buffer_v1_send_retained(z->resource, serial);
}

static void zbuf_sbuf_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_zbuf *z = wl_container_of(l, z, sbuf_destroy);
	zbuf_unbind_sbuf(z);
}

static void zbuf_bind_sbuf(struct pw_zbuf *z)
{
	struct wlr_scene_buffer *b = find_sbuf(&st.server->scene->tree, z->surface);
	if (!b)
		return;
	z->sbuf = b;
	z->sample.notify = zbuf_sample;
	wl_signal_add(&b->events.output_sample, &z->sample);
	z->sbuf_destroy.notify = zbuf_sbuf_destroy;
	wl_signal_add(&b->node.events.destroy, &z->sbuf_destroy);
}

static void zbuf_commit(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_zbuf *z = wl_container_of(l, z, commit);
	pw_copyrel_on_commit(&z->rel);
	if (!z->sbuf)
		zbuf_bind_sbuf(z);
	if (st.idle_timer)
		wl_event_source_timer_update(st.idle_timer, 1);
}

static void zbuf_surface_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct pw_zbuf *z = wl_container_of(l, z, surface_destroy);
	zbuf_detach(z);
}

static void buffer_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void buffer_attach_surface(struct wl_client *c, struct wl_resource *r,
	struct wl_resource *surf)
{
	(void)c;
	struct pw_zbuf *z = wl_resource_get_user_data(r);
	if (!z)
		return;
	zbuf_detach(z);
	z->surface = wlr_surface_from_resource(surf);
	pw_copyrel_init(&z->rel);
	z->commit.notify = zbuf_commit;
	wl_signal_add(&z->surface->events.commit, &z->commit);
	z->surface_destroy.notify = zbuf_surface_destroy;
	wl_signal_add(&z->surface->events.destroy, &z->surface_destroy);
}

static const struct picowl_buffer_v1_interface buffer_impl = {
	.destroy = buffer_destroy_req,
	.attach_surface = buffer_attach_surface,
};

static void buffer_resource_destroy(struct wl_resource *r)
{
	struct pw_zbuf *z = wl_resource_get_user_data(r);
	if (!z)
		return;
	zbuf_detach(z);
	wl_list_remove(&z->link);
	if (st.total_bytes >= z->bytes)
		st.total_bytes -= z->bytes;
	if (z->buffer)
		wlr_buffer_drop(z->buffer);
	free(z);
}

static void mgr_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void mgr_create_buffer(struct wl_client *client, struct wl_resource *mgr,
	uint32_t id, int32_t w, int32_t h, uint32_t format)
{
	struct wl_resource *res = wl_resource_create(client,
		&picowl_buffer_v1_interface, wl_resource_get_version(mgr), id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &buffer_impl, NULL, NULL);

	if (format != DRM_FORMAT_RGB565) {
		picowl_buffer_v1_send_failed(res,
			PICOWL_BUFFER_V1_REASON_UNSUPPORTED_FORMAT);
		return;
	}
	int mw, mh;
	largest_output(&mw, &mh);
	if (w <= 0 || h <= 0 || w > mw || h > mh) {
		wlr_log(WLR_DEBUG, "zero-copy: %dx%d too large (max %dx%d)",
			w, h, mw, mh);
		picowl_buffer_v1_send_failed(res, PICOWL_BUFFER_V1_REASON_TOO_LARGE);
		return;
	}
	uint32_t bytes = (uint32_t)w * (uint32_t)h * 2;
	int n = 0;
	struct pw_zbuf *it;
	wl_list_for_each(it, &st.zbufs, link)
		if (wl_resource_get_client(it->resource) == client)
			n++;
	if (n >= PW_ZB_MAX_PER_CLIENT || st.total_bytes + bytes > PW_ZB_BUDGET) {
		wlr_log(WLR_DEBUG, "zero-copy: buffer budget exceeded");
		picowl_buffer_v1_send_failed(res, PICOWL_BUFFER_V1_REASON_NO_MEMORY);
		return;
	}
	const struct wlr_drm_format *fmt =
		wlr_drm_format_set_get(&st.fmtset, DRM_FORMAT_RGB565);
	struct wlr_buffer *buf = fmt ?
		wlr_allocator_create_buffer(st.server->allocator, w, h, fmt) : NULL;
	struct wlr_dmabuf_attributes attr;
	if (!buf || !wlr_buffer_get_dmabuf(buf, &attr)) {
		wlr_log(WLR_INFO, "zero-copy: allocation of %dx%d failed", w, h);
		if (buf)
			wlr_buffer_drop(buf);
		picowl_buffer_v1_send_failed(res, PICOWL_BUFFER_V1_REASON_NO_MEMORY);
		return;
	}
	struct pw_zbuf *z = calloc(1, sizeof(*z));
	if (!z) {
		wlr_buffer_drop(buf);
		picowl_buffer_v1_send_failed(res, PICOWL_BUFFER_V1_REASON_NO_MEMORY);
		return;
	}
	z->resource = res;
	z->buffer = buf;
	z->bytes = bytes;
	pw_copyrel_init(&z->rel);
	wl_list_insert(&st.zbufs, &z->link);
	st.total_bytes += bytes;
	wl_resource_set_implementation(res, &buffer_impl, z, buffer_resource_destroy);

	picowl_buffer_v1_send_dmabuf(res, attr.fd[0], attr.stride[0],
		attr.offset[0], (uint32_t)(attr.modifier >> 32),
		(uint32_t)(attr.modifier & 0xffffffff));
	picowl_buffer_v1_send_done(res);
}

static const struct picowl_buffer_manager_v1_interface mgr_impl = {
	.destroy = mgr_destroy_req,
	.create_buffer = mgr_create_buffer,
};

static void mgr_resource_destroy(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}

static void mgr_bind(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(client,
		&picowl_buffer_manager_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &mgr_impl, NULL, mgr_resource_destroy);
	wl_list_insert(&st.mgrs, wl_resource_get_link(r));
	picowl_buffer_manager_v1_send_format(r, DRM_FORMAT_RGB565);
	send_copy_type(r);
}

/* ---- idle copied ---------------------------------------------------- */

static bool any_output_needs_frame(void)
{
	struct pw_output *o;
	wl_list_for_each(o, &st.server->outputs, link) {
		if (!o->wlr_output->enabled || !o->scene_output)
			continue;
		if (wlr_scene_output_needs_frame(o->scene_output))
			return true;
	}
	return false;
}

static bool any_output_enabled(void)
{
	struct pw_output *o;
	wl_list_for_each(o, &st.server->outputs, link)
		if (o->wlr_output->enabled)
			return true;
	return false;
}

/* Nothing is waiting to be drawn: commits which no output sampled (no
 * damage, occluded, off-output, outputs blanked) are answered "retained",
 * the compositor still holds the buffer as the surface's current content. */
static int idle_cb(void *data)
{
	(void)data;
	if (any_output_needs_frame())
		return 0;
	bool force = !any_output_enabled();
	struct pw_zbuf *z;
	wl_list_for_each(z, &st.zbufs, link) {
		uint32_t serial;
		if (!z->surface)
			continue;
		if (pw_copyrel_on_retain(&z->rel, force, &serial))
			picowl_buffer_v1_send_retained(z->resource, serial);
	}
	return 0;
}

/* ---- public API ------------------------------------------------------ */

bool pw_zerocopy_init(struct pw_server *s)
{
	const char *why = NULL;
	int drm_fd = -1;
	if (!s->config || !s->config->zerocopy)
		why = "disabled in config";
	else if ((drm_fd = wlr_backend_get_drm_fd(s->backend)) < 0)
		why = "no DRM backend";
	else if (!s->allocator || !(s->allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF))
		why = "allocator has no dmabuf support";
	if (why) {
		wlr_log(WLR_INFO, "zero-copy disabled: %s", why);
		return true;
	}
	struct stat sb;
	if (fstat(drm_fd, &sb) < 0) {
		wlr_log(WLR_INFO, "zero-copy disabled: fstat of DRM fd failed");
		return true;
	}

	memset(&st, 0, sizeof(st));
	st.server = s;
	wl_list_init(&st.zbufs);
	wl_list_init(&st.mgrs);

	if (!wlr_drm_format_set_add(&st.fmtset, DRM_FORMAT_RGB565,
			DRM_FORMAT_MOD_LINEAR)) {
		wlr_log(WLR_INFO, "zero-copy disabled: format set allocation failed");
		return true;
	}

	struct wlr_linux_dmabuf_feedback_v1 fb = {0};
	fb.main_device = sb.st_rdev;
	struct wlr_linux_dmabuf_feedback_v1_tranche *t =
		wlr_linux_dmabuf_feedback_add_tranche(&fb);
	if (!t) {
		wlr_drm_format_set_finish(&st.fmtset);
		wlr_log(WLR_INFO, "zero-copy disabled: feedback allocation failed");
		return true;
	}
	t->target_device = fb.main_device;
	t->flags = PW_TRANCHE_FLAGS_SCANOUT;
	wlr_drm_format_set_add(&t->formats, DRM_FORMAT_RGB565, DRM_FORMAT_MOD_LINEAR);
	s->linux_dmabuf = wlr_linux_dmabuf_v1_create(s->display, 4, &fb);
	wlr_linux_dmabuf_feedback_v1_finish(&fb);
	if (s->linux_dmabuf)
		wlr_linux_dmabuf_v1_set_check_dmabuf_callback(s->linux_dmabuf,
			check_dmabuf, NULL);
	else
		wlr_log(WLR_INFO, "zero-copy: linux-dmabuf global unavailable");
	/* wlr_scene_set_linux_dmabuf_v1 is deliberately never used: the pixman
	 * renderer has no DRM fd and no dmabuf texture formats. */

	st.global = wl_global_create(s->display, &picowl_buffer_manager_v1_interface,
		1, NULL, mgr_bind);
	st.idle_timer = wl_event_loop_add_timer(s->event_loop, idle_cb, NULL);
	if (!st.global || !st.idle_timer) {
		wlr_log(WLR_INFO, "zero-copy disabled: global or timer creation failed");
		if (st.global)
			wl_global_destroy(st.global);
		if (st.idle_timer)
			wl_event_source_remove(st.idle_timer);
		st.global = NULL;
		st.idle_timer = NULL;
		wlr_drm_format_set_finish(&st.fmtset);
		return true;
	}
	s->buffer_mgr = &st;
	st.active = true;
	st.copy_type = any_copy_type();
	wlr_log(WLR_INFO, "zero-copy enabled (copy_type=%d)", st.copy_type);
	return true;
}

void pw_zerocopy_finish(struct pw_server *s)
{
	if (!st.active)
		return;
	st.active = false;
	if (st.global)
		wl_global_destroy(st.global);
	st.global = NULL;
	struct pw_zbuf *z, *tmp;
	wl_list_for_each_safe(z, tmp, &st.zbufs, link) {
		/* destroying the resource frees z via buffer_resource_destroy */
		wl_resource_destroy(z->resource);
	}
	struct wl_resource *r, *rt;
	wl_resource_for_each_safe(r, rt, &st.mgrs)
		wl_resource_destroy(r);
	if (st.idle_timer)
		wl_event_source_remove(st.idle_timer);
	st.idle_timer = NULL;
	wlr_drm_format_set_finish(&st.fmtset);
	s->buffer_mgr = NULL;
}

void pw_zerocopy_output_added(struct pw_output *o)
{
	(void)o;
	if (st.active)
		refresh_copy_type();
}

void pw_zerocopy_output_removed(struct pw_output *o)
{
	(void)o;
	if (!st.active)
		return;
	/* the output may still be linked and enabled; exclude it */
	bool v = false;
	struct pw_output *it;
	wl_list_for_each(it, &st.server->outputs, link)
		if (it != o && it->copy_type && it->wlr_output->enabled)
			v = true;
	if (v != st.copy_type) {
		st.copy_type = v;
		struct wl_resource *r;
		wl_resource_for_each(r, &st.mgrs)
			send_copy_type(r);
	}
}

void pw_zerocopy_output_committed(struct pw_output *o, bool did_commit)
{
	(void)o; (void)did_commit;
	/* after a committed frame too: surfaces which that frame did not sample
	 * (occluded) would otherwise never be answered */
	if (st.active)
		idle_cb(NULL);
}

void pw_zerocopy_output_presented(struct pw_output *o)
{
	if (!st.active || !o->copy_type)
		return;
	struct pw_zbuf *z;
	wl_list_for_each(z, &st.zbufs, link) {
		uint32_t serial;
		if (!z->surface)
			continue;
		if (pw_copyrel_on_present(&z->rel, &serial))
			picowl_buffer_v1_send_copied(z->resource, serial);
	}
}

bool pw_zerocopy_active(const struct pw_server *s)
{
	(void)s;
	return st.active;
}
