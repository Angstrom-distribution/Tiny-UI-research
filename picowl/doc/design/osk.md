# On-screen keyboard

**Status:** Part 1 (section 2) exists in the repository as the eleven patches (`oe/recipes-graphics/wvkbd/files/`, `subprojects/packagefiles/wvkbd/`, `subprojects/wvkbd.wrap`, `tests/patches-sync.sh`) and the recipe `oe/recipes-graphics/wvkbd/wvkbd-ipaq_git.bb`. It is not built into an image and not run on a board yet, and the recipe has not been parsed with bitbake. Part 3 (section 4) is implemented in `src/imrelay.c` without the keyboard grab and the popup hold rules. Part 2 (section 3) is implemented in `src/osk.c` and `src/oskstate.c`, with the panel hookup of section 3.5 still interim. What remains is the hardware checklist (section 6).

The OSK is wvkbd, rebuilt for the iPAQ as a patch series on a pinned upstream commit, packaged in picowl's OE layer (Part 1). picowl starts and supervises it, shows and hides it by signal from a keybinding or the panel, and keeps its own tap-and-hold out of the way (Part 2). An input-method relay that shows the keyboard automatically on text focus is Part 3, implemented apart from the keyboard grab and the popup hold rules.

wvkbd citations are to upstream commit e14b53a (v0.20-9, upstream master HEAD on 2026-10-04). picowl citations are to commit 3f74900.

## 1. Problem

### 1.1 picowl today
- **Typing already works:** picowl offers `zwp_virtual_keyboard_manager_v1` (`src/input.c:867`). A virtual keyboard joins the seat with the keymap the client uploads (`src/input.c:342`, `:359-364`). The active seat keyboard is switched on every key event (`src/input.c:272`), so mixing wvkbd and hardware keys is fine.
- **Why not `pw_spawn`:** it double-forks and the real process is reparented to init (`src/server.c`), so the pid is lost: picowl could not signal the keyboard, notice a crash or restart it. The keyboard is started by `src/osk.c` instead (section 3.2).
- **The `osk` action** (section 3.4) replaces `spawn pkill -RTMIN wvkbd` in a keybinding.
- **Namespace:** wvkbd's layer-shell namespace is `wvkbd` (`main.c:32`, used at `:813-814`), so the hold rule is `[layer.wvkbd]`, not `[layer.osk]`.
- **Tap-and-hold would delay every key:** in the default `right-click` mode the left press is held back until lift, `slop_px` or `hold_ms`. On a keyboard that means a key is pressed only when the finger lifts, key repeat can't start, and a long press becomes a right click. picowl therefore has a built-in `[layer.wvkbd]` rule with `hold_action = none` (section 3.3).

### 1.2 Upstream wvkbd on an iPAQ
| Behaviour | Upstream | Problem on the iPAQ |
|---|---|---|
| Build | Makefile, one binary per `LAYOUT` (`Makefile:3-6`, `config.mk:5`) | Our rule is meson |
| Pixels | `WL_SHM_FORMAT_ARGB8888`, `CAIRO_FORMAT_ARGB32`, stride `width*4` (`drw.c:237-272`) | picowl alpha-blends it on the CPU on every repaint, and it blocks direct scanout of the app below. Twice the memory of RGB565 |
| Size | `KBD_PIXEL_HEIGHT 250`, landscape 120 (`layout.mobintl.h:3-6`) | 250 px is 78 % of a 320 px portrait screen |
| Text | pangocairo, `"Sans 14"` (`Makefile:10`, `config.mobintl.h:4`) | fontconfig scan at start-up; pango shaping per label draw. GTK2 apps load the same libraries, but the OSK runs before and without them |
| Exclusive zone | on by default (`main.c:920`; `--non-exclusive` at `:1104-1105`; the zone is only requested when `keyboard.exclusive` is set, `:830-831`) | Every show and hide resizes the focused app and repaints the whole screen: slow on the MediaQ bus (~8 MB/s). picowl pans a tiled stack instead of resizing it (section 3.7) |
| Hide/show | `hide()` destroys the layer surface, the popup and both surfaces (`main.c:742-786`), and leaks the popup's viewport; `show()` creates them again | A configure round trip and two new shm buffers on every show |
| Key preview popup | an `xdg_popup` with its own two buffers (`main.c:630-650`), on by default (`:922`; upstream already has `--no-popup`) | A second surface to composite and ~2× the buffer memory |
| Signals | blocked only just before the main loop (the mask is built at `main.c:1282-1287`, after `show()` and the Wayland round trips) | A SIGUSR1/2 or SIGRTMIN that arrives during start-up kills the process (default action) |
| Damage | per-rectangle, with a back buffer (`drw.c:18-55`) | Already good; keep it |

