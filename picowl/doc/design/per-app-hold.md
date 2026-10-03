# Per-app tap-and-hold setting

**Status:** implemented.

Implementation: `src/picowl.h` (enums and structures), `src/config.c` (parser and lookup), `src/touchhold.h/.c` (pw_touchhold_set_params), `src/input.c` (hold_identity and per-app binding).

Adds per-`app_id` (xdg toplevels) and per-namespace (layer-shell) overrides of the `[touch]` tap-and-hold keys. The effective setting is chosen at touch-down from the surface under the finger and stays fixed until lift. It needs no protocol, no new source file and no change to `view.c`.

## 1. Problem

- picowl converts touch to pointer events and advertises only `WL_SEAT_CAPABILITY_POINTER` (`src/input.c:102-110`). Tap-and-hold is the pure state machine in `src/touchhold.c`. It is configured once at startup from `[touch]` (`src/input.c:735-737`), and that one configuration applies to every surface.
- In the default `right-click` mode, the left press is held back until the finger lifts, moves more than `slop_px`, or reaches `hold_ms` (`src/touchhold.h:20-35`, `src/touchhold.c:58-82`, `:102-123`). At `hold_ms` the state machine sends a `BTN_RIGHT` click and drops everything until lift (`pw_touchhold_tick`, `src/touchhold.c:102-123`, `src/input.c:514-520`).
- For the media player (`doc/mediaplayer-integration.md:91-93`, work item 11 at `:137`), the default mode causes three problems:
  - **Seek bar.** A press, pause and drag (scrubbing) turns into a right click at 900 ms, and the drag is lost because the state machine is in TRIGGERED and drops all events (`src/touchhold.c:79-80`).
  - **Latency.** Every tap is delivered only at lift (`src/touchhold.c:94-95`), so the OSD cannot react to touch-down.
  - **Long press.** The player wants to time its own long press (for seeking, and the long-press menu in the player design). picowl's fixed 300/900 ms thresholds and its animation get in the way.
- `hold_action = none` already gives the right behaviour: an immediate `BTN_LEFT` press at down, motion forwarded, release at lift, and no animation (`src/touchhold.c:48-51`). Today it is global, though, and the panel needs `right-click` for its widget menus (`doc/panel.md:17-18`).

## 2. Design

### 2.1 Config

There are two new section families. They use the same keys as `[touch]`, so there is nothing new to learn.

```ini
[touch]                  # global defaults, unchanged
hold_action = right-click
hold_delay_ms = 300
hold_ms = 900
slop_px = 8

[app.mediaplayer]        # xdg_toplevel app_id, exact, case-sensitive
hold_action = none

[app.org.example.Viewer] # dots in app_id are fine: everything after "app."
hold_ms = 1200           # other keys inherited from [touch]

[layer.osk]              # zwlr_layer_surface_v1 namespace, exact
slop_px = 16
```

| Key (in `[app.<app_id>]` / `[layer.<namespace>]`) | Default | Range | Meaning |
|---|---|---|---|
| `hold_action` | from `[touch]` | `right-click`, `none` | Same parser as `[touch]` (`parse_hold_action`, `src/config.c:102-113`) |
| `hold_delay_ms` | from `[touch]` | int | Delay before the animation starts |
| `hold_ms` | from `[touch]` | > `hold_delay_ms` | Hold time, measured from touch-down, until the right click |
| `slop_px` | from `[touch]` | 0..64 | Movement tolerance |

Rules:
- **Inheritance.** A key left out of an `[app.*]` or `[layer.*]` section takes its value from `[touch]`. Unset keys are filled in after the whole file is parsed, so the order of sections in the file does not matter.
- **Matching.** Names match exactly and are case-sensitive, like section names today (`src/config.c:254-262`). There is no wildcard: `[touch]` is the fallback. `[app.]` and `[layer.]` with an empty name are logged and ignored.
- **Separate namespaces.** `[app.panel]` never matches a layer surface whose namespace is `panel`, and the reverse also holds.
- **Repeated sections.** A section that appears twice merges into one rule, and later keys win.
- **Values.** No new `hold_action` values. `none` already means "left-button hold": press at down, then motion, then release at lift. That is everything a client needs to time its own long press. Raw `wl_touch` is rejected (see §7).

