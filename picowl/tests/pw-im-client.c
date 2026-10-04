/*
 * pw-im-client - end-to-end check of the text-input-v3 / input-method-v2 relay.
 *
 * One process, two wl_display connections to the same compositor: "app" is an
 * application with an xdg toplevel and a text input that enables itself when it
 * is entered, "im" is the input method. It asserts that
 *   - a second input method object gets unavailable,
 *   - the input method gets activate (with the surrounding text) after enable,
 *   - a commit_string and a preedit_string from the input method arrive at the
 *     application followed by done,
 *   - a second text input of the same client taking over: deactivate, then
 *     activate, never two activates in a row,
 *   - destroying that second one activating the first again,
 *   - the input method gets deactivate after disable and activate after enable
 *     again,
 *   - the application surface going away (keyboard focus lost) giving
 *     deactivate and a leave.
 * Prints "pw-im-client: ok" and exits 0, else prints why and exits 1. No wlroots.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"

#define FAIL(...) do { \
	fprintf(stderr, "pw-im-client: FAIL: " __VA_ARGS__); \
	fputc('\n', stderr); \
	exit(1); \
} while (0)

/* ---- application side ------------------------------------------------- */

static struct wl_display *app_dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *app_seat;
static struct zwp_text_input_manager_v3 *ti_mgr;
static struct zwp_text_input_v3 *ti, *ti2;
static struct wl_surface *surface;

static bool configured, entered, left;
static uint32_t ti_serial;
static char got_commit[64], got_preedit[64];
static bool got_done;
static int got_done_serial = -1;

static void app_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name))
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name))
		app_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name))
		ti_mgr = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener app_reg = { app_global, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static int32_t cfg_w, cfg_h;

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
	struct wl_array *states)
{
	(void)d; (void)t; (void)states;
	cfg_w = w;
	cfg_h = h;
}
static void top_close(void *d, struct xdg_toplevel *t)
{
	(void)d; (void)t;
	FAIL("the compositor closed the toplevel");
}
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, NULL, NULL };

static struct wl_buffer *make_buffer(int w, int h)
{
	size_t size = (size_t)w * h * 4;
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char path[512];

	snprintf(path, sizeof(path), "%s/pw-im-XXXXXX", dir ? dir : "/tmp");
	int fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0)
		FAIL("cannot create the shm file");
	unlink(path);
	void *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED)
		FAIL("mmap failed");
	memset(px, 0x80, size);
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	configured = true;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

/* The text input enables itself the moment it is entered, like a toolkit does
 * when a field has the focus. */
static void ti_enter(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s)
{
	(void)d; (void)s;
	entered = true;
	zwp_text_input_v3_enable(t);
	zwp_text_input_v3_set_surrounding_text(t, "abc", 3, 3);
	zwp_text_input_v3_set_text_change_cause(t,
		ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
	zwp_text_input_v3_set_content_type(t, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
		ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
	zwp_text_input_v3_set_cursor_rectangle(t, 10, 10, 2, 16);
	zwp_text_input_v3_commit(t);
	ti_serial++;
}
static void ti_leave(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s)
{
	(void)d; (void)t; (void)s;
	left = true;
}
static void ti_preedit(void *d, struct zwp_text_input_v3 *t, const char *text,
	int32_t b, int32_t e)
{
	(void)d; (void)t; (void)b; (void)e;
	snprintf(got_preedit, sizeof(got_preedit), "%s", text ? text : "");
}
static void ti_commit_string(void *d, struct zwp_text_input_v3 *t, const char *text)
{
	(void)d; (void)t;
	snprintf(got_commit, sizeof(got_commit), "%s", text ? text : "");
}
static void ti_delete(void *d, struct zwp_text_input_v3 *t, uint32_t b, uint32_t a)
{
	(void)d; (void)t; (void)b; (void)a;
}
static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t serial)
{
	(void)d; (void)t;
	got_done = true;
	got_done_serial = (int)serial;
}
static const struct zwp_text_input_v3_listener ti_listener = {
	ti_enter, ti_leave, ti_preedit, ti_commit_string, ti_delete, ti_done
};

/* ---- input method side ------------------------------------------------ */

static struct wl_display *im_dpy;
static struct wl_seat *im_seat;
static struct zwp_input_method_manager_v2 *im_mgr;
static struct zwp_input_method_v2 *im, *im2;

static bool im_active_pending, im_activated, im2_unavailable, im_unavailable,
	im_double_activate;
static uint32_t im_activate_count, im_deactivate_count;
static uint32_t im_done_count;
static char im_surrounding[64];

static void im_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_seat_interface.name))
		im_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name))
		im_mgr = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
}
static const struct wl_registry_listener im_reg = { im_global, reg_remove };

