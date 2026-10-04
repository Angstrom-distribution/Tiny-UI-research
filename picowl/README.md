# picowl

A tiny wlroots 0.19 Wayland compositor optimized for GPU-less handhelds: HP iPAQ PDAs (SA-1110/PXA25x/PXA270, no GPU, no FPU, 64 MiB RAM) with 240×320/320×240/480×640 RGB565 panels on slow display buses. Single-threaded, damage-driven software rendering via pixman. Designed for embedded developers working with extreme constraints: read `compositor.md` and `hardware.md` in the [ipaq-ui research](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui) (Tiny-UI-research, branch `docs/ipaq-ui-research`) for the architecture, hardware rotation, memory optimization, and device profiles. Implementation details in `doc/buffers.md`, `doc/zero-copy.md`, `doc/lease.md`, `doc/capture.md`, and `subprojects/packagefiles/wlroots/README.md`.

## Status

- **Working**: basic window management (xdg-shell toplevels maximized to usable area), layer-shell (panel, overlay, background), panel autohide while an app is focused, on-screen keyboard input (virtual-keyboard protocol) with the keyboard process started, supervised and toggled by picowl (`[osk]`, `osk` key action), keyboard navigation (alt+Tab to cycle, logo+Escape to quit, Power key to blank), idle timeout and screen blanking (held off while a visible client holds an idle inhibitor), DRM lease of the output to the media player (`wp_drm_lease_device_v1`, `doc/lease.md`), per-output rotation (hardware via the DRM plane `rotation` property where available, else software), copy-type detection (mq11xx, w100, sa1100-lcdc) with a single-buffer swapchain, picowl-buffer-v1 zero-copy client buffers, linux-dmabuf feedback, direct scanout, malloc tuning, frame-damage commits only, pixman rendering with RGB565/XRGB8888/ARGB8888 format selection.
- **Planned**: video playback offload handshake (apps signal raw buffer availability for direct-to-framebuffer paths), C8 (8-bpp palettised) output on MediaQ (design and options documented in `doc/zero-copy.md`), fixing the per-commit `wlr_client_buffer` allocation in wlroots core. Hardware rotation, copy-type swapchain, and direct scanout are compiled and unit tested; the hardware paths still need the checklist in `doc/zero-copy.md` run on a device.
- **Known constraints**: no general cursor theme (only the tap-and-hold wait animation is drawn by picowl), no window animations or transitions, no composited blur/fade (CPU cost), single fixed render format per build.

## Build

```sh
# wlroots 0.19 must be found via pkg-config (or is built as the subproject);
# if it lives in a private prefix, source its environment first:
source <hostprefix>/env.sh

meson setup build
ninja -C build
# or: meson setup -Dwlroots:default_library=static build  # for static link
```

**Dependencies:**
- `wlroots 0.19` (fetched as meson subproject fallback if not installed)
- `wayland-server`, `wayland-protocols >= 1.32`, `xkbcommon`, `pixman-1`, `libdrm`; `libinput` and `libudev` go together and are optional (see below)
- C11 compiler, meson >= 1.3

picowl carries four wlroots patches (`subprojects/packagefiles/wlroots/`, applied by the wrap and by the OE recipe); see `subprojects/packagefiles/wlroots/README.md` for patch details and `doc/zero-copy.md` for the buffer model. libinput (with libudev) is linked directly when found (touch calibration and its matrix for hardware rotation); without it that code is compiled out.

`wlroots 0.19` is configured minimally: DRM/libinput backends, pixman renderer only, no GLES2/Vulkan/GBM. On OpenEmbedded systems (wrynose/blacksail), see `oe/README.md` for a prebuilt wlroots recipe.

## Running

### DRM/KMS with seatd (native hardware)

```sh
# Start seatd or logind session manager (picowl needs seat access for DRM/KMS):
seatd -l debug &
# or run under systemd-logind in a session:
systemctl --user start graphical-session.target

# If needed, set the DRM device explicitly:
export WLR_DRM_DEVICE=/dev/dri/card0

# Then start picowl:
picowl [-c /etc/picowl.ini] [-d 2]  # -d N sets the log level (0-3)
```

`picowl.service` does not use PAM or logind, and it sets `XDG_RUNTIME_DIR=/run/picowl` (created with mode 0700 by an `ExecStartPre=`) because nothing else creates a runtime directory. picowl exits with an error if `XDG_RUNTIME_DIR` is unset. It needs a seatd service started first, and it keeps `TTYPath=/dev/tty1` and `StandardInput=tty` so seatd has a VT; that tty setup has not been tested on a bare VT without logind. A boot splash that holds DRM master must hand the display over first (wlroots cannot allocate buffers without master): the unit starts after `picosplash.service` and asks it to hand over with `picosplash-write HANDOVER`; where no splash exists that step is skipped. The unit is wanted by `multi-user.target` and `graphical.target`, because the pico-init images have no `graphical.target`. Other programs that talk to the compositor must use the same `XDG_RUNTIME_DIR` to find its socket.

### Headless testing (no hardware)

Used by `tests/smoke.sh` to run the compositor without DRM:

```sh
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1
export XDG_RUNTIME_DIR=/tmp/picowl-test  # writable directory for Wayland socket
export WAYLAND_DISPLAY=wayland-0          # if multiple headless instances

picowl -d 2
```

Log output goes to stderr; use `-d 0` for silent, `-d 1` for errors only, `-d 2` for info (default), `-d 3` for debug.

## Configuration

Config file is INI format, read from the first file found of `$XDG_CONFIG_HOME/picowl/picowl.ini`, `$HOME/.config/picowl/picowl.ini`, `/etc/picowl.ini`. With `-c PATH` only PATH is read (no fallback). A missing file in the default search is not an error (defaults are used, logged at info); an unreadable `-c` file logs an error and also yields defaults. See `data/picowl.ini.example` for a template.

### [render] section

- `format = RGB565 | XRGB8888 | ARGB8888`
  - Preferred pixel format for rendering. If the backend cannot provide it, falls back to XRGB8888. Default: RGB565.

### [output] section

Per-output rotation. Format: `<output-name> = <transform>`.

- `<output-name>`: connector name (e.g., "DSI-1", "HDMI-1"), or `*` to match any output without its own entry.
- `<transform>`: `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`, `flipped-270`.

Example:
```ini
[output]
DSI-1 = 90
* = normal
```

### [rotation] section

Per-output rotation mode: `<output-name> = <mode>` (`*` matches any output).

- `auto` (default): hardware rotation whenever the primary plane's `rotation` property supports the requested transform (in practice the MediaQ mq11xx driver), else software.
- `hardware`: require hardware rotation; logs an error and falls back if unsupported.
- `software`: always rotate in the renderer. Hardware rotation stays the default until measurements show otherwise (test plan: `doc/rotation-measurement.md`). The h2200 media player measured it as neutral at 240 wide and about +2.5% CPU with roughly double the dropped frames at 320 wide with ordered dither, and it saves no bus bytes; no compositor workload has been measured yet. Copy-type outputs also depend on the kernel honouring `FB_DAMAGE_CLIPS`: h2200 kernel builds #201 and #202 ignore them after any rotation or format change and upload the full frame on every commit (about 17.5 ms), fixed by kernel commit daf8e6712ae1.

If the hardware rotation commit fails, picowl logs an error and uses software rotation. Hardware rotation disables hardware cursors (cursor plane has no rotation property); software cursor (hold animation) is used. See [compositor.md § 5.3](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/compositor.md) for implementation details and [hardware.md § 5](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/hardware.md) for per-device rotation capabilities.

### [copytype] section

Per-output override of copy-type detection: `<output-name> = auto | yes | no`. `auto` detects the driver via `drmGetVersion`:
- Copy-type (damage-clipped copy to VRAM): `mq11xx`, `w100`, `sa1100-lcdc`
- Scanout (direct DMA read): `pxa-lcdc` and others

Copy-type outputs use a single-buffer swapchain (kernel copies damage rectangles); scanout outputs may degrade to double-buffer if composition is needed. See [hardware.md](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/hardware.md) for device-specific details.

### [zerocopy] section

- `enable = true|false`: advertise picowl-buffer-v1 and custom linux-dmabuf feedback (default true). See `doc/buffers.md` for the protocol and client usage.
- `single_buffer = true|false`: single-buffer swapchain on copy-type outputs when safe (RGB565, copy-type driver, no format change needed; default true). See [compositor.md § 5.4](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/compositor.md) for swapchain management.
- `panel_autohide = true|false`: hide the panel while an app is focused so a single fullscreen buffer can be scanned out directly (reduces render list to 1 entry; default true). See [compositor.md § 7](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/compositor.md) for panel autohide behavior.
- `max_buffers_per_client` (default 3, range 1..32): picowl-buffer-v1 buffers one client may hold.
- `budget_kb` (default 2048, range 0..65536): memory shared by all clients without an `[app.*]` buffer rule (the default pool). 0 means none of them may allocate; they fall back to wl_shm.
- `total_kb` (default 0, range 0..65536): ceiling over all pools. 0 means no extra limit; the worst case is then `budget_kb` plus every app pool.
- `caching = auto|cacheable|write_combined` (default `auto`, case-insensitive): the value of the `caching` event that clients bound to picowl-buffer-v1 version 2 receive. It says whether the CPU mapping of the buffers is cacheable (reading back is cheap) or write-combined or uncached (write only, never read back or decode into). `auto` uses the DRM driver: `cacheable` for `mq11xx` and `w100` (shmem), `write_combined` for everything else, `sa1100-lcdc` (CMA) included. It is one global key because picowl has one allocator. An invalid value logs an error and keeps `auto`. The start-up log line is `zero-copy enabled (copy_type=N caching=X driver 'D')`.

Sizes are counted as the real allocation: the buffer rounded up to whole pages. An allocation over a limit is answered with `failed(no_memory)`, before any memory is taken. The defaults give 3 buffers and 2 MiB shared, as before the keys existed. Out-of-range values are rejected and the default is kept.

#### Per-app buffer limits

An `[app.<app_id>]` section (the same section as the per-app hold overrides below) with `zerocopy_*` keys gives that app its own pool:

```ini
[app.mediaplayer]
zerocopy_buffers = 7
# zerocopy_budget_kb = 4200
exe = /usr/bin/mediaplayer-wayland
```