### 2.2 Data structures (`src/picowl.h`)

```c
enum pw_rule_kind { PW_RULE_APP, PW_RULE_LAYER };

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
};

/* in struct pw_config (src/picowl.h:99-148), next to the [touch] fields */
	struct wl_list app_rules;     /* struct pw_app_rule */

/* Effective hold parameters for a surface identity. name NULL or no
 * matching rule -> the [touch] globals. Returns true if a rule matched. */
bool pw_config_hold(const struct pw_config *c, enum pw_rule_kind kind,
	const char *name, struct pw_hold_params *out);
```

- **Why a generic `pw_app_rule`.** The per-app buffer budget plan (`doc/mediaplayer-integration.md:55`) can add its own fields and `PW_*_SET_*` bits to the same struct and the same `[app.*]` section (§8).
- **Global fields stay.** The existing `hold_*` and `slop_px` fields of `struct pw_config` (`src/picowl.h:108-113`) are kept, so the existing tests and `input.c:735` need no change.

### 2.3 State machine (`src/touchhold.h` / `.c`)

Add one pure function. `pw_touchhold_init` becomes "set the parameters, then zero the state".

```c
/* Replace thresholds/action. Same clamping as init (delay<0 -> 0,
 * hold<delay -> delay, slop<0 -> 0). Not IDLE -> cancel first; returns the
 * cancel actions (STOP_ANIMATION or SEND_LEFT_RELEASE), else 0. */
unsigned pw_touchhold_set_params(struct pw_touchhold *th,
	enum pw_th_hold_action action, int delay_ms, int hold_ms, int slop_px);
```

- **Parameters are fixed for the whole gesture.** They are set only before `pw_touchhold_down()`, and `tick`, `motion` and `up` read the values stored in `th` (`src/touchhold.c:58-131`). A later `set_app_id`, a refocus or a raise therefore cannot change a gesture in flight.
- **Why cancel instead of mutating.** Changing `hold_ms` in PENDING would move `next_deadline` (`src/touchhold.c:125-131`) under a timer that is already armed (`src/input.c:478-493`).
- **`th_exec` ordering.** `th_exec` runs actions in a fixed order (`src/input.c:495-528`). If the release from cancel were ORed into the same mask as the press from down, the press would go out first. The caller therefore runs the result of `set_params` through `th_exec` on its own. The existing `pw_touchhold_down` already ORs cancel and down actions together (`src/touchhold.c:41-53`). input.c avoids hitting that because it ignores a second down while a touch is tracked (`src/input.c:552-553`).

### 2.4 Binding: which surface decides

The surface **under the touch-down point** decides, not `server->focused_view`:
- That surface receives the pointer enter and the implicit grab (`src/input.c:563-571`).
- A hold on the panel or the on-screen keyboard while the player is focused must still follow the panel's or keyboard's own rule.

A new static helper in `src/input.c` resolves the surface:

```c
/* Rule identity for a touched surface: toplevel app_id or layer namespace.
 * Walks subsurfaces (wlr_surface_get_root_surface only follows subsurfaces,
 * wlroots types/wlr_compositor.c:994-1000) and popup parents. */
static const char *hold_identity(struct wlr_surface *s, enum pw_rule_kind *kind)
{
	struct wlr_surface *root = wlr_surface_get_root_surface(s);
	for (int depth = 0; depth < 16; depth++) {
		struct wlr_xdg_toplevel *tl = wlr_xdg_toplevel_try_from_wlr_surface(root);
		if (tl) { *kind = PW_RULE_APP; return tl->app_id; }       /* may be NULL */
		struct wlr_xdg_popup *pp = wlr_xdg_popup_try_from_wlr_surface(root);
		if (pp) {
			if (!pp->parent) return NULL;
			root = wlr_surface_get_root_surface(pp->parent);
			continue;
		}
		struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(root);
		if (ls) { *kind = PW_RULE_LAYER; return ls->namespace; }
		return NULL;                                               /* lock surface, unknown role */
	}
	return NULL;
}
```