static void im_on_activate(void *d, struct zwp_input_method_v2 *m)
{
	(void)d; (void)m;
	if (im_active_pending)
		im_double_activate = true;
	im_active_pending = true;
	im_activate_count++;
}
static void im_on_deactivate(void *d, struct zwp_input_method_v2 *m)
{
	(void)d; (void)m;
	im_active_pending = false;
	im_deactivate_count++;
}
static void im_on_surrounding(void *d, struct zwp_input_method_v2 *m, const char *text,
	uint32_t c, uint32_t a)
{
	(void)d; (void)m; (void)c; (void)a;
	snprintf(im_surrounding, sizeof(im_surrounding), "%s", text ? text : "");
}
static void im_on_cause(void *d, struct zwp_input_method_v2 *m, uint32_t cause)
{
	(void)d; (void)m; (void)cause;
}
static void im_on_content_type(void *d, struct zwp_input_method_v2 *m, uint32_t hint,
	uint32_t purpose)
{
	(void)d; (void)m; (void)hint; (void)purpose;
}
static void im_on_done(void *d, struct zwp_input_method_v2 *m)
{
	(void)d; (void)m;
	im_done_count++;
	/* The state of the done is what counts: activate then done is active. */
	if (im_active_pending)
		im_activated = true;
}
static void im_on_unavailable(void *d, struct zwp_input_method_v2 *m)
{
	(void)d;
	if (m == im2)
		im2_unavailable = true;
	else
		im_unavailable = true;
}
static const struct zwp_input_method_v2_listener im_listener = {
	im_on_activate, im_on_deactivate, im_on_surrounding, im_on_cause,
	im_on_content_type, im_on_done, im_on_unavailable
};

/* ---- driver ----------------------------------------------------------- */

/* Both displays live in this thread, and each side's request reaches the
 * other through the compositor, so a few round trips on each in turn settle
 * any chain of events. */
static void settle(bool (*cond)(void), const char *what)
{
	for (int i = 0; i < 200 && !cond(); i++) {
		if (wl_display_roundtrip(app_dpy) < 0 || wl_display_roundtrip(im_dpy) < 0)
			FAIL("display error while waiting for %s", what);
		nanosleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
	}
	if (!cond())
		FAIL("timeout waiting for %s", what);
}

static bool c_im2_unavailable(void) { return im2_unavailable; }
static bool c_entered(void) { return entered; }
static bool c_activated(void) { return im_activated; }
static bool c_text_back(void) { return got_done && got_commit[0]; }

static uint32_t want_act, want_deact;
static bool c_counts(void)
{
	return im_activate_count >= want_act && im_deactivate_count >= want_deact;
}

/* Wait for the counters to reach act and deact, then give stray events a few
 * more round trips to show up: they must be exactly these. */
static void expect_counts(uint32_t act, uint32_t deact, const char *what)
{
	want_act = act;
	want_deact = deact;
	settle(c_counts, what);
	for (int i = 0; i < 3; i++) {
		wl_display_roundtrip(app_dpy);
		wl_display_roundtrip(im_dpy);
	}
	if (im_activate_count != act || im_deactivate_count != deact)
		FAIL("%s: %u activates and %u deactivates, expected %u and %u", what,
			im_activate_count, im_deactivate_count, act, deact);
	if (im_double_activate)
		FAIL("%s: activate while already active", what);
}

