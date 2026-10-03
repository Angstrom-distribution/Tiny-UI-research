/*
 * picowl.h - shared contract between all picowl modules.
 *
 * Each module (config, output, view, layer, input, idle, server, main) owns
 * its own .c file and implements exactly the entry points declared here.
 * Modules communicate only through struct pw_server and these functions.
 */
#ifndef PICOWL_H
#define PICOWL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#define pw_log(level, ...) wlr_log(level, __VA_ARGS__)

struct pw_server;

/* ---- configuration ---------------------------------------------------- */

enum pw_action {
	PW_ACTION_SPAWN,
	PW_ACTION_CYCLE_VIEWS,
	PW_ACTION_CLOSE_VIEW,
	PW_ACTION_TOGGLE_BLANK,
	PW_ACTION_ROTATE,
	PW_ACTION_QUIT,
};

struct pw_keybinding {
	struct wl_list link;       /* pw_config.keybindings */
	char *key_name;            /* xkb keysym name, or NULL if keycode is used */
	uint32_t keysym;           /* cached xkb keysym for key_name */
	bool keysym_cached;
	uint32_t keycode;          /* linux evdev keycode, used when key_name == NULL */
	uint32_t modifiers;        /* WLR_MODIFIER_* mask that must be held */
	enum pw_action action;
	char *command;             /* for PW_ACTION_SPAWN, else NULL */
};

struct pw_output_transform {
	struct wl_list link;       /* pw_config.transforms */
	char *name;                /* output name, e.g. "DSI-1" or "*" */
	enum wl_output_transform transform;
};

struct pw_autostart {
	struct wl_list link;       /* pw_config.autostart */
	char *command;
};

struct pw_config {
	char *render_format_pref;  /* e.g. "RGB565", "XRGB8888"; NULL = RGB565 */
	uint32_t render_format;    /* DRM fourcc resolved from the string */
	struct wl_list transforms;  /* struct pw_output_transform */
	int idle_timeout_ms;       /* 0 = never blank */
	struct wl_list autostart;   /* struct pw_autostart */
	struct wl_list keybindings; /* struct pw_keybinding */
	float background[4];       /* RGBA, 0..1 */
};

/*
 * config.c
 */

/* Return a freshly allocated config with built-in defaults (RGB565, no
 * transforms, idle off, default keybindings, dark background). Never NULL.
 * Called by main() and by pw_config_load() as the starting point. */
struct pw_config *pw_config_default(void);

/* Parse the INI file at path on top of defaults. A missing file is not an
 * error (returns defaults); a malformed line is logged and skipped. path may
 * be NULL to search $XDG_CONFIG_HOME/picowl/picowl.ini then /etc/picowl.ini.
 * Called by main(). Returns NULL only on allocation failure. */
struct pw_config *pw_config_load(const char *path);

/* Free a config and all lists it owns. NULL-safe. Called by main(). */
void pw_config_free(struct pw_config *config);

/* ---- objects ---------------------------------------------------------- */

struct pw_output {
	struct wl_list link;       /* pw_server.outputs */
	struct pw_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *background;
	struct wlr_box full_area;   /* layout coords, whole output */
	struct wlr_box usable_area; /* layout coords, excluding exclusive zones */
	struct wl_list layers[4];   /* struct pw_layer_surface, per zwlr_layer_shell layer */

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct pw_view {
	struct wl_list link;       /* pw_server.views, front (focused) first */
	struct pw_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wlr_foreign_toplevel_handle_v1 *foreign_handle;
	bool mapped;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener foreign_request_activate;
	struct wl_listener foreign_request_close;
};

struct pw_layer_surface {
	struct wl_list link;       /* pw_output.layers[layer] */
	struct pw_server *server;
	struct pw_output *output;
	struct wlr_layer_surface_v1 *wlr_layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wlr_scene_tree *scene_tree;
	bool mapped;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener output_destroy;
	struct wl_listener new_popup;
};

struct pw_keyboard {
	struct wl_list link;       /* pw_server.keyboards */
	struct pw_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct pw_server {
	struct wl_display *display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_tree *layer_background; /* background + layer-shell BACKGROUND/BOTTOM */
	struct wlr_scene_tree *layer_apps;       /* xdg toplevels */
	struct wlr_scene_tree *layer_panel;      /* layer-shell TOP (panel) */
	struct wlr_scene_tree *layer_overlay;    /* layer-shell OVERLAY (on-screen keyboard) */
	struct wlr_scene_tree *layer_lock;       /* lock surfaces, above everything */
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;
	struct wl_list outputs;    /* struct pw_output */
	struct wl_list views;      /* struct pw_view, stacking order, front first */

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;

	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wl_list keyboards;  /* struct pw_keyboard */
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;

	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_output_power_manager_v1 *output_power_mgr;
	struct wl_event_source *idle_timer;
	bool blanked;