wlroots 0.19 APIs used:

| API | Header |
|---|---|
| `wlr_xdg_toplevel_try_from_wlr_surface` | `include/wlr/types/wlr_xdg_shell.h:548` |
| `wlr_xdg_popup_try_from_wlr_surface` | `include/wlr/types/wlr_xdg_shell.h:557` |
| `wlr_xdg_popup.parent` | `include/wlr/types/wlr_xdg_shell.h:100` |
| `wlr_xdg_toplevel.app_id` | `include/wlr/types/wlr_xdg_shell.h:207` |
| `wlr_layer_surface_v1_try_from_wlr_surface` | `include/wlr/types/wlr_layer_shell_v1.h:147` |
| `wlr_layer_surface_v1.namespace` | `include/wlr/types/wlr_layer_shell_v1.h:89` |
| `wlr_surface_get_root_surface` | `include/wlr/types/wlr_compositor.h:368` |

Popups:
- Popups of layer surfaces get `parent = layer->surface` in `get_popup` (wlroots `types/wlr_layer_shell_v1.c:255`). A panel menu therefore resolves to the panel's namespace.
- An app's context-menu popup resolves to the app's `app_id`. This is the same parent walk picowl already uses for placement in `popup_root` (`src/view.c:399-423`).
- The depth cap of 16 only guards against a malformed chain.

**No `view.c` change.** The `app_id` is read from `wlr_xdg_toplevel` at the moment of use and is not cached in `pw_view` (`src/picowl.h:188-207` has no such field). Clients often call `set_app_id` after `get_toplevel` or after map; wlroots strdups it and emits `set_app_id` (wlroots `types/xdg_shell/wlr_xdg_toplevel.c:226-241`). Reading it at use time means any change applies from the next touch-down. `view_set_app_id` (`src/view.c:344-349`) stays as it is.

### 2.5 Interaction with existing state machines

| Machine | Interaction |
|---|---|
| dim/blank (`power.c`, `dim.c`) | None. A waking touch is dropped before any hold logic (`src/input.c:551-557`), so no lookup happens while blanked. |
| hold cursor (`cursor.c`) | `none` never starts the animation (`src/touchhold.c:48-51`). A per-app `hold_delay_ms` moves the animation start. |
| mouse/pointer devices | Unaffected. `cursor_handle_button` (`src/input.c:356-383`) never uses tap-and-hold. |
| multi-touch | Unchanged. Only the first touch point is tracked (`src/input.c:552-553`). |
| idle-inhibit, zerocopy | None. |

## 3. Code changes

| File | Change |
|---|---|
| `src/picowl.h` | Add `enum pw_rule_kind`, `struct pw_hold_params`, `PW_HOLD_SET_*`, `struct pw_app_rule`, `app_rules` in `struct pw_config`, and the `pw_config_hold()` prototype next to `pw_config_rot_mode` (`:448`). |
| `src/config.c` | See the list below the table. |
| `src/touchhold.h/.c` | Add `pw_touchhold_set_params()`. `pw_touchhold_init` (`touchhold.c:4-19`) calls it and then resets state and the down point. Update the header comment (`touchhold.h:1-38`). |
| `src/input.c` | Add `hold_identity()`. Change `touch_handle_down` (`:545-578`) as shown in the snippet after the list. Startup init at `:735-737` is unchanged. |
| `src/view.c`, `src/layer.c` | No change. |
| `data/picowl.ini.example` | Add a commented `[app.mediaplayer]` example after `[touch]` (`:40-48`). |
| `README.md` (`:190-197`), `doc/cursors.md` (`:62-69`) | Add a short "per-app overrides" paragraph and table. |
| `tests/test-config.c`, `tests/test-config.ini`, `tests/test-touchhold.c` | See §6. `tests/meson.build` needs no change: `config.c` and `touchhold.c` are already linked (`tests/meson.build:1-10,49-54`). |