## 2. Part 1: wvkbd-ipaq

### 2.1 Delivery
- **Patch series** on upstream e14b53a, generated with `git format-patch`. It lives in `picowl/oe/recipes-graphics/wvkbd/files/`, and a byte-identical copy in `picowl/subprojects/packagefiles/wvkbd/` so a meson wrap can build it for the tests. A `tests/patches-sync.sh` test compares the two copies.
- **Recipe** `picowl/oe/recipes-graphics/wvkbd/wvkbd-ipaq_git.bb`:
  - `SRC_URI = "git://github.com/jjsullivan5196/wvkbd.git;protocol=https;branch=master"` plus the patches; `SRCREV = "e14b53a..."` (full hash).
  - `S = "${UNPACKDIR}/${BP}"` per wrynose/blacksail conventions (as in `picowl_git.bb`).
  - `LICENSE = "GPL-3.0-only & MIT"` is a first guess: `LICENSE` is the GPLv3 text; `COPYING` says `os-compatibility.[ch]` are MIT (text in `COPYING_WESTON`) and everything else GPL v3, without saying "only" or "or later". Check the per-file headers before settling it; `shm_open.[ch]` carry no header. `LIC_FILES_CHKSUM` covers `LICENSE` and `COPYING_WESTON`.
  - `inherit meson pkgconfig`; `DEPENDS = "wayland wayland-native libxkbcommon cairo scdoc-native"`, plus `pango fontconfig` only for `text=pango`. Upstream links pangocairo unconditionally (`Makefile:10`), so a bitmap build has to remove the pango and fontconfig code paths first (patch 8). All protocol XMLs it needs are vendored in its `proto/` directory, so wayland-protocols is optional. `scdoc` builds the man page.
  - `picowl_git.bb` gets `RRECOMMENDS:${PN} += "wvkbd-ipaq"`, so a minimal image can leave it out.
- **Wrap** `picowl/subprojects/wvkbd.wrap` (git, pinned revision, `diff_files` = the series), used only by the `osk_tests` meson option; nothing from it is installed.

