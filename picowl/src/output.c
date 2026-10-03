/* output.c - outputs, render format, transforms, blanking. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <wlr/backend/drm.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output_layout.h>
#include "picowl.h"
#include "power.h"

#ifndef DRM_IOCTL_MODE_CLOSEFB
struct drm_mode_closefb { uint32_t fb_id; uint32_t pad; };
#define DRM_IOCTL_MODE_CLOSEFB DRM_IOWR(0xD0, struct drm_mode_closefb)
#endif

/* Copy-type outputs unlock the scanned-out fb right after the flip. A client
 * dmabuf which is destroyed afterwards must not disable the live plane, so
 * this needs DRM_IOCTL_MODE_CLOSEFB (Linux 6.9, or a backport). Closing fb 0
 * fails with ENOENT where the ioctl exists and ENOTTY/EINVAL where it does
 * not; no framebuffer is touched. */
static bool drm_closefb_supported(int fd)
{
	struct drm_mode_closefb cl = { 0 };
	return fd >= 0 && drmIoctl(fd, DRM_IOCTL_MODE_CLOSEFB, &cl) < 0 &&
		errno == ENOENT;
}

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

/* ---- enabling commits, rotation and the copy-type swapchain ----------- */

/* Fill state with "enabled + mode + transform + render format", testing the
 * configured render format and falling back to XRGB8888 if rejected. */
static void output_build_enable(struct pw_output *o, struct wlr_output_state *state,
	struct wlr_output_mode *mode, enum wl_output_transform transform)
{
	struct wlr_output *wo = o->wlr_output;

	wlr_output_state_set_enabled(state, true);
	if (mode)
		wlr_output_state_set_mode(state, mode);
	wlr_output_state_set_transform(state, transform);

	uint32_t fmt = o->server->config->render_format
		? o->server->config->render_format : DRM_FORMAT_RGB565;
	wlr_output_state_set_render_format(state, fmt);
	if (!wlr_output_test_state(wo, state)) {
		if (fmt != DRM_FORMAT_XRGB8888) {
			pw_log(WLR_INFO, "output %s: render format 0x%08x rejected, "
				"falling back to XRGB8888", wo->name, fmt);
			wlr_output_state_set_render_format(state, DRM_FORMAT_XRGB8888);
		}
		if (!wlr_output_test_state(wo, state))
			pw_log(WLR_ERROR, "output %s: state test failed, trying anyway",
				wo->name);
	}
}

static bool output_commit_enable(struct pw_output *o,
	struct wlr_output_mode *mode, enum wl_output_transform transform)
{
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	output_build_enable(o, &state, mode, transform);
	bool ok = wlr_output_commit_state(o->wlr_output, &state);
	wlr_output_state_finish(&state);
	return ok;
}

static bool output_commit_disable(struct pw_output *o)
{
	if (!o->wlr_output->enabled)
		return true;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, false);
	bool ok = wlr_output_commit_state(o->wlr_output, &state);
	wlr_output_state_finish(&state);
	return ok;
}

/* Hardware rotation for t is possible on this output. */
static bool output_hw_possible(struct pw_output *o, enum wl_output_transform t)
{
	return o->rot_mode != PW_ROT_SOFTWARE && wlr_output_is_drm(o->wlr_output) &&
		wlr_drm_connector_supports_hw_rotation(o->wlr_output, t);
}

/* Return to software rotation: the output must be disabled. Resets the
 * plane rotation and enables with the transform. */
static bool output_enable_software(struct pw_output *o)
{
	if (wlr_output_is_drm(o->wlr_output))
		wlr_drm_connector_set_hw_rotation(o->wlr_output,
			WL_OUTPUT_TRANSFORM_NORMAL);
	o->hw_rotation = false;
	return output_commit_enable(o, wlr_output_preferred_mode(o->wlr_output),
		o->rotation);
}

/* Apply o->rotation to an output that is currently disabled (initial
 * setup, unblank, or runtime rotation after a disabling commit), using
 * hardware rotation when possible. Leaves the output enabled. */
static bool output_enable_rotated(struct pw_output *o)
{
	struct wlr_output *wo = o->wlr_output;
	enum wl_output_transform t = o->rotation;

	if (output_hw_possible(o, t) &&
			wlr_drm_connector_set_hw_rotation(wo, t)) {
		/* The patch nulled current_mode: MODE is not stripped. */
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
		o->hw_rotation = true;
		/* wo->width/height come from the swapped mode, so the size cannot
		 * tell whether the kernel applied the rotation: only a failed
		 * commit falls back to software rotation. */
		if (mode && output_commit_enable(o, mode,
				WL_OUTPUT_TRANSFORM_NORMAL))
			return true;
		pw_log(WLR_ERROR, "output %s: hw rotation commit failed, "
			"using software rotation", wo->name);
	} else if (o->rot_mode == PW_ROT_HARDWARE) {
		pw_log(WLR_ERROR, "output %s: hardware rotation %d requested but "
			"unsupported, using software rotation", wo->name, (int)t);
	}
	return output_enable_software(o);
}