`src/config.c` changes:
- `pw_config_default` (`:170-214`): `wl_list_init(&c->app_rules)`.
- Section header (`:254-262`): reset a local `struct pw_app_rule *cur_rule = NULL`.
- New `else if` before the "Unknown config section" fallback (`:560-561`):
  - `strncmp(section, "app.", 4)` / `strncmp(section, "layer.", 6)`.
  - The rule is created lazily by `find_or_add_rule(c, kind, name)`, which merges repeated sections.
  - The keys are parsed with the same helpers as `[touch]` (`:374-389`), and each sets its `PW_HOLD_SET_*` bit.
  - An unknown key is logged at `WLR_INFO`. `[touch]` ignores unknown keys silently today.
  - The `app.` and `layer.` prefixes cannot collide with `power.` (`:516`).
- After the global `[touch]` validation (`:583-594`), a new `resolve_rules(c)`:
  - fills the unset fields from the globals;
  - applies the same checks (`hold_ms > hold_delay_ms`, `slop_px` 0..64);
  - on failure logs `WLR_ERROR` naming the section, and reverts that rule's timings to the globals.
- `pw_config_free` (`:649-690`): free each rule's `name` and the rule.
- New `pw_config_hold()`: a linear exact-match scan, modelled on `pw_config_rot_mode` (`:692-715`) without the `*` step.

`touch_handle_down` change, just before `pw_touchhold_down` at `:575`:

```c
struct pw_hold_params hp;
enum pw_rule_kind kind = PW_RULE_APP;
const char *id = s ? hold_identity(s, &kind) : NULL;
bool hit = pw_config_hold(server->config, kind, id, &hp);
th_exec(pw_touchhold_set_params(&st.th, (enum pw_th_hold_action)hp.action,
	hp.delay_ms, hp.hold_ms, hp.slop_px), ev->time_msec);
if (hit)
	pw_log(WLR_DEBUG, "hold: %s '%s' -> %s", kind == PW_RULE_APP ? "app" : "layer",
		id, hp.action == PW_HOLD_NONE ? "none" : "right-click");
```

Size: about 90 lines in `config.c`, 15 in `touchhold.c`, 35 in `input.c` [est].

## 4. Fallbacks and failure modes

| Case | Behaviour |
|---|---|
| No surface under the touch (background) | `[touch]` globals. Pointer focus is cleared as today (`src/input.c:572-574`). |
| `app_id` NULL (not yet set) or no rule matches | `[touch]` globals |
| App sets or changes `app_id` at runtime | Applies from the next touch-down. The current gesture keeps its parameters. |
| Session-lock surface, unknown role, orphan popup (`parent == NULL`) | `[touch]` globals |
| Surface destroyed mid-gesture | Unchanged from today. The parameters are stored in `st.th`, not looked up again. |
| Invalid `hold_action` in `[app.*]` | `WLR_ERROR` log; the value is inherited from `[touch]` |
| Bad timings in `[app.*]` | `WLR_ERROR` log; that rule's timings revert to `[touch]` |
| `calloc`/`strdup` fails while parsing | Rule dropped, same as the existing list sections (`src/config.c:412-424`) |
| Config reload | None exists today. Config is loaded once in `main.c:72`, and nothing in `src/` handles SIGHUP. A future reload would just work, because the lookup reads `server->config` at every down. |
| A client spoofs `app_id = mediaplayer` | It only changes hold behaviour on its own surfaces. Harmless. |

## 5. Memory and CPU cost

