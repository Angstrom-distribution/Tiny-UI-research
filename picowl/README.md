# picowl

A tiny wlroots 0.19 Wayland compositor optimized for GPU-less handhelds: HP iPAQ PDAs (SA-1110/PXA25x/PXA270, no GPU, no FPU, 64 MiB RAM) with 240×320/320×240/480×640 RGB565 panels on slow display buses. Single-threaded, damage-driven software rendering via pixman. Designed for embedded developers working with extreme constraints: read `../docs/ipaq-ui/compositor.md` and `../docs/ipaq-ui/hardware.md` for the architecture, hardware rotation, memory optimization, and device profiles. Implementation details in `doc/buffers.md`, `doc/zero-copy.md`, and `subprojects/packagefiles/wlroots/README.md`.

## Status

- **Working**: basic window management (xdg-shell toplevels maximized to usable area), layer-shell (panel, overlay, background), panel autohide while an app is fullscreen, on-screen keyboard input (virtual-keyboard protocol), keyboard navigation (alt+Tab to cycle, logo+Escape to quit, Power key to blank), idle timeout and screen blanking, per-output rotation (hardware via the DRM plane `rotation` property where available, else software), copy-type detection (mq11xx, w100, sa1100-lcdc) with a single-buffer swapchain, picowl-buffer-v1 zero-copy client buffers, linux-dmabuf feedback, direct scanout, malloc tuning, frame-damage commits only, pixman rendering with RGB565/XRGB8888/ARGB8888 format selection.
- **Planned**: video playback offload handshake (apps signal raw buffer availability for direct-to-framebuffer paths), C8 (8-bpp palettised) output on MediaQ (design and options documented in `doc/zero-copy.md`), fixing the per-commit `wlr_client_buffer` allocation in wlroots core. Hardware rotation, copy-type swapchain, and direct scanout are compiled and unit tested; the hardware paths still need the checklist in `doc/zero-copy.md` run on a device.
- **Known constraints**: no general cursor theme (only the tap-and-hold wait animation is drawn by picowl), no window animations or transitions, no composited blur/fade (CPU cost), single fixed render format per build.

## Build

```sh
# Set up the build environment to use the hosted wlroots 0.19 prefix:
source /tmp/claude-0/-home-user-repo/d2efe079-ffd0-5854-a788-66118881eac2/scratchpad/hostprefix/env.sh

meson setup build
ninja -C build
# or: meson setup -Dwlroots:default_library=static build  # for static link
```

**Dependencies:**
- `wlroots 0.19` (fetched as meson subproject fallback if not installed)
- `wayland-server`, `wayland-protocols >= 1.32`, `xkbcommon`, `pixman-1`, `libdrm`, `libinput`
- C11 compiler, meson >= 1.3

