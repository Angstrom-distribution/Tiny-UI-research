# picowl

picowl is a small wlroots 0.19 Wayland compositor for handhelds without a GPU, written for the HP iPAQ PDAs (SA-1110, PXA25x and PXA270; no FPU; 64 MiB of RAM; 240x320, 320x240 and 480x640 RGB565 panels). It is single-threaded, renders in software with pixman and draws only what was damaged. Next to the compositor this tree carries the pieces a usable handheld session needs: `picowl-panel`, an on-screen keyboard patch series, terminal and media player integration, five wlroots patches and OpenEmbedded recipes.

The reference board is the iPAQ h2200: a 400 MHz PXA255, and a MediaQ MQ1188 display controller whose video memory sits behind a bus that moves 7.9 to 9.1 MB/s, so a full 240x320 RGB565 frame (153,600 bytes) takes about 17.5 ms to upload while the CPU barely runs (`doc/design/mediaq-c8.md`). That bus, not the renderer, is the constraint: every design choice here (damage tracking everywhere, one shared render format, a single buffer on copy-type displays, zero-copy client buffers, hardware rotation, a panel that redraws one widget at a time) exists to move fewer bytes. Input is a resistive stylus, which is why touch calibration is mandatory and why a long press becomes a right click.

The architecture, the hardware rotation background, memory optimisation and the device profiles are in `compositor.md` and `hardware.md` of the [ipaq-ui research](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui) (branch `docs/ipaq-ui-research` of this repository). The implementation details are in `doc/` (index below), the design plans and the research in `doc/design/`, and everything not done yet in the [roadmap](doc/design/roadmap.md).

| Document | What it covers |
|---|---|
| [doc/buffers.md](doc/buffers.md) | `picowl-buffer-v1` and how a client uses it |
| [doc/zero-copy.md](doc/zero-copy.md) | the buffer model: copy-type outputs, single buffering, direct scanout, memory tuning, the hardware checklist |
| [doc/lease.md](doc/lease.md) | the DRM lease to the media player |
| [doc/capture.md](doc/capture.md) | screen capture |
| [doc/power.md](doc/power.md) | power profiles, dimming, blanking, idle inhibitors |
| [doc/cursors.md](doc/cursors.md) | the hold animation, cursor rules and `picowl-cursor-convert` |
| [doc/panel.md](doc/panel.md) | the panel's widget design |
| [doc/mediaplayer-integration.md](doc/mediaplayer-integration.md) | what the media player needs from picowl |
| [doc/rotation-measurement.md](doc/rotation-measurement.md), [doc/rotation-results.md](doc/rotation-results.md) | the hardware against software rotation measurement on an h2200 |
| [doc/design/README.md](doc/design/README.md) | index of the design plans and research |
| [subprojects/packagefiles/wlroots/README.md](subprojects/packagefiles/wlroots/README.md) | the five wlroots patches |
| [oe/README.md](oe/README.md) | the OpenEmbedded layer |

## Status

"Headless tests" means the test suite (see Testing) in a clean Debian trixie container; "h2200" means a measurement or capture recorded in this repository; "no board result" means the code builds and its headless or unit tests pass, but no document here records it running on a board.

| Area | State | Verified where |
|---|---|---|
| Window management: xdg-shell toplevels maximized to the usable area, one shown at a time, alt+Tab cycling, layer-shell | working | headless tests |
| Two-app tiled layout with keyboard pan (`[layout]`) | working | headless tests (configure sizes and pixels); no board result |
| Software rotation, `rotate` key action | working | headless tests; h2200 (rotation measurement) |
| Hardware rotation through the DRM plane (`[rotation] auto`, the default) | working | h2200 at 90 degrees and normal (`doc/rotation-results.md`); 270 degrees and touch under rotation not measured |
| Copy-type outputs (mq11xx, w100, sa1100-lcdc) with damage-clipped uploads | working | unit tests; on the h2200 the mq11xx uploads were measured during the rotation measurement, but whether picowl ran its copy-type path then, the single-buffer slot count and direct scanout are not recorded |
| `picowl-buffer-v1` zero-copy buffers, buffer limits, `caching` event | working | unit tests and a protocol test; the headless backend has no DRM, so the global is not offered there; no board result |
| Touch calibration (tslib pointercal, `[touch] calibration`) | working | unit tests; no board result recorded here |
| Tap-and-hold with the wait animation, per-app and per-layer rules | working | unit tests; no board result recorded here |
| Power profiles, dimming, blanking, idle inhibitors | working | unit tests and `power-e2e` against a fake sysfs |
| DRM lease to the media player | working | `lease-vkms` (opt-in, vkms); no board result |
| Screen capture (`[capture]`) | working | headless tests |
| Text input and input method relay | working, without the keyboard grab | headless tests |
| On-screen keyboard supervision (`[osk]`) and the wvkbd-ipaq patches | working | `osk-e2e` with a stand-in; the wvkbd-ipaq tests with `-Dosk_tests=enabled`; no board result recorded here |
| `picowl-panel`: bar, slider rows, date and time-left rows, crisp and smooth styles, strip on the short edge, density rule | working | `panel` and `panel-e2e` (pixel comparisons, headless, including an emulated hardware rotation); not seen on a board in the current version |
| `picowl-rotation-v1` | working | `panel-e2e` with `PICOWL_TEST_HW_ROTATION`; the real plane not tested |
| havoc patches (text-input-v3, per-cell damage) | working | havoc's own tests against a fake compositor (`oe/recipes-graphics/havoc/README.md`) |
| MediaQ 8 bpp (C8) output, remote access (VNC) | design only | `doc/design/mediaq-c8.md`, `doc/design/remote-access.md` |

What is planned, what only a board can verify, the known issues and the open decisions are kept in one place: [doc/design/roadmap.md](doc/design/roadmap.md).

Present limits: picowl draws no pointer cursor (only the tap-and-hold animation and the `wait` and `progress` cursor shapes), has no window animations or transitions and no blur or fade (each would cost CPU and bus bytes), and renders every output in the one format `[render] format` chooses at start.

## Build

```sh
# wlroots 0.19 is found through pkg-config, or built as the meson subproject;
# if it lives in a private prefix, source its environment first:
source <hostprefix>/env.sh

meson setup build
ninja -C build
# or: meson setup -Dwlroots:default_library=static build  # for a static link
```

Dependencies:
- `wlroots-0.19` with picowl's patches (see below; the meson subproject in `subprojects/wlroots.wrap` fetches 0.19.0 and applies them)
- `wayland-server`, `wayland-protocols >= 1.32`, `xkbcommon`, `pixman-1`, `libdrm`; `libinput` and `libudev` are optional and go together: libinput is linked directly when found, for touch calibration and the touch matrix of hardware rotation, and libudev then becomes required; without libinput that code is compiled out
- for `picowl-panel` only: `wayland-client` and `alsa` (meson option `panel`, default `auto`: built when both are found, `-Dpanel=disabled` skips it). Its smooth style needs a TrueType font file at run time; the crisp style, which `--style auto` picks on a QVGA display, needs none, and neither style needs one to build or to start (see Panel)
- a C11 compiler, meson >= 1.3

Meson options (`meson.options`): `tests` (default true), `systemd` (install the unit, default auto), `udev` and `udevrulesdir` (install the backlight rule), `tools` (default true: `picowl-cursor-convert`, and `picowl-commit-loop` when `wayland-client` is found), `panel` (default auto), `rss_ceiling_kb` (default 12288, the ceiling of the `rss` test) and `osk_tests` (default disabled: builds wvkbd-ipaq from `subprojects/wvkbd.wrap` and runs its tests with picowl's).

picowl needs five wlroots patches (`subprojects/packagefiles/wlroots/`, applied by the wrap and by the OE recipe; [their README](subprojects/packagefiles/wlroots/README.md) says what each does). Against an installed wlroots, `meson setup` checks for the symbols of patches 0003 (`wlr_drm_connector_set_copy_type`) and 0004 (`WLR_DRM_LEASE_OVERLAY_PLANES`) and stops with an error when they are missing; `--force-fallback-for=wlroots` builds the patched subproject instead. The subproject is configured minimally: the DRM and libinput backends, no renderer but pixman, no allocator but the built-in ones, no Xwayland. On OpenEmbedded (wrynose, blacksail) the layer in `oe/` builds the patched wlroots (see OpenEmbedded).

## Running

### On the device (DRM/KMS with seatd)

```sh
# picowl needs seat access for DRM/KMS: start seatd, or run inside a logind session
seatd -l debug &

# If needed, name the DRM device:
export WLR_DRM_DEVICE=/dev/dri/card0

picowl [-c /etc/picowl.ini] [-s COMMAND] [-d LEVEL]
```

Command line: `-c PATH` reads only that configuration file (see Configuration); `-s COMMAND` adds one command to the `[autostart]` list; `-d LEVEL` sets the log level, `0` silent, `1` errors, `2` info (the default), `3` debug; `-v` prints the version and `-h` the help. The log goes to stderr.

picowl exits with an error when `XDG_RUNTIME_DIR` is unset or empty, because libwayland would only report that it cannot bind the socket. Other programs that talk to the compositor must use the same `XDG_RUNTIME_DIR` to find the socket.

Autostarted commands, `spawn` commands and the keyboard run with picowl's environment. A service manager without a login session hands picowl `HOME=/` or none, and a client such as a terminal then looks for its `~/.config` in the wrong place. When `HOME` is unset, empty, relative or `/`, picowl sets it from the home directory of its effective user in the account database before anything is started, and logs the change. A `HOME` that is usable is left alone, and an account without a home directory changes nothing.

### The systemd unit

`data/picowl.service` uses neither PAM nor logind. It sets `XDG_RUNTIME_DIR=/run/picowl` and creates that directory with mode 0700 in its `ExecStartPre=`, because nothing else creates a runtime directory on these images. It needs `seatd.service`, runs with the supplementary group `video`, and keeps `TTYPath=/dev/tty1` and `StandardInput=tty` so that seatd has a VT; that tty setup has not been tested on a bare VT without logind. The log goes to the journal. A boot splash that holds DRM master must hand the display over first, because wlroots cannot allocate buffers without master: the unit starts after `picosplash.service` and runs `picosplash-write HANDOVER` in the same `ExecStartPre=` line (pico-init runs only one `ExecStartPre=` line); where no splash exists that step fails harmlessly. The unit is wanted by `multi-user.target` and `graphical.target`, because the pico-init images have no `graphical.target`, and it restarts on failure after 3 s.

### Headless (no hardware)

The tests run picowl this way:

```sh
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1
export XDG_RUNTIME_DIR=/tmp/picowl-test  # a writable directory for the socket
export WAYLAND_DISPLAY=wayland-0          # when several instances run

picowl -d 2
```

`PICOWL_HEADLESS_SIZE=WxH` (each side 16 to 8192, for example `240x320`) sets the size of the headless output, which is 1280x720 otherwise, so that a client or a screenshot can be tried at the size of a handheld panel; a malformed value is logged and ignored. It applies to headless outputs only, and the `[output]` transform is applied on top of it. The headless backend has no DRM device, so zero-copy, copy-type, hardware rotation and the lease are not offered there; the test seams that emulate parts of them are listed under Testing.

## Configuration

The configuration is an INI file, read from the first file found of `$XDG_CONFIG_HOME/picowl/picowl.ini`, `$HOME/.config/picowl/picowl.ini` and `/etc/picowl.ini`. With `-c PATH` only PATH is read, with no fallback. A missing file in the default search is not an error (defaults are used, logged at INFO); an unreadable `-c` file logs an error and also gives the defaults. `data/picowl.ini.example` is a commented template with every key; the OE recipe installs it as `/etc/picowl.ini`, so the values it sets (see OpenEmbedded) are what a fresh image runs with.

Syntax and validation, for every section:
- A line is a `[section]` header, `key = value`, a comment (starting with `#` or `;`) or empty. Keys and values are trimmed. There are no inline comments: a `#` after a value is part of the value.
- Booleans accept `yes`, `no`, `true`, `false`, `1`, `0`, `on` and `off`, in any case; anything else logs an error and keeps the previous value.
- Where a range is given below, a value outside it (or not a number) logs and keeps the default, unless the key says otherwise. Keys documented as read with `atoi` take a value that is not a number as 0, which is then checked against the range.
- An unknown section is logged at INFO and skipped. An unknown key is logged as an error in `[zerocopy]`, `[lease]`, `[layout]`, `[capture]`, `[memory]` and `[power.*]`, at INFO in `[app.*]` and `[layer.*]`, and ignored without a message in the other sections.

### [render]

- `format = RGB565 | XRGB8888 | ARGB8888` (default `RGB565`, case-sensitive): the render and scanout format of every output. An unknown name is logged at INFO and the default stays. When an output's test commit rejects the format, picowl logs it and uses XRGB8888 for that output. The single-buffer swapchain needs RGB565.

### [output]

Per-output transform: `<output-name> = <transform>`, where the name is the connector name (`DSI-1`, `HDMI-1`) or `*` for every output without its own entry, and the transform is `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180` or `flipped-270`. An unknown transform is logged at INFO and taken as `normal`.

```ini
[output]
DSI-1 = 90
* = normal
```

`subpixel = unknown|none|horizontal_rgb|horizontal_bgr|vertical_rgb|vertical_bgr` (in the same section; no output is called `subpixel`) sets the subpixel layout that `wl_output.geometry` advertises, so that a client can draw text for the panel's colour stripes (picowl-panel does, see Panel). It is the layout of the panel itself, in its native orientation, as the protocol defines it, and clients combine it with the `transform` of the same event. `horizontal_rgb` is red at the left of the panel as it is built (the h2200's 240x320 panel is portrait). With software rotation the transform is the configured rotation and the layout stays the native one, so the same event tells a client both. With hardware rotation picowl sends the transform `normal` (the display turns the picture, and the client sees an output that is already rotated), so the layout is advertised as the client sees it: `horizontal_rgb` on a panel rotated by 90 is sent as `vertical_rgb`, and a client that reads layout and transform together reaches the same stripe order either way. Without the key the value the backend reports stays: the DRM connector's, which is `unknown` for most handheld panels, or the headless output's. The key applies to every output. A wrong name (they are case-sensitive) is logged and ignored. The layout of each board is a property of its panel and is not detected; the iPAQ values in `data/picowl.ini.example` are the project owner's and not taken from a datasheet.