	struct pw_view *focused_view;
	struct pw_config *config;  /* not owned */

	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_layer_surface;
	struct wl_listener new_input;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener output_power_set_mode;
};

/*
 * output.c
 */

/* Register the backend new_output listener and the output power manager.
 * On new output: pick the render format per config (fallback to backend
 * default), apply the configured transform, enable, add to layout, create
 * scene output, commit only on frame with damage. Called by pw_server_init()
 * after the backend/renderer/scene exist. */
void pw_output_init(struct pw_server *server);

/* Blank (true) or unblank (false) all outputs by disabling/enabling them and
 * set server->blanked. Called by idle.c, input.c (TOGGLE_BLANK) and the
 * output-power-management handler. Idempotent. */
void pw_output_blank(struct pw_server *server, bool blank);

/* Fill *box with the area of the output not covered by exclusive layer-shell
 * surfaces, in layout coordinates. Called by view.c to place toplevels and by
 * layer.c after arranging. */
void pw_output_usable_area(struct pw_output *output, struct wlr_box *box);

/* Resize the background rect to the current output geometry and re-arrange
 * layers and views. Call after a transform/mode/scale change. */
void pw_output_update_geometry(struct pw_output *output);

/*
 * view.c
 */

/* Create the xdg-shell global, foreign-toplevel manager, and listeners. Apps
 * are always sized to the usable area (maximized). Called by pw_server_init(). */
void pw_view_init(struct pw_server *server);

/* Remove listeners that outlive their views (xdg popup). Called by
 * pw_server_finish() before the display is destroyed. */
void pw_view_finish(struct pw_server *server);

/* Raise view to the top of the apps tree, activate it, give it keyboard
 * focus, and update foreign-toplevel state. NULL view is a no-op. Called on
 * map, from input.c on cycle, and from foreign-toplevel activate requests. */
void pw_view_focus(struct pw_view *view);

/* Focus the next mapped view in stacking order (rotating the list). Called
 * by input.c for PW_ACTION_CYCLE_VIEWS. */
void pw_view_cycle(struct pw_server *server);

/* Send a close request to the focused view, if any. Called by input.c for
 * PW_ACTION_CLOSE_VIEW. */
void pw_view_close_focused(struct pw_server *server);

/* Re-size and re-position every mapped view to its output's usable area.
 * Called by output.c and layer.c when the usable area or transform changes. */
void pw_view_arrange_all(struct pw_server *server);

/* Create scene node and listeners for an xdg popup under parent_tree. Used
 * by view.c (xdg parents) and layer.c (layer-shell parents). */
void pw_popup_create(struct pw_server *server, struct wlr_xdg_popup *xp,
	struct wlr_scene_tree *parent_tree);

/*
 * layer.c
 */

/* Create the layer-shell global and new_layer_surface listener. Panel, OSK
 * and background clients are layer surfaces. Called by pw_server_init(). */
void pw_layer_init(struct pw_server *server);

/* Re-arrange all layer surfaces on output (top to bottom by layer),
 * recompute output->usable_area, then call pw_view_arrange_all(). Called
 * from layer commit/map/unmap handlers and when an output changes. */
void pw_layer_arrange(struct pw_output *output);

/* True if a mapped layer surface with exclusive keyboard interactivity
 * currently holds keyboard focus. */
bool pw_layer_has_exclusive_focus(struct pw_server *server);

/*
 * input.c
 */

/* Create seat, cursor (no drawn cursor), virtual keyboard manager, and the
 * new_input listener; handle keyboards, touch and pointer. Called by
 * pw_server_init(). */
void pw_input_init(struct pw_server *server);

/* Remove the listeners installed by pw_input_init() so the cursor and seat
 * can be destroyed. Called by pw_server_finish() before wlr_cursor_destroy. */
void pw_input_finish(struct pw_server *server);

/* Execute binding->action (spawn, cycle, close, blank, rotate, quit). Called
 * by input.c's key handler when a keybinding matches; also usable by tests. */
void pw_input_run_action(struct pw_server *server, const struct pw_keybinding *binding);

/*
 * idle.c
 */

/* Create the idle notifier and the idle timer from config->idle_timeout_ms.
 * Called by pw_server_init(). */
void pw_idle_init(struct pw_server *server);

/* Report user activity: notify idle clients, rearm the timer, and unblank if
 * blanked. Called by input.c on every input event. */
void pw_idle_activity(struct pw_server *server);

/*
 * server.c
 */

/* Initialise wl_display, backend, renderer (pixman), allocator, scene, and
 * call every module's *_init in order (output, view, layer, input, idle).
 * config must outlive the server. Returns false on failure (logged). Called
 * by main(). */
bool pw_server_init(struct pw_server *server, struct pw_config *config);

/* Start the backend, open the Wayland socket, export WAYLAND_DISPLAY, launch
 * config autostart commands, and run the event loop until quit. Returns the
 * process exit code. Called by main(). */
int pw_server_run(struct pw_server *server);

/* Destroy everything created by pw_server_init(), in reverse order. Called by
 * main() after pw_server_run() returns (also after a failed run). */
void pw_server_finish(struct pw_server *server);

/* Fork and exec "/bin/sh -c cmd" detached (double fork, no zombies). Returns
 * 0 on success, -1 on failure. Called by server.c (autostart) and input.c. */
int pw_spawn(const char *cmd);

#endif