| Key | Default | Meaning |
|-----|---------|---------|
| `zerocopy_buffers` | `max_buffers_per_client` | 1..32 buffers per client |
| `zerocopy_budget_kb` | `zerocopy_buffers` full frames of the largest output | 0..65536 KiB for the pool |
| `exe` | none | The client's `/proc/<pid>/exe` must be this file; symlinks are resolved when the config is read |

- **One pool per rule**, shared by every client matching it, and separate from the default pool: other clients cannot starve the app, and clients claiming its app_id cannot get more than the pool. With only `zerocopy_buffers = 7` one config fits every board (7 × 152 KiB on QVGA, 7 × 600 KiB on the hx4700).
- **Matching** is by the `app_id` of one of the client's xdg toplevels (exact, case-sensitive), checked at every `create_buffer`. The client has to call `set_app_id` before `create_buffer`; earlier buffers stay in the default pool. With several toplevels the newest one with a rule wins. Layer-shell clients always use the default pool.
- **`exe`** guards against another program claiming the app_id: on a mismatch, an unreadable link or a replaced binary (`(deleted)`) the client gets the default limits. Without `exe` the pool size is the only protection. `exe` only works when the client connected to the socket itself, not through a `WAYLAND_SOCKET` handed over by a launcher.
- **Memory:** on shmem drivers (mq11xx, w100) the buffers are resident RAM in picowl's process; don't set pools above free memory. On CMA drivers an allocation the kernel refuses is also `no_memory`. See `doc/zero-copy.md`.
- `[app.*]` rules without `zerocopy_*` keys (hold overrides only) have no pool. `zerocopy_*` and `exe` are ignored in `[layer.*]`.
- Buffers are refunded to the pool they were charged to when destroyed. Shrinking an output never revokes buffers.
- Log (`-d 3`, debug): a rejected request names the limit (count, pool or total), pid, app_id and used/cap.

### [lease] section

DRM lease: the media player (`--vo drm:lease`) drives KMS on the output without a VT switch, while picowl keeps input and power policy and takes the output back when the lease ends. See **[doc/lease.md](doc/lease.md)**.

- `enable = true|false`: offer `wp_drm_lease_device_v1` (default true). `false` creates no global. The global also needs the DRM backend and a user who can open the card as non-master (the `video` group).
- `allow = <app_id>[, <app_id>...]`: which clients may lease (default `mediaplayer`). The requester must own the focused toplevel and its `app_id` must be in the list. `*` accepts any focused client; an empty value rejects every request.

### [capture] section

Screen capture for screenshot and recording tools. See **[doc/capture.md](doc/capture.md)**.

- `enabled = true|false`: offer `zwlr_screencopy_manager_v1` (default false). `false` creates no global.
- Security: with capture on, any client that can connect to the Wayland socket can read the whole screen, including other applications. Enable it on development images only.

### [memory] section

malloc tuning via `mallopt`, applied before and after config is read. Reduces memory fragmentation and heap padding on constrained devices:

- `arena_max` (default 1, range 1..8): `M_ARENA_MAX`, number of malloc arenas; 1 reduces fragmentation.
- `trim_threshold_kb` (default 256, range 0..65536): `M_TRIM_THRESHOLD` in KiB, the free heap-top size above which glibc `free()` returns memory to the OS.
- `mmap_threshold_kb` (default 128): Size threshold for mmap-allocated blocks (range 16..4096 so malloc does not mmap tiny blocks).
- `top_pad_kb` (default 16): Extra bytes reserved at heap top (range 0..65536).
- `trim_after_start = true|false`: Call `malloc_trim(0)` once after startup (default true). picowl never trims on idle.

Out-of-range values are rejected and the default is kept. Measured VmHWM (headless, 1280×720): 9.5 MB (baseline 9.4 MB). See [compositor.md § 5.4](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/compositor.md) for memory optimization strategy and `tests/rss.sh` for the RSS measurement test.

### Power Management and Idle Timeouts

Power-aware idle timeout and backlight dimming, configured via `[power]` and `[power.ac]`, `[power.battery]`, `[power.low]` sections. Screen transitions through ACTIVE → DIMMED → BLANKED states based on inactivity and power profile (AC, BATTERY, LOW). Each profile also has an `inhibit = yes|no` key (default yes): while a visible surface holds a `zwp_idle_inhibitor_v1`, picowl does not dim or blank. All timings, the inhibit rules and backlight device selection are documented in **[doc/power.md](doc/power.md)**. The legacy `[idle]` section is supported for backwards compatibility; prefer the `[power.*]` sections.

### [background] section

- `color = #RRGGBB`
  - Background color as a 6-digit RGB hex code. Default: #101010 (dark gray). Alpha is always 1.0 (opaque).

### [autostart] section

Commands to run once at startup. Format: one per line.

- `cmd = <shell-command>`

Example:
```ini
[autostart]
cmd = picowl-panel
```

The on-screen keyboard does not belong here: `[osk]` starts it and keeps it running.

### [osk] section