`size_mm = WIDTHxHEIGHT` (the same section) overrides the physical size in millimetres that `wl_output.geometry` sends, for every output; `NAME.size_mm = WIDTHxHEIGHT` does it for one output. An entry with a name wins over the one for all, whatever their order, and for one name the last entry counts (no output is called `size_mm` or ends in `.size_mm`). A client works out the density of the display from this size (picowl-panel does, see Density), and several kernel drivers report none or a wrong one: 0x0 for the h3xxx and the hx4700, and 53x71 for the h5xxx on older kernels, where the panel is 57x77 (`doc/design/ipaq-displays.md`). The value goes out as given, in the panel's native orientation like the kernel's (the h3800 and h3900 are 77x57, their landscape scan frame), and a client that works from the diagonal does not care how it is turned. Each side is a whole number of millimetres from 1 to 2000, written `57x77` (or `57X77`); anything else is logged as `Invalid size_mm` and ignored, so the connector's own size stays. A size that gives a density outside 60 to 400 ppi is accepted here, but the panel does not use it (see Density). It is read when the output appears, so a change needs a restart of picowl, which logs at INFO that it replaced the connector's size. The values `data/picowl.ini.example` lists are the panel sizes of the iPAQs (53x71 h2200, 57x77 the other QVGA ones, 60x80 hx4700, provisional), not measured here.

### [rotation]

Per-output rotation mode: `<output-name> = <mode>` (`*` matches any output without its own entry; the mode is case-insensitive; an invalid mode is logged and the entry dropped).

- `auto` (default): hardware rotation when the DRM primary plane's `rotation` property supports the requested transform (in practice the MediaQ `mq11xx` driver with wlroots patch 0003), else software rotation.
- `hardware`: the same, but an unsupported transform is logged as an error before picowl falls back to software rotation.
- `software`: always rotate in the renderer.

Hardware rotation is the default because it was measured on an h2200: at 90 degrees with a full-surface client it used about a quarter less compositor CPU (16.5 against 21.9 ticks/s) and uploaded the same bytes; at the normal transform there was no difference ([doc/rotation-results.md](doc/rotation-results.md)). The media player's own earlier runs on the h2200, with ordered dither at 320 wide, showed about +2.5% CPU and roughly double the dropped frames with hardware rotation; that workload has not been measured again since picowl's hardware rotation started to take effect ([doc/rotation-measurement.md](doc/rotation-measurement.md)). Copy-type outputs also depend on the kernel honouring `FB_DAMAGE_CLIPS`: h2200 kernel builds #201 and #202 ignore them after any rotation or format change and upload the full frame on every commit (about 17.5 ms); kernel commit `daf8e6712ae1` fixes that.

The plane rotation can only change while the output is disabled, so picowl disables the output before it asks for hardware rotation (and again when the `rotate` action changes it). If the hardware rotation commit fails, picowl logs `hw rotation commit failed, using software rotation` and rotates in software. Under hardware rotation `wl_output` advertises the rotated mode and the transform `normal`; `picowl-rotation-v1` (Protocols) tells a client the real turn. Hardware rotation disables the hardware cursor (the cursor plane has no `rotation` property), so the hold animation is drawn in software. Background: [compositor.md § 5.3](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/compositor.md) and, for the rotation capabilities of each device, [hardware.md § 5](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/hardware.md).

### [copytype]

Per-output override of copy-type detection: `<output-name> = auto | yes | no` (`*` for all; case-insensitive; `yes` and `no` also accept the boolean spellings; an invalid value is logged and the entry dropped). A copy-type driver copies the damaged rectangles of a buffer into video memory during the commit, instead of scanning the buffer out by DMA. `auto` (the default) looks at the DRM driver name from `drmGetVersion`:
- copy-type: `mq11xx` (also `mediaq`), `w100` (also `imageon`), `sa1100-lcdc` (also `sa1100_lcdc`, `sa11x0-lcdc`, `sa1100`), in any case;
- scanout: `pxa-lcdc` and every other driver.

Copy-type outputs get a single-buffer swapchain (see `[zerocopy] single_buffer`); a scanout output keeps two buffers. `auto` turns copy-type off when the kernel lacks `DRM_IOCTL_MODE_CLOSEFB` (Linux 6.9 or a backport), because destroying a scanned-out client buffer could then blank the display; `yes` is honoured anyway and logs an error. Copy-type is also off with `WLR_DRM_NO_ATOMIC=1`, because wlroots ignores it on the legacy KMS path. Device details: [hardware.md](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/hardware.md); the buffer model: [doc/zero-copy.md](doc/zero-copy.md).

### [zerocopy]

The zero-copy globals need the DRM backend and an allocator with dmabuf support; otherwise picowl logs `zero-copy disabled: <reason>` and offers neither. [doc/buffers.md](doc/buffers.md) describes the protocol and its client side.

- `enable = true|false` (default true): advertise `picowl-buffer-v1` and the custom linux-dmabuf feedback.
- `single_buffer = true|false` (default true): one framebuffer on copy-type outputs when that is safe (RGB565 render format, an RGB565/LINEAR primary plane format); otherwise picowl logs once why it uses two slots. Background: [compositor.md § 5.4](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/compositor.md).
- `panel_autohide = true|false` (default true): hide the panel layer while an app has the focus, so that a single fullscreen buffer can be scanned out directly (a render list of one entry). Exclusive surfaces of the top layer stay visible. The `panel` key action shows it until the focus changes. Background: [compositor.md § 7](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/compositor.md).
- `max_buffers_per_client` (default 3, range 1..32): `picowl-buffer-v1` buffers one client may hold.
- `budget_kb` (default 2048, range 0..65536): memory shared by all clients without an `[app.*]` buffer rule (the default pool). 0 means none of them may allocate, and they fall back to wl_shm.
- `total_kb` (default 0, range 0..65536): ceiling over all pools. 0 means no extra limit, so the worst case is `budget_kb` plus every app pool. A ceiling below the sum of the pools is accepted and logged at INFO (the pools can then overcommit).
- `caching = auto|cacheable|write_combined` (default `auto`, case-insensitive): the value of the `caching` event that clients bound to `picowl-buffer-v1` version 2 receive. It says whether the CPU mapping of the buffers is cacheable (reading back is cheap) or write-combined or uncached (write only; never read back or decode into it). `auto` uses the DRM driver: `cacheable` for `mq11xx` and `w100` (and their aliases; their buffers are shmem), `write_combined` for everything else, `sa1100-lcdc` (CMA) included. It is one global key because picowl has one allocator. An invalid value logs an error and keeps `auto`. The start-up log line is `zero-copy enabled (copy_type=N caching=X driver 'D')`.

Sizes are counted as the real allocation, the buffer rounded up to whole pages. An allocation over a limit is answered with `failed(no_memory)` before any memory is taken. The defaults give 3 buffers and 2 MiB shared, as before the limits were configurable.

#### Per-app buffer limits

An `[app.<app_id>]` section (the one that also holds the hold overrides, see Touch input) with `zerocopy_*` keys gives that app its own pool:

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
| `exe` | none | the client's `/proc/<pid>/exe` must be this file; symlinks are resolved when the config is read (a path that cannot be resolved is kept as written, logged at INFO) |

- **One pool per rule**, shared by every client that matches it and separate from the default pool, so other clients cannot starve the app and clients claiming its app_id cannot get more than the pool. With only `zerocopy_buffers = 7` one config fits every board (7 x 152 KiB on QVGA, 7 x 600 KiB on the hx4700).
- **Matching** is by the `app_id` of one of the client's xdg toplevels (exact, case-sensitive), checked at every `create_buffer`. The client has to call `set_app_id` before `create_buffer`; earlier buffers stay in the default pool. With several toplevels the newest one with a rule wins. Layer-shell clients always use the default pool.
- **`exe`** guards against another program claiming the app_id: on a mismatch, an unreadable link or a replaced binary (`(deleted)`) the client gets the default limits. Without `exe` the pool size is the only protection. `exe` only works when the client connected to the socket itself, not through a `WAYLAND_SOCKET` handed over by a launcher.
- **Memory:** on shmem drivers (mq11xx, w100) the buffers are resident RAM in picowl's process, so do not set pools above free memory. On CMA drivers an allocation the kernel refuses is also `no_memory`. See [doc/zero-copy.md](doc/zero-copy.md).
- `[app.*]` rules without `zerocopy_*` keys have no pool. `zerocopy_*`, `exe` and `aspect` are not keys of `[layer.*]` (logged at INFO as unknown).
- Buffers are refunded to the pool they were charged to when they are destroyed. Shrinking an output never revokes buffers.
- With `-d 3` a rejected request is logged with the limit (count, pool or total), the pid, the app_id and used/cap.

### [lease]

DRM lease: the media player (`--vo drm:lease`) drives KMS on the output without a VT switch, while picowl keeps input and power policy and takes the output back when the lease ends. See [doc/lease.md](doc/lease.md).

- `enable = true|false` (default true): offer `wp_drm_lease_device_v1`. `false` creates no global. The global also needs the DRM backend and a user who can open the card as non-master (the `video` group).
- `allow = <app_id>[, <app_id>...]` (default `mediaplayer`): which clients may lease. The requester must own the focused toplevel and its `app_id` must be in the list. `*` accepts any focused client; an empty value rejects every request.

### [capture]

Screen capture for screenshot and recording tools. See [doc/capture.md](doc/capture.md).

- `enabled = true|false` (default false): offer `zwlr_screencopy_manager_v1`. `false` creates no global.
- With capture on, any client that can connect to the Wayland socket can read the whole screen, other applications included. Enable it on development images only. `data/picowl.ini.example`, and so the `/etc/picowl.ini` of the OE package, enables it.

### [memory]

malloc tuning with `mallopt`, applied once with the defaults before the config is read and again with the configured values after it. It reduces fragmentation and heap padding on a 64 MiB device. The values are read with `atoi`; an out-of-range value is logged and the default kept.

- `arena_max` (default 1, range 1..8): `M_ARENA_MAX`; one arena keeps fragmentation down in a single-threaded process.
- `trim_threshold_kb` (default 256, range 0..65536): `M_TRIM_THRESHOLD` in KiB, the free space at the top of the heap above which glibc's `free()` returns memory to the kernel.
- `mmap_threshold_kb` (default 128, range 16..4096): `M_MMAP_THRESHOLD` in KiB; the lower bound keeps malloc from using mmap for small blocks.
- `top_pad_kb` (default 16, range 0..65536): `M_TOP_PAD` in KiB, extra space taken at the top of the heap on each extension.
- `trim_after_start = true|false` (default true): call `malloc_trim(0)` once, from an idle callback after start-up, and log VmRSS and VmHWM. picowl never trims on idle.

Measured VmHWM, headless at 1280x720 with a test client mapped: about 9.5 MB (9528 to 9576 kB) against 9440 kB before the tuning ([doc/zero-copy.md](doc/zero-copy.md)); the `rss` test enforces a ceiling. Strategy: [compositor.md § 5.4](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/compositor.md).

### [power], [power.ac], [power.battery], [power.low]

The screen goes from ACTIVE to DIMMED to BLANKED after inactivity, with timings that depend on the power profile (AC, BATTERY, LOW). [doc/power.md](doc/power.md) describes the states, the backlight device selection, how a level set by another process is kept, and the idle inhibitors. The values are read with `atoi`; an out-of-range value is logged at INFO and the default kept.

`[power]` (other keys are ignored without a message):

