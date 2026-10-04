# On-screen keyboard

**Status:** design only. Nothing in this document is implemented yet.

The OSK is wvkbd, rebuilt for the iPAQ as a patch series on a pinned upstream commit, packaged in picowl's OE layer (Part 1). picowl starts and supervises it, shows and hides it by signal from a keybinding or the panel, and keeps its own tap-and-hold out of the way (Part 2). An input-method relay that shows the keyboard automatically on text focus is an optional Part 3.

wvkbd citations are to upstream commit e14b53a (v0.20-9). picowl citations are to commit 3f74900.

## 1. Problem

### 1.1 picowl today
- **Typing already works:** picowl offers `zwp_virtual_keyboard_manager_v1` (`src/input.c:867`). A virtual keyboard joins the seat with the keymap the client uploads (`src/input.c:342`, `:359-364`). The active seat keyboard is switched on every key event (`src/input.c:272`), so mixing wvkbd and hardware keys is fine.
- **No supervision:** `pw_spawn` double-forks and the real process is reparented to init (`src/server.c:139-175`). picowl doesn't know the OSK's pid, so it can't signal it, notice a crash or restart it. A keybinding can only `spawn pkill -RTMIN wvkbd`.
- **No `osk` action:** the actions are spawn, cycle, close, blank, rotate, panel and quit (`src/picowl.h:50-56`).
- **Placeholder and wrong namespace:** `README.md:183` and `data/picowl.ini.example:29` start a `picowl-osk` that doesn't exist. `README.md:251` and `doc/cursors.md:79` show `[layer.osk]`, but wvkbd's layer-shell namespace is `wvkbd` (`main.c:32`, used at `:813-814`).
- **Tap-and-hold delays every key:** in the default `right-click` mode the left press is held back until lift, `slop_px` or `hold_ms`. On a keyboard that means a key is pressed only when the finger lifts, key repeat can't start, and a long press becomes a right click. Every user would have to know to add `[layer.wvkbd] hold_action = none`.

### 1.2 Upstream wvkbd on an iPAQ
| Behaviour | Upstream | Problem on the iPAQ |
|---|---|---|
| Build | Makefile, one binary per `LAYOUT` (`Makefile:3-6`, `config.mk:5`) | Our rule is meson |
| Pixels | `WL_SHM_FORMAT_ARGB8888`, `CAIRO_FORMAT_ARGB32`, stride `width*4` (`drw.c:238-273`) | picowl alpha-blends it on the CPU on every repaint, and it blocks direct scanout of the app below. Twice the memory of RGB565 |
| Size | `KBD_PIXEL_HEIGHT 250`, landscape 120 (`layout.mobintl.h:3-6`) | 250 px is 78 % of a 320 px portrait screen |
| Text | pangocairo, `"Sans 14"` (`Makefile:10`, `config.mobintl.h:4`) | fontconfig scan at start-up; pango shaping per label draw. GTK2 apps load the same libraries, but the OSK runs before and without them |
| Exclusive zone | on by default (`main.c:920`; `--non-exclusive` at `:1104-1105`) | Every show and hide resizes the focused app and repaints the whole screen: slow on the MediaQ bus (~8 MB/s) |
| Hide/show | `hide()` destroys the layer surface, the popup and both surfaces (`main.c:742-786`); `show()` creates them again | A configure round trip and two new shm buffers on every show |
| Key preview popup | an `xdg_popup` with its own two buffers (`main.c:630-650`) | A second surface to composite and ~2× the buffer memory |
| Signals | blocked only just before the main loop (`main.c:1281-1291`) | A SIGUSR1/2 or SIGRTMIN that arrives during start-up kills the process (default action) |
| Damage | per-rectangle, with a back buffer (`drw.c:18-55`) | Already good; keep it |

## 2. Part 1: wvkbd-ipaq

