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
#include <wlr/types/wlr_idle_inhibit_v1.h>
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

#include "rotate.h"
#include "copytype.h"
#include "copyrel.h"
#include "zerocopy.h"
#include "mem.h"
#include "powerprofile.h"

#define pw_log(level, ...) wlr_log(level, __VA_ARGS__)

struct pw_server;
struct pw_cursor;
struct wlr_linux_dmabuf_v1;
struct wlr_content_type_manager_v1;
struct wlr_tablet_manager_v2;
struct wlr_input_device;
struct wlr_swapchain;

/* ---- configuration ---------------------------------------------------- */

enum pw_action {
	PW_ACTION_SPAWN,
	PW_ACTION_CYCLE_VIEWS,
	PW_ACTION_CLOSE_VIEW,
	PW_ACTION_TOGGLE_BLANK,
	PW_ACTION_ROTATE,
	PW_ACTION_TOGGLE_PANEL,     /* config name "panel" */
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

/* Per-output rotation mode override ([output NAME] rotation_mode). */
struct pw_output_rotmode {
	struct wl_list link;       /* pw_config.rotation_modes */
	char *name;                /* output name or "*" */
	enum pw_rot_mode mode;
};

/* Per-output copy-type override ([output NAME] copy_type). */
struct pw_output_copyover {
	struct wl_list link;       /* pw_config.copy_overrides */
	char *name;                /* output name or "*" */
	enum pw_copy_override ov;
};

struct pw_autostart {
	struct wl_list link;       /* pw_config.autostart */
	char *command;
};

enum pw_hold_action {
	PW_HOLD_RIGHT_CLICK,       /* hold -> BTN_RIGHT click (default) */
	PW_HOLD_NONE,              /* no hold detection, immediate left press */
};

enum pw_rule_kind {
	PW_RULE_APP,               /* app_id (xdg_toplevel) */
	PW_RULE_LAYER,             /* namespace (zwlr_layer_surface_v1) */
};

struct pw_hold_params {
	enum pw_hold_action action;
	int delay_ms, hold_ms, slop_px;
};

#define PW_HOLD_SET_ACTION (1u << 0)
#define PW_HOLD_SET_DELAY  (1u << 1)
#define PW_HOLD_SET_HOLD   (1u << 2)
#define PW_HOLD_SET_SLOP   (1u << 3)

struct pw_app_rule {
	struct wl_list link;          /* pw_config.app_rules, file order */
	enum pw_rule_kind kind;
	char *name;                   /* app_id or layer namespace */
	unsigned set;                 /* PW_HOLD_SET_*: keys given in the section */
	struct pw_hold_params hold;   /* fully resolved after pw_config_load() */
	/* [app.*] only: picowl-buffer-v1 limits (zerocopy_* keys) */
	int zb_buffers;               /* -1 = [zerocopy] max_buffers_per_client */
	int zb_budget_kb;             /* -1 = zb_buffers x frame of the largest output */
	unsigned zb_pool;             /* 0 = no pool (default pool), else 1..zb_n_pools-1 */
	char *exe;                    /* realpath the client binary must have, or NULL */
};

struct pw_config {
	char *render_format_pref;  /* e.g. "RGB565", "XRGB8888"; NULL = RGB565 */
	uint32_t render_format;    /* DRM fourcc resolved from the string */
	struct wl_list transforms;  /* struct pw_output_transform */
	int idle_timeout_ms;       /* 0 = never blank */
	struct wl_list autostart;   /* struct pw_autostart */
	struct wl_list keybindings; /* struct pw_keybinding */
	float background[4];       /* RGBA, 0..1 */

	/* [touch] tap-and-hold. */
	enum pw_hold_action hold_action; /* default PW_HOLD_RIGHT_CLICK */
	int hold_delay_ms;         /* animation starts after this (default 300) */
	int hold_ms;               /* right click fires after this, measured from
	                            * touch-down (default 900) */
	int slop_px;               /* movement tolerance (default 8) */
	struct wl_list app_rules;   /* struct pw_app_rule */

	/* [cursor] */
	char *hold_animation;      /* path to PAM strip, or NULL = builtin */
	uint32_t cursor_fill;      /* builtin dot colour 0xRRGGBB (default 0x2050c0) */
	uint32_t cursor_outline;   /* builtin outline colour 0xRRGGBB (default 0xffffff) */
	int cursor_frame_ms;       /* animation frame interval (default 83, ~12 fps) */