/* The enabling commits (no buffer) made wlroots allocate a swapchain of its
 * own. Frames come from copy_swapchain, so free it: a copy-type output must
 * hold one framebuffer, not two. Buffers still locked by a pending flip stay
 * alive until it completes. wlroots re-creates it if a commit needs it. */
static void copy_drop_core_swapchain(struct pw_output *o)
{
	if (o->copy_swapchain && o->wlr_output->swapchain) {
		wlr_swapchain_destroy(o->wlr_output->swapchain);
		o->wlr_output->swapchain = NULL;
	}
}

/* Why this output runs without the single-buffer swapchain; once per output. */
static void single_buffer_off(struct pw_output *o, const char *why)
{
	if (o->sb_off_logged)
		return;
	o->sb_off_logged = true;
	pw_log(WLR_INFO, "output %s: single-buffer off, using 2 slots: %s",
		o->wlr_output->name, why);
}

static struct wlr_swapchain *copy_swapchain_create(struct pw_output *o)
{
	static struct wlr_drm_format_set set; /* RGB565/LINEAR, kept for the process */
	struct pw_server *server = o->server;
	struct wlr_output *wo = o->wlr_output;

	if (!o->copy_type || !server->config->single_buffer || !wo->enabled ||
			wo->width <= 0 || wo->height <= 0)
		return NULL;
	if (wo->render_format != DRM_FORMAT_RGB565) {
		single_buffer_off(o, "render format is not RGB565");
		return NULL;
	}
	const struct wlr_drm_format_set *prim = wlr_output_get_primary_formats(wo,
		server->allocator->buffer_caps);
	if (!prim || !wlr_drm_format_set_has(prim, DRM_FORMAT_RGB565,
			DRM_FORMAT_MOD_LINEAR)) {
		single_buffer_off(o, "no RGB565/LINEAR primary plane format");
		return NULL;
	}
	if (!wlr_drm_format_set_get(&set, DRM_FORMAT_RGB565) &&
			!wlr_drm_format_set_add(&set, DRM_FORMAT_RGB565,
				DRM_FORMAT_MOD_LINEAR)) {
		single_buffer_off(o, "out of memory");
		return NULL;
	}
	struct wlr_swapchain *sc = wlr_swapchain_create(server->allocator,
		wo->width, wo->height, wlr_drm_format_set_get(&set, DRM_FORMAT_RGB565));
	if (sc)
		pw_log(WLR_INFO, "output %s: single-buffer swapchain %dx%d",
			wo->name, wo->width, wo->height);
	else
		single_buffer_off(o, "swapchain allocation failed");
	return sc;
}

/* Bring o->copy_swapchain in line with the output size/state. The new
 * swapchain is created first; the old one is destroyed only after the output
 * was disabled since it was last used (was_disabled) or after one frame was
 * committed from the new one, so its fb is no longer scanned out (an
 * old-kernel drmModeRmFB would otherwise disable the plane). */
static void copy_swapchain_sync_inner(struct pw_output *o, bool was_disabled)
{
	struct wlr_output *wo = o->wlr_output;
	struct wlr_swapchain *old = o->copy_swapchain;

	if (old && wo->enabled && old->width == wo->width &&
			old->height == wo->height)
		return;
	if (!old && !(o->copy_type && o->server->config->single_buffer))
		return;

	struct wlr_swapchain *sc = copy_swapchain_create(o);
	o->copy_swapchain = sc;
	if (!old)
		return;
	if (!was_disabled && wo->enabled) {
		struct wlr_scene_output_state_options opts = { .swapchain = sc };
		bool ok = o->scene_output &&
			wlr_scene_output_commit(o->scene_output, sc ? &opts : NULL);
		if (!ok) {
			/* Could not move the plane off the old buffer: disable. */
			output_commit_disable(o);
			wlr_swapchain_destroy(old);
			if (!output_enable_rotated(o))
				pw_log(WLR_ERROR, "output %s: re-enable failed", wo->name);
			wlr_output_schedule_frame(wo);
			return;
		}
	}
	wlr_swapchain_destroy(old);
}

static void copy_swapchain_sync(struct pw_output *o, bool was_disabled)
{
	copy_swapchain_sync_inner(o, was_disabled);
	copy_drop_core_swapchain(o);
}