### 2.2 Patches
| # | Patch | Content |
|---|---|---|
| 1 | meson build | `meson.build` and `meson_options.txt`: `kbd_layout` (`mobintl`, `deskintl`, `ipaq`; meson reserves `layout`), `text` (`bitmap`/`pango`), `tests`. Generates the protocol code with `wayland-scanner`. Installs one binary named after the layout (`wvkbd-ipaq`) and the man page from the existing `wvkbd.1.scd` |
| 2 | start-up fixes | Block the handled signals at the top of `main()`, before any Wayland setup. Free the keymap string after upload |
| 3 | RGB565 | `WL_SHM_FORMAT_RGB565`, `CAIRO_FORMAT_RGB16_565`, stride `width*2` rounded to 4 bytes; fall back to `XRGB8888` if the compositor doesn't advertise RGB565. All colours opaque; the `--alpha` and `--bg-alpha` options are ignored in RGB565 mode |
| 4 | popup default | Upstream already has `--no-popup` and defaults to a popup. Keep that flag, add `--popup`, and let the `ipaq` layout default to no popup |
| 5 | keep the surface | `hide()` unmaps by attaching a NULL buffer and committing; `show()` re-attaches the display buffer. No surface, buffer or configure churn per toggle |
| 6 | label damage | Redraw only keys whose label or state changed (shift, layer switch), not the whole keyboard |
| 7 | `ipaq` layout | Portrait 240 px wide: 10 keys per row (24 px), 4 rows plus a function row, ~100 px total. Landscape 320 and VGA 480/640: same rows, wider keys, ~80 px (QVGA landscape) or ~160 px (VGA portrait). Layers: letters, shifted, numbers/symbols, a small Dutch/German/French accent layer. `--non-exclusive` is the default. Upstream's `--dock`, `--ratio`, `--width` and `--corner-radius` (added in e14b53a) change the geometry too: either honour them in the `ipaq` layout or document that it ignores them |
| 8 | text backends | One label-mask path for all text, with three backends, see section 2.3. 8a: the mask atlas and its blitter replace the pango drawing in `drw.c`; 8b: built-in bitmap fonts (default); 8c: pango as an option; 8d: the persistent mask cache. Replacing the pango calls (font setup at `main.c:1264`, layout drawing around `drw.c:281`) and the fontconfig lookup is more than one small patch |
| 9 | long press | Long press on a letter for its accented variants, at 500 ms by default (`--long-press MS`, 0 = off). It is shorter than picowl's `hold_ms` default (900 ms), but picowl leaves the OSK alone anyway (§3.3) |

**Text path decision.** Bitmap text is the default and comes first; pango stays available as `text=pango`; a cache of pre-rendered labels (section 2.3) lets a pango build avoid pango at run time after the first start. All three feed the same mask blitter, so the drawing code is written once.

### 2.3 Text backends and the label cache
- **Label masks:** every key label becomes a small coverage mask (1, 4 or 8 bits per pixel, rows padded to 4 bytes) plus its width, height and bearing. One blitter fills a mask with the key's colour into the RGB565 (or XRGB8888) buffer: 1 bpp masks are a plain masked fill, 4 and 8 bpp use an alpha lookup table, so there is no per-pixel division on a CPU without an FPU. Special icons (shift, backspace, enter) stay vector shapes drawn with cairo.
- **`text=bitmap` (default):** built-in public-domain X11 fixed fonts, 6x13 (QVGA) and 10x20 (VGA), converted at build time from the BDF files into a C table covering Latin-1 and the symbols the layout needs, as 1 bpp masks. A label glyph missing from the font is drawn as a box and logged once, never skipped silently. No pango, fontconfig or harfbuzz in the binary.
- **`text=pango` (option):** the same labels rendered with pango and cairo into 8 bpp masks (or 4 bpp when it is enough), once per label at start-up instead of on every draw. Start-up pays the fontconfig scan, so this is the backend that benefits from the cache.
- **`text=cache` and the cache file:** the pango backend can save every mask to a file and a later start maps it instead of rendering. The file is written once, atomically (temporary file, then rename), by whichever happens first: the first start (the visible layer is rendered synchronously, the other layers in idle time, then the file is written), the explicit command `wvkbd-ipaq --prerender`, which a package post-install or first-boot hook can run, or a build-time tool in the OE native build that generates the files for the standard geometries so the target never needs pango at all.
- **File format:** a small header (magic, format version, layout name and version, key geometry in pixels, font description and a hash of it, mask depth, label count, checksum), an index table sorted by label hash (offset, width, height, bearing), then the mask data, 4-byte aligned. The reader maps the file read-only, checks the header, the checksum and every offset against the file size before use, and treats anything wrong as a miss: it falls back to rendering and rewrites the file. Identical labels in different layers share one entry. Nibble packing is enough; compression would cost CPU for a few kilobytes.
- **Key and location:** one file per layout, geometry and font hash, in `--cache-dir` (default `/var/cache/wvkbd-ipaq`; `--no-cache` disables it). It is written once and never touched again, so it costs no flash wear during normal use.
- **Size [est]:** about 100 labels per layer at roughly 12x16 px and 4 bpp is about 10 KB per layer, so tens of kilobytes per geometry.

