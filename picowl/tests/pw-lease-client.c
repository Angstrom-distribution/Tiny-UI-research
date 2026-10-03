/*
 * pw-lease-client - the lessee side of the DRM lease test (lease-vkms.sh).
 * Maps a fullscreen toplevel, leases the one connector, checks what the lease
 * contains, modesets it with a dumb buffer, then ends the lease.
 *
 *   pw-lease-client [--app-id ID] [--expect-overlay] MODE
 *   MODE: cycle         close the fd, destroy the lease, expect the connector
 *                       to be offered again (the player's exit path)
 *         close-fd-only close the fd only, expect the re-offer through the
 *                       kernel's LEASE uevent (needs a udev monitor in picowl)
 *         hang          print "leased" and wait to be killed
 *         reject        expect `finished` without an fd (policy test)
 *
 * Exit status: 0 ok, 1 failure, 3 rejected (reject mode: ok; other modes: failure).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "drm-lease-v1-client-protocol.h"

#define MAX_CONN 8

struct conn {
	struct wp_drm_lease_connector_v1 *obj;
	char name[32];
	uint32_t id;
	bool done;
};

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_drm_lease_device_v1 *device;
static bool device_done, activated, configured;
static struct conn conns[MAX_CONN];
static int n_conns;
static unsigned connector_events;      /* every wp_drm_lease_device_v1.connector */
static int lease_fd = -1;
static bool finished;

#define FAIL(...) do { fprintf(stderr, "pw-lease-client: FAIL: " __VA_ARGS__); \
	fputc('\n', stderr); exit(1); } while (0)

/* ---- wayland listeners ---------------------------------------------- */

static void conn_name(void *d, struct wp_drm_lease_connector_v1 *o, const char *name)
{
	(void)o;
	snprintf(((struct conn *)d)->name, sizeof(((struct conn *)d)->name), "%s", name);
}
static void conn_desc(void *d, struct wp_drm_lease_connector_v1 *o, const char *s)
{
	(void)d; (void)o; (void)s;
}
static void conn_id(void *d, struct wp_drm_lease_connector_v1 *o, uint32_t id)
{
	(void)o;
	((struct conn *)d)->id = id;
}
static void conn_done(void *d, struct wp_drm_lease_connector_v1 *o)
{
	(void)o;
	((struct conn *)d)->done = true;
}
/* wlroots withdraws the leased connector while granting: the event can come
 * before lease_fd. */
static void conn_withdrawn(void *d, struct wp_drm_lease_connector_v1 *o)
{
	struct conn *c = d;
	wp_drm_lease_connector_v1_destroy(o);
	c->obj = NULL;
}
static const struct wp_drm_lease_connector_v1_listener conn_listener = {
	conn_name, conn_desc, conn_id, conn_done, conn_withdrawn
};

static void dev_drm_fd(void *d, struct wp_drm_lease_device_v1 *dev, int32_t fd)
{
	(void)d; (void)dev;
	close(fd); /* the non-master fd is not needed to build the request */
}
static void dev_connector(void *d, struct wp_drm_lease_device_v1 *dev,
	struct wp_drm_lease_connector_v1 *obj)
{
	(void)d; (void)dev;
	connector_events++;
	if (n_conns == MAX_CONN)
		FAIL("too many connectors");
	struct conn *c = &conns[n_conns++];
	memset(c, 0, sizeof(*c));
	c->obj = obj;
	wp_drm_lease_connector_v1_add_listener(obj, &conn_listener, c);
}
static void dev_done(void *d, struct wp_drm_lease_device_v1 *dev)
{
	(void)d; (void)dev;
	device_done = true;
}
static void dev_released(void *d, struct wp_drm_lease_device_v1 *dev)
{
	(void)d; (void)dev;
}
static const struct wp_drm_lease_device_v1_listener dev_listener = {
	dev_drm_fd, dev_connector, dev_done, dev_released
};