| Item | Cost |
|---|---|
| RAM per rule | About 36 bytes for the struct on ARM32, plus the name and malloc overhead. Under 100 B per rule [est]. |
| RAM at runtime | No allocation; one `struct pw_hold_params` on the stack per down |
| CPU per touch-down | 1–4 role checks (pointer compares inside wlroots) and N `strcmp`, N = number of rules (typically under 5). Microseconds on a 206 MHz SA-1110 [est]. |
| CPU per motion or tick | Zero. Nothing changes on these paths. |
| Code | +1–2 KiB of `.text` [est]. No FPU, no `memcpy`, no new dependency. |

## 6. Tests

### Headless/unit tests (CI, no `/dev/dri`)

`tests/test-touchhold.c`:
- `set_params(NONE)` in IDLE returns 0. The following `down` returns `L` immediately (no deadline), `motion` returns `M`, `up` returns `R`, and `tick` returns 0.
- `set_params` in PENDING returns 0 and leaves the state IDLE with deadline -1. In ANIMATING it returns `SO`. In DRAGGING it returns `R`. In TRIGGERED it returns 0.
- Clamping matches `init`: delay -5 → 0, hold < delay → delay, slop -1 → 0.
- Parameters are fixed for the gesture: `down` with 300/900, then `tick(1300)` returns `SA` and `tick(1900)` returns `SO|RC|SW` (STOP_ANIMATION too, since the state is ANIMATING). Then `set_params(…, 500, 1500)`, `down` at 2000, and `next_deadline` = 2500.

`tests/test-config.ini` additions:
- `[app.mediaplayer]` with `hold_action = none`, placed **before** `[touch]` to test order independence.
- `[app.org.example.Viewer]` with `hold_ms = 1200`.
- `[layer.panel]` with `slop_px = 4`.
- `[app.bad]` with `hold_action = left` and `hold_ms = 100` (≤ delay).
- An empty `[app.]`.
- `[app.mediaplayer]` a second time, with `slop_px = 12`.

`tests/test-config.c`:
- The default config has an empty `app_rules` list.
- `mediaplayer` resolves to `{NONE, 300, 900, 12}`. This checks merging, and inheritance of `[touch]` values that appear later in the file.
- `org.example.Viewer` resolves to `{RIGHT_CLICK, 300, 1200, 8}`.
- `pw_config_hold(PW_RULE_LAYER, "panel")` gives slop 4, and `PW_RULE_APP, "panel"` gives the globals with `false` returned.
- `bad` falls back to the global action and timings.
- A NULL name, or a name differing only in case (`MediaPlayer`), gives the globals and `false`.
- The existing `test_touch_cursor_parsing` (`tests/test-config.c:155-173`) is unchanged.

`hold_identity()` needs real wlroots surfaces and touch events. The headless backend has no input-device API in 0.19 (`include/wlr/backend/headless.h:19,25`), so it is covered on hardware only. The `smoke` and `rss` tests must keep passing unchanged.

### Hardware-only checklist

Run `picowl -d 2` with `[app.mediaplayer] hold_action = none`, and the player with `WAYLAND_DEBUG=1`:
- [ ] A hold on the video sends `wl_pointer.button` 272 pressed at touch-down, no 273 (`BTN_RIGHT`), and no hold animation. The log shows `hold: app 'mediaplayer' -> none`.
- [ ] Seek bar: press, wait 2 s, then drag. Motion is delivered throughout, with release at lift.
- [ ] While the player is focused, a hold on the panel still opens the widget menu (right click, animation shown).
- [ ] A GTK+2 app without a rule still gets a right click at 900 ms.
- [ ] A popup menu of an app with a rule follows the app's rule. A popup of the panel follows `[layer.<ns>]` or the globals.
- [ ] An app that sets `app_id` late, or changes it: the next touch uses the new rule.
- [ ] A waking touch while blanked is still dropped and starts no gesture.
- [ ] Rotated outputs (h3800/h3900 software rotation, MediaQ hardware rotation): `slop_px` behaves the same in both orientations, because the state machine sees layout coordinates (`src/input.c:589-590`).