	/* Rotation / zero-copy / memory (added by the contract item). */
	struct wl_list rotation_modes; /* struct pw_output_rotmode */
	struct wl_list copy_overrides; /* struct pw_output_copyover */
	bool zerocopy;             /* enable picowl-buffer-v1 + dmabuf (default true) */
	enum pw_caching_override caching_override; /* [zerocopy] caching (default auto) */
	bool single_buffer;        /* allow single-buffer clients on copy-type outputs (default true) */
	int zb_max_buffers;        /* buffers per client (default 3) */
	int zb_budget_kb;          /* pool of clients without an [app.*] pool (default 2048) */
	int zb_total_kb;           /* ceiling over all pools, 0 = none (default 0) */
	unsigned zb_n_pools;       /* 1 (default pool) + [app.*] rules with a pool */
	bool panel_autohide;       /* hide the panel while an app is fullscreen (default true) */
	int arena_max;             /* M_ARENA_MAX (default 1) */
	int trim_threshold_kb;     /* M_TRIM_THRESHOLD in kB (default 256) */
	int mmap_threshold_kb;     /* M_MMAP_THRESHOLD in kB (default 128) */
	int top_pad_kb;            /* M_TOP_PAD in kB (default 16) */
	bool trim_after_start;     /* malloc_trim() once after startup (default true) */

	/* [power]. idle_timeout_ms above is kept for backwards compatibility:
	 * if set and no [power.*] blank_after_s is configured it becomes the
	 * blank timeout of every profile (resolved by the config parser). */
	char *backlight;           /* NULL or "auto" = auto-select, else device name */
	int low_capacity;          /* LOW at battery capacity <= this % (default 15, 0..100) */
	int dim_level;             /* dimmed brightness, % of user level (default 30, 1..100) */
	int poll_s;                /* fallback sysfs re-read period, 0 = off (default 300) */
	struct pw_power_timing power[PW_PROFILE_COUNT]; /* indexed by enum pw_power_profile */
	int low_max_brightness_pct; /* brightness cap while LOW, % of max (default 40, 1..100) */

	/* [lease] */
	bool lease_enable;         /* offer wp_drm_lease_device_v1 (default true) */
	char *lease_allow;         /* comma-separated app_ids, "*" = any (default "mediaplayer") */

	/* [capture] */
	bool capture_enable;       /* offer zwlr_screencopy_manager_v1 (default false) */
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

	enum pw_rot_mode rot_mode;  /* resolved rotation mode (config) */
	enum wl_output_transform rotation; /* current logical rotation */
	bool hw_rotation;           /* rotation is done by the display hardware */
	bool copy_type;             /* output copies damage to device memory */
	char drm_driver[32];        /* DRM driver name, "" if not DRM */
	struct wlr_swapchain *copy_swapchain; /* persistent buffer for copy-type outputs */
	bool sb_off_logged, sb_degraded_logged; /* single-buffer diagnostics, once */
	struct wl_listener present;

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
	struct pw_cursor *hold_cursor; /* owned by cursor.c, NULL until pw_cursor_init */
	struct wl_list keyboards;  /* struct pw_keyboard */
	struct wlr_tablet_manager_v2 *tablet_mgr; /* owned by tablet.c state; NULL: no tablets */
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;

	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wlr_content_type_manager_v1 *content_type_mgr; /* hint is only logged */
	bool idle_inhibited;       /* last visibility result pushed by idle.c */
	struct wlr_output_power_manager_v1 *output_power_mgr;
	struct wl_event_source *idle_timer;
	bool blanked;
	void *power;               /* struct pw_power state, owned by power.c */
	void *lease;               /* struct pw_lease state, owned by lease.c; NULL: no leasing */

	struct wlr_linux_dmabuf_v1 *linux_dmabuf; /* hand-built feedback, see zerocopy.c */
	void *buffer_mgr;          /* picowl_buffer_manager_v1 state, owned by zerocopy.c */
	bool panel_hidden;         /* panel currently hidden (autohide) */
	bool panel_forced_visible; /* user toggled the panel visible */

	struct pw_view *focused_view;
	struct pw_config *config;  /* not owned */

