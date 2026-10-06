# picowl

A tiny wlroots 0.19 Wayland compositor optimized for GPU-less handhelds: HP iPAQ PDAs (SA-1110/PXA25x/PXA270, no GPU, no FPU, 64 MiB RAM) with 240×320/320×240/480×640 RGB565 panels on slow display buses. Single-threaded, damage-driven software rendering via pixman. Designed for embedded developers working with extreme constraints: read `compositor.md` and `hardware.md` in the [ipaq-ui research](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui) (Tiny-UI-research, branch `docs/ipaq-ui-research`) for the architecture, hardware rotation, memory optimization, and device profiles. Implementation details in `doc/buffers.md`, `doc/zero-copy.md`, `doc/lease.md`, `doc/capture.md`, and `subprojects/packagefiles/wlroots/README.md`.

## Status

- **Working**: basic window management (xdg-shell toplevels maximized to usable area), layer-shell (panel, overlay, background), panel autohide while an app is focused, on-screen keyboard input (virtual-keyboard protocol) with the keyboard process started, supervised and toggled by picowl (`[osk]`, `osk` key action), keyboard navigation (alt+Tab to cycle, logo+Escape to quit, Power key to blank), idle timeout and screen blanking (held off while a visible client holds an idle inhibitor), DRM lease of the output to the media player (`wp_drm_lease_device_v1`, `doc/lease.md`), per-output rotation (hardware via the DRM plane `rotation` property where available, else software), copy-type detection (mq11xx, w100, sa1100-lcdc) with a single-buffer swapchain, picowl-buffer-v1 zero-copy client buffers, linux-dmabuf feedback, direct scanout, malloc tuning, frame-damage commits only, pixman rendering with RGB565/XRGB8888/ARGB8888 format selection. An optional touch panel client, `picowl-panel` (a slim bar with clock, battery, backlight and volume, and slider rows that pop out of it), is built next to the compositor (see Panel).
- **Planned**: video playback offload handshake (apps signal raw buffer availability for direct-to-framebuffer paths), C8 (8-bpp palettised) output on MediaQ (design and measured facts in `doc/design/mediaq-c8.md`), fixing the per-commit `wlr_client_buffer` allocation in wlroots core. Hardware rotation, copy-type swapchain, and direct scanout are compiled and unit tested; the hardware paths still need the checklist in `doc/zero-copy.md` run on a device.
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
- for `picowl-panel` only: `wayland-client` and `alsa-lib` (meson option `panel`, default `auto`: built when both are found, `-Dpanel=disabled` to skip it); its text needs a TrueType font file at run time, but not to build or start (see Panel)
- C11 compiler, meson >= 1.3

picowl carries five wlroots patches (`subprojects/packagefiles/wlroots/`, applied by the wrap and by the OE recipe); see `subprojects/packagefiles/wlroots/README.md` for patch details and `doc/zero-copy.md` for the buffer model. libinput (with libudev) is linked directly when found (touch calibration and its matrix for hardware rotation); without it that code is compiled out.

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

`PICOWL_HEADLESS_SIZE=WxH` (for example `240x320`) sets the size of the headless output, which is 1280x720 otherwise, so that a client or a screenshot can be tried at the size of a handheld panel. It applies to headless outputs only, and the `[output]` transform is applied on top of it.

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

`subpixel = unknown|none|horizontal_rgb|horizontal_bgr|vertical_rgb|vertical_bgr` (in the same section; no output is called "subpixel") sets the subpixel layout that `wl_output.geometry` advertises, so that a client can draw text for the panel's colour stripes (picowl-panel does, see Panel). It is the layout of the panel itself, in its native orientation, as the protocol defines it: clients combine it with the `transform` of the same event. `horizontal_rgb` is red at the left of the panel as it is built (the iPAQ h2200: its 240x320 panel is portrait). With software rotation the transform is the configured rotation and the layout stays the native one, so the same event tells a client both. With hardware rotation picowl sends the transform `normal` (the display turns the picture, the client sees an already rotated output), so the layout is advertised as the client sees it: `horizontal_rgb` on a panel rotated by 90 is sent as `vertical_rgb`, and a client that reads layout and transform together reaches the same stripe order either way. Not set: the value the backend reports stays (the DRM connector's, which is `unknown` for most handheld panels, and the headless output's). The key applies to every output. A wrong name is logged and ignored. The layout of each board is a property of its panel and not detected; the iPAQ values are the owner's, not taken from a datasheet: `data/picowl.ini.example` lists them.

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

Autostarted commands, `spawn` commands and the keyboard run with picowl's environment. A service manager without a login session hands picowl `HOME=/` (or none), and clients such as a terminal then look for their `~/.config` in the wrong place. When `HOME` is unset, empty, relative or `/`, picowl sets it from the home directory of its effective user in the account database before anything is started, and logs the change. A usable `HOME` that was set on purpose is left alone, and an account without a home directory changes nothing.

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

## Panel

`picowl-panel` is a layer-shell client in C for a 240x320 handheld: a slim bar with the clock, the battery and two icon buttons, for the backlight and the volume. Tapping a button pops a row out under the bar: a slider for the backlight and the volume, the weekday and the date for the clock, the time left for the battery. It is drawn with wl_shm, anti-aliased by default (`--style crisp` draws without any anti-aliasing, see Crisp style below): the text comes from a TrueType font file that is read at start-up (stb_truetype, vendored as `panel/stb_truetype.h`, public domain), the icons and the slider are vector shapes rasterized into coverage masks when the panel starts or its layout changes, and redraws only blend those masks. It is a single process with one poll loop. Measured on arm64 with glibc, headless, with the Liberation Sans Bold font: VmRSS 2.9 MB, of which 470 KB anonymous and the rest the mapped program and libraries (the font file is mapped for the start-up and released again, only the glyph bitmaps stay). The buffer of the closed bar is 240x18 RGB565 (8.6 KB); with a slider row open it is 240x54 ARGB8888 (52 KB) when the row is translucent (the default), RGB565 (26 KB) when it is opaque.

### What it shows

The bar is 18 px high by default (`--height`, 18 to 80) and as wide as the output; on an output rotated by 90 or 270 degrees it is on the short side instead, see Rotated output below. Margins are 8 px.

