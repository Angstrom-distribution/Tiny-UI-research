/*
 * server.c - display/backend/scene setup, signal handling, main loop and
 * process spawning.
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>

#include "picowl.h"
#include "mem.h"
#include "zerocopy.h"

/* File-local state; the header has no room for these and there is only ever
 * one server per process. */
static struct {
	struct pw_server *server;
	struct wl_event_source *sigterm;
	struct wl_event_source *sigint;
	struct wlr_xdg_activation_v1 *activation;
	struct wlr_xdg_decoration_manager_v1 *decoration_mgr;
	struct wl_listener request_activate;
	struct wl_listener new_decoration;
	struct wl_listener display_destroy;
	bool listeners_added;
	bool backend_started;
} S;

static void decoration_handle_request_mode(struct wl_listener *listener, void *data);
static void decoration_handle_destroy(struct wl_listener *listener, void *data);

/* Per-decoration listeners, freed with the decoration. */
struct pw_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wl_listener request_mode;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void decoration_apply(struct pw_decoration *d)
{
	/* We draw no decorations. Telling the client "server side" makes it
	 * draw none either. Only configure once the surface is initialised. */
	if (d->deco->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_handle_request_mode(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_decoration *d = wl_container_of(listener, d, request_mode);
	decoration_apply(d);
}

static void decoration_handle_commit(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_decoration *d = wl_container_of(listener, d, commit);
	if (d->deco->toplevel->base->initial_commit)
		wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_decoration *d = wl_container_of(listener, d, destroy);
	wl_list_remove(&d->commit.link);
	wl_list_remove(&d->request_mode.link);
	wl_list_remove(&d->destroy.link);
	free(d);
}

static void handle_new_decoration(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	struct pw_decoration *d = calloc(1, sizeof(*d));
	if (!d) {
		pw_log(WLR_ERROR, "decoration: out of memory");
		return;
	}
	d->deco = deco;
	d->request_mode.notify = decoration_handle_request_mode;
	wl_signal_add(&deco->events.request_mode, &d->request_mode);
	d->commit.notify = decoration_handle_commit;
	wl_signal_add(&deco->toplevel->base->surface->events.commit, &d->commit);
	d->destroy.notify = decoration_handle_destroy;
	wl_signal_add(&deco->events.destroy, &d->destroy);
	decoration_apply(d);
}

static void handle_request_activate(struct wl_listener *listener, void *data)
{
	(void)listener;
	const struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_try_from_wlr_surface(event->surface);
	if (!toplevel)
		return;
	struct pw_view *view;
	wl_list_for_each(view, &S.server->views, link) {
		if (view->xdg_toplevel == toplevel) {
			if (view->mapped)
				pw_view_focus(view);
			return;
		}
	}
}

static int handle_signal(int signo, void *data)
{
	struct pw_server *server = data;
	pw_log(WLR_INFO, "picowl: caught signal %d, exiting", signo);
	wl_display_terminate(server->display);
	return 0;
}

static void idle_trim_callback(void *data)
{
	(void)data;
	pw_mem_trim();
	pw_mem_log_status("startup");
}

int pw_spawn(const char *cmd)
{
	if (!cmd || !*cmd)
		return -1;

	pid_t pid = fork();
	if (pid < 0) {
		pw_log(WLR_ERROR, "fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		/* Intermediate child: detach, then fork the real process so that
		 * it is reparented to init and never becomes our zombie. */
		setsid();
		pid_t pid2 = fork();
		if (pid2 < 0)
			_exit(1);
		if (pid2 == 0) {
			sigset_t set;
			sigemptyset(&set);
			sigprocmask(SIG_SETMASK, &set, NULL);
			signal(SIGPIPE, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			signal(SIGTERM, SIG_DFL);
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}

	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

bool pw_server_init(struct pw_server *server, struct pw_config *config)
{
	memset(server, 0, sizeof(*server));
	memset(&S, 0, sizeof(S));
	server->config = config;
	S.server = server;
	wl_list_init(&server->outputs);
	wl_list_init(&server->views);
	wl_list_init(&server->keyboards);

	server->display = wl_display_create();
	if (!server->display) {
		pw_log(WLR_ERROR, "cannot create wl_display");
		return false;
	}
	server->event_loop = wl_display_get_event_loop(server->display);

	S.sigterm = wl_event_loop_add_signal(server->event_loop, SIGTERM, handle_signal, server);
	S.sigint = wl_event_loop_add_signal(server->event_loop, SIGINT, handle_signal, server);

	server->backend = wlr_backend_autocreate(server->event_loop, &server->session);
	if (!server->backend) {
		pw_log(WLR_ERROR, "cannot create backend");
		goto fail;
	}

	server->renderer = wlr_renderer_autocreate(server->backend);
	if (!server->renderer) {
		pw_log(WLR_ERROR, "cannot create renderer");
		goto fail;
	}
	if (!wlr_renderer_init_wl_display(server->renderer, server->display)) {
		pw_log(WLR_ERROR, "cannot initialise renderer globals");
		goto fail;
	}

	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (!server->allocator) {
		pw_log(WLR_ERROR, "cannot create allocator");
		goto fail;
	}

	if (!wlr_compositor_create(server->display, 6, server->renderer)) {
		pw_log(WLR_ERROR, "cannot create compositor");
		goto fail;
	}
	wlr_subcompositor_create(server->display);
	wlr_data_device_manager_create(server->display);
	/* Select-to-copy, middle-click-to-paste: GTK and Qt apps expect it. */
	if (!wlr_primary_selection_v1_device_manager_create(server->display))
		pw_log(WLR_ERROR, "cannot create the primary selection manager");

	server->output_layout = wlr_output_layout_create(server->display);
	server->scene = wlr_scene_create();
	if (!server->output_layout || !server->scene) {
		pw_log(WLR_ERROR, "cannot create scene");
		goto fail;
	}
	server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

	/* Z-order, bottom to top. */
	server->layer_background = wlr_scene_tree_create(&server->scene->tree);
	server->layer_apps = wlr_scene_tree_create(&server->scene->tree);
	server->layer_panel = wlr_scene_tree_create(&server->scene->tree);
	server->layer_overlay = wlr_scene_tree_create(&server->scene->tree);
	server->layer_lock = wlr_scene_tree_create(&server->scene->tree);
	if (!server->layer_background || !server->layer_apps || !server->layer_panel ||
			!server->layer_overlay || !server->layer_lock) {
		pw_log(WLR_ERROR, "cannot create scene trees");
		goto fail;
	}
	/* Per-output background rects are created by output.c (pw_output.background)
	 * inside layer_background. */

	/* Note: wlr_scene_set_linux_dmabuf_v1 is never called here. The pixman
	 * renderer has no DRM fd, so the scene feedback would fail per frame. The
	 * scene's direct_scanout stays at its default (enabled unless
	 * WLR_SCENE_DISABLE_DIRECT_SCANOUT is set): zero-copy relies on it. */

	wlr_presentation_create(server->display, server->backend, 2);
	wlr_viewporter_create(server->display);
	/* Clients such as grim take the logical output size and position from here and
	 * otherwise guess a size of zero. */
	wlr_xdg_output_manager_v1_create(server->display, server->output_layout);
	wlr_single_pixel_buffer_manager_v1_create(server->display);
	/* Advertised so clients stop falling back to guessing; picowl has no
	 * content-dependent policy yet, so the hint is only logged (view.c). */
	server->content_type_mgr = wlr_content_type_manager_v1_create(server->display, 1);
	if (!server->content_type_mgr)
		pw_log(WLR_ERROR, "cannot create the content-type manager");

	/* Opt-in: any client that can connect could otherwise read the screen. */
	if (server->config->capture_enable) {
		if (wlr_screencopy_manager_v1_create(server->display))
			pw_log(WLR_INFO, "capture: zwlr_screencopy_manager_v1 enabled");
		else
			pw_log(WLR_ERROR, "capture: cannot create the screencopy manager");
	}

	S.activation = wlr_xdg_activation_v1_create(server->display);
	S.decoration_mgr = wlr_xdg_decoration_manager_v1_create(server->display);
	if (!S.activation || !S.decoration_mgr) {
		pw_log(WLR_ERROR, "cannot create activation/decoration managers");
		goto fail;
	}
	S.request_activate.notify = handle_request_activate;
	wl_signal_add(&S.activation->events.request_activate, &S.request_activate);
	S.new_decoration.notify = handle_new_decoration;
	wl_signal_add(&S.decoration_mgr->events.new_toplevel_decoration, &S.new_decoration);
	S.listeners_added = true;

	if (!pw_zerocopy_init(server))
		goto fail;

	/* Module inits; input creates the seat/cursor and must precede idle. */
	pw_output_init(server);
	if (!pw_view_init(server))
		goto fail;
	pw_layer_init(server);
	if (!pw_input_init(server))
		goto fail;
	pw_cursor_init(server);
	pw_idle_init(server);
	pw_power_init(server);
	pw_lease_init(server); /* before the backend starts: the first output is offered */
	return true;

fail:
	pw_server_finish(server);
	return false;
}

int pw_server_run(struct pw_server *server)
{
	/* libwayland only says it failed to bind, which hides that the real
	 * cause is a service manager that does not create the runtime dir. */
	const char *rtdir = getenv("XDG_RUNTIME_DIR");
	if (!rtdir || !rtdir[0]) {
		pw_log(WLR_ERROR, "XDG_RUNTIME_DIR is not set; it must name a writable "
			"directory (mode 0700) for the Wayland socket");
		return 1;
	}

	const char *socket = wl_display_add_socket_auto(server->display);
	if (!socket) {
		pw_log(WLR_ERROR, "cannot open wayland socket");
		return 1;
	}

	if (!wlr_backend_start(server->backend)) {
		pw_log(WLR_ERROR, "cannot start backend");
		return 1;
	}
	S.backend_started = true;

	setenv("WAYLAND_DISPLAY", socket, 1);
	pw_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);

	pw_osk_init(server);

	if (server->config) {
		struct pw_autostart *a;
		wl_list_for_each(a, &server->config->autostart, link) {
			if (pw_spawn(a->command) < 0)
				pw_log(WLR_ERROR, "autostart failed: %s", a->command);
		}
	}

	if (server->config && server->config->trim_after_start) {
		wl_event_loop_add_idle(server->event_loop, idle_trim_callback, NULL);
	}

	wl_display_run(server->display);

	pw_mem_log_status("exit");

	return 0;
}

static void listener_drop(struct wl_listener *l)
{
	if (l->link.next) {
		wl_list_remove(&l->link);
		l->link.next = l->link.prev = NULL;
	}
}

void pw_server_finish(struct pw_server *server)
{
	if (!server->display)
		return;

	listener_drop(&server->new_output);
	listener_drop(&server->new_xdg_toplevel);
	listener_drop(&server->new_layer_surface);
	listener_drop(&server->new_idle_inhibitor); /* wlroots asserts it is empty at display destroy */
	listener_drop(&server->output_power_set_mode);

	/* Before the clients are destroyed: the keyboard then exits because it
	 * was told to, not because it lost its connection, and is not restarted. */
	pw_osk_finish(server);
	pw_zerocopy_finish(server);
	pw_lease_finish(server);

	wl_display_destroy_clients(server->display);

	if (S.listeners_added) {
		wl_list_remove(&S.request_activate.link);
		wl_list_remove(&S.new_decoration.link);
		S.listeners_added = false;
	}
	if (S.sigterm) {
		wl_event_source_remove(S.sigterm);
		S.sigterm = NULL;
	}
	if (S.sigint) {
		wl_event_source_remove(S.sigint);
		S.sigint = NULL;
	}
	pw_power_finish(server);
	pw_output_finish(server);
	if (server->idle_timer) {
		wl_event_source_remove(server->idle_timer);
		server->idle_timer = NULL;
	}

	pw_cursor_finish(server);
	pw_input_finish(server);
	pw_view_finish(server);
	/* Backend first: output destroy handlers still need the scene alive. */
	if (server->backend)
		wlr_backend_destroy(server->backend);
	server->backend = NULL;
	if (server->scene)
		wlr_scene_node_destroy(&server->scene->tree.node);
	if (server->cursor) {
		wlr_cursor_destroy(server->cursor);
		server->cursor = NULL;
	}
	if (server->allocator)
		wlr_allocator_destroy(server->allocator);
	if (server->renderer)
		wlr_renderer_destroy(server->renderer);
	wl_display_destroy(server->display);

	server->display = NULL;
	server->scene = NULL;
	server->allocator = NULL;
	server->renderer = NULL;
	server->backend = NULL;
}