picowl starts the on-screen keyboard, restarts it when it dies, signals it for the `osk` key action and stops it on exit. The keyboard is wvkbd; the iPAQ build of it is carried as a patch series and an OE recipe (`oe/recipes-graphics/wvkbd/wvkbd-ipaq_git.bb`, not yet built into an image or run on a board). Design: [doc/design/osk.md](doc/design/osk.md).

- `cmd = <command>`: the command line of the keyboard (default: empty, picowl does not handle a keyboard). picowl runs it as `/bin/sh -c "exec <command>"` and stays its parent, so the process that runs is the keyboard itself. Start it hidden: picowl assumes a freshly started keyboard is hidden. An empty value disables the section. Example: `cmd = /usr/bin/wvkbd-ipaq --hidden --auto`; `--auto` lets the keyboard show itself when a text input gets the focus (see the input method relay below).
- `restart = yes|no` (default `yes`): start the keyboard again after it exits. An exit within 10 s of the start is quick; after quick exits number 1, 2, 3 and 4 picowl waits 1, 2, 4 and 8 s, and after the fifth quick exit in a row it logs an error and gives up. A run of 10 s or more resets the count. picowl waits on a timer, never blocks, and keeps serving clients meanwhile.

```ini
[osk]
cmd = /usr/bin/wvkbd-ipaq --hidden --auto
restart = yes
```

- **Key action**: `osk show` (SIGUSR2), `osk hide` (SIGUSR1) and `osk toggle` (SIGRTMIN, looked up at run time; `osk` alone means toggle). When the keyboard is not running (it gave up, is waiting to restart, or `restart = no` and it exited), `osk show` and `osk toggle` start it right away with a fresh count, and it comes up hidden, so the key press that started it does not show it; press again. `osk hide` never starts it. Without a `cmd`, the action is logged and ignored. While a DRM lease is active (`doc/lease.md`) the key ends the lease first.
- **Visibility** is a guess: picowl notes what its own keys did and assumes hidden after every start. If the keyboard is signalled from elsewhere (or shows itself with `--auto`) the guess is wrong; `show` and `hide` are sent regardless, and the next `toggle` is the one that can be off.
- **Shutdown**: picowl sends SIGTERM when it exits, waits up to half a second and then kills the keyboard. It is not restarted. If picowl itself is killed, the keyboard gets SIGTERM from the kernel.
- **Tap-and-hold**: a built-in `[layer.wvkbd]` rule sets `hold_action = none`, so key presses are not delayed until the finger lifts. Your own `[layer.wvkbd]` section merges over it, see below.
- Children started by `[autostart]` and `spawn` are not touched by any of this and keep working as before.

### [keybindings] section

Keyboard shortcuts. Format: `<modifiers>+<key> = <action> [command]`.

- `<modifiers>`: zero or more of `alt`, `ctrl`, `shift`, `logo` separated by `+`.
- `<key>`: XKB keysym name (e.g., `Tab`, `F4`, `Return`), or a numeric evdev keycode as `code:116` (KEY_POWER).
- `<action>`: `spawn`, `cycle`, `close`, `blank`, `rotate`, `panel`, `osk`, `quit`.
  - `spawn <command>`: fork and exec `/bin/sh -c <command>` (detached, no zombies).
  - `cycle`: raise the next mapped toplevel in stacking order.
  - `close`: send a close request to the focused toplevel.
  - `blank`: toggle screen on/off (same as Power key or output-power-management requests).
  - `rotate`: cycle output rotation of the first output (normal, 90, 180, 270; keeps the flipped bit).
  - `panel`: toggle the panel while panel autohide is active (forces it visible until focus changes).
  - `osk <show|hide|toggle>`: show, hide or toggle the on-screen keyboard started by `[osk]` (default `toggle`); see the `[osk]` section.
  - `quit`: exit the compositor.
- `[command]`: the shell command for the `spawn` action, or `show`, `hide` or `toggle` for the `osk` action (an unknown one drops the binding with an error).

**Built-in defaults** (always present: bindings from the config are added to them, and a config entry cannot remove one):

| Binding | Action |
|---------|--------|
| `code:116` (Power/KEY_POWER) | Toggle blank |
| `alt+Tab` | Cycle views |
| `logo+Escape` | Quit |

Example config overrides:
```ini
[keybindings]
alt+F4 = close
code:116 = blank
logo+Return = spawn foot
code:397 = osk toggle
```

## Touch calibration

picowl never uses the raw axes of a touch device. Resistive panels such as the iPAQ ones report raw ADC values that are inverted and offset, so every touch device (a libinput device with the touch capability; not tablets) needs a calibration, taken from the first of these sources that gives one:

1. `[touch] calibration = a b c d e f` in the config: six numbers, a libinput normalized matrix (`x' = a*x + b*y + c`, `y' = d*x + e*y + f`, with x, y, x', y' in 0..1 over the panel). It overrides everything, also when the device has a udev matrix or a pointercal exists.
2. A tslib pointercal file, `[touch] pointercal = PATH` (default `/etc/pointercal`, an empty value disables this source). A missing file is skipped quietly; an unreadable or invalid one is logged as an error and the next source is tried.
3. A non-identity default matrix from libinput, which comes from the udev property `LIBINPUT_CALIBRATION_MATRIX`. libinput cannot tell an identity default from "none", so an identity default counts as none.
4. None: the device is disabled (libinput send-events mode `disabled`), never attached to the cursor, and an error naming the device and the fixes is logged once. It stays disabled.

