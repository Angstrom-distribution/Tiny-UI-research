# picowl

A tiny wlroots 0.19 Wayland compositor optimized for GPU-less handhelds: HP iPAQ PDAs (SA-1110/PXA25x/PXA270, no GPU, no FPU, 64 MiB RAM) with 240×320/320×240/480×640 RGB565 panels on slow display buses. Single-threaded, damage-driven software rendering via pixman, targeting sub-100ms latency and <2% idle CPU. Designed for embedded developers working with extreme constraints: read `../docs/ipaq-ui/` for the research, architecture, and hardware profiles.

## Status

- **Working**: basic window management (xdg-shell toplevels maximized to usable area), layer-shell (panel, overlay, background), on-screen keyboard input (virtual-keyboard protocol), keyboard navigation (alt+Tab to cycle, logo+Escape to quit, Power key to blank), idle timeout and screen blanking, per-output rotation via config, frame-damage commits only, pixman rendering with RGB565/XRGB8888/ARGB8888 format selection.
- **Planned**: zero-copy surface buffer model (mmap_ptr to avoid per-frame pixel copies on RGB565), MediaQ hardware rotation and pixel doubling (routes through DRM/KMS layer), video playback offload handshake (apps signal raw buffer availability for direct-to-framebuffer paths).
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
- `wayland-server`, `wayland-protocols >= 1.32`, `xkbcommon`, `pixman-1`, `libdrm`
- C11 compiler, meson >= 1.3

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

### [idle] section

- `timeout_ms = <milliseconds>`
  - Milliseconds of keyboard/pointer inactivity before the screen is blanked. 0 disables blanking. Default: 60000 (60 seconds).

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
- `<action>`: `spawn`, `cycle`, `close`, `blank`, `rotate`, `quit`.
  - `spawn <command>`: fork and exec `/bin/sh -c <command>` (detached, no zombies).
  - `cycle`: raise the next mapped toplevel in stacking order.
  - `close`: send a close request to the focused toplevel.
  - `blank`: toggle screen on/off (same as Power key or output-power-management requests).
  - `rotate`: cycle output rotation (planned, not yet implemented).
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