	struct wl_listener new_output;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_layer_surface;
	struct wl_listener new_idle_inhibitor;
	struct wl_listener new_input;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
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

/* Remove the session listener and forget parked outputs. Called by
 * pw_server_finish() before the backend (and its session) is destroyed. */
void pw_output_finish(struct pw_server *server);

/* Blank (true) or unblank (false) all outputs by disabling/enabling them and
 * set server->blanked from the outputs' real state (a failed unblank leaves
 * it true). Called by idle.c, input.c (TOGGLE_BLANK) and the
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
 * are always sized to the usable area (maximized). Called by pw_server_init().
 * Returns false on failure (logged). */
bool pw_view_init(struct pw_server *server);

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
 * imrelay.c
 */

/* Create the text-input-v3 and input-method-v2 globals next to
 * zwp_virtual_keyboard_manager_v1, which stays as it is (GTK+2 applications
 * and on-screen keyboard function keys still need it). Failure is logged and
 * leaves the protocols unsupported. Called by pw_input_init() once the seat
 * exists. */
void pw_im_init(struct pw_server *server);

/* Remove every listener on the managers and on their objects: wlroots asserts
 * that none is left when it destroys them. Safe without a prior pw_im_init().
 * Called by pw_input_finish(). */
void pw_im_finish(struct pw_server *server);

/*
 * input.c
 */

/* Create seat, cursor (no drawn cursor), virtual keyboard manager, and the
 * new_input listener; handle keyboards, touch and pointer. Called by
 * pw_server_init(). Returns false on failure (logged). */
bool pw_input_init(struct pw_server *server);

/* Remove the listeners installed by pw_input_init() so the cursor and seat
 * can be destroyed. Called by pw_server_finish() before wlr_cursor_destroy. */
void pw_input_finish(struct pw_server *server);

/* Execute binding->action (spawn, cycle, close, blank, rotate, quit). Called
 * by input.c's key handler when a keybinding matches; also usable by tests. */
void pw_input_run_action(struct pw_server *server, const struct pw_keybinding *binding);

/* Surface under layout point (surface-local coords in *sx, *sy), or NULL;
 * click-to-focus for a view or layer surface; activity report that returns
 * true when the display was blanked and the event must be dropped. For
 * tablet.c, implemented in input.c. */
struct wlr_surface *pw_input_surface_at(struct pw_server *server, double lx,
	double ly, double *sx, double *sy);
void pw_input_focus_surface(struct pw_server *server, struct wlr_surface *surface);
bool pw_input_activity(struct pw_server *server);

/*
 * tablet.c
 */

/* Create the tablet-v2 global and listen for tablet tool events on the
 * cursor. Called by pw_input_init() once the cursor exists. Failure is logged
 * and leaves tablets unsupported. */
void pw_tablet_init(struct pw_server *server);

/* Remove the cursor listeners; wlroots asserts they are gone when the cursor
 * is destroyed. Idempotent. Called by pw_input_finish(). */
void pw_tablet_finish(struct pw_server *server);

/* Create the tablet-v2 objects for a new tablet or tablet pad device. Return
 * false if that failed and the device must be ignored. Called by input.c. */
bool pw_tablet_add(struct pw_server *server, struct wlr_input_device *dev);
bool pw_tablet_pad_add(struct pw_server *server, struct wlr_input_device *dev);

/* Whether the output mapping of the tablet is right (input.c decides, see
 * tablet_check_mapping); events of an unusable tablet are dropped. */
void pw_tablet_set_usable(struct wlr_input_device *dev, bool usable);

/*
 * cursor.c (see cursor.h)
 */
#include "cursor.h"

#include "power.h"
#include "lease.h"

/*
 * idle.c
 */

/* Create the idle notifier and the idle-inhibit manager. Called by
 * pw_server_init(). */
void pw_idle_init(struct pw_server *server);

/* Report user activity: notify idle clients, rearm the timer, and unblank if
 * blanked. Called by input.c on every input event. */
void pw_idle_activity(struct pw_server *server);

/* Only notify ext-idle-notify clients of activity (no dim/blank handling);
 * input.c pairs it with pw_power_activity() to get the swallow result. */
void pw_idle_notify(struct pw_server *server);

/* Re-evaluate whether a visible surface holds an idle inhibitor and push the
 * result to the idle notifier and power.c on change. Cheap; called from
 * pw_panel_update() (every focus/stacking change) and from the inhibitor
 * create/destroy/map/unmap handlers. NULL-safe before pw_idle_init(). */
void pw_idle_inhibit_update(struct pw_server *server);

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

/*
 * Rotation, panel and config lookups (contract additions).
 */

/* Rotate output o to logical transform t (hardware if possible, else
 * software). Implemented in output.c. */
void pw_output_rotate(struct pw_output *o, enum wl_output_transform t);

/* Re-apply the touch calibration matrix of o after a rotation change.
 * Implemented in input.c. */
void pw_input_apply_rotation(struct pw_server *s, struct pw_output *o);

/* Unmap touch devices from output gone, which is about to be destroyed, and
 * remap them to another output if one remains. Implemented in input.c. */
void pw_input_output_removed(struct pw_server *s, struct pw_output *gone);

/* While the display is leased (no outputs), map every touch device to box,
 * given in panel-native mode pixels, and restore its default calibration
 * matrix, so touch coordinates are in the frame the lessee renders in. NULL
 * clears the mapping. Implemented in input.c, called by lease.c. */
void pw_input_lease_touch(struct pw_server *s, const struct wlr_box *box);

/* Recompute panel visibility (autohide). Implemented in layer.c. */
void pw_panel_update(struct pw_server *s);

/* Toggle forced panel visibility (PW_ACTION_TOGGLE_PANEL). layer.c. */
void pw_panel_toggle(struct pw_server *s);

/* Rotation mode for the named output (specific entry, then "*", else
 * PW_ROT_AUTO). Implemented in config.c. */
enum pw_rot_mode pw_config_rot_mode(const struct pw_config *c, const char *output_name);

/* Copy-type override for the named output (else PW_COPY_AUTO). config.c. */
enum pw_copy_override pw_config_copy_override(const struct pw_config *c, const char *output_name);

/* The [app.*] rule for an app_id (exact match), or NULL. NULL-safe. config.c */
const struct pw_app_rule *pw_config_app(const struct pw_config *c, const char *app_id);

/* Effective hold parameters for a surface identity. name NULL or no
 * matching rule -> the [touch] globals. Returns true if a rule matched. */
bool pw_config_hold(const struct pw_config *c, enum pw_rule_kind kind,
	const char *name, struct pw_hold_params *out);

#endif