The pointercal file is what tslib's `ts_calibrate` writes: ten whitespace-separated integers `a b c d e f scale xres yres rotation`, where the screen position is `x' = (a*x + b*y + c) / scale`, `y' = (d*x + e*y + f) / scale` for raw x, y, on a screen of `xres` by `yres` pixels. Example (a 240x320 panel, no trailing newline needed):

```
22841 -68 -3658260 -491 -28324 25242144 65536 240 320 0
```

Parsing is strict: 7 to 10 numeric fields, non-zero positive scale, sane magnitudes. `xres` and `yres` must be present and non-zero (older files with 7 fields are refused, recreate them), and a non-zero `rotation` is refused rather than guessed. picowl converts the file to a libinput matrix using the ABS_X/ABS_Y range of the device (ABS_MT_POSITION_X/Y on multitouch panels) read from its device node, normalized the way libinput does it, with `max - min + 1`. For the example above and a 0..1023 panel the matrix is `1.487044 -0.004427 -0.232586 -0.023975 -1.383008 1.203639`, the same as `xinput_calibrator` gives for it. If the ranges cannot be read, the device is disabled instead of falling back to another source.

Create the file on the device with tslib's `ts_calibrate`, in the native orientation of the panel and with picowl not running (it needs the screen and the touch device for itself). The calibration is applied when the device appears and is the base that hardware rotation composes with; software rotation restores it exactly. Tablets are not touchscreens: they never take a pointercal or `[touch] calibration`, only the libinput default matrix as before.

Escape hatch: a panel that really is calibrated already (for example a capacitive touchscreen with correct axes) is used by saying so explicitly with an identity matrix, `calibration = 1 0 0 0 1 0`. It is never assumed.

## Tap-and-hold and Cursor

### Tap-and-Hold Behaviour

Touch input on picowl supports a tap-and-hold gesture (enabled by default, configurable via `[touch]`):

- **Touch-down**: Focus the surface at the touch point and send pointer enter + motion, but defer the left-button press.
- **Tap** (touch-up before hold triggers): Send left button press and release as a single click.
- **Drag** (movement > `slop_px` before hold triggers): Send the deferred left press and normal motion; no hold animation.
- **Hold animation** (after `hold_delay_ms`): Display a wait cursor at the touch point, stepping frames via a timer at ~12 fps (configurable).
- **Right-click** (after `hold_ms` total from touch-down): Hide the animation, send right button press and release (Pocket PC convention; GTK2 sees button 3), and swallow remaining touch events. With `hold_button = middle` the hold sends a middle button press and release instead, which pastes the primary selection in a terminal such as havoc.
- **Lift during animation**: Counts as a tap (left press + release) only if the hold had not yet triggered.

The behaviour is controlled by the `[touch]` section (see `data/picowl.ini.example`):

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `hold_action` | `right-click` | `right-click`, `none` | Enable right-click on hold (`none` = immediate left press like traditional pointer) |
| `hold_button` | `right` | `right`, `middle` | Button the hold sends: `right` (BTN_RIGHT) or `middle` (BTN_MIDDLE). Unused when `hold_action = none`. An unknown value is logged and the default is kept |
| `hold_delay_ms` | 300 | integers | Milliseconds before the hold animation starts |
| `hold_ms` | 900 | > `hold_delay_ms` | Milliseconds from touch-down to the click |
| `slop_px` | 8 | 0..64 | Movement tolerance in pixels; exceeding this cancels hold and triggers drag |

### Per-App and Per-Layer Overrides

Per-app and per-layer-shell surface overrides allow fine-grained control over tap-and-hold behaviour without changing the global default. This is useful for applications like media players that time their own long press (`hold_action = none` sends the left press at touch-down, with no right-click and no animation):

```ini
[app.mediaplayer]
hold_action = none

[app.org.example.Viewer]
hold_ms = 1200

# holding pastes the primary selection in a terminal
[app.havoc]
hold_button = middle

# built in, shown here to say how to change it: the keyboard times its own presses
[layer.wvkbd]
hold_action = none
```

Keys in `[app.<app_id>]` and `[layer.<namespace>]` sections (`[app.*]` also takes the buffer keys above):

| Key | Default | Meaning |
|-----|---------|---------|
| `hold_action` | from `[touch]` | `right-click` or `none` |
| `hold_button` | from `[touch]` | `right` or `middle` |
| `hold_delay_ms` | from `[touch]` | Milliseconds before animation starts |
| `hold_ms` | from `[touch]` | Milliseconds to the click |
| `slop_px` | from `[touch]` | Movement tolerance in pixels |
| `aspect` | unset | `[app.*]` only: `W:H` for the tiled layout, see below |

Rules:
- **Inheritance**: Unset keys use the value from `[touch]`, whatever the order of sections in the file.
- **Bad timings**: If `hold_ms` is not greater than `hold_delay_ms` or `slop_px` is outside 0..64, the rule's three timing keys revert to `[touch]`; its `hold_action` and `hold_button` are kept. A bad `hold_action` or `hold_button` value is logged and ignored (inherited).
- **Matching**: Names match exactly and case-sensitively. No wildcards.
- **Merging**: A section appearing twice merges into one rule; later keys win.
- **Scope**: `[app.panel]` and `[layer.panel]` are separate namespaces. An empty `[app.]` or `[layer.]` is logged and ignored.
- **Binding**: The surface under the finger at touch-down decides (popups and subsurfaces follow their parent). The setting stays fixed until lift.

