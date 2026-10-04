/* A just-enough Wayland compositor to check havoc's text-input-v3 use without
 * a real compositor. It serves wl_compositor, wl_shm, xdg_wm_base, wl_seat
 * with a keyboard and, unless --no-text-input is given, text-input-v3. It
 * runs havoc as a child, moves the keyboard focus to and from its window and
 * types into it, and checks what havoc sent:
 *
 *  - nothing is enabled before the focus arrives
 *  - the focus enter is answered by exactly one enable of purpose terminal
 *    with a cursor rectangle, followed by a commit
 *  - typing does not enable (or commit) again
 *  - the focus leave is answered by a disable and a commit
 *  - the next enter enables again, once
 *  - the typed text reaches the shell in the terminal in every case
 *
 * With --no-text-input the same typing must still work. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-server.h"
#include "text-input-unstable-v3-server.h"

#define PURPOSE_TERMINAL 13

static struct {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_event_source *timer;
	bool with_ti;
	pid_t child;
	const char *outfile;

	struct wl_resource *surface, *xdg_surface, *toplevel;
	struct wl_resource *keyboard, *ti;
	bool mapped;
	uint32_t serial;
	int stage, waited;

	/* text-input state as havoc sent it */
	bool p_enable, p_disable, enabled;
	uint32_t hint, purpose;
	int32_t rx, ry, rw, rh;
	int enables, disables, commits;
	int enables_at_typing;
	int commits_at_typing;

	int failed;
} C;

static void fail(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	C.failed = 1;
}

#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

/* wl_surface */
static void s_nop(struct wl_client *c, struct wl_resource *r) {}
static void s_attach(struct wl_client *c, struct wl_resource *r,
		     struct wl_resource *buf, int32_t x, int32_t y)
{
	wl_resource_set_user_data(r, buf);
}
static void s_damage(struct wl_client *c, struct wl_resource *r,
		     int32_t x, int32_t y, int32_t w, int32_t h) {}
static struct wl_resource *pending_cb;
static void s_frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	pending_cb = wl_resource_create(c, &wl_callback_interface, 1, id);
}
static void s_region(struct wl_client *c, struct wl_resource *r,
		     struct wl_resource *region) {}
static void s_commit(struct wl_client *c, struct wl_resource *r)
{
	struct wl_resource *buf = wl_resource_get_user_data(r);

	if (buf) {
		wl_buffer_send_release(buf);
		wl_resource_set_user_data(r, NULL);
		C.mapped = true;
	}
	if (pending_cb) {
		wl_callback_send_done(pending_cb, 0);
		wl_resource_destroy(pending_cb);
		pending_cb = NULL;
	}
}
static void s_int(struct wl_client *c, struct wl_resource *r, int32_t v) {}
static void s_dmg_buf(struct wl_client *c, struct wl_resource *r,
		      int32_t x, int32_t y, int32_t w, int32_t h) {}

static const struct wl_surface_interface surface_impl = {
	.destroy = s_nop, .attach = s_attach, .damage = s_damage,
	.frame = s_frame, .set_opaque_region = s_region,
	.set_input_region = s_region, .commit = s_commit,
	.set_buffer_transform = s_int, .set_buffer_scale = s_int,
	.damage_buffer = s_dmg_buf,
};

static void comp_create_surface(struct wl_client *c, struct wl_resource *r,
				uint32_t id)
{
	struct wl_resource *s = wl_resource_create(c, &wl_surface_interface,
						   wl_resource_get_version(r),
						   id);

	wl_resource_set_implementation(s, &surface_impl, NULL, NULL);
	/* the first surface is havoc's window, the second its cursor surface */
	if (!C.surface)
		C.surface = s;
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r,
			       uint32_t id)
{
}
static const struct wl_compositor_interface comp_impl = {
	.create_surface = comp_create_surface,
	.create_region = comp_create_region,
};
static void comp_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface,
						   v, id);
	wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}

/* xdg-shell */
static void x_nop(struct wl_client *c, struct wl_resource *r) {}
static void x_set_str(struct wl_client *c, struct wl_resource *r,
		      const char *s) {}