picowl carries three wlroots patches (`subprojects/packagefiles/wlroots/`, applied by the wrap and by the OE recipe); see `subprojects/packagefiles/wlroots/README.md` for patch details and `doc/zero-copy.md` for the buffer model. libinput is linked directly (touch calibration).

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
picowl [-c /etc/picowl.ini] [-d 2]  # -d N sets wlroots log level (0-7)
```

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

Log output goes to stderr; use `-d 0` for WLR_ERROR only, `-d 3` for WLR_DEBUG, etc.

## Configuration

Config file is INI format, read from `$XDG_CONFIG_HOME/picowl/picowl.ini` (default `~/.config/picowl/picowl.ini`), then `/etc/picowl.ini`, or a path specified via `-c`. See `data/picowl.ini.example` for a template.

### [render] section

- `format = RGB565 | XRGB8888 | ARGB8888`
  - Preferred pixel format for rendering. If the backend cannot provide it, falls back to the backend default (usually the scanout format). Default: RGB565.

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

- `auto` (default): hardware rotation when the primary plane has the `rotation` property (MediaQ mq11xx only), else software.
- `hardware`: require hardware rotation; logs an error and falls back if unsupported.
- `software`: always rotate in the renderer.

If the hardware result has the wrong size, picowl logs "hw rotation size mismatch" and uses software rotation. Hardware rotation disables hardware cursors (cursor plane has no rotation property); software cursor (hold animation) is used. See `../docs/ipaq-ui/compositor.md § 5.3` for implementation details and `../docs/ipaq-ui/hardware.md § 5` for per-device rotation capabilities.

### [copytype] section

Per-output override of copy-type detection: `<output-name> = auto | yes | no`. `auto` detects the driver via `drmGetVersion`:
- Copy-type (damage-clipped copy to VRAM): `mq11xx`, `w100`, `sa1100-lcdc`
- Scanout (direct DMA read): `pxa-lcdc` and others

Copy-type outputs use a single-buffer swapchain (kernel copies damage rectangles); scanout outputs may degrade to double-buffer if composition is needed. See `../docs/ipaq-ui/hardware.md` for device-specific details.

### [zerocopy] section

- `enable = true|false`: advertise picowl-buffer-v1 and custom linux-dmabuf feedback (default true). See `doc/buffers.md` for the protocol and client usage.
- `single_buffer = true|false`: single-buffer swapchain on copy-type outputs when safe (RGB565, copy-type driver, no format change needed; default true). See `../docs/ipaq-ui/compositor.md § 5.4` for swapchain management.
- `panel_autohide = true|false`: hide the panel while an app is focused so a single fullscreen buffer can be scanned out directly (reduces render list to 1 entry; default true). See `../docs/ipaq-ui/compositor.md § 7` for panel autohide behavior.

### [memory] section

malloc tuning via `mallopt`, applied before and after config is read. Reduces memory fragmentation and heap padding on constrained devices:

- `arena_max` (default 1): Number of malloc arenas (1 per CPU core by default; constrain to 1 on single-core to reduce fragmentation).
- `trim_threshold_kb` (default 256): Bytes of excess before malloc_trim() on idle.
- `mmap_threshold_kb` (default 128): Size threshold for mmap-allocated blocks (range 16..4096 so malloc does not mmap tiny blocks).
- `top_pad_kb` (default 16): Extra bytes reserved at heap top (range 0..65536).
- `trim_after_start = true|false`: Call `malloc_trim(0)` once after startup (default true).

Out-of-range values are rejected and the default is kept. Measured VmHWM (headless, 1280×720): 9.5 MB (baseline 9.4 MB). See `../docs/ipaq-ui/compositor.md § 5.4` for memory optimization strategy and `tests/rss.sh` for the RSS measurement test.

### Power Management and Idle Timeouts

Power-aware idle timeout and backlight dimming, configured via `[power]` and `[power.ac]`, `[power.battery]`, `[power.low]` sections. Screen transitions through ACTIVE → DIMMED → BLANKED states based on inactivity and power profile (AC, BATTERY, LOW). All timings and backlight device selection are documented in **[doc/power.md](doc/power.md)**. The legacy `[idle]` section is supported for backwards compatibility; prefer the `[power.*]` sections.

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
cmd = picowl-osk
```

### [keybindings] section

Keyboard shortcuts. Format: `<modifiers>+<key> = <action> [command]`.

- `<modifiers>`: zero or more of `alt`, `ctrl`, `shift`, `logo` separated by `+`.
- `<key>`: XKB keysym name (e.g., `Tab`, `F4`, `Return`), or a numeric evdev keycode as `code:116` (KEY_POWER).
- `<action>`: `spawn`, `cycle`, `close`, `blank`, `rotate`, `panel`, `quit`.
  - `spawn <command>`: fork and exec `/bin/sh -c <command>` (detached, no zombies).
  - `cycle`: raise the next mapped toplevel in stacking order.
  - `close`: send a close request to the focused toplevel.
  - `blank`: toggle screen on/off (same as Power key or output-power-management requests).
  - `rotate`: cycle output rotation of the first output (normal, 90, 180, 270; keeps the flipped bit).
  - `panel`: toggle the panel while panel autohide is active (forces it visible until focus changes).
  - `quit`: exit the compositor.
- `[command]`: optional shell command for the `spawn` action.