static void output_frame(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *so = output->scene_output;

	/* Commits only when the scene has damage; nothing is forced. */
	struct wlr_scene_output_state_options opts = {
		.swapchain = output->copy_swapchain };
	bool needs = wlr_scene_output_needs_frame(so) && output->wlr_output->enabled;
	bool ok = wlr_scene_output_commit(so,
		output->copy_swapchain ? &opts : NULL);
	pw_zerocopy_output_committed(output, needs && ok);

	if (!output->sb_degraded_logged && output->copy_swapchain &&
			output->copy_swapchain->slots[1].buffer) {
		output->sb_degraded_logged = true;
		pw_log(WLR_INFO, "output %s: single-buffer degraded to 2 slots "
			"(the kernel keeps the scanned-out buffer locked, e.g. legacy "
			"KMS ignores copy_type)", output->wlr_output->name);
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *listener, void *data)
{
	const struct wlr_output_event_request_state *event = data;
	struct pw_output *output = wl_container_of(listener, output, request_state);

	bool disables = (event->state->committed & WLR_OUTPUT_STATE_ENABLED) &&
		!event->state->enabled;
	if (wlr_output_commit_state(output->wlr_output, event->state)) {
		copy_swapchain_sync(output, disables);
		pw_output_update_geometry(output);
	}
}

static void output_present(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_output *output = wl_container_of(listener, output, present);
	pw_zerocopy_output_presented(output);
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

	pw_input_output_removed(output->server, output);
	pw_zerocopy_output_removed(output);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->present.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	if (output->copy_swapchain)
		wlr_swapchain_destroy(output->copy_swapchain);
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

	output->rot_mode = pw_config_rot_mode(server->config, wlr_output->name);
	output->rotation = output_config_transform(server->config, wlr_output->name);

	output->copy_type = false;
	output->drm_driver[0] = '\0';
	if (wlr_output_is_drm(wlr_output)) {
		drmVersionPtr v = drmGetVersion(
			wlr_backend_get_drm_fd(wlr_output->backend));
		if (v) {
			snprintf(output->drm_driver, sizeof(output->drm_driver), "%s",
				v->name ? v->name : "");
			drmFreeVersion(v);
		}
		enum pw_copy_override ov =
			pw_config_copy_override(server->config, wlr_output->name);
		output->copy_type = pw_copytype_resolve(output->drm_driver, ov);
		if (output->copy_type && !drm_closefb_supported(
				wlr_backend_get_drm_fd(wlr_output->backend))) {
			if (ov == PW_COPY_YES) {
				pw_log(WLR_ERROR, "output %s: copy_type forced but the kernel "
					"has no DRM_IOCTL_MODE_CLOSEFB (needs 6.9): destroying a "
					"scanned-out client buffer can blank the display",
					wlr_output->name);
			} else {
				pw_log(WLR_INFO, "output %s: driver '%s' is copy-type but the "
					"kernel has no DRM_IOCTL_MODE_CLOSEFB (needs 6.9): "
					"copy_type disabled", wlr_output->name, output->drm_driver);
				output->copy_type = false;
			}
		}
		/* wlroots ignores copy_type without atomic modesetting. */
		const char *na = getenv("WLR_DRM_NO_ATOMIC");
		if (output->copy_type && na && strcmp(na, "1") == 0) {
			pw_log(WLR_INFO, "output %s: copy_type disabled: legacy KMS "
				"(WLR_DRM_NO_ATOMIC)", wlr_output->name);
			output->copy_type = false;
		}
		wlr_drm_connector_set_copy_type(wlr_output, output->copy_type);
	}
	pw_log(WLR_INFO, "output %s: driver '%s' copy_type=%s rotation_mode=%s",
		wlr_output->name, output->drm_driver,
		output->copy_type ? "yes" : "no", pw_rot_mode_name(output->rot_mode));

	/* The output is still disabled here, as hardware rotation requires. */
	bool ok = output_enable_rotated(output);
	if (!ok) {
		pw_log(WLR_ERROR, "output %s: commit failed", wlr_output->name);
		free(output);
		return;
	}
	if (output->hw_rotation)
		pw_log(WLR_INFO, "output %s: hardware rotation %d, %dx%d",
			wlr_output->name, (int)output->rotation,
			wlr_output->width, wlr_output->height);

	/* Hotplugged while blanked: stay dark, the unblank enables it. */
	if (server->blanked)
		output_commit_disable(output);

	const float *bg = server->config->background;
	output->background = wlr_scene_rect_create(server->layer_background,
		wlr_output->width, wlr_output->height, bg);
	if (!output->background) {
		output_commit_disable(output);
		free(output);
		return;
	}
	/* Before the listeners: output_frame needs it. */
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (!output->scene_output) {
		pw_log(WLR_ERROR, "output %s: cannot create scene output",
			wlr_output->name);
		wlr_scene_node_destroy(&output->background->node);
		output_commit_disable(output);
		free(output);
		return;
	}

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->present.notify = output_present;
	wl_signal_add(&wlr_output->events.present, &output->present);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *lo =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	if (lo)
		wlr_scene_output_layout_add_output(server->scene_layout, lo,
			output->scene_output);

	output->copy_swapchain = copy_swapchain_create(output);
	copy_drop_core_swapchain(output);

	pw_input_apply_rotation(server, output);
	pw_zerocopy_output_added(output);
	pw_output_update_geometry(output);
	pw_view_arrange_all(server);
}

void pw_output_rotate(struct pw_output *o, enum wl_output_transform t)
{
	struct wlr_output *wo = o->wlr_output;
	o->rotation = t;

	if (!wo->enabled) {
		/* Blanked: applied by the unblanking commit. */
		return;
	}

	bool disabled = false;
	if (o->hw_rotation || output_hw_possible(o, t)) {
		/* Plane rotation may only change while the output is off. */
		if (!output_commit_disable(o)) {
			pw_log(WLR_ERROR, "output %s: disable for rotation failed",
				wo->name);
		} else {
			disabled = true;
			if (!output_enable_rotated(o)) {
				pw_log(WLR_ERROR, "output %s: rotation commit failed",
					wo->name);
				o->hw_rotation = false;
			}
		}
	}
	if (!disabled) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_state_set_transform(&state, t);
		if (!wlr_output_commit_state(wo, &state))
			pw_log(WLR_ERROR, "output %s: transform %d commit failed",
				wo->name, (int)t);
		wlr_output_state_finish(&state);
	}
	if (!wo->enabled) {
		pw_log(WLR_ERROR, "output %s: dead after rotation, re-enabling",
			wo->name);
		o->hw_rotation = false;
		if (!output_enable_software(o))
			pw_log(WLR_ERROR, "output %s: re-enable failed", wo->name);
		disabled = true;
	}

	copy_swapchain_sync(o, disabled);
	pw_input_apply_rotation(o->server, o);
	pw_output_update_geometry(o);
	wlr_output_schedule_frame(wo);
}