static void x_set_res(struct wl_client *c, struct wl_resource *r,
		      struct wl_resource *o) {}
static void x_move(struct wl_client *c, struct wl_resource *r,
		   struct wl_resource *seat, uint32_t serial) {}
static void x_resize(struct wl_client *c, struct wl_resource *r,
		     struct wl_resource *seat, uint32_t serial, uint32_t e) {}
static void x_size(struct wl_client *c, struct wl_resource *r,
		   int32_t w, int32_t h) {}
static void x_menu(struct wl_client *c, struct wl_resource *r,
		   struct wl_resource *seat, uint32_t serial, int32_t x,
		   int32_t y) {}
static void x_fs(struct wl_client *c, struct wl_resource *r,
		 struct wl_resource *o) {}

static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = x_nop, .set_parent = x_set_res, .set_title = x_set_str,
	.set_app_id = x_set_str, .show_window_menu = x_menu, .move = x_move,
	.resize = x_resize, .set_max_size = x_size, .set_min_size = x_size,
	.set_maximized = x_nop, .unset_maximized = x_nop,
	.set_fullscreen = x_fs, .unset_fullscreen = x_nop,
	.set_minimized = x_nop,
};

static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r,
			    uint32_t id)
{
	struct wl_array states;

	C.toplevel = wl_resource_create(c, &xdg_toplevel_interface,
					wl_resource_get_version(r), id);
	wl_resource_set_implementation(C.toplevel, &toplevel_impl, NULL, NULL);
	wl_array_init(&states);
	xdg_toplevel_send_configure(C.toplevel, 0, 0, &states);
	wl_array_release(&states);
	xdg_surface_send_configure(r, ++C.serial);
}
static void xs_get_popup(struct wl_client *c, struct wl_resource *r,
			 uint32_t id, struct wl_resource *parent,
			 struct wl_resource *pos) {}
static void xs_geometry(struct wl_client *c, struct wl_resource *r,
			int32_t x, int32_t y, int32_t w, int32_t h) {}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t s) {}

static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = x_nop, .get_toplevel = xs_get_toplevel,
	.get_popup = xs_get_popup, .set_window_geometry = xs_geometry,
	.ack_configure = xs_ack,
};

static void wm_create_positioner(struct wl_client *c, struct wl_resource *r,
				 uint32_t id) {}
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r,
			       uint32_t id, struct wl_resource *surf)
{
	C.xdg_surface = wl_resource_create(c, &xdg_surface_interface,
					   wl_resource_get_version(r), id);
	wl_resource_set_implementation(C.xdg_surface, &xdg_surface_impl, NULL,
				       NULL);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t s) {}

static const struct xdg_wm_base_interface wm_impl = {
	.destroy = x_nop, .create_positioner = wm_create_positioner,
	.get_xdg_surface = wm_get_xdg_surface, .pong = wm_pong,
};
static void wm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface,
						   v, id);
	wl_resource_set_implementation(r, &wm_impl, NULL, NULL);
}

/* wl_seat with a keyboard that has a real keymap, so havoc's key handling
 * runs for real */
static void send_keymap(struct wl_resource *kbd)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, 0);
	char *s = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t len = strlen(s) + 1;
	int fd = memfd_create("keymap", 0);

	if (!km || fd < 0 || write(fd, s, len) != (ssize_t)len) {
		fprintf(stderr, "cannot build a keymap\n");
		exit(2);
	}
	wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
				len);
	close(fd);
	free(s);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
}

static void kbd_release(struct wl_client *c, struct wl_resource *r)
{
	if (C.keyboard == r)
		C.keyboard = NULL;
	wl_resource_destroy(r);
}
static const struct wl_keyboard_interface kbd_impl = { .release = kbd_release };

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r,
			     uint32_t id) {}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r,
			      uint32_t id)
{
	struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface,
						   wl_resource_get_version(r),
						   id);

	wl_resource_set_implementation(k, &kbd_impl, NULL, NULL);
	send_keymap(k);
	wl_keyboard_send_repeat_info(k, 25, 600);
	C.keyboard = k;
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r,
			   uint32_t id) {}