## 7. Mediaplayer side

- Call `xdg_toplevel.set_app_id("mediaplayer")` before the first commit (`doc/mediaplayer-integration.md:24`). Under picowl the touch config works without that ordering, but the buffer-budget plan may need it.
- The player cannot query the mode, so it must handle **both** modes:
  - **`BTN_RIGHT` click** (default mode): treat it as a long press and open the fuller menu.
  - **`BTN_LEFT` held** with motion under the player's own slop for its long-press time: run its own long-press logic (seek, menu). In default mode press and release arrive together at lift, so this timer never fires.
  - **Drag on the seek bar:** follow the pointer from the press.
- Recommended packaging: ship the `[app.mediaplayer]` snippet below in the player's documentation for `/etc/picowl.ini`. picowl reads exactly one file (`$HOME/.config/picowl/picowl.ini`, otherwise `/etc/picowl.ini`; `src/config.c:223-236`) and has no include directory. The player cannot drop in a fragment of its own.
  ```ini
  [app.mediaplayer]
  hold_action = none
  ```
- On the hx4700 (VGA), choose the player's long-press slop in surface pixels. Under `right-click`, the user can raise `slop_px` for the player alone.
- Raw `wl_touch` is not offered. It would need a second input path through `src/input.c`, and the touch capability would have to be advertised (`:102-110`). In `none` mode the pointer stream already carries press time, motion and release.

## 8. Open questions

1. **One `[app.*]` namespace for all per-app settings?** The buffer-budget plan should reuse `[app.<app_id>]` and `struct pw_app_rule` (for example a `budget_kb` key) rather than add a separate `[zerocopy.apps]` table. That needs agreement between the two plans.
2. **Built-in default rule for `mediaplayer`?** That would mean zero setup for the player. Proposed answer: no, keep policy in config.
3. **Prefix or glob matching for `app_id`** (`org.example.*`)? Not needed now; it can be added later.
4. **Per-region override by client request** (a small protocol request, so a client disables hold only over the video area)? Not needed while the player implements long press itself in `none` mode.
5. **Config reload on SIGHUP?** This design is ready for one, but reload is a separate feature.

## 9. Effort estimate

| Part | Effort [est] |
|---|---|
| `touchhold.c/.h` `set_params` and tests | 1 h |
| `config.c` parser, resolution, lookup, free | 3 h |
| `config` tests and ini | 1.5 h |
| `input.c` `hold_identity` and the `touch_handle_down` change | 1 h |
| Docs (README, `cursors.md`, `picowl.ini.example`) | 1 h |
| Hardware checklist on one copy-type board and the h3900 | 2 h |
| **Total** | **about 1.5 days** |

## Implementation notes

- **Parser shape.** The parser keeps a local `cur_rule`, reset at each section header and created lazily on the first key line by `find_or_add_rule()`. The `[app.*]` and `[layer.*]` sections share one branch. The empty-name error is logged at the section header, not per key.
- **Rule validation.** Rule resolution is a loop at the end of `pw_config_load()` (there is no separate `resolve_rules()`), after the `[touch]` validation. It uses the same checks as `[touch]` (`hold_ms > hold_delay_ms`, `slop_px` 0..64). When they fail it reverts only the three timings to `[touch]` and keeps the rule's resolved `hold_action`. An invalid `hold_action` value is ignored and inherited. A negative `hold_delay_ms` is not rejected, as in `[touch]`. The state machine clamps it.
- **`pw_touchhold_init`** is now `state = IDLE`, then `pw_touchhold_set_params()`, then clearing the down point.
- `hold_identity()` and the `touch_handle_down` change follow the plan. `hold_identity()` is covered on hardware only.
- Tests: `tests/test-config.ini` (the plan's rules plus `nonebad`, and empty `[app.]`/`[layer.]`) and a second file, `tests/test-config-hold.ini`, with a non-default `[touch]` to check inheritance independent of section order and the unset-key versus bad-value distinction.