int main(void)
{
	alarm(15);

	im_dpy = wl_display_connect(NULL);
	app_dpy = wl_display_connect(NULL);
	if (!im_dpy || !app_dpy)
		FAIL("cannot connect");

	struct wl_registry *r = wl_display_get_registry(im_dpy);
	wl_registry_add_listener(r, &im_reg, NULL);
	wl_display_roundtrip(im_dpy);
	r = wl_display_get_registry(app_dpy);
	wl_registry_add_listener(r, &app_reg, NULL);
	wl_display_roundtrip(app_dpy);
	if (!im_mgr || !im_seat)
		FAIL("no input-method manager or seat");
	if (!compositor || !shm || !wm_base || !app_seat || !ti_mgr)
		FAIL("application globals missing (text-input manager?)");

	/* The input method comes first: one is accepted, a second one is told it
	 * is unavailable. */
	im = zwp_input_method_manager_v2_get_input_method(im_mgr, im_seat);
	zwp_input_method_v2_add_listener(im, &im_listener, NULL);
	im2 = zwp_input_method_manager_v2_get_input_method(im_mgr, im_seat);
	zwp_input_method_v2_add_listener(im2, &im_listener, NULL);
	settle(c_im2_unavailable, "unavailable for the second input method");
	if (im_unavailable)
		FAIL("the first input method was refused");

	/* The application maps a toplevel; the compositor focuses it and enters
	 * the text input. */
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	ti = zwp_text_input_manager_v3_get_text_input(ti_mgr, app_seat);
	zwp_text_input_v3_add_listener(ti, &ti_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &top_listener, NULL);
	xdg_toplevel_set_app_id(top, "pw-im-client");
	wl_surface_commit(surface);
	for (int i = 0; i < 100 && !configured; i++)
		wl_display_roundtrip(app_dpy);
	if (!configured)
		FAIL("no configure");
	int w = cfg_w > 0 ? cfg_w : 64, h = cfg_h > 0 ? cfg_h : 64;
	struct wl_buffer *buf = make_buffer(w, h);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_commit(surface);
	settle(c_entered, "text input enter (keyboard focus relay)");

	/* enable: the input method must be activated with the state. */
	settle(c_activated, "input method activate");
	if (strcmp(im_surrounding, "abc"))
		FAIL("surrounding text is '%s', expected 'abc'", im_surrounding);

	/* The input method answers; the serial is the number of dones seen. */
	zwp_input_method_v2_commit_string(im, "hello");
	zwp_input_method_v2_set_preedit_string(im, "pre", 0, 3);
	zwp_input_method_v2_commit(im, im_done_count);
	settle(c_text_back, "commit_string at the application");
	if (strcmp(got_commit, "hello"))
		FAIL("commit_string is '%s', expected 'hello'", got_commit);
	if (strcmp(got_preedit, "pre"))
		FAIL("preedit_string is '%s', expected 'pre'", got_preedit);
	if (got_done_serial != (int)ti_serial)
		FAIL("done serial %d, expected %u", got_done_serial, ti_serial);

	expect_counts(1, 0, "the first activate only");

	/* A second text input of the same client is entered and enables itself:
	 * the first is deactivated, then the second activated. */
	ti2 = zwp_text_input_manager_v3_get_text_input(ti_mgr, app_seat);
	zwp_text_input_v3_add_listener(ti2, &ti_listener, NULL);
	expect_counts(2, 1, "takeover by a second text input");
	if (!im_active_pending)
		FAIL("not active after the takeover");

	/* The active one goes away: the first, still enabled and focused, must
	 * get its turn without any focus change. */
	zwp_text_input_v3_destroy(ti2);
	expect_counts(3, 2, "reactivate after the active text input is destroyed");
	if (!im_active_pending)
		FAIL("not active after the second text input was destroyed");

	/* disable: the input method must be deactivated. */
	zwp_text_input_v3_disable(ti);
	zwp_text_input_v3_commit(ti);
	expect_counts(3, 3, "deactivate after disable");

	/* enable again: the input method must be activated again. */
	zwp_text_input_v3_enable(ti);
	zwp_text_input_v3_commit(ti);
	expect_counts(4, 3, "activate after enable again");

	if (left)
		FAIL("the text input was left while the toplevel still has the focus");

	/* The application surface goes away and the keyboard focus with it. */
	xdg_toplevel_destroy(top);
	xdg_surface_destroy(xs);
	wl_surface_destroy(surface);
	expect_counts(4, 4, "deactivate when the keyboard focus is lost");
	if (!left)
		FAIL("no leave after the application surface went away");

	printf("pw-im-client: ok\n");
	return 0;
}