### Tiled layout

Every window is maximized and one is shown at a time, except for two apps you name in `[layout]`. While both of them have a mapped window they tile the usable area instead of stacking:

```ini
[layout]
stack = mediaplayer, havoc

[app.mediaplayer]
aspect = 4:3
```

- `stack`: two distinct app_ids, comma separated, in order. A list of more than two ignores the extra names; a repeated name is skipped, and tiling stays off unless two distinct names remain.
- `aspect` (in `[app.<app_id>]`): `W:H`, two integers from 1 to 4096, the natural aspect of the first app. A bad value is logged and ignored.

Rules:
- **Order**: in a portrait usable area (height at least width) the first app is on top and the second below, both at full width. In a landscape one the first is on the left and the second on the right, both at full height.
- **Size of the first window** along the split axis: its natural aspect ratio applied to the available extent. The source is, in this order, the client's own fixed size (xdg-shell `min_size` equal to `max_size`, both non-zero, re-read whenever the client commits a change, so a new clip with another aspect re-lays out), then `aspect` from the config, then half. The result is clamped so that each window keeps at least 25 percent of the extent, because a hint is only a preference and one window must not be squeezed to nothing. The second window gets the rest. Each client receives the exact size in a configure and draws its own letterboxing if its content has another aspect.
- **One of the two**: with only one of the apps mapped, nothing changes: it is maximized like any other window. When the second appears or goes away both are configured again.
- **Other windows**: apps outside the stack are not affected. A window raised above the pair covers both.
- **Focus**: touching a tiled window gives it the keyboard. alt+Tab (the `cycle` action) moves the keyboard between the two tiled windows, without restacking, while one of them has the focus; it does not reach other apps then (use the panel). Focusing one tiled window raises both together.
- **Keyboard overlay**: the on-screen keyboard is a layer surface without an exclusive zone, so it overlays the lower or right window and does not move the layout.
- **Rotation and panel**: the layout is recomputed whenever the output size, its transform or the usable area changes, the same as for maximized windows.
- **Fullscreen**: a fullscreen request from a tiled window is answered with its slot size.
- **Direct scanout**: two visible windows are two entries in the scene's render list, so the scene composes them and never scans out one buffer. A window which is alone is still eligible.
- **Idle inhibit**: an inhibitor counts while its window has the keyboard focus, so a playing window in the unfocused slot does not hold the screen on.

### Cursor Configuration

Picowl draws a cursor **only during the tap-and-hold animation** (and while the pointer-focused client asks for a `wait` or `progress` cursor shape, see `wp_cursor_shape_manager_v1`). The animation is configured in the `[cursor]` section:

| Key | Default | Meaning |
|-----|---------|---------|
| `hold_animation` | `builtin` | `builtin` (Pocket PC 2003 rotating circle) or path to a PAM/PPM strip file |
| `fill` | `#2050c0` | Foreground colour of the builtin animation (`#RRGGBB` hex) |
| `outline` | `#ffffff` | Outline colour of the builtin animation (`#RRGGBB` hex) |
| `frame_interval_ms` | 83 | Frame duration in milliseconds (20..1000; default ~12 fps) |

For comprehensive cursor documentation including platform constraints, custom animation format, and the conversion tool, see **[doc/cursors.md](doc/cursors.md)**.

### Cursor Requirements and Tools

Picowl enforces hardware cursor rules to avoid frame commit failures:

- **ARGB8888 format**, size 1..64 pixels
- **No translucency**: Every pixel is fully transparent (`0x00000000`) or fully opaque (alpha `0xff`)
- **Two-colour limit**: At most 2 distinct opaque colours per frame (compared at 6 bits per component)

These rules exist because:
- The MediaQ MQ1132/MQ1188 hardware cursor is 2 bpp AND/XOR with two colour registers
- The mq11xx DRM cursor plane rejects violations with `-EINVAL`, failing the entire frame
- wlroots 0.19 does not test-commit cursors before the next update
- Software cursor fallback boards benefit from the cost constraints

A custom `hold_animation` file (PAM or PPM) is a horizontal strip of square frames. Non-compliant frames are automatically converted at load time with a logged warning; see **[doc/cursors.md](doc/cursors.md)** for details.

#### picowl-cursor-convert Tool

Use the `picowl-cursor-convert` tool to create, validate, or convert cursor strips:

```
picowl-cursor-convert [--frame-width N] [--fg #rrggbb] [--bg #rrggbb]
                      [--premultiplied] [--hotspot X,Y] in.pam|ppm out.pam

picowl-cursor-convert --check [--frame-width N] in.pam|ppm

picowl-cursor-convert --export-builtin [--fg #rrggbb] [--bg #rrggbb] out.pam
```

**Options:**
- `--check`: Validate without output; exit 0 (compliant) or 1 (non-compliant)
- `--export-builtin`: Write the built-in animation as a template PAM file
- `--fg`, `--bg`: Force specific colours; otherwise the two dominant colours are kept
- `--premultiplied`: Input has premultiplied alpha