**Built-in defaults** (if no keybindings are defined in the config):

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
```

## Tap-and-hold and Cursor

### Tap-and-Hold Behaviour

Touch input on picowl supports a tap-and-hold gesture (enabled by default, configurable via `[touch]`):

- **Touch-down**: Focus the surface at the touch point and send pointer enter + motion, but defer the left-button press.
- **Tap** (touch-up before hold triggers): Send left button press and release as a single click.
- **Drag** (movement > `slop_px` before hold triggers): Send the deferred left press and normal motion; no hold animation.
- **Hold animation** (after `hold_delay_ms`): Display a wait cursor at the touch point, stepping frames via a timer at ~12 fps (configurable).
- **Right-click** (after `hold_ms` total from touch-down): Hide the animation, send right button press and release (Pocket PC convention; GTK2 sees button 3), and swallow remaining touch events.
- **Lift during animation**: Counts as a tap (left press + release) only if the hold had not yet triggered.

The behaviour is controlled by the `[touch]` section (see `data/picowl.ini.example`):

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `hold_action` | `right-click` | `right-click`, `none` | Enable right-click on hold (`none` = immediate left press like traditional pointer) |
| `hold_delay_ms` | 300 | integers | Milliseconds before the hold animation starts |
| `hold_ms` | 900 | > `hold_delay_ms` | Milliseconds from touch-down to right-click |
| `slop_px` | 8 | 0..64 | Movement tolerance in pixels; exceeding this cancels hold and triggers drag |

### Cursor Configuration

Picowl draws a cursor **only during the tap-and-hold animation**. The animation is configured in the `[cursor]` section:

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

- `picowl_buffer_manager_v1` / `picowl_buffer_v1` (`protocols/picowl-buffer-v1.xml`): compositor-allocated RGB565 dmabufs, `copy_type`, `copied` (direct scanout on copy-type outputs) and `retained` events so clients on copy-type outputs can use a single buffer until the compositor composites.
- `zwp_linux_dmabuf_v1` v4 with hand-built feedback (RGB565/LINEAR, scanout tranche).

Picowl advertises and implements (via wlroots 0.19):

**XDG Shell & Core Composition:**
- `xdg_wm_base` (xdg-shell v1): toplevel windows, popups, activation.
- `wl_compositor`, `wl_subcompositor`: surface trees.
- `zwlr_layer_shell_v1`: panel, overlay, background, lock surfaces with exclusive keyboard interactivity.
- `wl_data_device_manager`: copy/paste clipboard.

**Input & Interaction:**
- `wl_seat`, `wl_keyboard`, `wl_pointer`, `wl_touch` (if available): input focus.
- `zwp_virtual_keyboard_manager_v1`: on-screen keyboards (software input methods).
- `zwp_virtual_pointer_manager_v1`: remote pointer input (if supported).
- `xdg_activation_v1`: app activation requests.

**Output & Rendering:**
- `wl_output`: output geometry, mode, subpixel, transform.
- `wp_presentation`: frame timing hints (via wlr_presentation).
- `wp_viewporter`: scaling and cropping (via wlr_viewporter).
- `zwp_single_pixel_buffer_v1`: solid-color surfaces (useful for backgrounds).

**Window Management:**
- `zwlr_foreign_toplevel_manager_v1`: taskbar/panel integration (list, activate, close, minimize).
- `zxdg_decoration_manager_v1`: window decoration hints (picowl always chooses server-side, i.e., no decoration).

**Power & Idle:**
- `zwlr_output_power_management_v1`: screen blanking requests (dim/off).
- `org_kde_kwin_idle_notify` (formerly `org_kde_kwin_idle`): idle timeout notifications (used by screen locker).

## Testing

```sh
source <hostprefix>/env.sh
meson setup build && meson test -C build
WLR_SCENE_DISABLE_DIRECT_SCANOUT=1 meson test -C build   # force composition
```

### Automated Tests (Headless)

Tests: `config`, `smoke` (headless run, also with `--zerocopy`), `rotate`, `copytype`, `copyrel`, `pixman-pass`, `pixman-dmabuf`, cursor and touch-hold unit tests, and `rss`.

- **rss:** Memory test. Starts compositor headless (1280×720), maps test client, measures VmHWM. Fails if peak RSS exceeds ceiling (meson option `-Drss_ceiling_kb`, default 12288 kB; headless baseline ~9.5 MB). Override with `PW_RSS_CEILING_KB` for a single run.

### Hardware Validation Checklist

DRM paths (rotation, copy-type, swapchain, direct scanout) cannot run in the build container (no /dev/dri, no vkms); they are compiled and unit-tested only. On a real device, verify:

- Hardware rotation: `check_hw_size` succeeds, touch calibration correct, no "hw rotation size mismatch" log
- Copy-type single-buffer: swapchain stays at 1 slot, no "degraded to 2 slots" log
- Direct scanout: scene logs it, render list is 1 entry, no composition
- Damage clipping: only changed regions copied to VRAM

See the hardware-only checklist in `doc/zero-copy.md`.

## OpenEmbedded / Yocto

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