/* Blank/unblank one output. Unblanking re-applies the rotation (hardware
 * rotation needs the mode in the enabling commit). The copy swapchain is
 * dropped while blanked (the output is disabled, so that is safe) and
 * recreated on unblank. */
static void output_set_power(struct pw_output *o, bool on)
{
	struct wlr_output *wo = o->wlr_output;
	if (on == wo->enabled)
		return;
	bool ok;
	if (on) {
		if (wlr_output_is_drm(wo)) {
			ok = output_enable_rotated(o);
		} else {
			struct wlr_output_state state;
			wlr_output_state_init(&state);
			wlr_output_state_set_enabled(&state, true);
			wlr_output_state_set_transform(&state, o->rotation);
			ok = wlr_output_commit_state(wo, &state);
			wlr_output_state_finish(&state);
		}
	} else {
		ok = output_commit_disable(o);
	}
	if (!ok) {
		pw_log(WLR_ERROR, "output %s: %s failed", wo->name,
			on ? "unblank" : "blank");
		return;
	}
	if (!on) {
		if (o->copy_swapchain) {
			wlr_swapchain_destroy(o->copy_swapchain);
			o->copy_swapchain = NULL;
		}
		return;
	}
	copy_swapchain_sync(o, true);
	pw_input_apply_rotation(o->server, o);
	pw_output_update_geometry(o);
	wlr_output_schedule_frame(wo);
}

/* All outputs are disabled (the real state, which a failed commit can leave
 * different from the requested one). */
static bool outputs_all_off(struct pw_server *server)
{
	struct pw_output *o;
	wl_list_for_each(o, &server->outputs, link)
		if (o->wlr_output->enabled)
			return false;
	return !wl_list_empty(&server->outputs);
}

static void output_power_set_mode(struct wl_listener *listener, void *data)
{
	const struct wlr_output_power_v1_set_mode_event *event = data;
	struct pw_server *server =
		wl_container_of(listener, server, output_power_set_mode);

	bool off = event->mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF;
	struct pw_output *o;
	wl_list_for_each(o, &server->outputs, link)
		if (o->wlr_output == event->output)
			output_set_power(o, !off);

	server->blanked = outputs_all_off(server);
	pw_power_sync_blanked(server);
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

	wl_list_for_each(output, &server->outputs, link)
		output_set_power(output, !blank);
	/* From the real state: if the unblank commit failed, the display is
	 * still dark and power.c must keep swallowing input and retry. */
	server->blanked = wl_list_empty(&server->outputs) ? blank :
		outputs_all_off(server);
}

void pw_output_usable_area(struct pw_output *output, struct wlr_box *box)
{
	*box = output->usable_area;
}
