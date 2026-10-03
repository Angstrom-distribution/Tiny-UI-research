# Idle-inhibit

**Status:** implemented.

The code is in `src/idle.c` (inhibitors, visibility rule), `src/dim.c` (`pw_dim_set_inhibited`) and `src/power.c` (`pw_power_inhibit`); the user documentation is the "Idle inhibit" section of `doc/power.md`. Deviations from this plan are listed under [Implementation notes](#implementation-notes).

picowl offers `zwp_idle_inhibit_manager_v1`. An inhibitor counts only while its surface is visible: the focused toplevel, or a layer surface whose scene node is enabled. While at least one inhibitor counts, `power.c` stops its dim and blank timers. The power key and the output-power protocol can still blank the screen. A per-profile key, `[power.<profile>] inhibit`, lets the user disable inhibitors per profile (for example on LOW).

## Problem

- The player draws frames but gets no input during playback. picowl dims after `dim_after_s` and blanks after `blank_after_s` (BATTERY: 20 s / 60 s, `src/config.c:209`). A film on battery goes dark after one minute.
- `doc/mediaplayer-integration.md:97-100` (§2.8) and work items 5 and 6 (`:131-132`) ask picowl to "honour inhibitors from a visible surface in `power.c` (no dim, no blank)". Acceptance: "No dim or blank during playback with an inhibitor; normal timeouts after it is destroyed".
- Only picowl's own timer drives dim and blank. `dim_timer_cb` calls `pw_dim_tick` (`src/power.c:119-125`), and the only thing that restarts it is input through `pw_power_activity` (`src/power.c:225-235`, called from `activity()` in `src/input.c:95-100`). ext-idle-notify feeds nothing into `power.c`: `src/idle.c:13-18` only notifies clients. So `wlr_idle_notifier_v1_set_inhibited()` on its own would not stop dimming. The inhibit has to reach `dim.c`.
- Nothing about inhibit exists today. `dim.c` has no such concept (`src/dim.h:46-51`), and nothing in `src/` calls `wlr_idle_inhibit_v1_create`.

## Design

### Overview

```
zwp_idle_inhibitor_v1 (wlroots) ──new/destroy/map/unmap──┐
pw_panel_update() (focus/stacking/panel changed) ────────┤
                                                         ▼
                         idle.c: pw_idle_inhibit_update()  -- visibility rule
                               │ any visible?  (edge-triggered, cached)
               ┌───────────────┴─────────────────┐
               ▼                                 ▼
wlr_idle_notifier_v1_set_inhibited()   pw_power_inhibit(CLIENT, on)
 (ext-idle-notify clients)                       │  & config->power[profile].inhibit
                                                 ▼
                                   dim.c: pw_dim_set_inhibited()
                                   tick() -> 0, next_deadline() -> -1
```

### Protocol

- `zwp_idle_inhibit_manager_v1` version 1, from wlroots: `wlr_idle_inhibit_v1_create(display)` (`include/wlr/types/wlr_idle_inhibit_v1.h:59`). The global is created at version 1 (`types/wlr_idle_inhibit_v1.c:154-156`).
- wlroots compiles the protocol code itself (`types/wlr_idle_inhibit_v1.c:8`, `protocol/meson.build:49`), and the public header needs only `wayland-server-core.h` (`wlr_idle_inhibit_v1.h:12`). The rule in `meson.build:34-38` therefore applies: no XML and no generated header in picowl.
- No new picowl protocol and no XML.

### Visibility rule

The wlroots header says inhibitors "should only be in effect, while this surface is visible" (`wlr_idle_inhibit_v1.h:20-21`). picowl maximizes every toplevel to the usable area (`src/view.c:49-68`). `pw_view_focus` raises the focused view to the top and to the front of `server->views` (`src/view.c:116-119`). Toplevels cannot be minimized. So among toplevels, only `server->focused_view` is visible. Occlusion is not computed; this stacking rule replaces it.

The rule starts from the inhibitor's surface. It walks subsurfaces with `wlr_surface_get_root_surface` (`types/wlr_compositor.c:994-1000`, subsurfaces only) and popups with `wlr_xdg_popup.parent` (`include/wlr/types/wlr_xdg_shell.h`, `struct wlr_xdg_popup`), at most 8 levels.

| Root surface role | Counts when |
|---|---|
| xdg toplevel | `root->mapped` and it is `server->focused_view->xdg_toplevel` |
| xdg popup | the owner at the end of its parent chain counts |
| layer surface, TOP or OVERLAY | `root->mapped` and `wlr_scene_node_coords(&ls->scene_tree->node, …)` returns true, meaning the node and all its ancestors are enabled (`include/wlr/types/wlr_scene.h:312-317`). While autohide hides the panel, `layer_panel` is disabled (`src/layer.c:161`), so its surfaces don't count |
| layer surface, BACKGROUND or BOTTOM | mapped and `server->focused_view == NULL` (otherwise it is under a maximized app) |
| anything else (cursor, drag icon, role-less) | never |

Rules for the other states:

| Situation | Behaviour |
|---|---|
| BLANKED (idle timeout) | An inhibitor never unblanks. The screen stays blank until input or the power key. `dim.c` has no deadline while BLANKED anyway (`src/dim.c:100-101`) |
| Power key (`PW_ACTION_TOGGLE_BLANK`, `src/input.c:192-194`) | Always wins. `pw_dim_force_blank` ignores inhibit. On unblank the inhibit is still in force, so no timer starts |
| Output-power protocol off (`src/output.c:540-558`) | Same as the power key: it goes through `sync_blanked` (`src/power.c:214-223`) |
| Input while BLANKED and inhibited | ACTIVE plus the swallowed first event, as today (`src/dim.c:27-32`). The screen then stays on |
| Inhibitor appears while DIMMED | DIMMED becomes ACTIVE and the backlight is restored (UNDIM). Clients can undim but never unblank |
| Inhibit released | Timers restart from the moment of release (`last_activity = now`), so the screen doesn't blank right after a 2 h film |
| Profile change while inhibited | The new profile's `inhibit` key is applied. A profile with `inhibit = no` releases the inhibit with the timers restarted from now |
| LOW profile | Inhibitors are honoured by default. The LOW brightness cap (`src/power.c:53-63`) still applies; that cap is the battery saving |
| No usable backlight | `dim_ms` is 0 (`src/power.c:40-45`). The inhibit prevents blanking only |
| ext-idle-notify | `wlr_idle_notifier_v1_set_inhibited` gets the raw visibility result, not the profile-gated one; see the note below |

The "raw visibility" choice follows the header's wording, "because a visible client is using the idle-inhibit protocol" (`include/wlr/types/wlr_idle_notify_v1.h:36-43`). Notifications from version 2 `get_input_idle_notification` ignore inhibitors by protocol (`wayland-protocols/staging/ext-idle-notify/ext-idle-notify-v1.xml:64,97-98`). wlroots handles that through `obey_inhibitors` (`types/wlr_idle_notify_v1.c:84-95, 242-256`).

### dim.c API (pure, unit-testable)

```c
/* dim.h */
#include <stdbool.h>
struct pw_dim {
	enum pw_dim_state state;
	int64_t dim_ms, blank_ms, last_activity;
	bool inhibited;              /* new; pw_dim_init() clears it */
};
/* While inhibited, tick() returns 0 and next_deadline() returns -1; input,
 * force_blank and force_unblank behave as before.
 * on:  DIMMED -> ACTIVE, returns UNDIM; ACTIVE and BLANKED unchanged, 0.
 * off: if not BLANKED, last_activity = now, then tick(now) (normally 0).
 * Returns 0 if the value did not change. */
unsigned pw_dim_set_inhibited(struct pw_dim *d, bool on, int64_t now);
```

Two guards are added:
- `pw_dim_tick`: `if (d->state == PW_DIM_BLANKED || d->inhibited) return 0;` (`src/dim.c:42-43`)
- `pw_dim_next_deadline`: the same at `src/dim.c:100-101`

`pw_dim_set_timeouts` (`src/dim.c:71-77`) is unchanged: it calls tick, which returns 0 while inhibited. The new timeouts take effect on release.

### power.c API

```c
/* power.h */
enum {
	PW_INHIBIT_CLIENT  = 1 << 0,  /* idle-inhibit from a visible surface */
	PW_INHIBIT_SESSION = 1 << 1,  /* session inactive (VT switched away), optional */
};
/* Set or clear one inhibit reason. NULL-safe like the other pw_power_*. */
void pw_power_inhibit(struct pw_server *server, unsigned reason, bool on);
```

- `struct pw_power` (`src/power.c:16-26`) gains `unsigned inhibit_mask`.
- The effective value is `(mask & PW_INHIBIT_CLIENT && config->power[profile].inhibit) || (mask & ~PW_INHIBIT_CLIENT)`.
- `apply_inhibit(p)` computes the effective value. If it differs from `p->dim.inhibited`, it calls `sync_blanked(p)`, then `run_actions(p, pw_dim_set_inhibited(...))`, then `rearm(p)`. `rearm` disarms the timer when the deadline is -1 (`src/power.c:96-99`), so playback costs no wakeups.
- **`PW_INHIBIT_SESSION`** (optional, about 15 lines, for Path B step 1 in `doc/mediaplayer-integration.md` §3.1):
  - Today, while `mediaplayer-drm` owns the display on another VT, picowl gets no input, but its dim timer keeps running. It then writes the sysfs backlight under the other program's video (`src/power.c:65-79`).
  - Fix: listen to `server->session->events.active` (`include/wlr/backend/session.h:60`; `server->session` is filled at `src/server.c:197` and is NULL on headless). Set the bit while `!session->active`.

### Config keys

| Section | Key | Default | Values | Meaning |
|---|---|---|---|---|
| `[power.ac]` | `inhibit` | `yes` | yes/no | Honour client idle inhibitors on AC |
| `[power.battery]` | `inhibit` | `yes` | yes/no | The same on battery |
| `[power.low]` | `inhibit` | `yes` | yes/no | The same on LOW. Set `no` to let the screen dim and blank during playback when the battery is low |

- **Storage:** `bool inhibit` goes into `struct pw_power_timing` (`src/powerprofile.h:17-20`). The positional initializers at `src/config.c:208-210` must become `{ 120, 600, true }` and so on. Otherwise the field silently defaults to false; the config test catches that.
- **Strict parsing:** `parse_bool` returns false for invalid input (`src/config.c:115-125`). A typo would then disable inhibit without warning. Add `parse_bool_strict(const char *, bool *)`, which returns false on unknown text, so the caller can log at WLR_INFO and keep the default. This is the same pattern as the range checks at `src/config.c:534-557`.

### Data structures (idle.c)

```c
struct pw_inhibitor {                /* one per zwp_idle_inhibitor_v1 */
	struct pw_server *server;
	struct wlr_idle_inhibitor_v1 *wlr; /* wlr->data points back here */
	struct wl_listener destroy;      /* wlr->events.destroy */
	struct wl_listener map, unmap;   /* wlr->surface->events.map / unmap */
	bool dying;
};
```

- **No list of our own.** The update walks `mgr->inhibitors` (`wlr_idle_inhibit_v1.h:27,46`) and reads `wi->data` (`:52`). An entry with NULL `data` (calloc failed) is skipped.
- **The `dying` flag.** wlroots emits the inhibitor's `destroy` before it unlinks the inhibitor (`types/wlr_idle_inhibit_v1.c:35` vs `:40`). The handler sets `dying = true` and runs the update, which skips the entry. It then removes all three listeners (wlroots asserts the destroy list is empty, `:37`) and frees the struct. labwc uses `> 1` (`labwc/src/idle.c:37-39`) and dwl passes an `exclude` argument (`dwl/dwl.c:1052-1058`) for the same reason.
- **map/unmap listeners on the inhibitor's own surface.** These cover popups and subsurfaces without new hooks in `view.c`. `wlr_surface_unmap` clears `mapped` before it emits the event (`types/wlr_compositor.c:859-860`), so the update triggered by unmap already sees the surface as unmapped.
- **Stacking changes.** These arrive through `pw_panel_update()` (`src/layer.c:152-165`). It is already called on every focus or stacking change: `pw_view_focus` (`src/view.c:125`, which covers map and cycle), `view_unmap` (`:266`), `view_destroy` (`:303`), layer map and unmap (`src/layer.c:189,201`) and the panel toggle (`:170`).

## Code changes

| File | Change | Touches |
|---|---|---|
| `src/dim.h`, `src/dim.c` | Add the `inhibited` field, `pw_dim_set_inhibited`, and the two guards. About 20 lines [est] | `pw_dim_init` (`dim.c:4-10`), `pw_dim_tick` (`:38`), `pw_dim_next_deadline` (`:96`) |
| `src/power.h`, `src/power.c` | `PW_INHIBIT_*`, `inhibit_mask`, `apply_inhibit()`, `pw_power_inhibit()`. Call `apply_inhibit` in `profile_changed` before `pw_dim_set_timeouts`. Optional session listener added in `pw_power_init` and removed in `pw_power_finish`. About 40 lines [est] | `profile_changed` (`power.c:127-138`), `pw_power_init` (`:162-211`), `pw_power_finish` (`:262-281`) |
| `src/idle.c` | Manager creation, `new_inhibitor` handler, `struct pw_inhibitor`, the visibility walk, `pw_idle_inhibit_update()`, `pw_idle_finish()`. About 110 lines [est] | `pw_idle_init` (`idle.c:5-10`) |
| `src/picowl.h` | Server fields `struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr`, `struct wl_listener new_idle_inhibitor` and `bool idle_inhibited` (last pushed value). Include `<wlr/types/wlr_idle_inhibit_v1.h>` next to `:19`. Prototypes for `pw_idle_inhibit_update` and `pw_idle_finish` in the idle.c block (`:389-403`) | `struct pw_server` (`:240-288`) |
| `src/layer.c` | In `pw_panel_update`, call `pw_idle_inhibit_update(s)` as the first statement, before the early return at `:158-159`. In `handle_destroy`, set `ls->wlr_layer_surface->data = NULL` before `free(ls)` (`:243-244`), so the visibility walk never reads a freed pointer | `pw_panel_update`, `handle_destroy` |
| `src/server.c` | Call `pw_idle_finish(server)`, which drops `new_idle_inhibitor`, next to the other `listener_drop` calls (`:330-333`). It must run before `wl_display_destroy` (`:375`), because wlroots asserts that `new_inhibitor` has no listeners when the display is destroyed (`types/wlr_idle_inhibit_v1.c:120`). Per-inhibitor destroys run inside `wl_display_destroy_clients` (`:337`), before `pw_power_finish` (`:352`), while `power.c` is still alive. Init order is unchanged: `pw_idle_init` (`:272`) runs before `pw_power_init` (`:273`), and `pw_power_inhibit` is NULL-safe | `pw_server_finish` |
| `src/config.c`, `src/powerprofile.h` | The `inhibit` key in the `power.` branch (`config.c:533-557`), `parse_bool_strict`, and the updated initializers (`:208-210`) | `pw_config_default`, `pw_config_load` |
| `data/picowl.ini.example` | Commented `inhibit = yes` lines in `[power.ac]`, `[power.battery]` and `[power.low]` (`:131-150`) | |
| `doc/power.md` | New "Idle inhibit" section with the rule tables; `inhibit` rows in the three profile tables | |
| `README.md` | Add `zwp_idle_inhibit_manager_v1` to the protocol list. Fix `:291`, which names `org_kde_kwin_idle_notify`; picowl actually offers `ext_idle_notifier_v1` (`src/idle.c:7`) | |
| `tests/…` | See [Tests](#tests) | |

**Visibility walk.** Every function used here exists in wlroots 0.19: `wlr_surface_get_root_surface` (`wlr_compositor.h:368`), `wlr_xdg_popup_try_from_wlr_surface` (`wlr_xdg_shell.h:557`), `wlr_xdg_toplevel_try_from_wlr_surface` (`:548`), `wlr_layer_surface_v1_try_from_wlr_surface` (`wlr_layer_shell_v1.h:147`) and `wlr_scene_node_coords` (`wlr_scene.h:317`).

```c
static bool inhibitor_counts(struct pw_server *s, struct wlr_surface *surf)
{
	for (int depth = 0; surf && depth < 8; depth++) {
		struct wlr_surface *root = wlr_surface_get_root_surface(surf);
		if (!root->mapped)
			return false;
		struct wlr_xdg_popup *pp = wlr_xdg_popup_try_from_wlr_surface(root);
		if (pp) { surf = pp->parent; continue; }
		struct wlr_xdg_toplevel *tl = wlr_xdg_toplevel_try_from_wlr_surface(root);
		if (tl)
			return s->focused_view && s->focused_view->xdg_toplevel == tl;
		struct wlr_layer_surface_v1 *l = wlr_layer_surface_v1_try_from_wlr_surface(root);
		struct pw_layer_surface *ls = l ? l->data : NULL;  /* layer.c:325 */
		if (!ls)
			return false;
		if (l->current.layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
			return !s->focused_view;
		int x, y;
		return wlr_scene_node_coords(&ls->scene_tree->node, &x, &y);
	}
	return false;
}
```

**Update function.** `pw_idle_inhibit_update` sets `on = any counting inhibitor`. If `on == s->idle_inhibited` it returns. Otherwise it stores the new value, calls `wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, on)` when the notifier exists, calls `pw_power_inhibit(s, PW_INHIBIT_CLIENT, on)`, and logs `idle inhibit on (app_id X)` or `idle inhibit off` at WLR_INFO.

Unchanged: `src/input.c` (activity and swallow handling stay as they are) and `src/output.c`.

## Fallbacks and failure modes

| Case | Result |
|---|---|
| `wlr_idle_inhibit_v1_create` returns NULL | One WLR_ERROR log line and no global. The player sees no global and plays without it; the screen dims, as today |
| `calloc` of `pw_inhibitor` fails | The `new_inhibitor` signal cannot fail the request, so it is logged and the inhibitor is ignored (`data` stays NULL) |
| Client crashes or disconnects | wlroots destroys the resource, then the inhibitor, then the destroy event fires, and the timers restart from now |
| Surface destroyed before the inhibitor | wlroots destroys the inhibitor on surface destroy (`types/wlr_idle_inhibit_v1.c:52-57,95-96`). Same path as above |
| A hung but connected client keeps a visible inhibitor | The screen stays on. Remedies: the power key, or switching to another app. A cap on inhibit duration is an open question |
| Player sent to the background (alt-tab, panel launch) | The inhibit drops, and the normal timeouts start from that moment. Audio keeps playing; the player's frame callbacks stop when the outputs are disabled |
| Any client can keep the screen on while it is visible | This is the Wayland norm (cage, labwc, dwl). No allowlist |
| Panel applet "caffeine" style inhibitor | Doesn't count while autohide hides the panel. Documented as a limitation |
| VT switched away (Path B step 1) | Without `PW_INHIBIT_SESSION`, picowl's timer dims the backlight under the other VT's video. With it, the timers are frozen and restart from now when the session becomes active again |
| DRM lease (Path B step 2) | Out of scope here. A `PW_INHIBIT_LEASE` bit fits this reason mask; see the DRM-lease design |
| Config typo, e.g. `inhibit = ye` | WLR_INFO log; the default `yes` is kept |

## Memory/CPU cost on 64 MiB boards

- **Heap.** The wlroots manager is about 80 B, plus one `wl_global` [est]. Each inhibitor costs about 60 B in wlroots and 40 B in `pw_inhibitor` on ARM32 [est]. The player holds one at a time. The per-client `wl_resource` costs are negligible.
- **Code.** About 1.5-2.5 KiB of `.text` [est], with no new library. The `idle_inhibit` code is already in `libwlroots`.
- **CPU.** The update is O(number of inhibitors) and runs only on inhibitor create, destroy, map or unmap, or a focus/panel change; never per frame or per input event. There is no integer division and no floating point.
- **Saving.** While inhibited, the dim timer is disarmed (`src/power.c:96-99`), so the compositor has no idle wakeups during playback.

## Tests

### Unit and headless (CI, no `/dev/dri`)

1. **`tests/test-dim.c`** (already built, `tests/meson.build:88-91`). Cases, with `fresh()` = dim 100, blank 200:
   - inhibit at t=0 in ACTIVE returns 0. At t=10000: tick returns 0, `next_deadline` is -1, state is ACTIVE.
   - DIMMED, then inhibit: returns UNDIM, state is ACTIVE.
   - BLANKED, then inhibit: returns 0, state stays BLANKED. Then `force_unblank(t)`: returns UNBLANK, `next_deadline` is -1.
   - Release at t=5000: returns 0, `next_deadline` is 5100. At t=5100 tick returns DIM.
   - `force_blank` while inhibited returns BLANK. Activity while inhibited and BLANKED returns UNBLANK|SW, and `next_deadline` is -1.
   - `set_timeouts(50, 80)` while inhibited returns 0. Release at t: `next_deadline` is t+50.
   - Release while BLANKED returns 0, state stays BLANKED, `next_deadline` is -1.
   - Setting the same value twice returns 0.
2. **`tests/test-config.c` and `tests/test-config.ini`**:
   - defaults: `power[i].inhibit == true` for all three profiles (this catches the positional initializer);
   - `[power.battery] inhibit = no` parses to false;
   - `[power.low] inhibit = maybe` leaves the value true.
3. **`tests/power-e2e.sh`**, extended. It already runs headless picowl against a fake sysfs tree. Setup: AC profile, `dim_after_s = 1`.
   - Test client changes:
     - add `--inhibit`: bind `zwp_idle_inhibit_manager_v1` and create an inhibitor on the toplevel surface;
     - add `--linger S`: stay connected for S s after the first frame, and raise `alarm()` from 5 to S+5 (`tests/pw-test-client.c:351`);
     - generate the `idle-inhibit-unstable-v1` client header and code from `wl_proto_dir/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml` in `tests/meson.build`, as is already done for linux-dmabuf (`:32-38`).
   - **Case 3:** `--inhibit --linger 3`. Brightness is still 40 at 2.5 s. After the client exits it is still 40 at +0.5 s (timers restarted), then reaches 12 by +3 s.
   - **Case 4:** client A runs `--inhibit --linger 5`. 0.3 s later, client B runs `--linger 4`, maps and takes focus (`src/view.c:242`). Brightness reaches 12 while A is still connected (A is not visible).
   - **Case 5:** LOW tree with `[power.low] inhibit = no` and `dim_after_s = 1`: dims despite `--inhibit`.
   - The headless backend has no input devices, so input wake-up stays covered by `test-dim` only (as `doc/power.md` already says for dimming). Generous margins, because CI timing is noisy.
4. **`smoke` and `rss`** stay green. The rss ceiling must not move (`tests/meson.build:83-86`).

### Hardware checklist

Boards: h3870 (`pwm-backlight`), h5550 (on/off backlight: the blank-only path), hx4700 (BATTERY profile, no supply data). Run `picowl -d 3`.

1. Start playback: the log shows `idle inhibit on (app_id mediaplayer)`. There is no dim or blank for more than `blank_after_s` on BATTERY.
2. Pause: the log shows `idle inhibit off`. The screen dims `dim_after_s` after the pause, not right away.
3. Power key during playback blanks. The power key again unblanks, and the screen stays on with no dim.
4. A tap while blanked during playback is swallowed, and the screen stays on afterwards.
5. Alt-tab to another app during playback dims after `dim_after_s`. Going back to the player undims on the input event and holds again.
6. Plug and unplug AC during playback: the log shows `power profile A -> B` and the inhibit stays. With `[power.low] inhibit = no`, crossing `low_capacity` lets the screen dim.
7. On the h5550, playback longer than `blank_after_s` does not blank.
8. Kill the player with `kill -9`: the inhibit is released, and the screen dims after the normal timeout.
9. With `PW_INHIBIT_SESSION`: switch VT to `mediaplayer-drm` for more than `dim_after_s`. The backlight is not touched, and after the return the timers restart.

## Mediaplayer side

- Bind `zwp_idle_inhibit_manager_v1` v1 if it is advertised. If it is missing (older picowl, other compositors), log one line and continue.
- While playing video: `create_inhibitor(toplevel wl_surface)`. Use the root toplevel surface, not a video subsurface or a popup.
- Destroy the inhibitor on pause, stop, EOF, error and exit. Don't destroy it on backgrounding; picowl handles visibility.
- Audio-only playback should not hold an inhibitor, so the screen can blank and save power. The player may offer a switch for this.
- Don't fake input (for example through virtual-keyboard) to keep the screen awake. Don't use `zwlr_output_power_management_v1` for this either.
- Expect the first tap after a blank to be swallowed (`src/dim.c:27-32`), and expect frame callbacks to stop while blanked: pace from the audio clock (§2.6).
- **Path B:** before a VT switch, nothing extra is needed beyond the optional `PW_INHIBIT_SESSION` on the picowl side. For lease mode, see the DRM-lease design.

## Open questions

1. **UNDIM when an inhibitor appears while DIMMED.** Chosen: yes (a client may undim, never unblank). The alternative is to keep it dimmed until input.
2. **LOW default.** `yes`, or a three-way `inhibit = yes|no|dim` (allow dimming, prevent blanking)? The `dim` value costs about 10 lines in `dim.c` [est].
3. **Maximum inhibit duration.** For example `[power] inhibit_max_s`, as protection against hung clients. Not planned unless it is seen in the field.
4. **Panel-applet inhibitors** while the panel is auto-hidden. They could count while hidden, or a keybinding action `toggle_inhibit` could set a third reason bit (`PW_INHIBIT_USER`) instead.
5. **Whether to ship `PW_INHIBIT_SESSION` with this work** or with the Path B step 1 hardware checks (`doc/mediaplayer-integration.md:105-111`).
6. **Dead `server->idle_timer`.** It is never assigned in `src/`, only removed (`src/picowl.h:267`, `src/server.c:353-356`). It could be removed in the same change as cleanup.

## Effort estimate

| Part | Effort |
|---|---|
| `dim.c` and `test-dim` cases | 0.25 day [est] |
| `power.c` reason mask and profile key, config parser and test | 0.25 day [est] |
| `idle.c` inhibitor glue, `layer.c`/`server.c` hooks | 0.5 day [est] |
| Test client `--inhibit`/`--linger`, `power-e2e` cases 3-5 | 0.5 day [est] |
| Docs (`power.md`, README, ini example) | 0.1 day [est] |
| Optional `PW_INHIBIT_SESSION` | 0.1 day plus hardware VT test [est] |
| Hardware checklist on 3 boards | 0.5 day [est] |
| **Total** | **about 2-2.5 days [est]**, about 200 lines of C plus about 150 lines of tests [est] |

## Implementation notes

Deviations from the plan above. Line references in the plan are against the old commit; everything was re-located in the current code.

- **Strict bool parsing.** The current `src/config.c` already has `parse_bool_log()`, which returns false and keeps the old value on unknown text (the plan's `parse_bool` problem no longer exists). The `inhibit` key uses it, so no `parse_bool_strict` was added. It logs at WLR_ERROR, like the other boolean keys, not at WLR_INFO.
- **`pw_idle_inhibit_update()` runs at the end of `pw_panel_update()`**, not as its first statement. Called first, it would see the panel state from before the change, so a panel that autohide has just hidden would still count. It now runs after the panel node is enabled or disabled, on every call (also when the hidden state did not change).
- **No `pw_idle_finish()`.** The `new_idle_inhibitor` listener is dropped with `listener_drop()` in `pw_server_finish()` next to `new_layer_surface`, as the existing listeners are. Nothing else needed teardown.
- **No `dying` flag.** `struct pw_inhibitor` keeps a pointer to the wlroots inhibitor, and its destroy handler sets `wlr->data = NULL` before running the update. The walk skips entries with NULL `data`, which has the same effect as `dying` and also covers the calloc-failure case.
- **`PW_INHIBIT_SESSION` is included** (open question 5): `power.c` adds the listener on `server->session->events.active` when a session exists, and takes the initial state from `session->active`. It ignores the `inhibit` key. It cannot be tested headless; the hardware checklist item 9 covers it.
- **Open questions as chosen in the plan:** an inhibitor undims a DIMMED screen; LOW honours inhibitors by default (`yes|no` only, no `dim` value); no maximum inhibit duration; the dead `server->idle_timer` is left alone (it is not part of this feature).
- **Tests.** The `power-e2e` cases are numbered 4 to 6 (cases 1 to 3 were already there). They use `dim_after_s = 2`, not 1, so that a slow start cannot dim before the client has mapped. Case 5 starts client B after 3 s and first checks that the screen is still at the user level with A visible, so it fails if A's inhibitor is not counted at all. The test needs the test client, so `tests/meson.build` passes it to `power-e2e.sh`, and the test timeout is 90 s. `smoke.sh` also runs `--inhibit` and checks the two log lines.
