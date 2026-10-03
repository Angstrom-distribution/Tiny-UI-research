/*
 * layer.c - wlr-layer-shell v1 support (panel, on-screen keyboard, background).
 */
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include "picowl.h"

/* Layers are arranged top (overlay) to bottom (background). */
static const enum zwlr_layer_shell_v1_layer layer_order[4] = {
	ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
	ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
	ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
};

static struct wlr_scene_tree *tree_for_layer(struct pw_server *server,
		enum zwlr_layer_shell_v1_layer layer)
{
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return server->layer_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return server->layer_panel;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
	default:
		return server->layer_overlay;
	}
}

static struct pw_output *output_from_wlr(struct pw_server *server,
		struct wlr_output *wlr_output)
{
	struct pw_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == wlr_output) {
			return o;
		}
	}
	return NULL;
}

static void focus_layer_surface(struct pw_layer_surface *ls)
{
	struct wlr_seat *seat = ls->server->seat;
	struct wlr_surface *surface = ls->wlr_layer_surface->surface;

	if (!seat || seat->keyboard_state.focused_surface == surface) {
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes,
			kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
}

/* Give keyboard focus to the topmost mapped exclusive surface on the TOP or
 * OVERLAY layers, if there is one. Returns true if one holds focus. */
static bool focus_exclusive(struct pw_server *server)
{
	struct pw_output *o;
	for (int i = 0; i < 2; i++) { /* overlay, then top */
		wl_list_for_each(o, &server->outputs, link) {
			struct pw_layer_surface *ls;
			wl_list_for_each(ls, &o->layers[layer_order[i]], link) {
				if (ls->mapped && ls->wlr_layer_surface->current.keyboard_interactive
						== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
					focus_layer_surface(ls);
					return true;
				}
			}
		}
	}
	return false;
}

/* The layer surface lost focus eligibility (unmap/destroy): hand focus back. */
static void restore_focus(struct pw_layer_surface *ls)
{
	struct pw_server *server = ls->server;
	struct wlr_seat *seat = server->seat;

	if (!seat || seat->keyboard_state.focused_surface != ls->wlr_layer_surface->surface) {
		return;
	}
	if (focus_exclusive(server)) {
		return;
	}
	if (server->focused_view) {
		pw_view_focus(server->focused_view);
	} else {
		wlr_seat_keyboard_notify_clear_focus(seat);
	}
}

static void arrange_layers(struct pw_output *output, const struct wlr_box *full,
		struct wlr_box *usable, bool exclusive, bool hide_top)
{
	for (int i = 0; i < 4; i++) {
		struct pw_layer_surface *ls;
		wl_list_for_each(ls, &output->layers[layer_order[i]], link) {
			struct wlr_layer_surface_v1 *s = ls->wlr_layer_surface;
			if (!s->initialized) {
				continue;
			}
			if (exclusive != (s->current.exclusive_zone > 0)) {
				continue;
			}
			if (hide_top && layer_order[i] == ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
				/* Panel hidden: configure it, but its exclusive zone
				 * must not shrink the usable area. */
				struct wlr_box scratch = *usable;
				wlr_scene_layer_surface_v1_configure(ls->scene_layer, full, &scratch);
				continue;
			}
			wlr_scene_layer_surface_v1_configure(ls->scene_layer, full, usable);
		}
	}
}

void pw_layer_arrange(struct pw_output *output)
{
	struct pw_server *server = output->server;
	struct wlr_box full = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);

	struct wlr_box usable = full;
	arrange_layers(output, &full, &usable, true, output->server->panel_hidden);
	arrange_layers(output, &full, &usable, false, output->server->panel_hidden);

	bool changed = full.x != output->full_area.x
		|| full.y != output->full_area.y
		|| full.width != output->full_area.width
		|| full.height != output->full_area.height
		|| usable.x != output->usable_area.x
		|| usable.y != output->usable_area.y
		|| usable.width != output->usable_area.width
		|| usable.height != output->usable_area.height;
	output->usable_area = usable;
	output->full_area = full;

	focus_exclusive(server);

	if (changed) {
		pw_view_arrange_all(server);
	}
}

/* A mapped TOP surface with exclusive keyboard focus (launcher, OSK) must
 * stay visible, or it would grab the keyboard unseen. */
static bool top_has_exclusive(struct pw_server *s)
{
	struct pw_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct pw_layer_surface *ls;
		wl_list_for_each(ls, &o->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], link) {
			if (ls->mapped && ls->wlr_layer_surface->current.keyboard_interactive
					== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
				return true;
		}
	}
	return false;
}

void pw_panel_update(struct pw_server *s)
{
	struct pw_view *fv = s->focused_view;
	bool hidden = s->config && s->config->panel_autohide
		&& fv && fv->mapped && !s->panel_forced_visible
		&& !top_has_exclusive(s);

	if (hidden != s->panel_hidden) {
		s->panel_hidden = hidden;
		wlr_scene_node_set_enabled(&s->layer_panel->node, !hidden);
		struct pw_output *o;
		wl_list_for_each(o, &s->outputs, link)
			pw_layer_arrange(o);
	}
	/* Focus, stacking or panel visibility changed: a hidden panel's
	 * inhibitors must not count. */
	pw_idle_inhibit_update(s);
}

void pw_panel_toggle(struct pw_server *s)
{
	s->panel_forced_visible = !s->panel_forced_visible;
	pw_panel_update(s);
}

