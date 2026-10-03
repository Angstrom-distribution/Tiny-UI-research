/* test-bufproto.c - picowl_buffer_manager_v1 bind events and version gating.
 *
 * No wlroots: a server wl_display in this process registers the manager
 * global and answers the bind with pw_zbproto_send_bind().
 *   1. a client in the same process, over a socketpair, checks the events
 *      and their order for a bind at version 1 and 2;
 *   2. pw-test-client (argv[1]) runs against a socket of that server:
 *      the real client, at version 2 and forced down to version 1.
 * usage: test-bufproto PW_TEST_CLIENT */
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <wayland-server.h>
#include <wayland-client.h>
#include "../src/zbproto.h"
#include "picowl-buffer-v1-protocol.h"
#include "picowl-buffer-v1-client-protocol.h"

/* ---- server: a manager global like zerocopy.c's ---------------------- */

static void srv_destroy(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void srv_create_buffer(struct wl_client *c, struct wl_resource *mgr,
	uint32_t id, int32_t w, int32_t h, uint32_t format)
{
	(void)w; (void)h; (void)format;
	/* like mgr_create_buffer: the buffer takes the manager's version */
	struct wl_resource *res = wl_resource_create(c,
		&picowl_buffer_v1_interface, wl_resource_get_version(mgr), id);
	assert(res);
	picowl_buffer_v1_send_failed(res, PICOWL_BUFFER_V1_REASON_NO_MEMORY);
}

static const struct picowl_buffer_manager_v1_interface srv_impl = {
	.destroy = srv_destroy,
	.create_buffer = srv_create_buffer,
};

static void srv_bind(struct wl_client *c, void *data, uint32_t version,
	uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(c,
		&picowl_buffer_manager_v1_interface, version, id);
	assert(r);
	wl_resource_set_implementation(r, &srv_impl, NULL, NULL);
	pw_zbproto_send_bind(r, true, 1);
}

/* ---- in-process client ------------------------------------------------ */

enum { EV_FORMAT = 1, EV_COPY_TYPE, EV_CACHING, EV_SYNC };

static struct {
	int ev[16];
	int n;
	uint32_t caching, copy_type;
	struct wl_display *dpy;
	struct picowl_buffer_manager_v1 *mgr;
	bool want_caching_listener;
	uint32_t want_version;
	int failed;
} cl;

static void rec(int e)
{
	assert(cl.n < 16);
	cl.ev[cl.n++] = e;
}
static void c_format(void *d, struct picowl_buffer_manager_v1 *m, uint32_t f)
{
	(void)d; (void)m;
	assert(f == 0x36314752);
	rec(EV_FORMAT);
}
static void c_copy_type(void *d, struct picowl_buffer_manager_v1 *m, uint32_t t)
{
	(void)d; (void)m;
	cl.copy_type = t;
	rec(EV_COPY_TYPE);
}
static void c_caching(void *d, struct picowl_buffer_manager_v1 *m, uint32_t c)
{
	(void)d; (void)m;
	cl.caching = c;
	rec(EV_CACHING);
}
static const struct picowl_buffer_manager_v1_listener l_full = {
	c_format, c_copy_type, c_caching
};
/* a client which never learnt about caching: the member is NULL, and
 * libwayland aborts if an event reaches it */
static const struct picowl_buffer_manager_v1_listener l_nocaching = {
	c_format, c_copy_type, NULL
};

static void sync_done(void *d, struct wl_callback *cb, uint32_t t)
{
	(void)d; (void)t;
	wl_callback_destroy(cb);
	rec(EV_SYNC);
}
static const struct wl_callback_listener sync_listener = { sync_done };

static void c_buf_failed(void *d, struct picowl_buffer_v1 *b, uint32_t r)
{
	(void)b; (void)r;
	cl.failed++;
	(void)d;
}
static void c_buf_dmabuf(void *d, struct picowl_buffer_v1 *b, int32_t fd,
	uint32_t a, uint32_t o, uint32_t h, uint32_t l)
{
	(void)d; (void)b; (void)fd; (void)a; (void)o; (void)h; (void)l;
	abort();
}
static void c_buf_done(void *d, struct picowl_buffer_v1 *b) { (void)d; (void)b; abort(); }
static void c_buf_serial(void *d, struct picowl_buffer_v1 *b, uint32_t s)
{
	(void)d; (void)b; (void)s;
	abort();
}
static const struct picowl_buffer_v1_listener buf_listener = {
	c_buf_dmabuf, c_buf_done, c_buf_failed, c_buf_serial, c_buf_serial
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	(void)d;
	if (strcmp(iface, picowl_buffer_manager_v1_interface.name))
		return;
	uint32_t v = ver < cl.want_version ? ver : cl.want_version;
	cl.mgr = wl_registry_bind(r, name, &picowl_buffer_manager_v1_interface, v);
	picowl_buffer_manager_v1_add_listener(cl.mgr,
		cl.want_caching_listener ? &l_full : &l_nocaching, NULL);
	/* the sync is queued right behind the bind */
	wl_callback_add_listener(wl_display_sync(cl.dpy), &sync_listener, NULL);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void pump(struct wl_display *sdpy, struct wl_event_loop *loop)
{
	for (int i = 0; i < 6; i++) {
		wl_display_flush(cl.dpy);
		wl_event_loop_dispatch(loop, 0);
		wl_display_flush_clients(sdpy);
		while (wl_display_prepare_read(cl.dpy) != 0)
			wl_display_dispatch_pending(cl.dpy);
		wl_display_read_events(cl.dpy);
		wl_display_dispatch_pending(cl.dpy);
	}
}

/* Server global at server_ver, client binds min(advertised, bind_ver). */
static void run_inproc(uint32_t server_ver, uint32_t bind_ver, bool caching_listener,
	const int *expect, int nexpect, uint32_t expect_ver)
{
	struct wl_display *sdpy = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(sdpy);
	assert(sdpy);
	assert(wl_global_create(sdpy, &picowl_buffer_manager_v1_interface,
		server_ver, NULL, srv_bind));

	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
	assert(wl_client_create(sdpy, sv[0]));
	assert(fcntl(sv[1], F_SETFL, O_NONBLOCK) == 0);

	memset(&cl, 0, sizeof(cl));
	cl.want_version = bind_ver;
	cl.want_caching_listener = caching_listener;
	cl.dpy = wl_display_connect_to_fd(sv[1]);
	assert(cl.dpy);
	struct wl_registry *reg = wl_display_get_registry(cl.dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	pump(sdpy, loop);

	assert(cl.mgr);
	assert(wl_proxy_get_version((struct wl_proxy *)cl.mgr) == expect_ver);
	assert(cl.n == nexpect);
	for (int i = 0; i < nexpect; i++)
		assert(cl.ev[i] == expect[i]);
	if (expect_ver >= 2) {
		assert(cl.caching == 1);
		assert(cl.copy_type == 1);
	}

	/* the buffer follows the manager's version; its events still work */
	struct picowl_buffer_v1 *b =
		picowl_buffer_manager_v1_create_buffer(cl.mgr, 8, 8, 0x36314752);
	picowl_buffer_v1_add_listener(b, &buf_listener, NULL);
	assert(wl_proxy_get_version((struct wl_proxy *)b) == expect_ver);
	pump(sdpy, loop);
	assert(cl.failed == 1);
	assert(cl.n == nexpect);	/* nothing more from the manager: sent once */

	picowl_buffer_v1_destroy(b);
	picowl_buffer_manager_v1_destroy(cl.mgr);
	wl_registry_destroy(reg);
	wl_display_flush(cl.dpy);
	wl_display_disconnect(cl.dpy);
	wl_display_destroy(sdpy);
}

static void test_inproc(void)
{
	static const int v2[] = { EV_FORMAT, EV_COPY_TYPE, EV_CACHING, EV_SYNC };
	static const int v1[] = { EV_FORMAT, EV_COPY_TYPE, EV_SYNC };

	/* v2 server, v2 client: format, copy_type, caching, in that order,
	 * all before the sync issued behind the bind */
	run_inproc(2, 2, true, v2, 4, 2);
	/* v2 server, client bound 1 with the v2 listener: gated */
	run_inproc(2, 1, true, v1, 3, 1);
	/* v2 server, client bound 1 with a NULL caching member: no abort */
	run_inproc(2, 1, false, v1, 3, 1);
	/* v1 server (old picowl): the client binds min(1, 2) */
	run_inproc(1, 2, true, v1, 3, 1);
	printf("✓ bufproto in-process\n");
}

/* ---- pw-test-client against the same server --------------------------- */

/* Runs the client (stdout into out) while serving; returns its exit status. */
static int run_client(struct wl_display *sdpy, const char *dir, const char *client,
	const char *extra, char *out, size_t outsz)
{
	int pfd[2];
	assert(pipe(pfd) == 0);
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		dup2(pfd[1], 1);
		close(pfd[0]);
		close(pfd[1]);
		setenv("XDG_RUNTIME_DIR", dir, 1);
		setenv("WAYLAND_DISPLAY", "wayland-bufproto", 1);
		if (extra)
			execl(client, client, "--probe", "--bind-version", extra, (char *)NULL);
		else
			execl(client, client, "--probe", (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	struct wl_event_loop *loop = wl_display_get_event_loop(sdpy);
	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int status = -1;
	for (;;) {
		wl_event_loop_dispatch(loop, 20);
		wl_display_flush_clients(sdpy);
		if (waitpid(pid, &status, WNOHANG) == pid)
			break;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (t.tv_sec - t0.tv_sec > 10) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			status = -1;
			break;
		}
	}
	ssize_t n = read(pfd[0], out, outsz - 1);
	out[n > 0 ? n : 0] = '\0';
	close(pfd[0]);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void run_exec(const char *client, const char *dir, uint32_t server_ver,
	const char *bind, const char *expect)
{
	struct wl_display *sdpy = wl_display_create();
	assert(sdpy);
	assert(wl_global_create(sdpy, &picowl_buffer_manager_v1_interface,
		server_ver, NULL, srv_bind));
	assert(wl_display_add_socket(sdpy, "wayland-bufproto") == 0);
	char out[512];
	int rc = run_client(sdpy, dir, client, bind, out, sizeof(out));
	if (rc != 0 || !strstr(out, expect)) {
		fprintf(stderr, "server v%u, client bind %s: status %d, output '%s'\n"
			"expected '%s'\n", server_ver, bind ? bind : "default", rc, out, expect);
		abort();
	}
	wl_display_destroy(sdpy);
}

static void test_exec(const char *client)
{
	char dir[] = "/tmp/picowl-bufproto-XXXXXX";
	assert(mkdtemp(dir));
	setenv("XDG_RUNTIME_DIR", dir, 1);

	/* v2 client on a v2 compositor */
	run_exec(client, dir, 2, NULL,
		"bufmgr version=2 format=1 copy_type=1 caching=1");
	/* a v1 client (the same binary, forced to bind 1): no caching, no error */
	run_exec(client, dir, 2, "1",
		"bufmgr version=1 format=1 copy_type=1 caching=-1");
	/* a v2 client on an old (v1) compositor binds 1 */
	run_exec(client, dir, 1, NULL,
		"bufmgr version=1 format=1 copy_type=1 caching=-1");

	char p[128];
	snprintf(p, sizeof(p), "%s/wayland-bufproto", dir);
	unlink(p);
	snprintf(p, sizeof(p), "%s/wayland-bufproto.lock", dir);
	unlink(p);
	rmdir(dir);
	printf("✓ bufproto pw-test-client v1 and v2\n");
}

int main(int argc, char **argv)
{
	test_inproc();
	if (argc > 1)
		test_exec(argv[1]);
	return 0;
}