## 3. Part 2: picowl

### 3.1 Config
```ini
[osk]
# Command; empty disables picowl's OSK handling. Run directly (no shell
# double fork) so picowl knows its pid.
cmd = wvkbd-ipaq --hidden
# Restart after a crash: yes/no
restart = yes
```
- Defaults: `cmd` empty (no OSK), `restart = yes`. The example ini and README show the line above with `--hidden --auto`, which is what the board runs (`/usr/bin/wvkbd-ipaq --hidden --auto`).
- An empty `cmd` value switches the handling off. `[autostart]` keeps working for anything else, and the keyboard needs no entry there.
- There is no config reload, so the give-up state of section 3.2 ends with an `osk show` or `osk toggle` key.

### 3.2 Supervision (`src/osk.c`, `src/oskstate.c`)
- **Spawn:** one `fork()` + `execl("/bin/sh", "sh", "-c", "exec " cmd)`, so the pid is the OSK itself. Signal mask and dispositions are reset in the child as in `pw_spawn`, and `PR_SET_PDEATHSIG` (SIGTERM) stops the keyboard when picowl is killed. Nothing waits for the child, and no start-up signal is sent: the keyboard is started hidden.
- **Reap:** `wl_event_loop_add_signal(SIGCHLD)`, then `waitpid(pid, WNOHANG)`. Only the OSK pid is reaped; `pw_spawn` children are never picowl's children, so this cannot steal their status.
- **Restart policy (`oskstate.c`, pure, unit-tested):** an exit within 10 s of start counts as quick. Delays 1, 2, 4, 8 s; after 5 quick exits in a row picowl logs an error and gives up until an `osk show` or `osk toggle` key starts it again with a fresh count. An exit after a longer run restarts after 1 s. The delay is a `wl_event_loop` timer. A run longer than 10 s resets the counter.
- **Visibility:** picowl keeps `visible`, set by its own show/hide and reset to hidden on every (re)start, since the command starts with `--hidden`. It is a best guess: a user who signals wvkbd from outside makes it wrong, and the next toggle corrects it.
- **Shutdown:** `pw_osk_finish` runs before the clients are destroyed, so the keyboard's exit is never taken for a crash. It sends SIGTERM, waits up to 0.5 s in 10 ms steps and then sends SIGKILL; no restart. This is the only place picowl waits for the keyboard, and the compositor is exiting by then.
- **Sizes:** `oskstate.c` is about 70 lines, `osk.c` about 190.

### 3.3 Tap-and-hold default
- picowl adds a built-in `[layer.wvkbd]` rule with `hold_action = none` in `pw_config_default()`, before the user config is read. A user `[layer.wvkbd]` section merges over it (`find_or_add_rule` in `src/config.c`), so it can still be changed, for example `hold_action = right-click`.

### 3.4 `osk` action
```ini
[keybindings]
code:<button> = osk toggle      # or: osk show, osk hide
```
- `PW_ACTION_OSK` with an argument `show|hide|toggle` (default `toggle`), parsed like the `spawn` command field (`src/picowl.h:67`).
- Sends SIGUSR2 (show), SIGUSR1 (hide) or SIGRTMIN (toggle, resolved at run time) to the pid. With no running OSK (gave up, waiting to restart, or `restart = no` and exited), `show` and `toggle` start it at once and reset the give-up state; `hide` does nothing. The keyboard starts hidden, so that key press does not show it; a second press does. A signal sent straight after the fork would hit the shell before `exec`, which is why the start is not followed by one.
- Show and hide are sent even when picowl's `visible` guess already says so, since the guess can be wrong.
- During a DRM lease the key ends the lease first, like `spawn` (`doc/lease.md`). Without a `cmd` the action is logged and ignored.
- A key press that wakes a blanked display is swallowed as today and doesn't toggle.