| Key | Default | Range | Meaning |
|---|---|---|---|
| `backlight` | `auto` | `auto` or a name | the device in `/sys/class/backlight/`; `auto` prefers the type `firmware`, then `platform`, then `raw`, the first by name |
| `low_capacity` | 15 | 0..100 | percent of battery capacity at or below which the LOW profile applies |
| `dim_level` | 30 | 1..100 | dimmed brightness, percent of the user's level |
| `poll_s` | 300 | 0 or 30..86400 | seconds between sysfs re-reads for drivers without change events; 0 turns polling off |

`[power.ac]`, `[power.battery]`, `[power.low]` (an unknown key or profile is logged as an error):

| Key | Default (ac / battery / low) | Range | Meaning |
|---|---|---|---|
| `dim_after_s` | 120 / 20 / 10 | 0..86400 | seconds of inactivity before the backlight dims; 0 turns dimming off |
| `blank_after_s` | 600 / 60 / 30 | 0..86400 | seconds of inactivity before the screen blanks; 0 turns blanking off |
| `inhibit` | yes | boolean | while a visible surface holds a `zwp_idle_inhibitor_v1`, do not dim or blank (the power key still blanks) |
| `max_brightness_pct` | 40 | 1..100 | `[power.low]` only: cap of the backlight, percent of `max_brightness`, while the profile is LOW |

Legacy keys: `[idle] timeout_ms` (milliseconds) and its alias `[core] idle_timeout_ms` set `blank_after_s` of all three profiles, rounded up to whole seconds and clamped to 0..86400, but only when no `[power.*]` section sets `blank_after_s`; the change is logged at INFO. Prefer the `[power.*]` keys. `data/picowl.ini.example` still sets `[idle] timeout_ms = 60000`, so with that file every profile blanks after 60 s.

### [background]

- `color = #RRGGBB`: the colour behind all windows, six hex digits, always opaque. The default is 0.1 in each channel (about `#1a1a1a`); `data/picowl.ini.example` sets `#101010`. An invalid value is logged and ignored.

### [autostart]

Commands to run once at start-up, one `cmd = <shell command>` line each, in order, detached from picowl. `-s COMMAND` on the command line adds one more.

```ini
[autostart]
cmd = picowl-panel
```

picowl does not restart them. The on-screen keyboard does not belong here: `[osk]` starts it and keeps it running.

### [osk]

picowl starts the on-screen keyboard, restarts it when it dies, signals it for the `osk` key action and stops it on exit. The keyboard is wvkbd-ipaq (see Companion pieces). Design: [doc/design/osk.md](doc/design/osk.md).

- `cmd = <command>` (default empty: picowl does not handle a keyboard): the command line of the keyboard. picowl runs it as `/bin/sh -c "exec <command>"` and stays its parent, so the process that runs is the keyboard itself. Start it hidden, because picowl assumes a freshly started keyboard is hidden. Example: `cmd = /usr/bin/wvkbd-ipaq --hidden --auto`; `--auto` lets the keyboard show itself when a text input gets the focus (see the input method relay under Protocols).
- `restart = yes|no` (default `yes`): start the keyboard again after it exits. An exit within 10 s of the start is quick; after quick exits number 1, 2, 3 and 4 picowl waits 1, 2, 4 and 8 s, and after the fifth quick exit in a row it logs an error and gives up. A run of 10 s or more resets the count. picowl waits on a timer and keeps serving clients meanwhile.

```ini
[osk]
cmd = /usr/bin/wvkbd-ipaq --hidden --auto
restart = yes
```

- **Key action**: `osk show` (SIGUSR2), `osk hide` (SIGUSR1) and `osk toggle` (SIGRTMIN, looked up at run time; `osk` alone means toggle). When the keyboard is not running (it gave up, is waiting to restart, or `restart = no` and it exited), `osk show` and `osk toggle` start it at once with a fresh count, and it comes up hidden, so the key press that started it does not show it; press again. `osk hide` never starts it. Without a `cmd` the action is logged and ignored. While a DRM lease is active ([doc/lease.md](doc/lease.md)) the key ends the lease first.
- **Visibility** is a guess: picowl notes what its own keys did and assumes hidden after every start. If the keyboard is signalled from elsewhere, or shows itself with `--auto`, the guess is wrong; `show` and `hide` are sent regardless, and the next `toggle` is the one that can be off.
- **Shutdown**: picowl sends SIGTERM when it exits, waits up to half a second and then kills the keyboard; it is not restarted. If picowl itself is killed, the keyboard gets SIGTERM from the kernel (`PR_SET_PDEATHSIG`).
- **Tap-and-hold**: a built-in `[layer.wvkbd]` rule sets `hold_action = none`, so key presses are not delayed until the finger lifts. A `[layer.wvkbd]` section of your own merges over it (see Touch input).
- Children started by `[autostart]` and `spawn` are not affected by any of this.

### [keybindings]

Keyboard shortcuts: `<modifiers>+<key> = <action> [argument]`.

- `<modifiers>`: zero or more of `alt`, `ctrl`, `shift` and `logo`, joined by `+`.
- `<key>`: an XKB keysym name (`Tab`, `F4`, `Return`), or a numeric evdev keycode as `code:116` (KEY_POWER).
- `<action>`:
  - `spawn <command>`: run `/bin/sh -c <command>`, detached, without zombies.
  - `cycle`: raise the next mapped toplevel in stacking order (in a tiled pair: move the keyboard between the two, see Tiled layout).
  - `close`: send a close request to the focused toplevel.
  - `blank`: toggle the screen off and on (the same as the power key and output-power-management requests).
  - `rotate`: cycle the rotation of the first output through normal, 90, 180 and 270, keeping the flipped bit.
  - `panel`: show the panel while panel autohide hides it, until the focus changes.
  - `osk [show|hide|toggle]`: the on-screen keyboard started by `[osk]` (default `toggle`).
  - `quit`: exit the compositor (logged at INFO).
- An unknown action, or an `osk` argument other than `show`, `hide` and `toggle`, drops the binding with an error.

Built-in bindings are always present; bindings from the config are added to them and cannot remove one:

| Binding | Action |
|---------|--------|
| `code:116` (KEY_POWER) | `blank` |
| `alt+Tab` | `cycle` |
| `logo+Escape` | `quit` |

```ini
[keybindings]
alt+F4 = close
logo+Return = spawn foot
# the iPAQ email button (KEY_MAIL) rotates, the calendar button the keyboard
code:155 = rotate
code:397 = osk toggle
```

### [touch], [app.*] and [layer.*]