**Exit codes:** 0 (success/compliant), 1 (non-compliant in `--check`), 2 (I/O or usage error)

To convert PNG to PAM:

```sh
pngtopam -alphapam image.png image.pam          # netpbm
convert image.png pam:image.pam                 # ImageMagick
```

For complete usage examples and troubleshooting, see **[doc/cursors.md](doc/cursors.md)**.

## Supported Protocols

Overview of what picowl adds on top of the wlroots set below:

- `picowl_buffer_manager_v1` / `picowl_buffer_v1` (`protocols/picowl-buffer-v1.xml`): compositor-allocated RGB565 dmabufs, `copy_type`, `caching` (version 2: cacheable or write-combined mapping), `copied` (direct scanout on copy-type outputs) and `retained` events so clients on copy-type outputs can use a single buffer until the compositor composites.
- `zwp_linux_dmabuf_v1` v4 with hand-built feedback (RGB565/LINEAR, scanout tranche).

Picowl advertises and implements (via wlroots 0.19):

**XDG Shell & Core Composition:**
- `xdg_wm_base` (xdg-shell, advertised at version 3): toplevel windows, popups, activation.
- `wl_compositor`, `wl_subcompositor`: surface trees.
- `zwlr_layer_shell_v1`: panel, overlay, background, lock surfaces with exclusive keyboard interactivity.
- `wl_data_device_manager`: copy/paste clipboard.
- `zwp_primary_selection_device_manager_v1`: select-to-copy and middle-click paste; picowl accepts every client request, as it does for the clipboard.

**Input & Interaction:**
- `wl_seat`, `wl_keyboard`, `wl_pointer`, `wl_touch` (if available): input focus.
- `zwp_virtual_keyboard_manager_v1`: on-screen keyboards (software input methods). Kept next to text-input and input-method: GTK+2 applications have no text-input and on-screen keyboard function keys still inject key events through it.
- `zwp_text_input_manager_v3` and `zwp_input_method_manager_v2` (`src/imrelay.c`, `protocols/input-method-unstable-v2.xml`): the globals are advertised. Keyboard focus is relayed to text-input: when the keyboard focus changes, the text inputs of the client that lost it get `leave` and those of the client that gained it get `enter`, also for a text input created after the client already has the focus. The text input of the focused client drives the input method: `enable` becomes `activate`, followed by surrounding text, content type and change cause (only the parts the client used) and `done`; every later `commit` resends that state; `disable`, losing the focus or destroying the text input becomes `deactivate`. What the input method commits (`delete_surrounding_text`, `commit_string`, `preedit_string`) goes to the active text input, followed by `done`. At most one text input is active, and only one whose surface has the keyboard focus. Only one input method is accepted per seat; a second one gets `unavailable`. Input method popup surfaces (candidate lists) are shown in the overlay layer below the text cursor rectangle of the active text input, above it when there is no room below, clamped to the usable area of the output the cursor is on (so they stay clear of exclusive layer surfaces), and told where the cursor is inside them with `text_input_rectangle`; they are placed again when the text input commits, the popup commits or the layout is arranged (rotation, panel changes), and hidden while the input method is inactive. Tapping a popup does not move the keyboard focus. **Not implemented:** the input method keyboard grab (`grab_keyboard`, for engines that want raw keys): the request is logged and the grab object is destroyed, so keys always go to the focused application (an on-screen keyboard injects them through virtual-keyboard); and touch hold rules for popups (a long press on a popup uses the global `[touch]` hold defaults). An on-screen keyboard that uses input-method must use layer-shell keyboard interactivity `none`, or tapping it moves the focus away and hides it again. Tested by `tests/pw-im-client.c` (two connections: application and input method) in the smoke test. See `doc/design/osk.md` part 3.
- `xdg_activation_v1`: app activation requests.
- `zwp_tablet_manager_v2`: drawing tablets with libinput (`tablet.c`). Proximity, tip, motion, pressure, distance, tilt, rotation, slider, wheel and button events go to the surface under the tool when its client bound tablet-v2, and that surface keeps the tool while a tip or button is down; pads get focus with the tool and forward buttons, rings and strips. Tablet coordinates are mapped to the output and follow its rotation like touch does (software rotation by wlroots, hardware rotation by the libinput calibration matrix). A tablet that cannot follow a hardware rotation because it has no calibration matrix is logged and ignored until the output is unrotated. Events are dropped while the display is leased or blanked (the waking event is swallowed). There is no pointer emulation for clients without tablet-v2, no configuration keys, and tablet tools do not set the cursor image.
- `wp_cursor_shape_manager_v1` (version 1): only the client with pointer focus (or the surface a tablet tool is in) is heard. `wait` and `progress` show the hold animation (stopped when the client asks for another shape or loses focus); every other shape shows no image, as picowl draws no pointer. The animation of a touch hold is never replaced or stopped by a client request.