### 3.5 Panel
The panel's Keyboard widget (`doc/panel.md` §2) needs a way to call this. Until `picowl-control-v1` exists, the panel can't reach picowl's action. Interim: the widget sends the signal itself (`pkill -x -RTMIN wvkbd-ipaq`), and picowl's `visible` guess is then wrong until the next picowl-driven toggle. `picowl-control-v1` gets `osk(show|hide|toggle)` and a `osk_visible` event.

### 3.6 Fallbacks
| Case | Behaviour |
|---|---|
| `cmd` not found / exits at once | Quick-exit backoff, then give up with one ERROR log |
| wvkbd's keymap fails to compile | wlroots disconnects the client (`keymap_fail` → `wl_client_post_no_memory` in `types/wlr_virtual_keyboard_v1.c`); picowl restarts it |
| Compositor has no RGB565 | Patch 3 falls back to XRGB8888 (picowl always offers RGB565) |
| OSK over a direct-scanned client | The output composites while the keyboard is visible (`doc/zero-copy.md`); direct scanout resumes when it hides |
| Rotation | wvkbd gets a new size from layer-shell configure and picks the landscape height |

### 3.7 Tiled stack: the keyboard pans, it does not resize
With `[layout] stack` set and both apps mapped (README, Tiled layout), a keyboard with an exclusive zone would resize both windows on every show and hide, and a video player pays for that with a new scaler. picowl therefore moves the stack instead:
- **Which surfaces:** `[layout] pan`, comma separated layer-shell namespaces, default `wvkbd`; an empty value restores the shrinking. A surface counts when it is mapped, anchored to the bottom edge (alone or with left and right) and has a positive exclusive zone. The amount is what wlroots takes off the usable area for it (zone plus bottom margin), measured in `arrange_layers` (`src/layer.c`) and kept in `pw_output.pan_zone`; `pw_output.tile_area` is the usable area plus that amount. A top panel, a bottom panel or any other exclusive surface shrinks `usable_area` as before.
- **Layout:** `tile_active` (`src/view.c`) lays the pair out on `tile_area`, so the windows have the size they have without a keyboard, and `view_arrange` only moves the scene nodes up by `pw_tile_pan(zone, output height, top of the lower window)` (`src/tile.c`), which is the zone clamped so that the lower window's top is never above the top of the screen. `view_arrange` calls the xdg-toplevel setters only for values that differ from the scheduled ones, so the clients get no configure at all for a pan.
- **Not panned:** a pair split side by side (landscape): both windows span the full height, so the move would cut the top off each, and the pair is laid out on the shrunk `usable_area` instead. Windows outside the pair and a single app of the pair on its own are always maximized into `usable_area`. When the pair breaks up while the keyboard is shown, the survivor is configured to the shrunk area at once.
- **Input and placement:** `wlr_scene_node_at` returns surface-local coordinates computed from the node position, so pointer and touch input reach the window where it is drawn (`tests/test-scenehit.c`). xdg popups are unconstrained once, at their first commit, against `usable_area` relative to the moved node; text input popups (`src/imrelay.c`) read the node position and `usable_area` on every `pw_view_arrange_all`, which runs after the nodes moved. An xdg popup that is open when the keyboard shows or hides stays where it is.
- **Layer:** a surface on the TOP layer is hidden by panel autohide while an app has the focus, and its zone is ignored then (`arrange_layers` with `hide_top`), so it neither shows nor pans. The test stand-in is on the overlay layer; which layer the wvkbd patches use is not checked here.
- **Tests:** `tests/test-tile.c` (offset and clamp), `tests/test-config.c` (`pan` key), `tests/tile-e2e.sh` with `pw-tile-client` modes `pan`, `nopan` and `pan-landscape` (no configure to the pair, others shrunk, hide restores, pair break up), `tests/pan-e2e.sh` (screenshots of the stack with and without the keyboard), `tests/test-scenehit.c` (hit testing).