### 2.1 Delivery
- **Patch series** on upstream e14b53a, generated with `git format-patch`. It lives in `picowl/oe/recipes-graphics/wvkbd/files/`, and a byte-identical copy in `picowl/subprojects/packagefiles/wvkbd/` so a meson wrap can build it for the tests. A `tests/patches-sync.sh` test compares the two copies.
- **Recipe** `picowl/oe/recipes-graphics/wvkbd/wvkbd-ipaq_git.bb`:
  - `SRC_URI = "git://github.com/jjsullivan5196/wvkbd.git;protocol=https;branch=master"` plus the patches; `SRCREV = "e14b53a..."` (full hash).
  - `S = "${UNPACKDIR}/${BP}"` per wrynose/blacksail conventions (as in `picowl_git.bb`).
  - `LICENSE = "GPL-3.0-only & MIT"`: `LICENSE` is the GPLv3 text; `COPYING` says `os-compatibility.[ch]` are MIT (text in `COPYING_WESTON`). `LIC_FILES_CHKSUM` covers `LICENSE` and `COPYING_WESTON`.
  - `inherit meson pkgconfig`; `DEPENDS = "wayland wayland-native wayland-protocols libxkbcommon"`, plus `pango cairo` only for `text=pango`.
  - `picowl_git.bb` gets `RRECOMMENDS:${PN} += "wvkbd-ipaq"`, so a minimal image can leave it out.
- **Wrap** `picowl/subprojects/wvkbd.wrap` (git, pinned revision, `diff_files` = the series), used only by the `osk_tests` meson option; nothing from it is installed.

### 2.2 Patches
| # | Patch | Content |
|---|---|---|
| 1 | meson build | `meson.build` and `meson_options.txt`: `kbd_layout` (`mobintl`, `deskintl`, `ipaq`; meson reserves `layout`), `text` (`bitmap`/`pango`), `tests`. Generates the protocol code with `wayland-scanner`. Installs one binary named after the layout (`wvkbd-ipaq`) and the man page from the existing `wvkbd.1.scd` |
| 2 | start-up fixes | Block the handled signals at the top of `main()`, before any Wayland setup. Free the keymap string after upload |
| 3 | RGB565 | `WL_SHM_FORMAT_RGB565`, `CAIRO_FORMAT_RGB16_565`, stride `width*2` rounded to 4 bytes; fall back to `XRGB8888` if the compositor doesn't advertise RGB565. All colours opaque; the `--alpha` option is ignored in RGB565 mode |
| 4 | optional popup | Key preview popup only with `--popup`; off by default in the `ipaq` layout |
| 5 | keep the surface | `hide()` unmaps by attaching a NULL buffer and committing; `show()` re-attaches the display buffer. No surface, buffer or configure churn per toggle |
| 6 | label damage | Redraw only keys whose label or state changed (shift, layer switch), not the whole keyboard |
| 7 | `ipaq` layout | Portrait 240 px wide: 10 keys per row (24 px), 4 rows plus a function row, ~100 px total. Landscape 320 and VGA 480/640: same rows, wider keys, ~80 px (QVGA landscape) or ~160 px (VGA portrait). Layers: letters, shifted, numbers/symbols, a small Dutch/German/French accent layer. `--non-exclusive` is the default |
| 8 | bitmap text | `text=bitmap`: two built-in public-domain X11 fonts, 6x13 (QVGA) and 10x20 (VGA), rasterised once into a glyph mask and blitted per label. No fontconfig, pango or harfbuzz at run time. Font chosen so the glyph is at most 85 % of the inner key height |
| 9 | long press | Long press on a letter for its accented variants, at 500 ms by default (`--long-press MS`, 0 = off). It is shorter than picowl's `hold_ms` default (900 ms), but picowl leaves the OSK alone anyway (§3.3) |

**Text path decision.** Bitmap by default. GTK2 apps do load pango and cairo, but the OSK starts before any of them and stays resident; with bitmap text it needs neither fontconfig's cache scan at start-up nor pango shaping per label. `text=pango` remains for builds that want scalable fonts or complex scripts.

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
- Defaults: `cmd` empty (no OSK), `restart = yes`. The example ini and README show the line above.
- `[autostart] cmd = picowl-osk` is replaced by this section in `README.md` and `data/picowl.ini.example`.