static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = x_nop,
};
static void seat_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, v, id);

	wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
	wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_KEYBOARD);
	wl_seat_send_name(r, "seat0");
}

/* text-input-v3, with the double buffered state of the protocol */
static void ti_destroy(struct wl_client *c, struct wl_resource *r)
{
	if (C.ti == r)
		C.ti = NULL;
	wl_resource_destroy(r);
}
static void ti_enable(struct wl_client *c, struct wl_resource *r)
{
	C.p_enable = true;
	C.p_disable = false;
}
static void ti_disable(struct wl_client *c, struct wl_resource *r)
{
	C.p_disable = true;
	C.p_enable = false;
}
static void ti_surrounding(struct wl_client *c, struct wl_resource *r,
			   const char *t, int32_t cur, int32_t anchor) {}
static void ti_cause(struct wl_client *c, struct wl_resource *r, uint32_t v) {}
static void ti_content(struct wl_client *c, struct wl_resource *r,
		       uint32_t hint, uint32_t purpose)
{
	C.hint = hint;
	C.purpose = purpose;
}
static void ti_rect(struct wl_client *c, struct wl_resource *r,
		    int32_t x, int32_t y, int32_t w, int32_t h)
{
	C.rx = x;
	C.ry = y;
	C.rw = w;
	C.rh = h;
}
static void ti_commit(struct wl_client *c, struct wl_resource *r)
{
	C.commits++;
	if (C.p_enable) {
		C.enables++;
		C.enabled = true;
	}
	if (C.p_disable) {
		C.disables++;
		C.enabled = false;
	}
	C.p_enable = C.p_disable = false;
}
static const struct zwp_text_input_v3_interface ti_impl = {
	.destroy = ti_destroy, .enable = ti_enable, .disable = ti_disable,
	.set_surrounding_text = ti_surrounding,
	.set_text_change_cause = ti_cause, .set_content_type = ti_content,
	.set_cursor_rectangle = ti_rect, .commit = ti_commit,
};
static void tim_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}
static void tim_get(struct wl_client *c, struct wl_resource *r, uint32_t id,
		    struct wl_resource *seat)
{
	C.ti = wl_resource_create(c, &zwp_text_input_v3_interface,
				  wl_resource_get_version(r), id);
	wl_resource_set_implementation(C.ti, &ti_impl, NULL, NULL);
}
static const struct zwp_text_input_manager_v3_interface tim_impl = {
	.destroy = tim_destroy, .get_text_input = tim_get,
};
static void tim_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c,
		&zwp_text_input_manager_v3_interface, v, id);

	wl_resource_set_implementation(r, &tim_impl, NULL, NULL);
}

static void focus(bool in)
{
	struct wl_array keys;

	wl_array_init(&keys);
	if (in) {
		wl_keyboard_send_enter(C.keyboard, ++C.serial, C.surface,
				       &keys);
		if (C.ti)
			zwp_text_input_v3_send_enter(C.ti, C.surface);
	} else {
		if (C.ti)
			zwp_text_input_v3_send_leave(C.ti, C.surface);
		wl_keyboard_send_leave(C.keyboard, ++C.serial, C.surface);
	}
	wl_array_release(&keys);
}

static void type(uint32_t key)
{
	wl_keyboard_send_key(C.keyboard, ++C.serial, 0, key,
			     WL_KEYBOARD_KEY_STATE_PRESSED);
	wl_keyboard_send_key(C.keyboard, ++C.serial, 0, key,
			     WL_KEYBOARD_KEY_STATE_RELEASED);
}

static void finish(void)
{
	char buf[64] = "";
	FILE *f = fopen(C.outfile, "r");
	int status;

	if (f) {
		if (!fgets(buf, sizeof buf, f))
			buf[0] = 0;
		fclose(f);
	}
	CHECK(!strcmp(buf, "abc"), "the shell read '%s' instead of 'abc'",
	      buf);
	if (waitpid(C.child, &status, WNOHANG) != 0)
		fail("havoc exited early");
	kill(C.child, SIGTERM);
	waitpid(C.child, &status, 0);
	printf("%s: %d enables, %d disables, %d commits\n",
	       C.failed ? "FAILED" : "PASSED", C.enables, C.disables,
	       C.commits);
	exit(C.failed);
}