- **Left: the clock**, `HH:MM`, 24 h, local time, no seconds. A timerfd on the realtime clock fires at the start of every minute and is cancelled when the system time is set, so a change of the time or the date is shown at once. It is a button too: the touch target is the text widened to at least 36 px and as high as the bar, and while its row is open it has the same highlighted background as the icon buttons.
- **Right, from the left: the backlight button** (a sun), **the volume button** (a speaker) and **the battery**. Each button is at least 36 px wide and the whole height of the bar; the icon in it is about 12 px. The speaker has no waves and a slash at 0 percent (muted), one wave up to 50 percent and two above. A button without a device is greyed out and ignores taps. While a slider row is open, its button has a highlighted background and the icon is drawn in the accent colour.
- **The battery** is the first power supply of type `Battery` with a readable `capacity` (a battery with `scope` `Device`, as input devices have, is not the system's), shown as a battery outline with a fill proportional to the capacity and `NN%`. The fill is light grey, red at 15 percent and below, and green while the status is `Charging` (red wins over green: a nearly empty battery that charges is red, with the bolt); a small lightning bolt over the outline means `Charging`. Without a battery, a `Mains` or `USB*` supply that is online shows `AC`, and nothing at all shows `--`. The supplies are read again every 30 s; there is no uevent code, so a plug event shows up at the next read. The battery icon and its percentage are a button too (its touch target runs to the edge of the output and is at least 36 px wide), and highlighted while its row is open.

The slider row is 36 px high under a bar of up to 28 px (taller for taller bars) and as wide as the output: a large icon (24 px) on the left, a rounded track whose filled part is blue and whose rest is dark grey, a circular thumb 22 px in diameter (white, with a darker ring) and the value as `NN%` on the right. The row has a 1 px line on its edge towards the windows.

The date row, opened by the clock, is the same row with one centred line of text in the larger font (4/3 of the bar's text size, 14 px at the default, never above 26 px) and no slider: the weekday and the date as `Monday 5 October 2026`, in local time (the system's time zone, as for the clock). The names are in a table in the panel and not from the locale, so the line is English whatever `LANG` says; the day has no leading zero. A line that does not fit the width less the margins is shrunk in three steps (to 3/4 and then 5/8 of the size, never below 8 px) and never clipped. The date is refreshed by the same minute tick as the clock and only while the row is open does it redraw, so an open row follows midnight and a closed panel has no extra timer or wake-up.

The battery row, opened by the battery, is a row of the same kind with the time left, the percentage in front when the line has room for it (`94%  1 h 30 min left`). The line is the first of four wordings that fits the width in the largest size, each shorter than the one before (without the percentage, then without `left` or `to full`, then both), and only when none of them fits is the next smaller size tried, so the row loses words before it loses size. See Time left below.

The look is a dark flat theme: bar `#1c1f24` with a 1 px lighter line at the bottom, text `#e8eaed`, accent `#4c8dff`. Everything with a curved edge is anti-aliased, and the text and the icons are blended in linear light so that light text on the dark ground keeps its weight.

**Font.** The text is rasterized from a TrueType file that is looked up at run time, the first that exists of `/usr/share/fonts/ttf/LiberationSans-Bold.ttf`, `/usr/share/fonts/ttf/LiberationSans-Regular.ttf`, `/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf`, `/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf` and `/usr/share/fonts/TTF/DejaVuSans.ttf`. `--font PATH` names another file instead of the list. The size is `--font-size PX` (default 11 px on the 18 px bar, in proportion to `--height` otherwise); the percentage in the slider row is 10 px at the default and the text rows 14, 12, 10 and 8 px. Only the glyphs the panel can show (the digits, the Latin letters, `:`, `%`, `-`, `,` and `.`) are rasterized, once, at start-up, in the bar's size, in the slider row's value size and in the four sizes of the text rows. The built-in bitmap font has capitals only and draws lower case as capitals. If there is no usable font file (none of the list exists, `--font` names a file that is missing or is not a font), the panel says so once on stderr and uses the built-in 5x7 bitmap font, drawn at twice its size, so that it never fails for lack of a font. The font file is trusted: stb_truetype does no range checking of a font's offsets, so `--font` must not name a file from an untrusted source.

**Subpixel text.** On an LCD with colour stripes the text of the opaque bar is drawn per stripe, which makes it about three times sharper across than a gray anti-aliased glyph. `--subpixel auto|rgb|bgr|none` (default `auto`) chooses: `auto` uses the layout the compositor advertises in `wl_output.geometry` for the output the bar is on (picowl's `[output] subpixel`), `horizontal_rgb` giving RGB (red at the left) and `horizontal_bgr` BGR, and only when the transform tells that the stripes run across the bar: the order is worked out from the panel's native layout and the output's transform, so a landscape panel with vertical stripes (the h3900) gets horizontal subpixel text once it is shown in portrait, and a h2200 turned by 90 or 270 degrees with `--edge top` (or advertising `unknown`, `none` or a vertical stack with a normal transform, which is what hardware rotation looks like to a client) gets grayscale; the strip of a rotated output is drawn in the panel's own orientation, so there the panel's native stripes count, whatever the transform is (under hardware rotation the panel has them from `picowl-rotation-v1`, `wl_output` having the layout as the view sees it, so the strip draws subpixel text there too); `rgb` and `bgr` force an order whatever the compositor says, `none` forces grayscale. The panel follows the output when it is rotated while the panel runs. Each glyph is rasterized once, at start-up, at three times the horizontal resolution and filtered across the subpixels with a 5-tap FIR filter (14 61 106 61 14 over 256, a little wider than FreeType's default so the colour fringes of light text on the dark ground stay calm); drawing then blends the red, green and blue coverage into their channels, in linear light like the rest, in 8 bits and packed afterwards for an RGB565 buffer. The pen is on a whole pixel and so is the baseline, so equal strings are equal wherever they are drawn and the digits do not jitter when the minute changes (the font's digits are tabular), and the height of the digits is made a whole number of pixels (a vertical scale of at most a few percent) so that the flat tops and bottoms of the digits are on pixel edges. Subpixel text needs to know the colour under it: text on a translucent ground is drawn in grayscale, which is the value in the slider row while `--popup-alpha` is below 255 (the default 224 is) and the whole bar when `--bar-alpha` is below 255; the value in an opaque row has subpixels. The icons are not text and stay anti-aliased shapes. Not verified on a real panel: the stripe order of the h2200 is taken from its owner, not from a datasheet.

**Crisp style.** `--style crisp` (default `smooth`, the look described above) draws nothing anti-aliased: every pixel of the text, the icons, the slider and the highlights is one of a few flat colours (the theme's: ground, line, text, accent, highlight, track, thumb and its ring, the battery's three fills, the row's ground and line) and every edge, stem and line lies on a pixel boundary. The text is set in compiled-in bitmap fonts, so no font file is read and nothing is rasterized at run time: by default 7x13 bold for the bar and the value in the slider row, and 10x20 bold, 9x15 bold, 7x14 bold and 6x10 for the text rows, the largest in which a wording fits (the same fit-or-shorten rule as for TrueType: the row loses words before it loses size). The fonts are the X11 misc-fixed fonts, which are in the public domain (the COPYRIGHT line of each BDF file says so; `panel/fonts/LICENSES` records it), as BDF files in `panel/fonts/` reduced to printable ASCII; `panel/fonts/gen-pixfont.py` turns them into `panel/panel-pixfont-data.c` (about 10 KB of const data, covering every character the panel prints, including `>` and `~`), and `gen-pixfont.py` documents how to regenerate it. `--crisp-font fixed|dejavu` (default `fixed`) picks the text face: `fixed` is the misc-fixed set above; `dejavu` sets the same text in DejaVu Sans Bold hinted to bi-level (FreeType's v35 TrueType interpreter, mono target, no auto-hinter), baked offline into const data by `panel/fonts/gen-dejavu-bitmaps.c`, so the panel still has no FreeType and no font file at run time: Bold 11 px for the bar and the value in the slider row, and Bold 12, 11, 10 and 9 px for the text rows, with the same fit rule and the same whole-number scaling (a bar twice as tall gets glyphs twice as big). DejaVu is proportional (the hinted integer advance of each glyph and its own bearing, no kerning; the digits are all 8 px wide at 11 and 12 px, so the clock does not jitter), which changes three things. The bar's text widths follow the font like they do with a TrueType font: the clock is 36 px wide instead of 35 and the battery text 35 instead of 28, so with the default bar the battery's button and the two buttons left of it sit 7 px further left than with `fixed` (their size is the same: touch targets stay 36 px wide and as high as the bar). The rows' text area, the thumb, the track and the palette do not change. Wordings that no longer fit a size step down less often: `Wednesday 30 September 2026` is 222 px of the 224 px of the text area in Bold 12, and in `fixed` it falls to the third face (7x14 bold). The bar's digits are 8 px high instead of 9, so in the 17 rows of the bar they sit with 4 rows above and 5 below, a pixel above the middle. The DejaVu data is about 9 KB (95 characters per face, with an advance table); the misc-fixed data is about 10 KB. The bitmaps derive from Bitstream Vera, whose copyright, trademark and permission notice is shipped as `panel/fonts/DEJAVU-LICENSE` and has to go with copies of the generated data; `panel/fonts/LICENSES` records the details. Which of the two looks better on the real panel is not decided (host renders only, see [doc/design/panel-text.md](doc/design/panel-text.md)).

The icons are 12x12 bitmaps (sun, speaker with no, one or two waves, a cross when muted) drawn at a whole multiple of their grid (24x24 in the row), the battery is an exact outline of 1 px with a nub, a gap and a fill in whole columns, the highlight and the slider track are flat rectangles without their corner pixels, and the thumb is a disc of odd diameter with a 1 px ring. The thumb is 21 px and the track 5 px high in this style (22 and 6 in the smooth one), so that both have a centre pixel; nothing else of the layout, the touch targets or the behaviour differs. The ground still honours `--popup-alpha` and `--bar-alpha` (a flat alpha over the window is not anti-aliasing), but the ring of the thumb is opaque; for an exact set of colours use `--popup-alpha 255`. A bar taller than the default scales the fonts by whole numbers (twice from 38 px, three times from 58 px), never by a fraction. `--font`, `--font-size` and `--subpixel` are ignored in this style, without a message (`--crisp-font` does nothing in the smooth style). To regenerate the font data: `python3 panel/fonts/gen-pixfont.py gen panel/panel-pixfont-data.c panel/fonts/6x10.bdf panel/fonts/7x13B.bdf panel/fonts/7x14B.bdf panel/fonts/9x15B.bdf panel/fonts/10x20.bdf`; the DejaVu data needs FreeType and the font files, so `gen-dejavu-bitmaps.c` is meant to run in a debian:trixie container with `libfreetype-dev` and `fonts-dejavu-core` (the commands are in its header), and `gen-dejavu-bitmaps --check panel/panel-pixfont-dejavu.c` fails if the committed file is not what it writes. The font choice and what WinCE did for comparison are in [doc/design/panel-text.md](doc/design/panel-text.md).

**Translucency.** The row is translucent by default so that what is behind it (a video, say) shows faintly through: `--popup-alpha N` (0 to 255, default 224, about 88 percent opaque; 255 is opaque) sets the opacity of the row's ground and of the ring around the thumb, not of the icon, the text, the track or the thumb. `--bar-alpha N` (default 255) does the same for the bar. A surface with any translucent part needs an ARGB8888 buffer (premultiplied alpha, as Wayland wants it); the panel picks the format of each buffer when it allocates it: RGB565 (XRGB8888 if the compositor does not offer RGB565) whenever everything on the surface is opaque, so the closed bar with the default options is RGB565 and only the surface with the row open is ARGB8888. The opaque region of the surface is the bar when the bar is opaque (and the row when it is opaque too), so the compositor need not blend under it.

### Rotated output: the bar on the short side

A 240x320 panel turned to landscape (`[output] * = 90` or `270`) is a view of 320x240, and a bar along its top would take 18 of the 240 pixels of height. With `--edge auto` (the default) the bar goes to the edge that is the physical top of the device instead, a vertical strip: the right edge of the view at 90, the left edge at 270 (with `--bottom` the physical bottom, the other one). On the panel it looks exactly like the bar of the portrait orientation, with upright text, the same layout and the rows below it: it is drawn in the panel's own orientation, into a buffer as long as the short side (240x18, 240x54 with a row open), and `wl_surface.set_buffer_transform` (90 or 270, the output's own transform) tells the compositor how the buffer maps onto the strip, so that the buffer reaches the scanout unchanged. A row opens inward of the bar, as it opens below it in portrait. The exclusive zone is the bar's width on that edge and never the row's: a toplevel is 302x240 in the view whether the row is open or not, and the tiled layout and the keyboard pan work on that usable area (the strip is anchored to the top and the bottom as well, so it is not a keyboard to pan for). Pointer and touch positions arrive in the strip's own coordinates and are mapped back into the bar's, so taps on the buttons, drags on the sliders and the 3 s auto-close are the same as in portrait.

The panel learns the output's transform from `wl_output.geometry` and follows a rotation while it runs: it anchors to the other edge and draws into a new buffer once the compositor has configured that. Normal, 180 and the flipped transforms keep the bar on the top edge, and `--edge top` keeps it there at every rotation, along the long edge of the rotated view as before. Hardware rotation is not seen in `wl_output`: picowl then sends the transform `normal` (the display turns the picture), so the panel learns the turn from `picowl-rotation-v1` (see Protocols): picowl sends `rotation(transform, hardware, subpixel)` for the output when the panel asks, and again whenever the rotation changes at run time (the `rotate` key action). When it says the display does the turning the panel uses that transform for the strip (the h2200 with `[output] * = 90` gets the strip on the physical top of the device), and the panel's own subpixel layout from the same event: the strip is drawn in the panel's orientation, so its text is subpixel text there under hardware rotation as under software rotation, with `--edge top` or at 180 degrees the bar is in the view's orientation and the subpixel layout of `wl_output.geometry` (the view's, vertical for a portrait panel turned by 90) decides, which is grayscale for vertical stripes. With software rotation the panel reads `wl_output` as before and ignores the hint. A compositor without the global (an older picowl, another compositor) leaves the bar on the top of the view, as before. Nothing else changes for other clients: `wl_output`, the layout and the buffers are what they were.

### Touch

picowl turns the stylus into pointer events: a press is `BTN_LEFT`, a drag is motion. The panel's layer surface has a built-in `[layer.panel] hold_action = none` rule, so the press is sent at touch-down: with the default tap-and-hold it would be deferred until the stylus moved `slop_px` (8 px) and a slow drag would turn into a right click after `hold_ms`, which swallows the rest of the touch. A `[layer.panel]` section of your own merges over it.

- **A tap on a button opens its row** below the bar (the action is at the press, not the release): a slider row for the backlight and the volume, the date row for the clock, the estimate row for the battery. A tap on the same button closes it, a tap on another button switches the row to that one. A button never sets a value: a stray tap on the sun cannot darken the screen and one on the speaker cannot mute, and the clock and the battery set nothing. The battery is read again when its row opens, so the row does not show what was true up to 30 s ago.
- **The row closes by itself 3 s after the last touch** (any press, motion or release on the panel moves the deadline; a stylus that is still down keeps the row open). The timer exists only while the row is open and is not polled: when it fires it is set again for what is left.
- **In the row**, a press on the track area, which is the track plus 6 px on either side and the whole 36 px height of the row, sets the value from the x position of the stylus, motion while pressed keeps setting it and the release ends the drag; a drag keeps its slider when the stylus leaves the row. The row's icon and the value text are inert, and so is a press anywhere else on the panel. A text row has no slider: a press or a drag in it does nothing, except that it counts as a touch and so keeps the row open for another 3 s. The redraw is immediate; the value is written to the system at most every 50 ms during a drag and always at the release.
- **The surface grows, the windows stay.** Opening the row asks the compositor for a surface of bar plus row height (`set_size`) and the row is drawn when the new size has been configured and acknowledged; the old, smaller buffer stays on screen until then. The exclusive zone stays the height of the bar the whole time, so toplevels keep their size (240x302 on a 240x320 output with the default bar, open row or not) and the row covers them. The input region is the bar and, while it is open, the row; never more than that, whatever size the compositor made the surface. Closing is the same in reverse, and a redraw damages only the rectangles that changed (the track area and the value for a drag, one button for a highlight).

- **Backlight:** `/sys/class/backlight/<dev>/brightness` and `max_brightness` of the first device (by name) with a `max_brightness` of at least 1; the sysfs root honours `PICOWL_SYSFS_ROOT` as in picowl. The percent maps linearly onto the raw range, rounded to nearest, with a floor of 5 percent, so the screen cannot be turned black by accident. The level is read at start, again whenever the pointer enters the panel surface (every touch starts with an enter) and before a drag starts, so the thumb follows what picowl or another tool did meanwhile (an idle dim, the `[power.low]` cap, `brightness` written by a script); there is no timer, so a change while nobody touches the panel shows at the next touch. The `[power.low]` cap wins over the slider: on the LOW profile picowl lowers a level above the cap when the profile changes (logged by picowl), and the thumb then shows the capped level at the next touch. `brightness` must be writable by the user the panel runs as (the udev rule installed by picowl gives the `video` group write access); when a write fails (EACCES) or the mixer refuses a volume, the thumb goes back to the level the device holds and one message is logged on stderr. The panel flushes a value that is still waiting for the 50 ms throttle on every way out, including a lost display connection. If the board has several backlight devices, give picowl and the panel the same one: picowl prefers `firmware`, `platform`, `raw` (`[power] backlight`), the panel takes the first name.
- **Volume:** alsa-lib's simple mixer on the card `default`. The element is the first of `Master`, `PCM`, `Headphone`, `Speaker` that has a playback volume, else the first element with one. 0..100 percent maps linearly onto the element's raw range (not onto decibels), and raising the volume above 0 also switches the playback on (it is never switched off). Changes made by other programs are followed: the mixer's descriptors are in the poll loop, so there is no timer, and the thumb and the speaker icon move while nobody touches them (not during the panel's own drag, where the quantized value would make it jump). With no mixer or no element the button is greyed out and ignores taps (and an open row closes if the mixer goes away). The user needs access to the sound device (the `audio` group; picowl's unit runs as root).

### Time left

The battery row says how long the battery lasts or how long it takes to fill. The estimate is made by the panel from what the battery driver exports under `<sysfs root>/class/power_supply/<battery>/` (`PICOWL_SYSFS_ROOT` as everywhere), because the drivers of the handhelds this is for disagree on what they have. Every attribute is optional and any subset gives an answer.

- **Rate.** The current in uA, `current_avg` if the battery has it, else `current_now`, else the power `power_now` in uW. It is sampled every 30 s (the panel's battery poll, and once when the row opens) and smoothed in integer arithmetic: a running mean over the first eight samples, then an exponential average with weight 1/8. A sample under 10 mA (40 mW for power) is idle and is skipped, one above 3 A (15 W) is a glitch and dropped. A change between charging and discharging, a change of the attribute used, or more than 600 s between samples starts the average again, and two samples after a plug event are ignored while the current settles. The filter is kept across a suspend; there are no made-up samples.
- **Direction.** From the charger supply (a `Mains` or `USB*` supply, the same scan as for the `AC` text) and the sign of the smoothed current (negative while discharging, positive into the battery) rather than from `status` alone, which the DS2760 driver only polls every 60 s: no charger online is discharging; with one online the battery is charging above +10 mA, not charging below -5 mA, and in between what `status` says. `status` `Full` is `Fully charged` and `Not charging` is `Not charging`, with no time. Without a charger supply at all the status decides.
- **Remaining charge** (discharging, rate a current): `charge_now` less `charge_empty` (`charge_now` is the raw counter including the reserve below `charge_empty`; 0 if there is none, and never below 0), else `capacity` times `charge_full`, else `capacity` times `charge_full_design`, else `capacity` times `--battery-mah`. The charge to go (charging) is `charge_full` less `charge_now`, with `charge_full_design` and `--battery-mah` as fallbacks for `charge_full`, and the capacity in place of `charge_now`. A battery that only has a power uses `energy_now` (else `capacity` times `energy_full`, then `energy_full_design`) and `energy_full` in the same way; charge and energy are never mixed. The time is the amount times 60 over the smoothed rate, in minutes, in 64 bits.
- **`--battery-mah N`** (0 to 100000, default 0: unknown) is the capacity of the battery in mAh, the last resort when the driver exports neither `charge_full` nor `charge_full_design` (the rated capacity table of some drivers is known to be wrong, which is why a figure from the user is last).
- **The kernel's own time.** `time_to_empty_now` (seconds) is used only as a hint until the filter is warm, between a minute and 24 h and while discharging, and is shown with a `~`; after that the panel's number replaces it. It is the only number that can be shown before the first two minutes are over.

Wording, with the percentage in front when it fits: `1 h 30 min left` (60 min and more, minutes zero-padded) and `45 min left`; `~1 h 20 min left` for the kernel's hint; `1 h 15 min to full` while charging below 90 percent (above it the current-based time is optimistic, the constant-voltage taper, so the row says `Charging`); `Fully charged`; `Not charging`; `> 24 h left` and `> 24 h to full` beyond a day; `Estimating...` while the filter warms up (four valid samples, two minutes) or there is no usable rate (idle, no current attribute); `On AC power` only when a mains supply is online and there is no battery; `--` for no battery data at all. A time under a minute is never shown as `0 min`: it says `Estimating...`. Minutes are rounded to the nearest minute below 10 min, to the nearest 5 min up to 5 h and to the nearest 15 min above, and while discharging the minutes shown move only when the new value is more than max(3, shown/8) minutes away (more than 1 below 10 min), so an open row does not flicker. The row is drawn again only when its text changes.

These numbers (the 10 mA idle floor, the 3 A ceiling, four samples, weight 1/8, two skipped samples, 600 s, the thresholds of the direction, 90 percent, the rounding steps and the hysteresis) are a synthesis from how the kernel documents the attributes and from what other programs do. They are named constants at the top of the estimator in `panel/panel-logic.h`, are covered by unit tests of the arithmetic and have not been tuned on a real battery. The iPAQ h2200's chip is a DS2760 (`ds2760-battery.0`); the panel is written for the attributes its driver exports as read from the kernel source (`status`, `voltage_now`, `current_now`, `charge_full_design`, `charge_full`, `charge_empty`, `charge_now`, `temp`, `time_to_empty_now`, `capacity`: no `current_avg`, no `time_to_full_*`, no `energy_*`, so the time to full is always the panel's), and the end-to-end test models exactly that. Not verified on a real h2200: that the driver on its kernel exports those, what the numbers look like, and the accuracy of the rated capacity.

### Backlight and picowl's dimming

picowl dims the backlight after `dim_after_s` and restores the user level on the next input. It used to take the user level from the device once, at startup, so a level set by the panel was undone by the next dim. It now compares the level the device shows with the one it left it at just before it writes a reduced level (the dim, and the `[power.low]` cap when the profile changes) and adopts a different level as the user level, so a slider change survives dim and undim (`doc/power.md`, "Changes by Another Process"). The touch that undoes a dim restores the user level first and the slider write follows it, so dragging a dimmed screen works and is picked up at the next dim.

### Starting it and panel_autohide

Start it from picowl:

```ini
[autostart]
cmd = picowl-panel
```

picowl does not restart it when it exits. The panel exits with status 0 when the compositor goes away or on SIGTERM, and with status 1 and one message on stderr when it cannot connect, the compositor has no `zwlr_layer_shell_v1` or no output.

The panel is a normal layer-shell panel: `[zerocopy] panel_autohide` (default `true`) hides it while an app has the focus, and with it the bar and its slider row. With the default, the bar is only visible on an empty desktop, or after the `panel` key action forced it to show. To keep it on screen with an app open set `panel_autohide = false`; the exclusive zone then makes toplevels use the rest of the output, 240x302 below the 18 px bar on a 240x320 output, with the slider row open or not. The panel follows a rotation: picowl sends the new width and the panel lays itself out and allocates a new buffer (the open row stays open).

### Options

- `--height N`: height of the bar, clamped to 18..80 (default 18). The slider row is 36 px, or the bar plus 8 px when that is more.
- `--edge auto|top`: `auto` (default) puts the bar on the short side of an output rotated by 90 or 270 degrees, see Rotated output above; `top` keeps it on the top (or with `--bottom` the bottom) edge of the view at every rotation.
- `--bottom`: anchor at the bottom edge (default: top). The slider row then opens above the bar, so that the bar stays at the edge of the screen; the bar's and the row's 1 px lines are on the side of the windows.
- `--font PATH`: a TrueType font file instead of the default list; the built-in bitmap font when it cannot be used.
- `--font-size PX`: size of the text, 8..48 (default 11 at the default height).
- `--popup-alpha N`: opacity of the slider row, 0..255 (default 224).
- `--bar-alpha N`: opacity of the bar, 0..255 (default 255).
- `--subpixel auto|rgb|bgr|none`: the order of the colour stripes for subpixel text (default `auto`); see Subpixel text above.
- `--dump-state`: print the widget values and geometry as `key=value` lines on stdout after the first frame and exit 0 (for tests).
- `--exit-after-frame`: exit 0 after the first frame.
- `--help`.
- Test only: `--watch` is `--dump-state` that does not exit and prints the state again after every redraw, naming the widgets that were redrawn; `--inject SPEC` feeds pointer events through the same handlers as `wl_pointer` (`p X,Y` press, `m X,Y` motion, `r` release, `e X,Y` enter, `ibl` and `ivol` tap a button, `w MS` runs the event loop for MS milliseconds, `b RAW` writes the backlight as another process would, separated by `;`) after the first frame; `PICOWL_PANEL_BATTERY_POLL_S` shortens the 30 s battery interval. The removed `--scale` option is an error now: at this size the text and the thumb are sized by `--height` and `--font-size`.

### Redraws and wake-ups

A redraw happens only when a value changes or the stylus drags, and only the changed rectangles are damaged (a clock update is 40x17 px; a drag damages the track area and the value of the row, about 206x35 px). The program wakes up for the Wayland socket, once a minute for the clock, every 30 s for the battery, for the mixer, only while a slider row is open for the timer that closes it, and, only while a drag has a value waiting, for a deadline of at most 50 ms. Nothing is rasterized or allocated while it redraws: the glyphs, the icons and the parts of the slider are masks built when the panel starts or its layout changes (a unit test counts the heap across 300 redraws), and a redraw blends them. A resize (rotation, a row opening or closing) lays out again and allocates a new buffer, which is where the masks are rebuilt if their size changed; the surface keeps one wl_shm buffer at a time and the old one is released after the new one is attached.

### Not implemented

Everything else in `doc/panel.md`: popups and tap-and-hold menus, the app list, the network and the other widgets, and brightness through a picowl protocol instead of sysfs.

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

# built in too: the panel's sliders are dragged, so its presses are not deferred
[layer.panel]
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
- `focus`: one of the two `stack` app_ids, default empty. That app keeps the keyboard when the other one maps (and when the pair forms), so a player which starts or restarts later does not take the keys of the on-screen keyboard from the terminal. The other window maps without being activated. Touching a tiled window and alt+Tab still move the keyboard as usual, and apps outside the stack take the focus when they map. A name which is not one of the two in `stack` is logged and ignored, whatever the order of the two keys in the file.
- `second_min`: integer percent, 25..75, default 50. The share of the split axis the second window keeps at least, so a terminal next to a player always gets at least half the width (landscape) or height (portrait) whatever the player's aspect ratio. A value outside 25..75 or not a number is logged and the previous value (the default 50) is kept. `second_min = 25` gives the earlier behaviour.
- `aspect` (in `[app.<app_id>]`): `W:H`, two integers from 1 to 4096, the natural aspect of the first app. A bad value is logged and ignored.
- `pan`: layer-shell namespaces, comma separated, default `wvkbd`. A mapped surface in one of these namespaces which is anchored to the bottom edge and not to the top (the on-screen keyboard, with or without an exclusive zone; the shipped wvkbd-ipaq asks for none) pans the tiled pair instead of shrinking it, see Keyboard below. An empty value (`pan =`) turns panning off, and the keyboard shrinks the usable area for the pair like for every other window. Names match exactly and case-sensitively, at most 8, each shorter than 64 characters without spaces inside; an empty name between commas, a name with a space or too many names is logged and the previous value is kept.

Rules:
- **Order**: in a portrait usable area (height at least width) the first app is on top and the second below, both at full width. In a landscape one the first is on the left and the second on the right, both at full height.
- **Size of the first window** along the split axis: its natural aspect ratio applied to the available extent. The source is, in this order, the client's own fixed size (xdg-shell `min_size` equal to `max_size`, both non-zero, re-read whenever the client commits a change, so a new clip with another aspect re-lays out), then `aspect` from the config, then half. The result is clamped on both split axes (top and bottom in portrait, left and right in landscape): the second window keeps at least `second_min` percent of the extent (50 by default, so the first gets at most half) and the first at least 25 percent, because a hint is only a preference and neither window may be squeezed. The clamp only shrinks the first window: one whose natural size is smaller than its cap keeps exactly that size. Where rounding would make the two limits collide the 25 percent of the first wins. The second window gets the rest. Each client receives the exact size in a configure and draws its own letterboxing if its content has another aspect.
- **One of the two**: with only one of the apps mapped, nothing changes: it is maximized like any other window. When the second appears or goes away both are configured again.
- **Other windows**: apps outside the stack are not affected. A window raised above the pair covers both.
- **Focus**: touching a tiled window gives it the keyboard. alt+Tab (the `cycle` action) moves the keyboard between the two tiled windows, without restacking, while one of them has the focus; it does not reach other apps then (use the panel). Focusing one tiled window raises both together. With `focus` set, a window of the other stack app which maps does not take the keyboard from the named app, and the pair raises as if the named app had been focused.
- **Keyboard**: while both apps are mapped and split top and bottom (portrait), a bottom anchored layer surface named by `pan` does not shrink the pair. The windows are laid out on the usable area without that surface's exclusive zone, keep that size and receive no configure, and the whole stack is moved up by the height the keyboard takes at the bottom (its exclusive zone plus margin when it asks for one, else its height plus its bottom margin; a keyboard which is hidden by attaching no buffer takes nothing): the lower window ends where the keyboard starts and the upper one slides partly or wholly off the top of the screen. The move is limited so that the lower window is never pushed above the top of the screen; a keyboard taller than the room above the lower window leaves the rest of it under the keyboard. When the keyboard hides, or the pair breaks up because one of the apps goes away, the move ends: the stack returns, or the surviving window is maximized into the area the keyboard leaves (the whole output, for a keyboard without a zone). Only the tiled windows move. Every other window, a single app of the pair on its own, and a pair split side by side (landscape, where both windows span the height and each would lose its top) are resized to the usable area, which has the keyboard's zone taken off (none, for a keyboard without a zone, which then overlays them), as without a stack. Other exclusive surfaces (a top or bottom panel, or a keyboard whose namespace is not in `pan`) keep shrinking the usable area, and the pair is laid out on what they leave. Touch and pointer coordinates follow the moved windows, because the surface under a point and its local coordinates come from the scene node positions; a touch or tablet tool held on a window keeps reporting in the coordinates of that window as it moves, since the window's origin is read from its scene node on every motion. xdg popups of tiled windows are constrained, when they first map, to the usable area (the screen above the keyboard) using the moved window's position, and an open popup is not moved when the keyboard shows or hides, as with a maximized window. Text input popups (`src/imrelay.c`) are placed again on every layout change, from the moved position of the focused surface and inside the usable area.
- **Rotation and panel**: the layout is recomputed whenever the output size, its transform or the usable area changes, the same as for maximized windows. In landscape the panel's strip is on a short edge of the view (Panel, Rotated output), so the usable area loses 18 px of width, not of height: a 320x240 view tiles into 302x240, side by side.
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
- `wl_seat`, `wl_keyboard`, `wl_pointer`, `wl_touch` (if available): input focus. The seat always advertises the keyboard capability, also with no keyboard device (headless, or before one is plugged in), and always has an active keyboard with a keymap: an internal keyboard that never produces events holds picowl's xkb keymap, and takes over when the active keyboard is destroyed (the on-screen keyboard's virtual keyboard, an unplugged device) until a real keyboard sends its next key. A client that creates its `wl_keyboard` at any time therefore gets an xkb_v1 keymap and not none (a client without a keymap ignores all keys). A client that probes the capability to learn whether a keyboard is attached gets no such answer. Tested in the smoke test with `pw-test-client --expect-keymap`, also after a virtual keyboard was destroyed.
- `zwp_virtual_keyboard_manager_v1`: on-screen keyboards (software input methods). Kept next to text-input and input-method: GTK+2 applications have no text-input and on-screen keyboard function keys still inject key events through it.
- `zwp_text_input_manager_v3` and `zwp_input_method_manager_v2` (`src/imrelay.c`, `protocols/input-method-unstable-v2.xml`): the globals are advertised. Keyboard focus is relayed to text-input: when the keyboard focus changes, the text inputs of the client that lost it get `leave` and those of the client that gained it get `enter`, also for a text input created after the client already has the focus. The text input of the focused client drives the input method: `enable` becomes `activate`, followed by surrounding text, content type and change cause (only the parts the client used) and `done`; every later `commit` resends that state; `disable`, losing the focus or destroying the text input becomes `deactivate`. What the input method commits (`delete_surrounding_text`, `commit_string`, `preedit_string`) goes to the active text input, followed by `done`. At most one text input is active, and only one whose surface has the keyboard focus. Only one input method is accepted per seat; a second one gets `unavailable`. Input method popup surfaces (candidate lists) are shown in the overlay layer below the text cursor rectangle of the active text input, above it when there is no room below, clamped to the usable area of the output the cursor is on (so they stay clear of exclusive layer surfaces), and told where the cursor is inside them with `text_input_rectangle`; they are placed again when the text input commits, the popup commits or the layout is arranged (rotation, panel changes), and hidden while the input method is inactive. Tapping a popup does not move the keyboard focus. **Not implemented:** the input method keyboard grab (`grab_keyboard`, for engines that want raw keys): the request is logged and the grab object is destroyed, so keys always go to the focused application (an on-screen keyboard injects them through virtual-keyboard); and touch hold rules for popups (a long press on a popup uses the global `[touch]` hold defaults). An on-screen keyboard that uses input-method must use layer-shell keyboard interactivity `none`, or tapping it moves the focus away and hides it again. Tested by `tests/pw-im-client.c` (two connections: application and input method) in the smoke test. See `doc/design/osk.md` part 3.
- `xdg_activation_v1`: app activation requests.
- `zwp_tablet_manager_v2`: drawing tablets with libinput (`tablet.c`). Proximity, tip, motion, pressure, distance, tilt, rotation, slider, wheel and button events go to the surface under the tool when its client bound tablet-v2, and that surface keeps the tool while a tip or button is down; pads get focus with the tool and forward buttons, rings and strips. Tablet coordinates are mapped to the output and follow its rotation like touch does (software rotation by wlroots, hardware rotation by the libinput calibration matrix). A tablet that cannot follow a hardware rotation because it has no calibration matrix is logged and ignored until the output is unrotated. Events are dropped while the display is leased or blanked (the waking event is swallowed). There is no pointer emulation for clients without tablet-v2, no configuration keys, and tablet tools do not set the cursor image.
- `wp_cursor_shape_manager_v1` (version 1): only the client with pointer focus (or the surface a tablet tool is in) is heard. `wait` and `progress` show the hold animation (stopped when the client asks for another shape or loses focus); every other shape shows no image, as picowl draws no pointer. The animation of a touch hold is never replaced or stopped by a client request.

**Output & Rendering:**
- `wl_output`: output geometry, mode, subpixel, transform.
- `picowl_rotation_manager_v1` (`protocols/picowl-rotation-v1.xml`, `src/rotationproto.c`): how an output is turned, including by the display. `get_rotation(wl_output)` gives a `picowl_rotation_v1` that sends `rotation(transform, hardware, subpixel)` at once and on each change: the `wl_output.transform` that turns the panel as built, whether the display does it, and the panel's native `wl_output.subpixel`. It is additive: it does not change `wl_output`, so a client that does not bind it sees what it saw before. picowl-panel is the one client that uses it (see Panel).
- `wp_presentation`: frame timing hints (via wlr_presentation).
- `wp_viewporter`: scaling and cropping (via wlr_viewporter).
- `zxdg_output_manager_v1`: the logical position and size of each output, from the output layout (via wlr_xdg_output_v1). Screenshot tools such as grim need it; without it they guess a zero-sized output. With hardware rotation the logical size is the rotated one, as clients see it.
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

Tests: `config`, `tile` (the layout arithmetic), `tile-e2e` (two toplevels over one connection, landscape and portrait, and with the panel's strip on a short edge: the configure sizes, hint changes at run time and a partner going away), `zbquota`, `smoke` (headless run, also with `--zerocopy`, `--zerocopy-count`, `--probe` and `--expect-global` for the always-on globals), `bufproto` (bind events and version gating over a socketpair, then `pw-test-client` at version 2 and 1), `touchhold`, `pointercal` (tslib parsing and matrix conversion), `cursorfit`, `cursor-builtin`, `cursorshape` (shape mapping), `rotate`, `copytype`, `copyrel`, `pixman-pass`, `pixman-dmabuf`, `rss`, `backlight`, `powersupply`, `dim`, `power-e2e`, `panel` and `panel-e2e` (picowl-panel, see below), `leasepolicy`, `capture` checks inside `smoke` (`pw-capture-client`: no screencopy global by default, the background colour when `[capture]` is enabled), and `lease-vkms` (suite `vkms`: opt-in with `PW_LEASE_VKMS=1`, needs root and the vkms module, skips otherwise).

- **rss:** Memory test. Starts compositor headless (1280×720), maps test client, measures VmHWM. Fails if peak RSS exceeds ceiling (meson option `-Drss_ceiling_kb`, default 12288 kB; headless baseline ~9.5 MB). Override with `PW_RSS_CEILING_KB` for a single run.

- **panel:** the wayland-free parts of `picowl-panel` (value mapping, text, layout, touch state machine, font, drawing in RGB565 and XRGB8888 into a canvas with guard bytes, sysfs access on fake trees).
- **pointer fixture:** `PICOWL_TEST_VIRTUAL_POINTER=1` in picowl's environment makes it offer `zwlr_virtual_pointer_manager_v1`, so that a test can put the pointer at a place of the output (`tests/pw-pointer-client.c`); the headless backend has no pointer of its own. It is a test fixture and is never on in a session: any client could inject input through it.

- **panel-e2e:** `picowl-panel` against headless picowl at 240x320 (`PICOWL_HEADLESS_SIZE`) with a fake `PICOWL_SYSFS_ROOT` tree (backlight 600 of 1023, battery 73 percent) and `tests/pw-fake-ctl.c`, an alsa-lib control plugin whose state is a file, standing in for a sound card. It checks the first frame (`--dump-state`), touch through `--inject` (values reach sysfs and the mixer, the 5 percent floor, a drag from outside, a greyed slider), the usable area of a second client (top, bottom, `--height`, restored when the panel exits), that an idle panel does not redraw or wake up, that battery and external volume changes are followed, the relayout after a rotation (`pw-key-client` presses the `rotate` binding), and the exit codes (no compositor, `tests/pw-bare-server.c` without a layer shell, no output, compositor gone). With `PW_PANEL_TEST_SETTIME=1` (root only: it sets the system clock and puts it back) it also checks that the clock follows a change of the system time and the minute timer. Section V is the panel on an output rotated by 90 and 270 degrees with software rotation: the raw 240x320 scanout of the bar, the slider row and the date row is compared pixel by pixel with the portrait one (the same inject tokens), and a pointer put at the logical position of the physical bar's clock, the backlight button and the slider (the pointer fixture) opens the date row, drags the value and lets the row close after 3 s. Section W repeats the pixel comparison, the placement, taps and a rotation at run time on the path hardware rotation takes: `PICOWL_TEST_HW_ROTATION=1` (headless, with `PICOWL_HEADLESS_SIZE`) makes the output advertise as with hardware rotation (the rotated mode, the transform `normal`, the subpixel layout as the client sees it, `hw_rotation` set, `picowl-rotation-v1` saying hardware) while nothing turns the frame, and the capture client turns the capture back by the plane's rotation (`--plane 90|270`); `PICOWL_TEST_NO_ROTATION_HINT=1` leaves out the global to test the panel's fallback. What a plane does to the picture is not tested there.

### Hardware Validation Checklist

DRM paths (rotation, copy-type, swapchain, direct scanout) cannot run in the build container (no /dev/dri, no vkms); they are compiled and unit-tested only. On a real device, verify:

- Hardware rotation: the hardware rotation commit succeeds, touch calibration correct, no "hw rotation commit failed" log
- Touch calibration: touch the four corners of the screen and expect the pointer at the matching corners, in all four rotations, with software rotation and with hardware rotation; with no pointercal, no `[touch] calibration` and no udev matrix, expect the one-time error and a dead touchscreen
- Panel strip: with `[output] * = 90` (and 270) and hardware rotation the panel is on the strip on the physical top of the device, via `picowl-rotation-v1` (`picowl-panel --dump-state` prints `rotation hint=hardware`); check that the bar is upright and on the short edge, that a tap on it works, that the subpixel text has no colour fringes of the wrong order, and that the `rotate` key action moves it. The pixel path is tested headless with the plane emulated (`PICOWL_TEST_HW_ROTATION`), the real plane and the touch matrix are not
- Copy-type single-buffer: swapchain stays at 1 slot, no "degraded to 2 slots" log
- Direct scanout: scene logs it, render list is 1 entry, no composition
- Damage clipping: only changed regions copied to VRAM
- DRM lease: the checklist in `doc/lease.md`
- Tiled layout: with `[layout] stack = mediaplayer, havoc`, start both on the panel in portrait and in landscape: expect the first app on top (left), the second below (right), touching either gives it the keyboard, alt+Tab moves the keyboard between them, the on-screen keyboard pans the pair up (portrait) without the player rescaling and the terminal above the keyboard stays fully visible, rotating re-tiles, closing one maximizes the other and the log shows no direct scanout while both are visible
- Panel: with `[autostart] cmd = picowl-panel` and `[zerocopy] panel_autohide = false`, drag the stylus across each slider: the thumb follows the stylus, the backlight and the volume change while dragging and stay at the level of the release, a touch on the clock opens the date row and a touch on the battery the time left, the sound card's mixer shows the same volume (`amixer`), a volume change with `amixer` moves the thumb, the backlight keeps the level through an idle dim and the touch that undims, and rotating redraws the panel at the new width

See the hardware-only checklist in `doc/zero-copy.md`.

## OpenEmbedded

For embedded systems using OpenEmbedded (wrynose/blacksail releases), picowl includes pre-configured bitbake recipes. See `oe/README.md` for adding the layer, configuring the machine/distro, and building.

Key points:
- Recipes: `recipes-graphics/picowl/picowl_git.bb`, `recipes-graphics/wlroots/wlroots_0.19.0.bb`.
- Layers: compatible with `wrynose` and `blacksail` (LAYERSERIES_COMPAT).
- `S = "${UNPACKDIR}/picowl"` (git subpath: will change when picowl moves to its own repo).
- Requires `wayland` DISTRO_FEATURE; `systemd` for the unit file.
- Installs `picowl.service` (not auto-enabled by default) and `/etc/picowl.ini`.
- The panel is built by default (PACKAGECONFIG `panel`, depends on `alsa-lib`) and packaged as `picowl-panel`; add that package to the image and start it from `[autostart]`.
- Avoid Thumb mode on ARM (set `ARM_INSTRUCTION_SET = "arm"` globally or per-package).

## License

MIT. See `LICENSE` in the picowl root.