### 3.2 Supervision (`src/osk.c`, `src/oskstate.c`)
- **Spawn:** one `fork()` + `execl("/bin/sh", "sh", "-c", "exec " cmd)`, so the pid is the OSK itself. Signal mask and dispositions reset in the child as in `pw_spawn`.
- **Reap:** `wl_event_loop_add_signal(SIGCHLD)`, then `waitpid(pid, WNOHANG)`. Only the OSK pid is reaped; `pw_spawn` children are never picowl's children, so this cannot steal their status.
- **Restart policy (`oskstate.c`, pure, unit-tested):** an exit within 10 s of start counts as quick. Delays 1, 2, 4, 8 s; after 5 quick exits in a row picowl logs and gives up until the config is reloaded or the `osk` action is used. A run longer than 10 s resets the counter.
- **Visibility:** picowl keeps `visible`, set by its own show/hide and reset to hidden on every (re)start, since the command starts with `--hidden`. It is a best guess: a user who signals wvkbd from outside makes it wrong, and the next toggle corrects it.
- **Shutdown:** SIGTERM to the pid on exit; no restart.
- Size [est]: `oskstate.c` ~120 lines, `osk.c` ~200 lines.

### 3.3 Tap-and-hold default
- picowl adds a built-in `[layer.wvkbd]` rule with `hold_action = none` before the user config is read. A user `[layer.wvkbd]` section merges over it (`src/config.c:177` find-or-add), so it can still be changed.
- `README.md:251` and `doc/cursors.md:79` change `[layer.osk]` to `[layer.wvkbd]`.

### 3.4 `osk` action
```ini
[keybindings]
code:<button> = osk toggle      # or: osk show, osk hide
```
- `PW_ACTION_OSK` with an argument `show|hide|toggle` (default `toggle`), parsed like the `spawn` command field (`src/picowl.h:67`).
- Sends SIGUSR2, SIGUSR1 or SIGRTMIN to the pid. With no running OSK, `show`/`toggle` (re)starts it and resets the give-up state.
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

## 4. Part 3 (optional): automatic show

- picowl creates `wlr_input_method_manager_v2` and `wlr_text_input_manager_v3` (wlroots 0.19 `wlr/types/wlr_input_method_v2.h`, `wlr_text_input_v3.h`) and relays between them: focused surface ↔ text input `enter/leave`; `enable`/`commit`/`disable` → input method `activate`/`deactivate`/`done`; input method `commit` → `commit_string`, `delete_surrounding_text`, `preedit`. wvkbd then runs with `--auto` and shows itself on text focus.
- Reference: labwc's `src/input/ime.c` (~720 lines with keyboard grab and popups). picowl needs no keyboard grab and no popups: ~300 lines [est].
- **Who benefits:** only clients that speak text-input-v3, i.e. GTK3/4 and Qt apps, which are too large for these boards. GTK+2 apps get nothing until a GDK2 Wayland backend exists and gains an input-method module. Typing never needs this part; virtual-keyboard alone is enough.
- Recommendation: defer until the GDK2 backend exists.

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
- `tests/test-oskstate.c`: backoff series, give-up after 5 quick exits, reset after a long run, `osk show` resetting give-up.
- `tests/test-config.c`: `[osk]` keys, the `osk` action and its arguments, the built-in `[layer.wvkbd]` rule and a user override of it.
- smoke: picowl with `[osk] cmd = <test client that exits>` logs the backoff and gives up.

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
| Part 1: 9 patches, fork tests | 2 days |
| Part 1: recipe, wrap, `osk.sh`, `patches-sync.sh` | 0.5 day |
| Part 2: `oskstate.c`/`osk.c`, `[osk]`, `osk` action, built-in rule, tests | 1 day |
| Part 2: docs (README, ini example, cursors.md, panel.md) | 0.25 day |
| Part 3 (optional) | 1.5 days |
| Hardware checklist, 5 boards | 0.5 day |
