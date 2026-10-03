/* output.c - outputs, render format, transforms, blanking. */
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <wlr/types/wlr_output_layout.h>
#include "picowl.h"

static void output_update_background(struct pw_output *output)
{
	struct wlr_box box;
	wlr_output_layout_get_box(output->server->output_layout,
		output->wlr_output, &box);
	if (wlr_box_empty(&box))
		return;
	wlr_scene_node_set_position(&output->background->node, box.x, box.y);
	wlr_scene_rect_set_size(output->background, box.width, box.height);
}

void pw_output_update_geometry(struct pw_output *output)
{
	output_update_background(output);
	pw_layer_arrange(output);
}

static void output_frame(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *so = output->scene_output;

	/* Commits only when the scene has damage; nothing is forced. */
	wlr_scene_output_commit(so, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *listener, void *data)
{
	const struct wlr_output_event_request_state *event = data;
	struct pw_output *output = wl_container_of(listener, output, request_state);

	if (wlr_output_commit_state(output->wlr_output, event->state)) {
		pw_output_update_geometry(output);
	}
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_output *output = wl_container_of(listener, output, destroy);

	/* Layer surfaces live in lists embedded in this struct; detach and close
	 * them before freeing (this listener runs before layer.c's own). */
	for (int i = 0; i < 4; i++) {
		struct pw_layer_surface *ls, *tmp;
		wl_list_for_each_safe(ls, tmp, &output->layers[i], link) {
			struct wlr_layer_surface_v1 *s = ls->wlr_layer_surface;
			wl_list_remove(&ls->link);
			wl_list_init(&ls->link);
			ls->output = NULL;
			s->output = NULL;
			wlr_layer_surface_v1_destroy(s);
		}
	}

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	if (output->background)
		wlr_scene_node_destroy(&output->background->node);
	free(output);
}

static enum wl_output_transform output_config_transform(
	const struct pw_config *config, const char *name)
{
	const struct pw_output_transform *t, *wild = NULL;

	wl_list_for_each(t, &config->transforms, link) {
		if (name && t->name && strcmp(t->name, name) == 0)
			return t->transform;
		if (t->name && strcmp(t->name, "*") == 0)
			wild = t;
	}
	return wild ? wild->transform : WL_OUTPUT_TRANSFORM_NORMAL;
}

static void output_new(struct wl_listener *listener, void *data)
{
	struct pw_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (!wlr_output_init_render(wlr_output, server->allocator,
			server->renderer))
		return;

	struct pw_output *output = calloc(1, sizeof(*output));
	if (!output)
		return;
	output->server = server;
	output->wlr_output = wlr_output;
	for (int i = 0; i < 4; i++)
		wl_list_init(&output->layers[i]);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode)
		wlr_output_state_set_mode(&state, mode);

	wlr_output_state_set_transform(&state,
		output_config_transform(server->config, wlr_output->name));

	uint32_t fmt = server->config->render_format
		? server->config->render_format : DRM_FORMAT_RGB565;
	wlr_output_state_set_render_format(&state, fmt);
	if (!wlr_output_test_state(wlr_output, &state)) {
		if (fmt != DRM_FORMAT_XRGB8888) {
			pw_log(WLR_INFO, "output %s: render format 0x%08x rejected, "
				"falling back to XRGB8888", wlr_output->name, fmt);
			wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB8888);
		}
		if (!wlr_output_test_state(wlr_output, &state))
			pw_log(WLR_ERROR, "output %s: state test failed, trying anyway",
				wlr_output->name);
	}

	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		pw_log(WLR_ERROR, "output %s: commit failed", wlr_output->name);
		free(output);
		return;
	}

	const float *bg = server->config->background;
	output->background = wlr_scene_rect_create(server->layer_background,
		wlr_output->width, wlr_output->height, bg);
	if (!output->background) {
		free(output);
		return;
	}

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *lo =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (lo && output->scene_output)
		wlr_scene_output_layout_add_output(server->scene_layout, lo,
			output->scene_output);

	pw_output_update_geometry(output);
	pw_view_arrange_all(server);
}

static void output_power_set_mode(struct wl_listener *listener, void *data)
{
	const struct wlr_output_power_v1_set_mode_event *event = data;
	struct pw_server *server =
		wl_container_of(listener, server, output_power_set_mode);

	bool off = event->mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, !off);
	if (!wlr_output_commit_state(event->output, &state))
		pw_log(WLR_ERROR, "output %s: power mode change failed",
			event->output->name);
	wlr_output_state_finish(&state);
	if (!off)
		wlr_output_schedule_frame(event->output);

	bool all_off = true;
	struct pw_output *o;
	wl_list_for_each(o, &server->outputs, link)
		if (o->wlr_output->enabled)
			all_off = false;
	server->blanked = all_off && !wl_list_empty(&server->outputs);
}

void pw_output_init(struct pw_server *server)
{
	wl_list_init(&server->outputs);

	server->new_output.notify = output_new;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	server->output_power_mgr = wlr_output_power_manager_v1_create(server->display);
	if (server->output_power_mgr) {
		server->output_power_set_mode.notify = output_power_set_mode;
		wl_signal_add(&server->output_power_mgr->events.set_mode,
			&server->output_power_set_mode);
	}
}

void pw_output_blank(struct pw_server *server, bool blank)
{
	struct pw_output *output;

	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, !blank);
		if (!wlr_output_commit_state(output->wlr_output, &state))
			pw_log(WLR_ERROR, "output %s: %s failed",
				output->wlr_output->name, blank ? "blank" : "unblank");
		wlr_output_state_finish(&state);
		if (!blank)
			wlr_output_schedule_frame(output->wlr_output);
	}
	server->blanked = blank;
}

void pw_output_usable_area(struct pw_output *output, struct wlr_box *box)
{
	*box = output->usable_area;
}