static void handle_map(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_layer_surface *ls = wl_container_of(listener, ls, map);
	ls->mapped = true;

	enum zwlr_layer_shell_v1_layer layer = ls->wlr_layer_surface->current.layer;
	enum zwlr_layer_surface_v1_keyboard_interactivity ki =
		ls->wlr_layer_surface->current.keyboard_interactive;
	/* An on-demand TOP surface hidden by panel autohide must not take the
	 * keyboard unseen (an exclusive one unhides the panel in the update). */
	if (ki != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
			&& (layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
				|| (layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP
					&& (!ls->server->panel_hidden
						|| ki == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)))) {
		focus_layer_surface(ls);
	}
	if (ls->output) {
		pw_layer_arrange(ls->output);
	}
	pw_panel_update(ls->server);
}

static void handle_unmap(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_layer_surface *ls = wl_container_of(listener, ls, unmap);
	ls->mapped = false;
	restore_focus(ls);
	if (ls->output) {
		pw_layer_arrange(ls->output);
	}
	pw_panel_update(ls->server);
}

static void handle_commit(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_layer_surface *ls = wl_container_of(listener, ls, commit);
	struct wlr_layer_surface_v1 *s = ls->wlr_layer_surface;

	if (!ls->output) {
		return;
	}

	if (s->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		/* Layer changed: move between lists and scene trees. */
		wl_list_remove(&ls->link);
		wl_list_insert(&ls->output->layers[s->current.layer], &ls->link);
		wlr_scene_node_reparent(&ls->scene_tree->node,
			tree_for_layer(ls->server, s->current.layer));
	}

	if (s->initial_commit || s->current.committed != 0
			|| ls->mapped != s->surface->mapped) {
		ls->mapped = s->surface->mapped;
		pw_layer_arrange(ls->output);
		/* interactivity may have changed to or from exclusive */
		pw_panel_update(ls->server);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_layer_surface *ls = wl_container_of(listener, ls, destroy);
	struct pw_output *output = ls->output;

	restore_focus(ls);

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->output_destroy.link);
	wl_list_remove(&ls->new_popup.link);
	wl_list_remove(&ls->link);
	ls->wlr_layer_surface->data = NULL; /* idle.c reads it */
	free(ls);

	/* The scene node is destroyed by wlroots with the surface; re-arrange so
	 * the exclusive zone is released. */
	if (output) {
		pw_layer_arrange(output);
	}
}

static void handle_output_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct pw_layer_surface *ls = wl_container_of(listener, ls, output_destroy);
	struct wlr_layer_surface_v1 *s = ls->wlr_layer_surface;

	/* Detach from the dying output (so destroy does not re-arrange it) and
	 * close the surface; clients may re-create it on another output. */
	wl_list_remove(&ls->link);
	wl_list_init(&ls->link);
	ls->output = NULL;
	s->output = NULL;
	wlr_layer_surface_v1_destroy(s);
}

static void handle_new_popup(struct wl_listener *listener, void *data)
{
	struct pw_layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct wlr_xdg_popup *xp = data;

	pw_popup_create(ls->server, xp, ls->scene_tree);
}

bool pw_layer_has_exclusive_focus(struct pw_server *server)
{
	struct wlr_surface *focused = server->seat ?
		server->seat->keyboard_state.focused_surface : NULL;
	if (!focused) {
		return false;
	}
	struct wlr_layer_surface_v1 *s =
		wlr_layer_surface_v1_try_from_wlr_surface(focused);
	return s && s->surface->mapped && s->current.keyboard_interactive
		== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data)
{
	struct pw_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *s = data;

	struct pw_output *output = NULL;
	if (s->output) {
		output = output_from_wlr(server, s->output);
	} else if (!wl_list_empty(&server->outputs)) {
		output = wl_container_of(server->outputs.next, output, link);
	}
	if (!output) {
		pw_log(WLR_ERROR, "layer surface '%s' has no output, closing",
			s->namespace ? s->namespace : "?");
		wlr_layer_surface_v1_destroy(s);
		return;
	}
	s->output = output->wlr_output;

	struct pw_layer_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) {
		wlr_layer_surface_v1_destroy(s);
		return;
	}
	ls->server = server;
	ls->output = output;
	ls->wlr_layer_surface = s;

	ls->scene_layer = wlr_scene_layer_surface_v1_create(
		tree_for_layer(server, s->pending.layer), s);
	if (!ls->scene_layer) {
		free(ls);
		wlr_layer_surface_v1_destroy(s);
		return;
	}
	ls->scene_tree = ls->scene_layer->tree;
	s->data = ls;

	wl_list_insert(&output->layers[s->pending.layer], &ls->link);

	ls->map.notify = handle_map;
	wl_signal_add(&s->surface->events.map, &ls->map);
	ls->unmap.notify = handle_unmap;
	wl_signal_add(&s->surface->events.unmap, &ls->unmap);
	ls->commit.notify = handle_commit;
	wl_signal_add(&s->surface->events.commit, &ls->commit);
	ls->destroy.notify = handle_destroy;
	wl_signal_add(&s->events.destroy, &ls->destroy);
	ls->new_popup.notify = handle_new_popup;
	wl_signal_add(&s->events.new_popup, &ls->new_popup);
	ls->output_destroy.notify = handle_output_destroy;
	wl_signal_add(&output->wlr_output->events.destroy, &ls->output_destroy);
}

void pw_layer_init(struct pw_server *server)
{
	server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
	if (!server->layer_shell) {
		pw_log(WLR_ERROR, "failed to create layer shell");
		return;
	}
	server->new_layer_surface.notify = handle_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}