static void lease_got_fd(void *d, struct wp_drm_lease_v1 *l, int32_t fd)
{
	(void)d; (void)l;
	lease_fd = fd;
}
static void lease_finished(void *d, struct wp_drm_lease_v1 *l)
{
	(void)d; (void)l;
	finished = true;
}
static const struct wp_drm_lease_v1_listener lease_listener = {
	lease_got_fd, lease_finished
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name))
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
	else if (!strcmp(iface, wp_drm_lease_device_v1_interface.name)) {
		device = wl_registry_bind(r, name, &wp_drm_lease_device_v1_interface, 1);
		wp_drm_lease_device_v1_add_listener(device, &dev_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	configured = true;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
	struct wl_array *states)
{
	(void)d; (void)t; (void)w; (void)h;
	uint32_t *s;
	wl_array_for_each(s, states)
		if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
			activated = true;
}
static void tl_close(void *d, struct xdg_toplevel *t)
{
	(void)d; (void)t;
	exit(1);
}
static const struct xdg_toplevel_listener tl_listener = { tl_configure, tl_close, NULL, NULL };

/* Dispatch events for up to ms; returns false on a timeout. */
static bool dispatch_for(struct wl_display *dpy, int ms)
{
	while (wl_display_prepare_read(dpy) != 0)
		wl_display_dispatch_pending(dpy);
	wl_display_flush(dpy);
	struct pollfd pfd = { .fd = wl_display_get_fd(dpy), .events = POLLIN };
	int r = poll(&pfd, 1, ms);
	if (r > 0) {
		if (wl_display_read_events(dpy) < 0)
			FAIL("display connection lost");
	} else {
		wl_display_cancel_read(dpy);
	}
	wl_display_dispatch_pending(dpy);
	return r > 0;
}

/* ---- the lessee's side of KMS -------------------------------------------- */

static bool prop_get(int fd, uint32_t obj, uint32_t type, const char *name,
	uint32_t *id, uint64_t *val)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	bool found = false;
	for (uint32_t i = 0; props && i < props->count_props && !found; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (p && !strcmp(p->name, name)) {
			*id = p->prop_id;
			if (val)
				*val = props->prop_values[i];
			found = true;
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

static void atomic_add(int fd, drmModeAtomicReq *req, uint32_t obj, uint32_t type,
	const char *name, uint64_t value)
{
	uint32_t id;
	if (!prop_get(fd, obj, type, name, &id, NULL))
		FAIL("object %u has no property %s", obj, name);
	if (drmModeAtomicAddProperty(req, obj, id, value) < 0)
		FAIL("cannot add %s", name);
}

/* Check the lease and light the connector with a dumb buffer. */
static void check_and_modeset(int fd, uint32_t want_connector, bool want_overlay)
{
	/* A new drm_file: client caps are not inherited from picowl. */
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
			drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
		FAIL("client caps: %s", strerror(errno));

	drmModeObjectListPtr lease = drmModeGetLease(fd);
	if (!lease)
		FAIL("drmModeGetLease: %s", strerror(errno));
	drmModeRes *res = drmModeGetResources(fd);
	drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
	if (!res || !pres || res->count_connectors != 1 || res->count_crtcs < 1)
		FAIL("lease resources: connectors %d crtcs %d", res ? res->count_connectors : -1,
			res ? res->count_crtcs : -1);
	if (res->connectors[0] != want_connector)
		FAIL("leased connector %u, offered %u", res->connectors[0], want_connector);

	drmModeConnector *c = drmModeGetConnector(fd, want_connector);
	if (!c || c->count_modes < 1)
		FAIL("connector %u has no modes", want_connector);
	uint32_t crtc = res->crtcs[0];
	int crtc_index = 0;

	uint32_t primary = 0;
	unsigned n_overlay = 0, n_cursor = 0;
	for (uint32_t i = 0; i < pres->count_planes; i++) {
		uint64_t type = 0;
		uint32_t pid;
		if (!prop_get(fd, pres->planes[i], DRM_MODE_OBJECT_PLANE, "type", &pid, &type))
			FAIL("plane %u has no type", pres->planes[i]);
		drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
		if (type == DRM_PLANE_TYPE_PRIMARY && p && (p->possible_crtcs & (1u << crtc_index)))
			primary = p->plane_id;
		else if (type == DRM_PLANE_TYPE_OVERLAY)
			n_overlay++;
		else if (type == DRM_PLANE_TYPE_CURSOR)
			n_cursor++;
		drmModeFreePlane(p);
	}
	if (!primary)
		FAIL("no primary plane for the CRTC");
	/* The lease holds the connector, the CRTCs and the planes, nothing else. */
	uint32_t expect = 1 + res->count_crtcs + pres->count_planes;
	if (lease->count != expect)
		FAIL("lease holds %u objects, resources list %u", lease->count, expect);
	printf("pw-lease-client: lease: connector %u crtc %u primary %u overlay %u cursor %u\n",
		want_connector, crtc, primary, n_overlay, n_cursor);
	if (want_overlay && n_overlay == 0)
		FAIL("no overlay plane in the lease (wlroots patch 0004)");

	/* Modeset: the CRTC arrives disabled, picowl committed it off. */
	drmModeModeInfo mode = c->modes[0];
	struct drm_mode_create_dumb cd = {
		.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32,
	};
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		FAIL("create dumb buffer: %s", strerror(errno));
	uint32_t handles[4] = { cd.handle }, pitches[4] = { cd.pitch }, offsets[4] = { 0 };
	uint32_t fb;
	if (drmModeAddFB2(fd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
			handles, pitches, offsets, &fb, 0) < 0)
		FAIL("add fb: %s", strerror(errno));
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		FAIL("map dumb buffer: %s", strerror(errno));
	void *map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (map == MAP_FAILED)
		FAIL("mmap dumb buffer: %s", strerror(errno));
	for (uint32_t y = 0; y < cd.height; y++) {
		uint32_t *row = (uint32_t *)((char *)map + (size_t)y * cd.pitch);
		for (uint32_t x = 0; x < cd.width; x++)
			row[x] = 0xff000000u | (x & 0xff) << 8 | (y & 0xff);
	}
	munmap(map, cd.size);

	uint32_t blob;
	if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob) < 0)
		FAIL("mode blob: %s", strerror(errno));
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	atomic_add(fd, req, want_connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc);
	atomic_add(fd, req, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob);
	atomic_add(fd, req, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "FB_ID", fb);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)mode.hdisplay << 16);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)mode.vdisplay << 16);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "CRTC_W", mode.hdisplay);
	atomic_add(fd, req, primary, DRM_MODE_OBJECT_PLANE, "CRTC_H", mode.vdisplay);
	if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) < 0)
		FAIL("modeset commit: %s", strerror(errno));
	drmModeAtomicFree(req);
	printf("pw-lease-client: modeset %ux%u ok\n", mode.hdisplay, mode.vdisplay);

	drmModeFreeConnector(c);
	drmModeFreePlaneResources(pres);
	drmModeFreeResources(res);
	drmFree(lease);
}