## 4. Part 3 (optional): automatic show

**Status:** implemented (`src/imrelay.c`, `src/implace.c`), except the keyboard grab. Tested by `tests/pw-im-client.c` (run from `tests/smoke.sh`) and `tests/test-implace.c`.

- **wvkbd already does its half.** With `--auto` it binds `zwp_input_method_manager_v2` and calls `show()` on `activate` and `hide()` on `deactivate` (`main.c:472-473`, `:1106-1108`, `:1257-1259`, `:525-535`); the other input-method events are empty stubs. It never uses text-input-v3 and types through virtual-keyboard only. No wvkbd patch is needed for this part. `activate` acts immediately, not on `done`.
- picowl creates `wlr_input_method_manager_v2` and `wlr_text_input_manager_v3` (wlroots 0.19 `wlr/types/wlr_input_method_v2.h`, `wlr_text_input_v3.h`) and relays between them: focused surface ↔ text input `enter/leave`; `enable`/`commit`/`disable` → input method `activate`/`deactivate`/`done` with surrounding text, content type and change cause; input method `commit` → `delete_surrounding_text`, `commit_string`, `preedit_string` and `done`. An OSK that speaks input-method-v2 (squeekboard) shows itself on text focus; wvkbd stays on virtual-keyboard and does not use any of this.
- `zwp_virtual_keyboard_manager_v1` is untouched and stays next to the new globals: GTK+2 applications have no text-input and OSK function keys (Ctrl, Esc, arrows) still need key events.
- Focus follows the seat's keyboard `focus_change` signal, so all four focus paths (touch, exclusive layer surface, view focus, unmap) are covered without hooks. At most one text input is active, and only one whose surface has the keyboard focus. After every focus change and every new input method the relay looks for an enabled, focused text input, because wlroots keeps `current_enabled` across leave and enter.
- One input method per seat. A second `get_input_method` gets `unavailable`. If the input method client dies, the text inputs keep their state and the next one is activated at once.
- Popups: `zwp_input_popup_surface_v2` surfaces are placed in the overlay layer below the text cursor rectangle (above it when there is no room), clamped to the usable area of the output the cursor is on, and told the cursor rectangle in popup coordinates. They are placed again on popup commit, text input commit and `pw_view_arrange_all` (rotation, panel changes). wlroots maps them only while the input method is active; the scene node follows map and unmap.
- **Not implemented: the input method keyboard grab** (`zwp_input_method_v2.grab_keyboard`, used by engines such as fcitx and ibus that want raw keys). A grab request is logged and the grab object is destroyed in the compositor, so it stays inert and keys always go to the focused application. Implementing it needs the key and modifier handlers in `input.c` to forward to the grab (after the keybindings, skipping keys that come from the input method's own virtual keyboard to avoid an echo loop), plus `wlr_input_method_keyboard_grab_v2_set_keyboard`.
- **Not implemented: hold-action rules for popups.** A touch on an input method popup falls back to the global `[touch]` hold defaults (`hold_identity` in `input.c` knows no popup role); a long press could send a right click to the candidate list.
- **OSK requirement:** the OSK must use layer-shell keyboard interactivity `none`. With `on_demand` or `exclusive` a tap on its keys moves the keyboard focus to the OSK, the text input gets `leave`, the input method gets `deactivate` and the OSK hides itself.
- Reference: labwc's `src/input/ime.c` (~720 lines with keyboard grab and popups).
- **Who benefits:** only clients that speak text-input-v3, i.e. GTK3/4 and Qt apps, which are too large for these boards, and the havoc terminal with the patch in `oe/recipes-graphics/havoc/files/` (it enables the text input once per focus, so a keyboard hidden with the toggle button stays hidden until the focus returns). GTK+2 apps get nothing until a GDK2 Wayland backend exists and gains an input-method module. Typing never needs this part; virtual-keyboard alone is enough.
- Recommendation: the relay is built because wvkbd needs no change for it; it only helps clients that speak text-input-v3, and virtual-keyboard stays for everything else.

## 5. Cost on 64 MiB [est]
| Item | Upstream wvkbd | wvkbd-ipaq |
|---|---|---|
| Keyboard buffers (portrait 240×100, back + display) | 2 × 96 KB ARGB | 2 × 48 KB RGB565 |
| Popup buffers | 2 × (240×200×4) ≈ 384 KB | none by default |
| Text | fontconfig + pango + harfbuzz + freetype resident, cache scan at start | ~10 KB glyph masks |
| RSS | several MB | well under 1 MB plus libwayland/xkbcommon |
| Per key press | label damage only (already), alpha blend in picowl | label damage only, opaque copy in picowl |

## 6. Tests
**CI (headless, no `/dev/dri`):**
- Fork tests (patch 1 `tests` option): keymap generation, layout geometry for 240/320/480/640 widths, the RGB565 draw path, key state.
- `tests/patches-sync.sh`: OE and packagefiles patch copies are identical.
- `tests/osk.sh` (skipped, exit 77, unless `PW_OSK_BIN` points at a `wvkbd-ipaq`): starts headless picowl, checks that the OSK binds virtual-keyboard and layer-shell with namespace `wvkbd`, maps an opaque RGB565 surface with no exclusive zone, and that 100 SIGRTMIN toggles cause no protocol error and no fd growth; a signal sent during start-up must not kill it.
- `tests/test-oskstate.c`: backoff series, give-up after 5 quick exits, reset after a long run, `restart = no`, the visibility guess, `osk show` and `toggle` resetting give-up and `hide` not starting anything.
- `tests/test-config.c`: `[osk]` keys, the `osk` action and its arguments, the built-in `[layer.wvkbd]` rule and a user override of it. `tests/test-leasepolicy.c`: the `osk` key ends a lease.
- `tests/osk-e2e.sh` (meson test `osk-e2e`, about 20 s): headless picowl with `pw-osk-fake` (logs the signals it gets, can crash on a timer) in place of wvkbd, and `pw-key-client` pressing keys through virtual-keyboard. It checks that the keyboard is picowl's child, that the three `osk` keys arrive as the right signals in order, that `[autostart]` still works, that SIGTERM stops the keyboard and it is not restarted, the 1, 2, 4, 8 s backoff and the give-up, a start by key press with the backoff starting over, a missing command, `restart = no` and an `osk` key without `cmd`.

**Hardware checklist:**
- [ ] Toggle from a hardware button on each board; no repaint of the app below (check damage logging).
- [ ] Key press reaches the app at touch-down, key repeat works, long press gives accents, no right click.
- [ ] Rotation to landscape and back resizes the keyboard.
- [ ] Media player playing under a visible keyboard: composites while visible, direct scanout after hide.
- [ ] RSS of `wvkbd-ipaq` with bitmap text on the h3870.

## 7. Open questions
1. Accent layers: which languages beyond nl/de/fr?
2. Should the panel reserve space when the OSK shows (exclusive zone just for the panel's text-entry dialogs)?
3. Is `pkill` from the panel acceptable until `picowl-control-v1` exists?

## 8. Effort estimate [est]
| Part | Effort |
|---|---|
| Part 1: 11 patches, fork tests | 2 days |
| Part 1: recipe, wrap, `osk.sh`, `patches-sync.sh` | 0.5 day |
| Part 2: `oskstate.c`/`osk.c`, `[osk]`, `osk` action, built-in rule, tests | 1 day |
| Part 2: docs (README, ini example, cursors.md, panel.md) | 0.25 day |
| Part 3 (done, without keyboard grab and popup hold rules) | 1.5 days |
| Hardware checklist, 5 boards | 0.5 day |