The touch calibration keys and the tap-and-hold keys, with the per-app and per-layer overrides, are described with the behaviour they control under [Touch input](#touch-input). `[app.*]` also takes the buffer keys above and `aspect` (Tiled layout).

### [layout]

The two-app tiled layout (`stack`, `focus`, `second_min`, `pan`) is described under [Tiled layout](#tiled-layout).

### [cursor]

The hold animation (`hold_animation`, `fill`, `outline`, `frame_interval_ms`) is described under [Cursor](#cursor).

## Touch input

### Touch calibration

picowl never uses the raw axes of a touch device. Resistive panels such as the iPAQ ones report raw ADC values that are inverted and offset, so every touch device (a libinput device with the touch capability; not tablets) needs a calibration, taken from the first of these sources that gives one:

1. `[touch] calibration = a b c d e f` in the config: six numbers, a libinput normalized matrix (`x' = a*x + b*y + c`, `y' = d*x + e*y + f`, with x, y, x', y' in 0..1 over the panel). It overrides everything, also when the device has a udev matrix or a pointercal exists. A value that is not exactly six finite numbers, or one with a magnitude above 1000, is logged and ignored.
2. A tslib pointercal file, `[touch] pointercal = PATH` (default `/etc/pointercal`; an empty value disables this source). A missing file is skipped quietly; an unreadable or invalid one is logged as an error and the next source is tried.
3. A non-identity default matrix from libinput, which comes from the udev property `LIBINPUT_CALIBRATION_MATRIX`. libinput cannot tell an identity default from none, so an identity default counts as none.
4. None: the device is disabled (libinput send-events mode `disabled`), never attached to the cursor, and an error naming the device and the fixes is logged once. It stays disabled.

The pointercal file is what tslib's `ts_calibrate` writes: ten whitespace-separated integers `a b c d e f scale xres yres rotation`, where the screen position is `x' = (a*x + b*y + c) / scale`, `y' = (d*x + e*y + f) / scale` for raw x, y, on a screen of `xres` by `yres` pixels. Example (a 240x320 panel; no trailing newline needed):

```
22841 -68 -3658260 -491 -28324 25242144 65536 240 320 0
```

Parsing is strict: 7 to 10 numeric fields, a positive scale and sane magnitudes. `xres` and `yres` must be present and non-zero (older files with 7 fields are refused; recreate them), and a non-zero `rotation` is refused rather than guessed. picowl converts the file to a libinput matrix using the ABS_X/ABS_Y range of the device (ABS_MT_POSITION_X/Y on multitouch panels) read from its device node, normalized the way libinput does it, with `max - min + 1`. For the example above and a 0..1023 panel the matrix is `1.487044 -0.004427 -0.232586 -0.023975 -1.383008 1.203639`, the same as `xinput_calibrator` gives for it. If the ranges cannot be read, the device is disabled instead of falling back to another source.

Create the file on the device with tslib's `ts_calibrate`, in the native orientation of the panel and with picowl not running (it needs the screen and the touch device for itself). The calibration is applied when the device appears and is the base that hardware rotation composes with; software rotation restores it exactly. Tablets are not touchscreens: they never take a pointercal or `[touch] calibration`, only the libinput default matrix.

A panel that really is calibrated already (for example a capacitive touchscreen with correct axes) is used by saying so explicitly with an identity matrix, `calibration = 1 0 0 0 1 0`. It is never assumed.

### Tap-and-hold

picowl turns a touch into pointer events, with a tap-and-hold gesture for the right click (Pocket PC convention; GTK2 sees button 3):

- **Touch-down**: focus the surface at the touch point and send pointer enter and motion, but hold back the left button press.
- **Tap** (touch-up before the hold triggers): send the left press and release as one click.
- **Drag** (movement over `slop_px` before the hold triggers): send the held-back left press and normal motion; no animation.
- **Hold animation** (after `hold_delay_ms`): show the wait animation at the touch point, stepped by a timer (see Cursor).
- **Hold** (`hold_ms` after touch-down): hide the animation, send a right button press and release, and swallow the rest of the touch. With `hold_button = middle` the hold sends a middle click instead, which pastes the primary selection in a terminal such as havoc.
- **Lift during the animation**: a tap (left press and release), as the hold has not triggered.

`[touch]`:

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `hold_action` | `right-click` | `right-click`, `none` | `none` sends the left press at touch-down, like a plain pointer, with no hold and no animation; an unknown value is logged and the default kept |
| `hold_button` | `right` | `right`, `middle` | the button the hold sends; unused with `hold_action = none`; an unknown value is logged and the default kept |
| `hold_delay_ms` | 300 | integer (`atoi`) | milliseconds before the animation starts |
| `hold_ms` | 900 | greater than `hold_delay_ms` | milliseconds from touch-down to the click; when it is not greater than `hold_delay_ms`, both are logged and reset to 300 and 900 |
| `slop_px` | 8 | 0..64 | movement in pixels that turns the touch into a drag; outside the range it is logged and reset to 8 |
| `calibration` | none | six numbers | see Touch calibration |
| `pointercal` | `/etc/pointercal` | path, or empty | see Touch calibration |

### Per-app and per-layer overrides

`[app.<app_id>]` (xdg toplevels) and `[layer.<namespace>]` (layer-shell surfaces) override the hold keys for one application or one layer namespace. This serves applications that time their own long press, such as a media player (`hold_action = none` sends the left press at touch-down, with no right click and no animation):

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

# built in too: the panel's sliders are dragged, so its presses are not held back
[layer.panel]
hold_action = none
```

| Key | Default | Meaning |
|-----|---------|---------|
| `hold_action` | from `[touch]` | `right-click` or `none` |
| `hold_button` | from `[touch]` | `right` or `middle` |
| `hold_delay_ms` | from `[touch]` | milliseconds before the animation starts |
| `hold_ms` | from `[touch]` | milliseconds to the click |
| `slop_px` | from `[touch]` | movement tolerance in pixels |
| `aspect` | unset | `[app.*]` only: `W:H` for the tiled layout |
| `zerocopy_buffers`, `zerocopy_budget_kb`, `exe` | unset | `[app.*]` only: the buffer pool, see `[zerocopy]` |

- **Inheritance**: unset keys take the value of `[touch]`, wherever `[touch]` is in the file.
- **Bad timings**: if `hold_ms` is not greater than `hold_delay_ms`, or `slop_px` is outside 0..64, the rule's three timing keys revert to `[touch]` (logged); its `hold_action` and `hold_button` are kept. A bad `hold_action` or `hold_button` is logged and ignored, so the value is inherited.
- **Matching**: names match exactly and case-sensitively, without wildcards.
- **Merging**: a section that appears twice merges into one rule; later keys win. The built-in `[layer.wvkbd]` and `[layer.panel]` rules (`hold_action = none`) merge with sections of the same name in the file.
- **Scope**: `[app.panel]` and `[layer.panel]` are separate namespaces. An empty `[app.]` or `[layer.]` is logged and ignored.
- **Binding**: the surface under the finger at touch-down decides (popups and subsurfaces follow their parent, input method popups use `[touch]`). The setting stays fixed until the lift.

## Tiled layout

Every window is maximized and one is shown at a time, except for two apps named in `[layout]`. While both of them have a mapped window they tile the usable area instead of stacking:

```ini
[layout]
stack = mediaplayer, havoc

[app.mediaplayer]
aspect = 4:3
```

- `stack`: two distinct app_ids, comma separated, in order. A list of more than two ignores the extra names; a repeated name is skipped; tiling stays off, with an error, unless two distinct names remain.
- `focus`: one of the two `stack` app_ids, default empty. That app keeps the keyboard when the other one maps (and when the pair forms), so a player which starts or restarts later does not take the keys of the on-screen keyboard from the terminal. The other window maps without being activated. Touching a tiled window and alt+Tab still move the keyboard, and apps outside the stack take the focus when they map. A name that is not one of the two in `stack` is logged and ignored, whatever the order of the two keys in the file.
- `second_min`: integer percent, 25..75, default 50: the share of the split axis that the second window keeps at least, so a terminal next to a player always gets at least half the width (landscape) or height (portrait), whatever the player's aspect ratio. A value outside 25..75 or not a number is logged and the previous value is kept. `second_min = 25` gives the rule picowl had before the key existed.
- `aspect` (in `[app.<app_id>]`): `W:H`, two integers from 1 to 4096, the natural aspect of the first app. A bad value is logged and ignored.
- `pan`: layer-shell namespaces, comma separated, default `wvkbd`. A mapped surface in one of these namespaces that is anchored to the bottom edge and not to the top (the on-screen keyboard, with or without an exclusive zone; the shipped wvkbd-ipaq asks for none) pans the tiled pair instead of shrinking it, see Keyboard below. An empty value (`pan =`) turns panning off, and the keyboard then shrinks the usable area for the pair as for every other window. Names match exactly and case-sensitively; at most 8, each shorter than 64 characters, without spaces or control characters inside. A list with an empty name between commas, such a name or too many names is logged and the previous value kept.

Rules:
- **Order**: in a portrait usable area (height at least width) the first app is on top and the second below, both at full width. In a landscape one the first is on the left and the second on the right, both at full height.
- **Size of the first window** along the split axis: its natural aspect ratio applied to the available extent. The source is, in this order, the client's own fixed size (xdg-shell `min_size` equal to `max_size`, both non-zero, read again whenever the client commits a change, so a new clip with another aspect lays out again), then `aspect` from the config, then half. The result is clamped: the second window keeps at least `second_min` percent of the extent (so with the default the first gets at most half) and the first at least 25 percent, because a hint is only a preference and neither window may be squeezed. The clamp only shrinks the first window: one whose natural size is smaller than its cap keeps exactly that size. Where rounding would make the two limits collide, the first window's 25 percent wins. The second window gets the rest. Each client receives its exact size in a configure and draws its own letterboxing if its content has another aspect.
- **One of the two**: with only one of the apps mapped, nothing changes; it is maximized like any other window. When the second appears or goes away both are configured again.
- **Other windows**: apps outside the stack are not affected. A window raised above the pair covers both.
- **Focus**: touching a tiled window gives it the keyboard. alt+Tab (the `cycle` action) moves the keyboard between the two tiled windows, without restacking, while one of them has the focus; it does not reach other apps then (use the panel). Focusing one tiled window raises both together. With `focus` set, a window of the other stack app which maps does not take the keyboard from the named app, and the pair raises as if the named app had been focused.
- **Keyboard**: while both apps are mapped and split top and bottom (portrait), a bottom-anchored layer surface named by `pan` does not shrink the pair. The windows are laid out on the usable area without that surface's exclusive zone, keep that size and receive no configure, and the whole stack moves up by the height the keyboard takes at the bottom (its exclusive zone plus margin when it asks for one, else its height plus its bottom margin; a keyboard which hides by attaching no buffer takes nothing). The lower window ends where the keyboard starts and the upper one slides partly or wholly off the top of the screen. The move is limited so that the lower window is never pushed above the top of the screen; a keyboard taller than the room above the lower window leaves the rest of it under the keyboard. When the keyboard hides, or the pair breaks up because one of the apps goes away, the move ends: the stack returns, or the surviving window is maximized into the area the keyboard leaves (the whole output, for a keyboard without a zone). Only the tiled windows move. Every other window, a single app of the pair on its own, and a pair split side by side (landscape, where both windows span the height and each would lose its top) are resized to the usable area, which has the keyboard's zone taken off (none, for a keyboard without a zone, which then overlays them), as without a stack. Other exclusive surfaces (a top or bottom panel, or a keyboard whose namespace is not in `pan`) keep shrinking the usable area, and the pair is laid out on what they leave. Touch and pointer coordinates follow the moved windows, because the surface under a point and its local coordinates come from the scene node positions; a touch or tablet tool held on a window keeps reporting in the coordinates of that window as it moves, since the window's origin is read from its scene node on every motion. xdg popups of tiled windows are constrained, when they first map, to the usable area (the screen above the keyboard) using the moved window's position, and an open popup is not moved when the keyboard shows or hides, as with a maximized window. Text input popups (`src/imrelay.c`) are placed again on every layout change, from the moved position of the focused surface and inside the usable area.
- **Rotation and panel**: the layout is recomputed whenever the output size, its transform or the usable area changes, as for maximized windows. In landscape the panel's strip is on a short edge of the view (Panel, Rotated output), so the usable area loses 18 px of width, not of height: a 320x240 view tiles into 302x240, side by side.
- **Fullscreen**: a fullscreen request from a tiled window is answered with its slot size.
- **Direct scanout**: two visible windows are two entries in the scene's render list, so the scene composes them and never scans out one buffer. A window which is alone is still eligible.
- **Idle inhibit**: an inhibitor counts while its window has the keyboard focus, so a playing window in the unfocused slot does not hold the screen on.

## Cursor

picowl draws a cursor only during the tap-and-hold animation, and while the pointer-focused client asks for the `wait` or `progress` cursor shape (`wp_cursor_shape_manager_v1`, see Protocols). The animation is configured in `[cursor]`:

| Key | Default | Meaning |
|-----|---------|---------|
| `hold_animation` | `builtin` | `builtin` (the Pocket PC 2003 rotating circle) or the path of a PAM or PPM strip file |
| `fill` | `#2050c0` | foreground colour of the built-in animation (`#RRGGBB`; an invalid value is logged and ignored) |
| `outline` | `#ffffff` | outline colour of the built-in animation (`#RRGGBB`; an invalid value is logged and ignored) |
| `frame_interval_ms` | 83 | frame duration in milliseconds, 20..1000 (about 12 fps by default); outside the range it is logged and reset to 83 |

Every frame must follow the rules of the hardware cursor, because a frame that breaks them fails the whole commit:

- ARGB8888, 1 to 64 pixels square;
- no translucency: every pixel is fully transparent (`0x00000000`) or fully opaque (alpha `0xff`);
- at most two distinct opaque colours per frame (compared at 6 bits per component).

The MediaQ MQ1132/MQ1188 hardware cursor is 2 bpp AND/XOR with two colour registers, the mq11xx DRM cursor plane rejects anything else with `-EINVAL`, which fails the entire frame, and wlroots 0.19 does not test-commit the cursor before the next update. Boards that fall back to a software cursor gain from the same limits, because the cursor then costs less to draw. A custom `hold_animation` file is a horizontal strip of square frames; frames that break the rules are converted at load time with a logged warning.

`picowl-cursor-convert` (built with the `tools` option) creates, checks and converts strips:

```
picowl-cursor-convert [--frame-width N] [--fg #rrggbb] [--bg #rrggbb]
                      [--premultiplied] [--hotspot X,Y] in.pam|ppm out.pam
picowl-cursor-convert --check [--frame-width N] in.pam|ppm
picowl-cursor-convert --export-builtin [--fg #rrggbb] [--bg #rrggbb] out.pam
```

- `--check`: validate only; exit 0 (compliant) or 1 (not compliant).
- `--export-builtin`: write the built-in animation as a template PAM file.
- `--fg`, `--bg`: force these colours; otherwise the two dominant colours are kept.
- `--premultiplied`: the input has premultiplied alpha.
- Exit codes: 0 success or compliant, 1 not compliant with `--check`, 2 an I/O or usage error.

A PNG converts to PAM with `pngtopam -alphapam image.png > image.pam` (netpbm) or `convert image.png pam:image.pam` (ImageMagick). The file format, the conversion rules, examples and troubleshooting are in [doc/cursors.md](doc/cursors.md).

## Panel

`picowl-panel` is a layer-shell client in C for a 240x320 handheld: a slim bar with the clock, the battery and two icon buttons, for the backlight and the volume. Tapping a button pops a row out under the bar: a slider for the backlight and the volume, the weekday and the date for the clock, the time left for the battery. It is drawn with wl_shm, in a style that follows the density of the display (`--style auto`, the default: crisp, without any anti-aliasing, on a QVGA iPAQ, smooth on the hx4700; see Density below). The crisp style needs no font file, because its pixel fonts are compiled in. The smooth style rasterizes its text from a TrueType font file (stb_truetype, vendored as `panel/stb_truetype.h`, public domain), which the panel reads when that style is chosen, at start-up or when the density changes to it, and again on every later change of look (see Font). The icons and the slider are vector shapes rasterized into coverage masks when the panel starts, when its layout changes or when its look does, and redraws only blend those masks. It is a single process with one poll loop. The buffer of the closed bar is 240x18 RGB565 (8.6 KB); with a slider row open it is 240x54 ARGB8888 (52 KB) when the row is translucent (the default), RGB565 (26 KB) when it is opaque. These are the sizes of the 18 px bar; a taller bar (34 px on the hx4700) has larger buffers. The resident size depends on the style and the font file and has not been measured for the current panel. The widget design it implements part of is [doc/panel.md](doc/panel.md).

### What it shows

The bar is 18 px high on a QVGA display and scales with the density on a denser one (`--height auto`, the default; `--height N`, 18 to 80, forces one) and is as wide as the output; on an output rotated by 90 or 270 degrees it is on the short side instead, see Rotated output below. Margins are 8 px.

- **Left: the clock**, `HH:MM`, 24 h, local time, no seconds. A timerfd on the realtime clock fires at the start of every minute and is cancelled when the system time is set, so a change of the time or the date shows at once. It is a button too: the touch target is the text widened to at least 36 px and as high as the bar, and while its row is open it has the same highlighted background as the icon buttons.
- **Right, from the left: the backlight button** (a sun), **the volume button** (a speaker) and **the battery**. Each button is at least 36 px wide and the whole height of the bar; the icon in it is about 12 px. The speaker has no waves and a slash at 0 percent (muted), one wave up to 50 percent and two above. A button without a device is greyed out and ignores taps. While a slider row is open, its button has a highlighted background and the icon is drawn in the accent colour.
- **The battery** is the first power supply of type `Battery` with a readable `capacity` (a battery with `scope` `Device`, as input devices have, is not the system's), shown as a battery outline with a fill proportional to the capacity and `NN%`. The fill is light grey, red at 15 percent and below, and green while the status is `Charging` (red wins over green, so a nearly empty battery that charges is red, with the bolt); a small lightning bolt over the outline means `Charging`. Without a battery, a `Mains` or `USB*` supply that is online shows `AC`, and nothing at all shows `--`. The supplies are read again every 30 s; there is no uevent code, so a plug event shows at the next read. The battery icon and its percentage are a button too (its touch target runs to the edge of the output and is at least 36 px wide), highlighted while its row is open.

The slider row is 36 px high, or the bar's height plus 8 px when that is more, and as wide as the output: a large icon (24 px) on the left, a rounded track whose filled part is blue and whose rest is dark grey, a circular thumb 22 px in diameter (white, with a darker ring) and the value as `NN%` on the right. The row has a 1 px line on its edge towards the windows.

The date row, opened by the clock, is the same row with one centred line of text in the larger font (4/3 of the bar's text size, 14 px at the default, never above 26 px) and no slider: the weekday and the date as `Monday 5 October 2026`, in local time (the system's time zone, as for the clock). The names are in a table in the panel and not from the locale, so the line is English whatever `LANG` says; the day has no leading zero. A line that does not fit the width less the margins is shrunk in three steps (to 3/4 and then 5/8 of the size, never below 8 px) and never clipped. The date is refreshed by the same minute tick as the clock and redraws only while the row is open, so an open row follows midnight and a closed panel has no extra timer or wake-up.

The battery row, opened by the battery, is a row of the same kind with the time left, with the percentage in front when the line has room for it (`94%  1 h 30 min left`). The line is the first of four wordings that fits the width in the largest size, each shorter than the one before (without the percentage, then without `left` or `to full`, then both), and only when none of them fits is the next smaller size tried, so the row loses words before it loses size. See Time left below.

The look is a dark flat theme: bar `#1c1f24` with a 1 px lighter line at the bottom, text `#e8eaed`, accent `#4c8dff`. In the smooth style everything with a curved edge is anti-aliased, and the text and the icons are blended in linear light so that light text on the dark ground keeps its weight.

### Font

This applies to the smooth style only; the crisp style has its fonts compiled in and never opens a font file, which is why the QVGA boards, where `--style auto` is crisp, need none installed. The text is rasterized from a TrueType file looked up at run time, the first that exists of `/usr/share/fonts/ttf/LiberationSans-Bold.ttf`, `/usr/share/fonts/ttf/LiberationSans-Regular.ttf`, `/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf`, `/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf` and `/usr/share/fonts/TTF/DejaVuSans.ttf`. `--font PATH` names another file instead of the list. The size is `--font-size PX` (default 11 px on the 18 px bar, in proportion to `--height` otherwise); the percentage in the slider row is 10 px at the default and the text rows 14, 12, 10 and 8 px. Only the glyphs the panel can show (the digits, the Latin letters, `:`, `%`, `-`, `,`, `.`, `>` and `~`) are rasterized, when the smooth style is first set up and again after every change of look that loads the assets (the file is opened and mapped again each time), in the bar's size, in the slider row's value size and in the four sizes of the text rows. If there is no usable font file (none of the list exists, or `--font` names a file that is missing or is not a font), the panel says so once on stderr and uses a built-in 5x7 bitmap font, drawn at twice its size, so that it never fails for lack of a font; that font has capitals only and draws lower case as capitals. The font file is trusted: stb_truetype does no range checking of a font's offsets, so `--font` must not name a file from an untrusted source.

### Subpixel text

This applies to the smooth style only (`--style auto` is smooth from 150 ppi, see Density). On an LCD with colour stripes the text of the opaque bar is drawn per stripe, which makes it about three times sharper across than a grey anti-aliased glyph. `--subpixel auto|rgb|bgr|none` (default `auto`) chooses. `auto` uses the layout the compositor advertises in `wl_output.geometry` for the output the bar is on (picowl's `[output] subpixel`), `horizontal_rgb` giving RGB (red at the left) and `horizontal_bgr` BGR, and only when the transform tells that the stripes run across the bar. The order is worked out from the panel's native layout and the output's transform, so a landscape panel with vertical stripes (the h3900) gets horizontal subpixel text once it is shown in portrait, and an h2200 turned by 90 or 270 degrees with `--edge top` gets greyscale, as does an output advertising `unknown`, `none` or a vertical stack with a normal transform (which is what hardware rotation looks like to a client). The strip of a rotated output is drawn in the panel's own orientation, so there the panel's native stripes count, whatever the transform is; under hardware rotation the panel learns them from `picowl-rotation-v1`, `wl_output` having the layout as the view sees it, so the strip gets subpixel text there too. `rgb` and `bgr` force an order whatever the compositor says, and `none` forces greyscale. The panel follows the output when it is rotated while the panel runs.

Each glyph is rasterized once, at start-up, at three times the horizontal resolution and filtered across the subpixels with a 5-tap FIR filter (14 61 106 61 14 over 256, a little wider than FreeType's default so the colour fringes of light text on the dark ground stay calm); drawing then blends the red, green and blue coverage into their channels, in linear light like the rest, in 8 bits, packed afterwards for an RGB565 buffer. The pen and the baseline are on whole pixels, so equal strings are equal wherever they are drawn and the digits do not jitter when the minute changes (the font's digits are tabular), and the height of the digits is made a whole number of pixels (a vertical scale of at most a few percent) so that their flat tops and bottoms are on pixel edges. Subpixel text needs to know the colour under it: text on a translucent ground is drawn in greyscale, which is the value in the slider row while `--popup-alpha` is below 255 (the default 224 is) and the whole bar when `--bar-alpha` is below 255; the value in an opaque row has subpixels. The icons are not text and stay anti-aliased shapes. Not verified on a real panel: the stripe order of the h2200 comes from the project owner, not from a datasheet.

### Crisp style

`--style crisp` (chosen by `--style auto` below 150 ppi; `smooth` is the look described above) draws nothing anti-aliased: every pixel of the text, the icons, the slider and the highlights is one of a few flat colours (the theme's ground, line, text, accent, highlight, track, thumb and its ring, the battery's three fills, the row's ground and line) and every edge, stem and line lies on a pixel boundary. The text is set in compiled-in bitmap fonts, so no font file is read and nothing is rasterized at run time: by default 7x13 bold for the bar and the value in the slider row, and 10x20, 9x15 bold, 7x14 bold and 6x10 for the text rows, the largest in which a wording fits (the same fit-or-shorten rule as for TrueType: the row loses words before it loses size). The fonts are the X11 misc-fixed fonts, which are in the public domain (the COPYRIGHT line of each BDF file says so; `panel/fonts/LICENSES` records it), as BDF files in `panel/fonts/` reduced to printable ASCII; `panel/fonts/gen-pixfont.py` turns them into `panel/panel-pixfont-data.c` (about 10 KB of const data, covering every character the panel prints, `>` and `~` included).

`--crisp-font fixed|dejavu` (default `fixed`) picks the text face. `fixed` is the misc-fixed set above. `dejavu` sets the same text in DejaVu Sans Bold hinted to bi-level (FreeType's v35 TrueType interpreter, mono target, no auto-hinter), baked offline into const data by `panel/fonts/gen-dejavu-bitmaps.c`, so the panel still has no FreeType and no font file at run time: Bold 11 px for the bar and the value in the slider row, and Bold 12, 11, 10 and 9 px for the text rows, with the same fit rule and the same whole-number scaling (a bar twice as tall gets glyphs twice as big). DejaVu is proportional (the hinted integer advance of each glyph and its own bearing, no kerning; the digits are all 8 px wide at 11 and 12 px, so the clock does not jitter), which changes three things. The bar's text widths follow the font as they do with a TrueType font: the clock is 36 px wide instead of 35 and the battery text 35 instead of 28, so with the default bar the battery's button and the two buttons left of it sit 7 px further left than with `fixed` (their size is the same: touch targets stay 36 px wide and as high as the bar); the rows' text area, the thumb, the track and the palette do not change. Wordings that no longer fit step down a size less often: `Wednesday 30 September 2026` is 222 px of the 224 px of the text area in Bold 12, while in `fixed` it falls to the third face (7x14 bold). The bar's digits are 8 px high instead of 9, so in the 17 rows of the bar they sit with 4 rows above and 5 below, a pixel above the middle. The DejaVu data is about 9 KB (95 characters per face, with an advance table). The bitmaps derive from Bitstream Vera, whose copyright, trademark and permission notice is shipped as `panel/fonts/DEJAVU-LICENSE` and has to go with copies of the generated data; `panel/fonts/LICENSES` records the details. Which of the two looks better on the real panel is not decided; only host renders exist ([doc/design/panel-text.md](doc/design/panel-text.md), and the [roadmap](doc/design/roadmap.md)).

The icons are 12x12 bitmaps (sun, speaker with no, one or two waves, a cross when muted) drawn at a whole multiple of their grid (24x24 in the row), the battery is an exact outline of 1 px with a nub, a gap and a fill in whole columns, the highlight and the slider track are flat rectangles without their corner pixels, and the thumb is a disc of odd diameter with a 1 px ring. The thumb is 21 px and the track 5 px high in this style (22 and 6 in the smooth one), so that both have a centre pixel; nothing else of the layout, the touch targets or the behaviour differs. The ground still honours `--popup-alpha` and `--bar-alpha` (a flat alpha over the window is not anti-aliasing), but the ring of the thumb is opaque; for an exact set of colours use `--popup-alpha 255`. A bar taller than the default scales the fonts by whole numbers (twice from 38 px, three times from 58 px), never by a fraction. `--font`, `--font-size` and `--subpixel` are ignored in this style, without a message, and `--crisp-font` does nothing in the smooth style.

To regenerate the font data: `python3 panel/fonts/gen-pixfont.py gen panel/panel-pixfont-data.c panel/fonts/6x10.bdf panel/fonts/7x13B.bdf panel/fonts/7x14B.bdf panel/fonts/9x15B.bdf panel/fonts/10x20.bdf`. The DejaVu data needs FreeType and the font files, so `gen-dejavu-bitmaps.c` is meant to run in a debian:trixie container with `libfreetype-dev` and `fonts-dejavu-core` (the commands are in its header), and `gen-dejavu-bitmaps --check panel/panel-pixfont-dejavu.c` fails if the committed file is not what it writes. The font choice, and what Windows CE did for comparison, are in [doc/design/panel-text.md](doc/design/panel-text.md).

### Translucency

The row is translucent by default so that what is behind it (a video, say) shows faintly through. `--popup-alpha N` (0 to 255, default 224, about 88 percent opaque; 255 is opaque) sets the opacity of the row's ground and of the ring around the thumb, not of the icon, the text, the track or the thumb. `--bar-alpha N` (default 255) does the same for the bar. A surface with any translucent part needs an ARGB8888 buffer (premultiplied alpha, as Wayland wants it); the panel picks the format of each buffer when it allocates it: RGB565 (XRGB8888 if the compositor does not offer RGB565) whenever everything on the surface is opaque, so the closed bar with the default options is RGB565 and only the surface with the row open is ARGB8888. The opaque region of the surface is the bar when the bar is opaque (and the row when it is opaque too), so the compositor need not blend under it. wlroots patch 0005 makes the scene notice when that region shrinks, so a window under a row that turns translucent stays visible.

### Density: the style and the height follow the display

A QVGA iPAQ (the h2200, the h3xxx and the h5xxx, about 106 to 115 ppi) cannot carry anti-aliased 11 px text, which is why the crisp style exists; the hx4700's 480x640 at about 203 ppi can, with subpixel text. One binary and one package serve all of them: the panel decides when it runs, from what the compositor tells about the output the bar is on, and again whenever that output changes. Nothing is chosen at build or packaging time.

The density is the diagonal of the mode in use, in pixels, over the diagonal of the physical size `wl_output.geometry` reports, in inches (25.4 mm), rounded to a whole ppi. The diagonals make it independent of the orientation: a rotation, a mode that hardware rotation reports swapped, and the landscape scan frame of the h3800 and h3900 (77x57) give the same number. The mode is the one `wl_output.mode` flags as current. Reference values: the h2200, 53x71 mm at 240x320, is 115 ppi; the h5xxx, 57x77, is 106; the hx4700, 60x80, is 203 at 480x640 and 102 at 240x320 (both its modes carry the same size, so the density of the mode in use comes out right by itself). The sources of these sizes are in [doc/design/ipaq-displays.md](doc/design/ipaq-displays.md).

`--style auto` (the default) takes the crisp style below 150 ppi and the smooth one, with the subpixel rule of `--subpixel`, from 150 ppi on; the rounded number is what is compared and what is logged. `--style crisp` and `--style smooth` win over the density. `--height auto` (the default) is 18 for the crisp style, whose fonts scale by whole numbers only, and for the smooth style 18 at 110 ppi scaled with the density, `round(18 * ppi / 110)` made even by adding one and held to 18..80 (203 ppi gives 34; 106 ppi gives 18); `--height N` wins. The font follows the height (`--font-size` wins). `--dpi N` (20 to 1000; anything else is an error) uses N instead of the output's density, and the style and the height follow it.

Without a usable physical size (0 in either side, as `wl_output` of the h3xxx and hx4700 on older kernels and the headless output report) the density comes from the class of the mode: a long side of 640 or more is the VGA class, taken as 200 ppi; anything smaller, or no mode, is the QVGA class, 110 ppi. `[output] size_mm` in picowl.ini (Configuration) puts a size in for such a board, so that the density is real. A reported or configured size that works out to less than 60 or more than 400 ppi (the iPAQs are 102 to 203) is taken for a typo or a bad value, such as `size_mm = 5x7` for 57x77, and is not used: the panel warns once and takes the class of the mode as without a size. `--dpi` is the way to a density outside that range. The panel logs one line at INFO on stderr for each decision, `info: 203 ppi (reported: 60x80 mm, mode 480x640), vga class, style smooth, height 34`, the source being `reported`, `fallback`, `override` or `implausible, using mode class`; `--dump-state` prints a `look` line with the same style, ppi, source, class, height, mode and size.

When the output sends `done` after new geometry or a new mode (a rotation, a mode switch, a hotplug of the output the bar is on) the decision is made again. If the style or the height changed, the panel loads the fonts and icons of the new look, closes the open row and any drag, asks the compositor for the new surface size and the new exclusive zone (sent whenever the bar's height changes, never for the row), and draws nothing until a configure arrives for the size it asked for last, so no frame mixes the two looks; it then rebuilds the layout, the regions and the buffer, which is the old buffer when the size is the same. The strip of a rotated output (below) is unaffected, the density being the same at every rotation. Not verified on a board: the densities of the real kernels' sizes, a live mode switch of the hx4700, and the look of either style on the hx4700's panel. The headless picowl cannot change its density at run time (its mode only turns, with hardware rotation, and its size is read at start), so the tests drive a change of look through the `d N` token of `--inject` (Options).

### Rotated output: the bar on the short side

A 240x320 panel turned to landscape (`[output] * = 90` or `270`) is a view of 320x240, and a bar along its top would take 18 of the 240 pixels of height. With `--edge auto` (the default) the bar goes to the edge that is the physical top of the device instead, as a vertical strip: the right edge of the view at 90, the left edge at 270 (with `--bottom` the physical bottom, the other one). On the panel it looks exactly like the bar of the portrait orientation, with upright text, the same layout and the rows below it. It is drawn in the panel's own orientation, into a buffer as long as the short side (240x18, 240x54 with a row open), and `wl_surface.set_buffer_transform` (90 or 270, the output's own transform) tells the compositor how the buffer maps onto the strip, so that the buffer reaches the scanout unchanged. A row opens inward of the bar, as it opens below it in portrait. The exclusive zone is the bar's width on that edge and never the row's: a toplevel is 302x240 in the view whether the row is open or not, and the tiled layout and the keyboard pan work on that usable area (the strip is anchored to the top and the bottom as well, so it is not a keyboard to pan for). Pointer and touch positions arrive in the strip's own coordinates and are mapped back into the bar's, so taps on the buttons, drags on the sliders and the 3 s auto-close are the same as in portrait.

The panel learns the output's transform from `wl_output.geometry` and follows a rotation while it runs: it anchors to the other edge and draws into a new buffer once the compositor has configured that. Normal, 180 and the flipped transforms keep the bar on the top edge, and `--edge top` keeps it there at every rotation, along the long edge of the rotated view. Hardware rotation is not seen in `wl_output`, because picowl then sends the transform `normal` (the display turns the picture), so the panel learns the turn from `picowl-rotation-v1` (Protocols): picowl sends `rotation(transform, hardware, subpixel)` for the output when the panel asks, and again whenever the rotation changes at run time (the `rotate` key action). When it says the display does the turning, the panel uses that transform for the strip (the h2200 with `[output] * = 90` gets the strip on the physical top of the device) and the panel's own subpixel layout from the same event, so the strip's text is subpixel text under hardware rotation as under software rotation. With `--edge top` or at 180 degrees the bar is in the view's orientation and the subpixel layout of `wl_output.geometry` (the view's, vertical for a portrait panel turned by 90) decides, which gives greyscale for vertical stripes. With software rotation the panel reads `wl_output` and ignores the hint. With a compositor without the global (another compositor, or picowl before the protocol existed) the bar stays on the top of the view. Nothing changes for other clients: `wl_output`, the layout and the buffers are what they were.

### Touch

picowl turns the stylus into pointer events: a press is `BTN_LEFT`, a drag is motion. The panel's layer surface has a built-in `[layer.panel] hold_action = none` rule, so the press is sent at touch-down; with the default tap-and-hold it would be held back until the stylus moved `slop_px` (8 px), and a slow drag would turn into a right click after `hold_ms`, which swallows the rest of the touch. A `[layer.panel]` section of your own merges over it.

- **A tap on a button opens its row** below the bar (the action is at the press, not the release): a slider row for the backlight and the volume, the date row for the clock, the estimate row for the battery. A tap on the same button closes it, and a tap on another button switches the row to that one. A button never sets a value: a stray tap on the sun cannot darken the screen and one on the speaker cannot mute, and the clock and the battery set nothing. The battery is read again when its row opens, so the row does not show what was true up to 30 s ago.
- **The row closes by itself 3 s after the last touch** (any press, motion or release on the panel moves the deadline; a stylus that is still down keeps the row open). The timer exists only while the row is open and is not polled: when it fires it is set again for what is left.
- **In the row**, a press on the track area (the track plus 6 px on either side, and the whole height of the row) sets the value from the x position of the stylus, motion while pressed keeps setting it, and the release ends the drag; a drag keeps its slider when the stylus leaves the row. The row's icon and the value text are inert, and so is a press anywhere else on the panel. A text row has no slider: a press or a drag in it does nothing except count as a touch, which keeps the row open for another 3 s. The redraw is immediate; the value is written to the system at most every 50 ms during a drag and always at the release.
- **The surface grows, the windows stay.** Opening the row asks the compositor for a surface of bar plus row height (`set_size`), and the row is drawn when the new size has been configured and acknowledged; the old, smaller buffer stays on screen until then. The exclusive zone stays the height of the bar the whole time, so toplevels keep their size (240x302 on a 240x320 output with the default bar, open row or not) and the row covers them. The input region is the bar and, while it is open, the row, never more, whatever size the compositor made the surface. Closing is the same in reverse, and a redraw damages only the rectangles that changed (the track area and the value for a drag, one button for a highlight).
- **Backlight:** `/sys/class/backlight/<dev>/brightness` and `max_brightness` of the first device (by name) with a `max_brightness` of at least 1; the sysfs root honours `PICOWL_SYSFS_ROOT` as in picowl. The percent maps linearly onto the raw range, rounded to nearest, with a floor of 5 percent, so the screen cannot be turned black by accident. The level is read at start, again whenever the pointer enters the panel surface (every touch starts with an enter) and before a drag starts, so the thumb follows what picowl or another tool did meanwhile (an idle dim, the `[power.low]` cap, `brightness` written by a script); there is no timer, so a change while nobody touches the panel shows at the next touch. The `[power.low]` cap wins over the slider: on the LOW profile picowl lowers a level above the cap when the profile changes (logged by picowl), and the thumb shows the capped level at the next touch. `brightness` must be writable by the user the panel runs as (the udev rule installed by picowl, `data/90-picowl-backlight.rules`, gives the `video` group write access); when a write fails (EACCES) or the mixer refuses a volume, the thumb goes back to the level the device holds and one message is logged on stderr. The panel flushes a value still waiting for the 50 ms throttle on every way out, a lost display connection included. If the board has several backlight devices, give picowl and the panel the same one: picowl prefers `firmware`, `platform`, `raw` (`[power] backlight`), while the panel takes the first name.
- **Volume:** alsa-lib's simple mixer on the card `default`. The element is the first of `Master`, `PCM`, `Headphone` and `Speaker` that has a playback volume, else the first element with one. 0..100 percent maps linearly onto the element's raw range (not onto decibels), and raising the volume above 0 also switches the playback on (it is never switched off). Changes made by other programs are followed: the mixer's descriptors are in the poll loop, so there is no timer, and the thumb and the speaker icon move while nobody touches them (not during the panel's own drag, where the quantized value would make it jump). With no mixer or no element the button is greyed out and ignores taps (and an open row closes if the mixer goes away). The user needs access to the sound device (the `audio` group; picowl's unit runs as root).

### Time left

The battery row says how long the battery lasts or how long it takes to fill. The panel makes the estimate itself from what the battery driver exports under `<sysfs root>/class/power_supply/<battery>/` (`PICOWL_SYSFS_ROOT` as everywhere), because the drivers of these handhelds disagree on what they have. Every attribute is optional and any subset gives an answer.

- **Rate.** The current in uA, `current_avg` if the battery has it, else `current_now`, else the power `power_now` in uW. It is sampled every 30 s (the panel's battery poll, and once when the row opens) and smoothed in integer arithmetic: a running mean over the first eight samples, then an exponential average with weight 1/8. A sample under 10 mA (40 mW for power) is idle and skipped, one above 3 A (15 W) is a glitch and dropped. A change between charging and discharging, a change of the attribute used, or more than 600 s between samples starts the average again, and two samples after a plug event are ignored while the current settles. The filter is kept across a suspend; there are no made-up samples.
- **Direction.** From the charger supply (a `Mains` or `USB*` supply, the same scan as for the `AC` text) and the sign of the smoothed current (negative while discharging, positive into the battery) rather than from `status` alone, which the DS2760 driver only polls every 60 s: no charger online is discharging; with one online the battery is charging above +10 mA, not charging below -5 mA, and in between what `status` says. `status` `Full` is `Fully charged` and `Not charging` is `Not charging`, with no time. Without a charger supply at all the status decides.
- **Remaining charge** (discharging, rate a current): `charge_now` less `charge_empty` (`charge_now` is the raw counter including the reserve below `charge_empty`; 0 if there is none, never below 0), else `capacity` times `charge_full`, else `capacity` times `charge_full_design`, else `capacity` times `--battery-mah`. The charge to go (charging) is `charge_full` less `charge_now`, with `charge_full_design` and `--battery-mah` as fallbacks for `charge_full`, and the capacity in place of `charge_now`. A battery that only has a power uses `energy_now` (else `capacity` times `energy_full`, then `energy_full_design`) and `energy_full` in the same way; charge and energy are never mixed. The time is the amount times 60 over the smoothed rate, in minutes, in 64 bits.
- **`--battery-mah N`** (0 to 100000, default 0: unknown; a value outside is clamped) is the capacity of the battery in mAh, the last resort when the driver exports neither `charge_full` nor `charge_full_design` (the rated capacity table of some drivers is known to be wrong, which is why a figure from the user comes last).
- **The kernel's own time.** `time_to_empty_now` (seconds) is used only as a hint until the filter is warm, between a minute and 24 h and while discharging, and is shown with a `~`; after that the panel's number replaces it. It is the only number that can be shown before the first two minutes are over.

Wording, with the percentage in front when it fits: `1 h 30 min left` (60 min and more, minutes zero-padded) and `45 min left`; `~1 h 20 min left` for the kernel's hint; `1 h 15 min to full` while charging below 90 percent (above it the current-based time is optimistic because of the constant-voltage taper, so the row says `Charging`); `Fully charged`; `Not charging`; `> 24 h left` and `> 24 h to full` beyond a day; `Estimating...` while the filter warms up (four valid samples, two minutes) or there is no usable rate (idle, no current attribute); `On AC power` only when a mains supply is online and there is no battery; `--` for no battery data at all. A time under a minute is never shown as `0 min`; it says `Estimating...`. Minutes are rounded to the nearest minute below 10 min, to the nearest 5 min up to 5 h and to the nearest 15 min above, and while discharging the minutes shown move only when the new value is more than max(3, shown/8) minutes away (more than 1 below 10 min), so an open row does not flicker. The row is drawn again only when its text changes.

These numbers (the 10 mA idle floor, the 3 A ceiling, four samples, weight 1/8, two skipped samples, 600 s, the thresholds of the direction, 90 percent, the rounding steps and the hysteresis) are a synthesis from how the kernel documents the attributes and from what other programs do. They are named constants at the top of the estimator in `panel/panel-logic.h`, are covered by unit tests of the arithmetic and have not been tuned on a real battery. The iPAQ h2200's gauge is a DS2760 (`ds2760-battery.0`); the panel is written for the attributes its driver exports as read from the kernel source (`status`, `voltage_now`, `current_now`, `charge_full_design`, `charge_full`, `charge_empty`, `charge_now`, `temp`, `time_to_empty_now`, `capacity`; no `current_avg`, no `time_to_full_*`, no `energy_*`, so the time to full is always the panel's), and the end-to-end test models exactly that. Not verified on a real h2200: that the driver on its kernel exports those, what the numbers look like, and the accuracy of the rated capacity.

### Backlight and picowl's dimming

picowl dims the backlight after `dim_after_s` and restores the user level on the next input. Just before it writes a reduced level (the dim, and the `[power.low]` cap when the profile changes) it compares the level the device shows with the one it left it at, and adopts a different level as the user level, so a slider change survives dim and undim ([doc/power.md](doc/power.md)). The touch that undoes a dim restores the user level first and the slider write follows it, so dragging a dimmed screen works and is picked up at the next dim.

### Starting it and panel_autohide

Start it from picowl:

```ini
[autostart]
cmd = picowl-panel
```

picowl does not restart it when it exits. The panel exits with status 0 when the compositor goes away or on SIGTERM, and with status 1 and one message on stderr when it cannot connect, when the compositor has no `zwlr_layer_shell_v1` or no output, on a protocol error, and on a bad option.

The panel is a normal layer-shell panel: `[zerocopy] panel_autohide` (default `true`) hides it while an app has the focus, and with it the bar and its slider row. With the default the bar is only visible on an empty desktop, or after the `panel` key action forced it to show. To keep it on screen with an app open set `panel_autohide = false`; the exclusive zone then makes toplevels use the rest of the output, 240x302 below the 18 px bar on a 240x320 output, with the slider row open or not. The panel follows a rotation: picowl sends the new size and the panel lays itself out again and allocates a new buffer (the open row stays open).

### Options

- `--height N|auto`: height of the bar; N is clamped to 18..80. `auto` (default) is 18 for the crisp style and scales with the density for the smooth one (Density). The slider row is 36 px, or the bar plus 8 px when that is more.
- `--edge auto|top`: `auto` (default) puts the bar on the short side of an output rotated by 90 or 270 degrees (Rotated output); `top` keeps it on the top (with `--bottom`, the bottom) edge of the view at every rotation.
- `--bottom`: anchor at the bottom edge (default: top). The slider row then opens above the bar, so that the bar stays at the edge of the screen; the bar's and the row's 1 px lines are on the side of the windows.
- `--style auto|smooth|crisp`: the look (default `auto`: crisp below 150 ppi, smooth from there, see Density); `smooth` and `crisp` win over the density.
- `--dpi N`: use this density, 20..1000, instead of the output's; the style and the height follow it.
- `--crisp-font fixed|dejavu`: the pixel font of the crisp style (default `fixed`).
- `--font PATH`: a TrueType font file instead of the default list (smooth style); the built-in bitmap font when it cannot be used.
- `--font-size PX`: size of the text, clamped to 8..48 (default 11 at the default height; smooth style).
- `--bar-alpha N`: opacity of the bar, clamped to 0..255 (default 255).
- `--popup-alpha N`: opacity of the row, clamped to 0..255 (default 224).
- `--subpixel auto|rgb|bgr|none`: the order of the colour stripes for subpixel text (default `auto`; smooth style); see Subpixel text.
- `--battery-mah N`: the battery's capacity in mAh, 0..100000 (default 0, unknown); see Time left.
- `--dump-state`: print the widget values, the look and the geometry as `key=value` lines on stdout after the first frame, and exit 0 (for tests).
- `--exit-after-frame`: exit 0 after the first frame.
- `--help` (or `-h`).
- Test only: `--watch` is `--dump-state` that does not exit and prints the state again after every redraw, naming the widgets that were redrawn. `--inject SPEC` feeds events through the same handlers as `wl_pointer` after the first frame, tokens separated by `;` or spaces: `p X,Y` press, `m X,Y` motion, `r` release, `e X,Y` enter, `ibl`, `ivol`, `icl` and `ibat` tap the backlight, volume, clock or battery button, `w MS` runs the event loop for MS milliseconds, `b RAW` writes the backlight as another process would, and `d N` makes the panel decide its look again as if the output's density were N ppi (0: the output's own). A `+` in front of a token leaves the compositor's answer to it unread until the next token without one, which is how a test makes a touch or a second change of look arrive before the configure of the first. `PICOWL_PANEL_BATTERY_POLL_S` shortens the 30 s battery interval.

A missing option argument, an unknown option (the old `--scale` among them) or a value that is not a number exits with status 1.

### Redraws and wake-ups

A redraw happens only when a value changes or the stylus drags, and only the changed rectangles are damaged (a clock update is 40x17 px; a drag damages the track area and the value of the row, about 206x35 px). The program wakes up for the Wayland socket, once a minute for the clock, every 30 s for the battery, for the mixer, only while a slider row is open for the timer that closes it, and only while a drag has a value waiting, for a deadline of at most 50 ms. Nothing is rasterized or allocated while it redraws: the glyphs, the icons and the parts of the slider are masks built when the panel starts or its layout changes (a unit test counts the heap across 300 redraws), and a redraw blends them. A resize (a rotation, a row opening or closing) lays out again and allocates a new buffer, which is where the masks are rebuilt if their size changed; the surface keeps one wl_shm buffer at a time and the old one is released after the new one is attached.

The rest of the widget design in `doc/panel.md` (popups and tap-and-hold menus, the app list, the network and the other widgets, brightness through a picowl protocol instead of sysfs) is not implemented; see the [roadmap](doc/design/roadmap.md).

## Companion pieces

- **On-screen keyboard, wvkbd-ipaq.** wvkbd rebuilt for the iPAQ as a series of eleven patches on a pinned upstream commit: a meson build, RGB565 rendering, labels through coverage masks with built-in bitmap text, a 240x320 `ipaq` layout, damage of only the changed keys, keeping the surfaces on hide, the pointer position from `wl_pointer.enter`, and a minimum time a pressed key stays highlighted. The series is in `oe/recipes-graphics/wvkbd/files/` for the recipe `oe/recipes-graphics/wvkbd/wvkbd-ipaq_git.bb` and again in `subprojects/packagefiles/wvkbd/` for the meson wrap (`-Dosk_tests=enabled` builds it and runs its tests); the `wvkbd-patches-sync` test keeps the two copies the same apart from the `Upstream-Status` line. picowl supervises it through `[osk]`. Design and status: [doc/design/osk.md](doc/design/osk.md).
- **havoc terminal patches.** `oe/recipes-graphics/havoc/` carries two patches for the havoc terminal, for a havoc recipe outside this layer: 0001 binds text-input-v3, so that the on-screen keyboard can appear when the terminal has the focus; 0002 damages only the cells that changed instead of the whole surface, which cuts the bytes a typed character uploads over the MediaQ bus. The tests and the measured damage are in [its README](oe/recipes-graphics/havoc/README.md).
- **Media player.** The player drives the display itself through a DRM lease (`[lease]`, [doc/lease.md](doc/lease.md)) and uses `picowl-buffer-v1` with a per-app buffer pool, the `caching` event, an idle inhibitor and a per-app hold rule. What it needs from picowl and the state of each item: [doc/mediaplayer-integration.md](doc/mediaplayer-integration.md).
- **wlroots patches.** Five patches on wlroots 0.19.0 ([README](subprojects/packagefiles/wlroots/README.md)): 0001 lets the pixman renderer read dmabuf client buffers through mmap; 0002 adds memcpy and fill fast paths to the pixman render pass; 0003 adds DRM hardware rotation and copy-type outputs; 0004 offers overlay planes in DRM leases and fixes a use-after-free in the lease grant; 0005 makes the scene update a buffer node when its opacity changes.
- **Tools.** `picowl-cursor-convert` (Cursor) and `picowl-commit-loop`, a paced wl_shm commit loop for the rotation measurement (workloads W3 and W4 of [doc/rotation-measurement.md](doc/rotation-measurement.md)), are built with the `tools` option.

## Protocols

picowl's own protocols:

- `picowl_buffer_manager_v1` / `picowl_buffer_v1` (`protocols/picowl-buffer-v1.xml`, version 2): compositor-allocated RGB565 dmabufs, with the `copy_type`, `caching` (version 2: cacheable or write-combined mapping), `copied` (direct scanout on copy-type outputs) and `retained` events, so that clients on copy-type outputs can use a single buffer until the compositor composites. Only with the DRM backend and `[zerocopy] enable`. See [doc/buffers.md](doc/buffers.md).
- `picowl_rotation_manager_v1` / `picowl_rotation_v1` (`protocols/picowl-rotation-v1.xml`, version 1, `src/rotationproto.c`): how an output is turned, including by the display. `get_rotation(wl_output)` gives a `picowl_rotation_v1` that sends `rotation(transform, hardware, subpixel)` at once and on each change: the `wl_output.transform` that turns the panel as built, whether the display does it, and the panel's native `wl_output.subpixel`. It is additive and does not change `wl_output`, so a client that does not bind it sees what it saw before. picowl-panel is the one client that uses it (Panel).
- `zwp_linux_dmabuf_v1` version 4 with feedback built by picowl (RGB565/LINEAR, a scanout tranche), next to `picowl-buffer-v1`.

From wlroots 0.19, set up by picowl:

**Shell and composition**
- `wl_compositor` (version 6), `wl_subcompositor`, `wl_shm`.
- `xdg_wm_base` (version 3): toplevels, popups.
- `zwlr_layer_shell_v1` (version 4): panel, overlay, background and lock surfaces, with exclusive keyboard interactivity.
- `wl_data_device_manager`: the clipboard.
- `zwp_primary_selection_device_manager_v1`: select to copy, middle click to paste; picowl accepts every client request, as it does for the clipboard.
- `xdg_activation_v1`: activation requests.
- `zxdg_decoration_manager_v1`: picowl always chooses server-side, which means no decoration.

**Input**
- `wl_seat` with `wl_keyboard`, `wl_pointer` and `wl_touch` (when a device has it). The seat always advertises the keyboard capability, also with no keyboard device (headless, or before one is plugged in), and always has an active keyboard with a keymap: an internal keyboard that never produces events holds picowl's xkb keymap and takes over when the active keyboard is destroyed (the on-screen keyboard's virtual keyboard, an unplugged device), until a real keyboard sends its next key. A client that creates its `wl_keyboard` at any time therefore gets an xkb_v1 keymap (a client without one ignores all keys); a client that probes the capability to learn whether a keyboard is attached gets no answer from it. Tested in `smoke` with `pw-test-client --expect-keymap`, also after a virtual keyboard was destroyed.
- `zwp_virtual_keyboard_manager_v1`: on-screen keyboards. Kept next to text-input and input-method, because GTK+2 applications have no text-input and the keyboard's function keys still inject key events through it.
- `zwp_text_input_manager_v3` and `zwp_input_method_manager_v2` (`src/imrelay.c`, `protocols/input-method-unstable-v2.xml`). Keyboard focus is relayed to text-input: when the keyboard focus changes, the text inputs of the client that lost it get `leave` and those of the client that gained it get `enter`, also for a text input created after the client already has the focus. The text input of the focused client drives the input method: `enable` becomes `activate`, followed by surrounding text, content type and change cause (only the parts the client used) and `done`; every later `commit` sends that state again; `disable`, losing the focus or destroying the text input becomes `deactivate`. What the input method commits (`delete_surrounding_text`, `commit_string`, `preedit_string`) goes to the active text input, followed by `done`. At most one text input is active, and only one whose surface has the keyboard focus. One input method is accepted per seat; a second one gets `unavailable`. Input method popup surfaces (candidate lists) are shown in the overlay layer below the text cursor rectangle of the active text input, above it when there is no room below, clamped to the usable area of the output the cursor is on (so they stay clear of exclusive layer surfaces), and told where the cursor is inside them with `text_input_rectangle`; they are placed again when the text input commits, the popup commits or the layout is arranged (rotation, panel changes), and hidden while the input method is inactive. Tapping a popup does not move the keyboard focus. Limits: the input method keyboard grab (`grab_keyboard`, for engines that want raw keys) is logged and the grab object destroyed, so keys always go to the focused application (an on-screen keyboard injects them through virtual-keyboard), and a long press on a popup uses the `[touch]` hold settings. An on-screen keyboard that uses input-method must use layer-shell keyboard interactivity `none`, or tapping it moves the focus away and hides it again. Tested by `tests/pw-im-client.c` (two connections, an application and an input method) in `smoke`. Design: [doc/design/osk.md](doc/design/osk.md).
- `zwp_tablet_manager_v2`: drawing tablets through libinput (`src/tablet.c`). Proximity, tip, motion, pressure, distance, tilt, rotation, slider, wheel and button events go to the surface under the tool when its client bound tablet-v2, and that surface keeps the tool while a tip or button is down; pads get focus with the tool and forward buttons, rings and strips. Tablet coordinates are mapped to the output and follow its rotation as touch does (software rotation by wlroots, hardware rotation by the libinput calibration matrix). A tablet that cannot follow a hardware rotation because it has no calibration matrix is logged and ignored until the output is unrotated. Events are dropped while the display is leased or blanked (the waking event is swallowed). There is no pointer emulation for clients without tablet-v2, there are no configuration keys, and tablet tools do not set the cursor image. Not tested on a tablet.
- `wp_cursor_shape_manager_v1` (version 1): only the client with pointer focus (or the surface a tablet tool is in) is heard. `wait` and `progress` show the hold animation (stopped when the client asks for another shape or loses focus); every other shape shows no image, as picowl draws no pointer. A client request never replaces or stops the animation of a touch hold.
- `zwlr_virtual_pointer_manager_v1`: only with `PICOWL_TEST_VIRTUAL_POINTER=1`, for tests (Testing); any client could inject input through it, so it is never on in a session.

**Output and rendering**
- `wl_output`: geometry (physical size, see `[output] size_mm`; subpixel, see `[output] subpixel`), mode, transform.
- `wp_presentation` (version 2): frame timing.
- `wp_viewporter`: scaling and cropping.
- `zxdg_output_manager_v1`: the logical position and size of each output, from the output layout. Screenshot tools such as grim need it and otherwise guess a zero-sized output. With hardware rotation the logical size is the rotated one, as clients see it.
- `wp_single_pixel_buffer_manager_v1`: solid-colour buffers.
- `wp_content_type_manager_v1` (version 1): clients may declare photo, video or game content. picowl only logs the hint of a toplevel at debug level when it maps; nothing acts on it.

**Window management**
- `zwlr_foreign_toplevel_manager_v1`: list, activate and close for a taskbar; minimize requests are ignored.
- `ext_foreign_toplevel_list_v1` (version 1): toplevel enumeration.

**DRM lease**
- `wp_drm_lease_device_v1` (version 1): with the DRM backend and `[lease] enable` only; see [doc/lease.md](doc/lease.md).

**Screen capture**
- `zwlr_screencopy_manager_v1` (version 3): only with `[capture] enabled = true`; see [doc/capture.md](doc/capture.md). `ext_image_copy_capture_v1` is not implemented.

**Power and idle**
- `zwlr_output_power_management_v1`: blanking requests (on and off).
- `ext_idle_notifier_v1`: idle and resume notifications for clients such as screen lockers.
- `zwp_idle_inhibit_manager_v1` (version 1): a video player keeps the screen on while its surface is visible. picowl honours it in its own dim and blank timers and reports it to `ext_idle_notifier_v1` clients; see [doc/power.md](doc/power.md).

## Testing

```sh
meson setup build -Dtests=true      # add -Dosk_tests=enabled for wvkbd-ipaq's tests
ninja -C build
meson test -C build --print-errorlogs
WLR_SCENE_DISABLE_DIRECT_SCANOUT=1 meson test -C build   # force composition
```

All tests run picowl headless (`WLR_BACKENDS=headless`, the pixman renderer, no input devices), so they need no hardware and no root, except `lease-vkms`. `tests/meson.build` defines 36 tests; with `-Dosk_tests=enabled` the wvkbd-ipaq subproject adds 21 of its own, 57 in all. `pan-e2e-*` need `grim` and python3 with PIL and skip without them; `panel` and `panel-e2e` need the panel to be built, and `panel-e2e` fails without a TrueType font for its smooth-style sections.

Unit tests (one C program each, against the module named): `config` (the INI parser, with `tests/test-config*.ini`), `touchhold`, `home`, `oskstate`, `cursorfit`, `cursor-builtin`, `leasepolicy`, `cursorshape` (shape mapping), `implace` (input method popup placement), `tile` (the layout arithmetic), `scenehit` (hit testing of moved scene nodes), `grab` (a touch held on a moving window), `rotate`, `pointercal` (tslib parsing and matrix conversion), `copytype` (driver table and caching), `zbquota` (buffer pools), `copyrel`, `pixman-pass` and `pixman-dmabuf` (wlroots patches 0001 and 0002), `scene-opaque` (patch 0005), `bufproto` (bind events and version gating of `picowl-buffer-v1` over a socketpair, then `pw-test-client` at versions 2 and 1), `backlight`, `powersupply`, `dim`, `subpixel` (the stripe order a client sees), and `panel` (the wayland-free parts of `picowl-panel`: value mapping, text, layout, the touch state machine, fonts, the density decision `pl_look_resolve` and the stale-configure rule `pl_configure_stale`, drawing in RGB565 and XRGB8888 into a canvas with guard bytes, the battery estimator, sysfs access on fake trees).

End-to-end tests (shell scripts that start picowl headless with test clients from `tests/`):
- `smoke`: start-up and an xdg-shell client, the always-on globals (`--expect-global`), the degraded zero-copy paths without a DRM backend (`--zerocopy`, `--zerocopy-count`, `--probe`), the keymap of every `wl_keyboard`, the text-input to input-method relay (`pw-im-client`), capture off by default and the background colour with `[capture]` on (`pw-capture-client`), `PICOWL_HEADLESS_SIZE`, and `[output] subpixel` in `wl_output.geometry`.
- `tile-e2e`: two toplevels over one connection, in landscape and portrait: the configure sizes, `second_min`, hint changes at run time, a partner going away, `[layout] focus`, the keyboard stand-in with and without an exclusive zone, and the panel's strip on a short edge.
- `pan-e2e-pixels` and `pan-e2e-pixels-zone`: screenshots (grim) of the keyboard pan, with a keyboard that asks for no exclusive zone and one that asks for a zone.
- `osk-e2e`: `[osk]` supervision with `pw-osk-fake` standing in for wvkbd: the signals of the key actions, the restart backoff and giving up, no `cmd`, a missing program, `restart = no`.
- `power-e2e`: dimming and the LOW cap against a fake sysfs tree (`PICOWL_SYSFS_ROOT`), and idle inhibitors.
- `rss`: VmHWM of picowl with a test client mapped (headless, 1280x720) against a ceiling, the meson option `rss_ceiling_kb` (default 12288 kB; the headless value is about 9.5 MB); `PW_RSS_CEILING_KB` overrides it for one run.
- `wvkbd-patches-sync`: the two copies of the wvkbd-ipaq series are the same apart from the `Upstream-Status` line.
- `panel-e2e`: `picowl-panel` against headless picowl at 240x320 with a fake sysfs tree (backlight 600 of 1023, battery 73 percent) and `tests/pw-fake-ctl.c`, an alsa-lib control plugin whose state is a file, standing in for a sound card. Sections A to K: the first frame (`--dump-state`), touch through `--inject` (values reach sysfs and the mixer, the 5 percent floor, a drag from outside, a greyed slider), the usable area of a second client (top, bottom, `--height`, restored when the panel exits), no redraw or wake-up while nothing changes, battery and external volume changes followed, the relayout after a rotation with `--edge top` (`pw-key-client` presses the `rotate` binding), the exit codes (no compositor, `tests/pw-bare-server.c` without a layer shell, no output, compositor gone), translucency over a window (capture), subpixel text on the clock, the row over a video that was already playing, the date row and the battery row with the estimator on a fake DS2760. Section U: the crisp style has exactly the theme's colours. Section V: the panel on an output rotated by 90 and 270 degrees with software rotation; the raw 240x320 scanout of the bar, the slider row and the date row is compared pixel by pixel with the portrait one, and a pointer put at the logical position of the physical bar's clock, the backlight button and the slider (`pw-pointer-client`) opens the date row, drags the value and lets the row close after 3 s. Section W repeats the pixel comparison, the placement, taps, a rotation at run time and a blank and unblank with the power key (after which `wl_output` still says the transform `normal`) on the path hardware rotation takes (`PICOWL_TEST_HW_ROTATION=1`), with the capture turned back by the plane's rotation (`pw-capture-client --plane 90|270`), and the fallback without the hint (`PICOWL_TEST_NO_ROTATION_HINT=1`); what a real plane does to the picture is not tested. Section X: the density. The headless output gets a physical size from `[output] size_mm` (and 480x640 from `PICOWL_HEADLESS_SIZE`); `--style auto` is crisp at 240x320 on 57x77 mm with exactly the palette of the crisp style, and smooth, 34 px, with colour fringes on the clock at 480x640 on 60x80 mm; the fallback class without a size, a refused `size_mm`, the per-output `NAME.size_mm`, `--style`, `--dpi` and `--height` winning, a bad `--dpi`, a `size_mm` of 5x7 that is warned about once and not used, the same density under software and hardware rotation with no new decision when the output rotates, and a mode turned at run time (`wl_output.mode` with the current flag and `done` reach the panel, which takes no new decision, the diagonal being the same). A change of the density itself cannot happen on the headless output, so the change of look goes through the `d N` inject token, which calls the panel's `look_update` directly: crisp 18 to smooth 34 and back with the exclusive zone, the usable area, the palette and the rows checked, an open row closed, a touch right after the change (before the surface is rebuilt) that must not open a row for the old layout, two changes in a row, the same height in the other style rebuilt in place, the exclusive zone and the sizes on the wire, and the resident size after 45 round trips. The code from `wl_output` to `look_update`, and a configure that arrives in the same batch as a change of look, are not driven end to end; `test-panel` covers the decision and the stale-configure rule. The sections before X run the panel with `--style smooth`, since its own default is crisp on the headless output, which has no size. With `PW_PANEL_TEST_SETTIME=1` (root only: it sets the system clock and puts it back) it also checks that the clock follows a change of the system time and the minute timer.
- `lease-vkms` (suite `vkms`): the DRM lease end to end on the vkms virtual KMS device. Opt-in with `PW_LEASE_VKMS=1`; it needs root, the vkms module and `/dev/dri`, and skips (exit 77) otherwise.

Test seams, environment variables that picowl or the panel read for tests only: `PICOWL_HEADLESS_SIZE` (Running), `PICOWL_SYSFS_ROOT` (a fake sysfs for the backlight and the power supplies), `PICOWL_TEST_VIRTUAL_POINTER=1` (offers `zwlr_virtual_pointer_manager_v1`, so that `tests/pw-pointer-client.c` can put the pointer at a place of the output; the headless backend has no pointer of its own), `PICOWL_TEST_HW_ROTATION=1` (headless, with `PICOWL_HEADLESS_SIZE`: the output advertises as with hardware rotation, with the rotated mode, the transform `normal`, the subpixel layout as the client sees it and `picowl-rotation-v1` saying hardware, while nothing turns the frame), `PICOWL_TEST_NO_ROTATION_HINT=1` (no `picowl-rotation-v1` global) and `PICOWL_PANEL_BATTERY_POLL_S` (the panel's battery interval).

To run the full suite in a clean container, as the project does (Apple `container` on macOS here; any Debian trixie container works the same way), stream the tree into the container's own filesystem and unpack it before running `apt-get`, because `apt-get` otherwise reads the stream from stdin and `tar` fails; do not build on a bind mount; install `fonts-liberation`, because `panel-e2e` needs a TrueType file. A run takes about 4 to 10 minutes with 4 CPUs, most of it `panel-e2e`:

```sh
git archive HEAD picowl | container run --rm -i -m 6G -c 4 debian:trixie sh -c '
  mkdir /src && cd /src && tar xf - &&
  apt-get update && apt-get install -y --no-install-recommends </dev/null \
    meson ninja-build gcc g++ pkg-config git ca-certificates xz-utils \
    libwayland-dev wayland-protocols libxkbcommon-dev libudev-dev libinput-dev \
    libdrm-dev libpixman-1-dev libseat-dev libasound2-dev libcairo2-dev \
    libpango1.0-dev libglib2.0-dev libharfbuzz-dev libfreetype-dev libxcb1-dev \
    libxcb-composite0-dev libxcb-icccm4-dev libxcb-render0-dev libxcb-xfixes0-dev \
    libxcb-res0-dev libxcb-ewmh-dev libxcb-present-dev grim python3 python3-pil \
    fonts-liberation fonts-dejavu-core hwdata libdisplay-info-dev libliftoff-dev \
    libgbm-dev libegl-dev libffi-dev cmake patch wget glslang-tools faketime &&
  cd /src/picowl && meson setup build -Dtests=true -Dosk_tests=enabled &&
  ninja -C build && meson test -C build --print-errorlogs'
```

The expected result is no failure, no compiler warning and one skip, `lease-vkms`. The DRM paths (hardware rotation, copy-type, the single-buffer swapchain, direct scanout, the lease on real hardware) cannot run in such a container, which has no `/dev/dri`; the checks that need a board are listed in the [roadmap](doc/design/roadmap.md) and, for the buffer paths, in [doc/zero-copy.md](doc/zero-copy.md).

## OpenEmbedded

`oe/` is a small layer (`LAYERSERIES_COMPAT` wrynose and blacksail) with the recipes for picowl (`oe/recipes-graphics/picowl/picowl_git.bb`), the patched wlroots (`oe/recipes-graphics/wlroots/wlroots_0.19.0.bb`) and wvkbd-ipaq (`oe/recipes-graphics/wvkbd/wvkbd-ipaq_git.bb`). [oe/README.md](oe/README.md) explains how to add the layer, configure the machine and distro, and build. In short:
- picowl needs the `wayland` DISTRO_FEATURE; `systemd` (through PACKAGECONFIG) installs the unit, which the recipe does not enable (`SYSTEMD_AUTO_ENABLE = "disable"`), because picowl takes over the display.
- The recipe fetches the `picowl` subdirectory of this repository (`subpath=picowl`, so `S = "${UNPACKDIR}/picowl"`) at `SRCREV = "${AUTOREV}"`; pin SRCREV for a reproducible build.
- `data/picowl.ini.example` is installed as `/etc/picowl.ini`, a conffile. It enables `[capture]`, sets the legacy `[idle] timeout_ms = 60000` (blank after 60 s in every profile), the background `#101010` and `code:155 = rotate`.
- The panel is built by default (PACKAGECONFIG `panel`, which adds `alsa-lib`) and packaged as `picowl-panel`, which recommends `liberation-fonts` for the smooth style; add the package to the image and start it from `[autostart]`.
- The recipe builds in ARM state (`ARM_INSTRUCTION_SET:arm = "arm"`), because the render paths are tuned for it; oe/README.md says what to set for pixman and wlroots.

## Roadmap

The state of the work, what comes next, what only a board can verify, the decisions that belong to the project owner, the ideas not started and the known issues are in [doc/design/roadmap.md](doc/design/roadmap.md), the one place where plans and open items are kept.

## License

MIT. See `LICENSE` in the picowl root.