/* ---- main ------------------------------------------------------------- */

static void map_toplevel(struct wl_display *dpy, const char *app_id)
{
	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, NULL);
	xdg_toplevel_set_title(tl, "lease");
	xdg_toplevel_set_app_id(tl, app_id);
	xdg_toplevel_set_fullscreen(tl, NULL);
	wl_surface_commit(surface);
	while (!configured)
		if (wl_display_dispatch(dpy) < 0)
			FAIL("no configure");

	/* one pixel, like the player's single-pixel buffer */
	int fd = memfd_create("pw-lease", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, 4) < 0)
		FAIL("memfd: %s", strerror(errno));
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 4);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_commit(surface);
	while (!activated)
		if (wl_display_dispatch(dpy) < 0)
			FAIL("never activated");
}

int main(int argc, char **argv)
{
	const char *app_id = "mediaplayer", *mode = NULL;
	bool want_overlay = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--app-id") && i + 1 < argc)
			app_id = argv[++i];
		else if (!strcmp(argv[i], "--expect-overlay"))
			want_overlay = true;
		else
			mode = argv[i];
	}
	if (!mode)
		FAIL("usage: pw-lease-client [--app-id ID] [--expect-overlay] cycle|close-fd-only|hang|reject");

	alarm(20);
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy)
		FAIL("cannot connect");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm || !wm_base)
		FAIL("missing globals");
	if (!device)
		FAIL("no wp_drm_lease_device_v1");
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	while (!device_done)
		if (wl_display_dispatch(dpy) < 0)
			FAIL("no device done");
	wl_display_roundtrip(dpy);
	if (n_conns != 1 || !conns[0].obj || !conns[0].done)
		FAIL("expected one offered connector, got %d", n_conns);
	printf("pw-lease-client: connector %s id %u\n", conns[0].name, conns[0].id);

	map_toplevel(dpy, app_id);

	struct wp_drm_lease_request_v1 *req = wp_drm_lease_device_v1_create_lease_request(device);
	wp_drm_lease_request_v1_request_connector(req, conns[0].obj);
	struct wp_drm_lease_v1 *lease = wp_drm_lease_request_v1_submit(req);
	wp_drm_lease_v1_add_listener(lease, &lease_listener, NULL);
	while (lease_fd < 0 && !finished)
		if (wl_display_dispatch(dpy) < 0)
			FAIL("connection lost waiting for the lease");
	if (lease_fd < 0) {
		printf("pw-lease-client: rejected\n");
		return !strcmp(mode, "reject") ? 0 : 3;
	}
	if (!strcmp(mode, "reject"))
		FAIL("expected a rejection, got a lease");
	printf("pw-lease-client: got the lease fd\n");

	check_and_modeset(lease_fd, conns[0].id, want_overlay);
	printf("pw-lease-client: leased\n");
	fflush(stdout);
	if (!strcmp(mode, "hang"))
		for (;;)
			pause();

	/* The picowl side: output re-created and offered again, which shows as a
	 * new connector event. */
	unsigned before = connector_events;
	close(lease_fd);
	if (!strcmp(mode, "cycle"))
		wp_drm_lease_v1_destroy(lease);
	wl_display_flush(dpy);
	for (int i = 0; i < 50 && connector_events == before; i++)
		dispatch_for(dpy, 100);
	if (connector_events == before) {
		printf("pw-lease-client: connector not offered again\n");
		return 4;
	}
	printf("pw-lease-client: connector offered again\n");
	return 0;
}