static int tick(void *data)
{
	wl_event_source_timer_update(C.timer, 150);

	switch (C.stage) {
	case 0:
		if (!C.mapped || !C.keyboard || (C.with_ti && !C.ti)) {
			if (++C.waited > 40) {
				fail("havoc never got ready: mapped %d, keyboard %d, text input %d",
				     C.mapped, !!C.keyboard, !!C.ti);
				finish();
			}
			return 0;
		}
		CHECK(C.commits == 0 && !C.enabled,
		      "text input touched before the focus arrived");
		focus(true);
		break;
	case 1:
		if (C.with_ti) {
			CHECK(C.enables == 1 && C.enabled && C.commits == 1,
			      "after enter: %d enables, %d commits, enabled %d",
			      C.enables, C.commits, C.enabled);
			CHECK(C.purpose == PURPOSE_TERMINAL,
			      "purpose %u is not terminal", C.purpose);
			CHECK(C.rw > 0 && C.rh > 0 && C.rx >= 0 && C.ry >= 0,
			      "bad cursor rectangle %d,%d %dx%d", C.rx, C.ry,
			      C.rw, C.rh);
		}
		C.enables_at_typing = C.enables;
		C.commits_at_typing = C.commits;
		type(30);
		type(48);
		type(46);
		type(28);
		break;
	case 2:
		CHECK(C.enables == C.enables_at_typing &&
		      C.commits == C.commits_at_typing,
		      "typing changed the text input state");
		focus(false);
		break;
	case 3:
		if (C.with_ti)
			CHECK(!C.enabled && C.disables == 1 && C.commits == 2,
			      "after leave: %d disables, %d commits, enabled %d",
			      C.disables, C.commits, C.enabled);
		focus(true);
		break;
	case 4:
		if (C.with_ti)
			CHECK(C.enabled && C.enables == 2 && C.commits == 3,
			      "after re-enter: %d enables, %d commits",
			      C.enables, C.commits);
		finish();
	}
	C.stage++;
	return 0;
}

int main(int argc, char *argv[])
{
	const char *sock;
	int i = 1;

	C.with_ti = true;
	if (i < argc && !strcmp(argv[i], "--no-text-input")) {
		C.with_ti = false;
		i++;
	}
	if (argc - i != 2) {
		fprintf(stderr, "usage: %s [--no-text-input] HAVOC OUTFILE\n",
			argv[0]);
		return 2;
	}
	C.outfile = argv[i + 1];
	unlink(C.outfile);

	C.display = wl_display_create();
	C.loop = wl_display_get_event_loop(C.display);
	sock = wl_display_add_socket_auto(C.display);
	wl_display_init_shm(C.display);
	wl_global_create(C.display, &wl_compositor_interface, 4, NULL,
			 comp_bind);
	wl_global_create(C.display, &xdg_wm_base_interface, 1, NULL, wm_bind);
	wl_global_create(C.display, &wl_seat_interface, 5, NULL, seat_bind);
	if (C.with_ti)
		wl_global_create(C.display,
				 &zwp_text_input_manager_v3_interface, 1, NULL,
				 tim_bind);

	C.child = fork();
	if (C.child == 0) {
		setenv("WAYLAND_DISPLAY", sock, 1);
		setenv("HAVOC_OUT", C.outfile, 1);
		/* -c "" skips the user's configuration */
		execl(argv[i], argv[i], "-c", "", "/bin/sh", "-c",
		      "IFS= read -r l; printf %s \"$l\" > \"$HAVOC_OUT\"; "
		      "sleep 30", (char *)NULL);
		_exit(127);
	}

	C.timer = wl_event_loop_add_timer(C.loop, tick, NULL);
	wl_event_source_timer_update(C.timer, 150);
	signal(SIGALRM, SIG_DFL);
	alarm(30);
	wl_display_run(C.display);
	return 1;
}