**Output & Rendering:**
- `wl_output`: output geometry, mode, subpixel, transform.
- `wp_presentation`: frame timing hints (via wlr_presentation).
- `wp_viewporter`: scaling and cropping (via wlr_viewporter).
- `zwp_single_pixel_buffer_v1`: solid-color surfaces (useful for backgrounds).
- `wp_content_type_manager_v1` (version 1): clients may declare photo, video or game content. picowl only logs the hint of a toplevel at debug level when it maps; nothing acts on it yet.

**Window Management:**
- `zwlr_foreign_toplevel_manager_v1`: taskbar/panel integration (list, activate, close; minimize requests are ignored).
- `ext_foreign_toplevel_list_v1` (v1): toplevel enumeration.
- `zxdg_decoration_manager_v1`: window decoration hints (picowl always chooses server-side, i.e., no decoration).

**DRM lease:**
- `wp_drm_lease_device_v1` (version 1): offered with the DRM backend only; see `doc/lease.md` and `[lease]` above.

**Screen capture:**
- `zwlr_screencopy_manager_v1` (version 3): only with `[capture] enabled = true`; see `doc/capture.md`. `ext_image_copy_capture_v1` is not implemented.

**Power & Idle:**
- `zwlr_output_power_management_v1`: screen blanking requests (on/off).
- `ext_idle_notifier_v1` (ext-idle-notify-v1): idle/resume notifications for clients such as screen lockers.
- `zwp_idle_inhibit_manager_v1` (version 1): a video player keeps the screen on while its surface is visible. picowl honours it in its own dim and blank timers and reports it to `ext_idle_notifier_v1` clients; see `doc/power.md`.

## Testing

```sh
source <hostprefix>/env.sh
meson setup build && meson test -C build
WLR_SCENE_DISABLE_DIRECT_SCANOUT=1 meson test -C build   # force composition
```

### Automated Tests (Headless)

Tests: `config`, `tile` (the layout arithmetic), `tile-e2e` (two toplevels over one connection, landscape and portrait: the configure sizes, hint changes at run time and a partner going away), `zbquota`, `smoke` (headless run, also with `--zerocopy`, `--zerocopy-count`, `--probe` and `--expect-global` for the always-on globals), `bufproto` (bind events and version gating over a socketpair, then `pw-test-client` at version 2 and 1), `touchhold`, `pointercal` (tslib parsing and matrix conversion), `cursorfit`, `cursor-builtin`, `cursorshape` (shape mapping), `rotate`, `copytype`, `copyrel`, `pixman-pass`, `pixman-dmabuf`, `rss`, `backlight`, `powersupply`, `dim`, `power-e2e`, `leasepolicy`, `capture` checks inside `smoke` (`pw-capture-client`: no screencopy global by default, the background colour when `[capture]` is enabled), and `lease-vkms` (suite `vkms`: opt-in with `PW_LEASE_VKMS=1`, needs root and the vkms module, skips otherwise).

- **rss:** Memory test. Starts compositor headless (1280×720), maps test client, measures VmHWM. Fails if peak RSS exceeds ceiling (meson option `-Drss_ceiling_kb`, default 12288 kB; headless baseline ~9.5 MB). Override with `PW_RSS_CEILING_KB` for a single run.

### Hardware Validation Checklist

DRM paths (rotation, copy-type, swapchain, direct scanout) cannot run in the build container (no /dev/dri, no vkms); they are compiled and unit-tested only. On a real device, verify:

- Hardware rotation: the hardware rotation commit succeeds, touch calibration correct, no "hw rotation commit failed" log
- Touch calibration: touch the four corners of the screen and expect the pointer at the matching corners, in all four rotations, with software rotation and with hardware rotation; with no pointercal, no `[touch] calibration` and no udev matrix, expect the one-time error and a dead touchscreen
- Copy-type single-buffer: swapchain stays at 1 slot, no "degraded to 2 slots" log
- Direct scanout: scene logs it, render list is 1 entry, no composition
- Damage clipping: only changed regions copied to VRAM
- DRM lease: the checklist in `doc/lease.md`
- Tiled layout: with `[layout] stack = mediaplayer, havoc`, start both on the panel in portrait and in landscape: expect the first app on top (left), the second below (right), touching either gives it the keyboard, alt+Tab moves the keyboard between them, the on-screen keyboard overlays the lower window, rotating re-tiles, closing one maximizes the other and the log shows no direct scanout while both are visible

See the hardware-only checklist in `doc/zero-copy.md`.

## OpenEmbedded

For embedded systems using OpenEmbedded (wrynose/blacksail releases), picowl includes pre-configured bitbake recipes. See `oe/README.md` for adding the layer, configuring the machine/distro, and building.

Key points:
- Recipes: `recipes-graphics/picowl/picowl_git.bb`, `recipes-graphics/wlroots/wlroots_0.19.0.bb`.
- Layers: compatible with `wrynose` and `blacksail` (LAYERSERIES_COMPAT).
- `S = "${UNPACKDIR}/picowl"` (git subpath: will change when picowl moves to its own repo).
- Requires `wayland` DISTRO_FEATURE; `systemd` for the unit file.
- Installs `picowl.service` (not auto-enabled by default) and `/etc/picowl.ini`.
- Avoid Thumb mode on ARM (set `ARM_INSTRUCTION_SET = "arm"` globally or per-package).

## License

MIT. See `LICENSE` in the picowl root.
