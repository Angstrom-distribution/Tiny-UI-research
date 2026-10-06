# picowl roadmap

**Status:** the one maintained list of what picowl has, what comes next, what only a board can verify, what the project owner has to decide, the ideas not started and the known issues. Other documents link here instead of keeping their own to-do lists; a list in another document that describes the limits of a feature as it is today stays with that feature. Tick an item (`- [x]`) when it is done and say where the result is recorded.

## Where things stand

Verification so far means the headless test suite in a clean Debian trixie container (36 picowl tests and the 21 tests of wvkbd-ipaq: 56 pass, `lease-vkms` skips without vkms, no compiler warning) and, for rotation, the h2200 measurement. No package built from the work since the tiled layout has been recorded running on a board.

| Area | State | Verified where | Document |
|---|---|---|---|
| Compositor core: xdg-shell, layer-shell, autohide, keybindings, protocols | implemented | headless tests | [README](../../README.md#protocols) |
| Hardware rotation (default) and software rotation | implemented | h2200 at 90 degrees and normal; 270 and touch under rotation not measured | [rotation-results.md](../rotation-results.md) |
| Copy-type outputs, single buffer, zero-copy buffers, direct scanout | implemented | unit tests; mq11xx uploads measured on the h2200, with picowl's copy-type path not recorded; the buffer checklist not run | [zero-copy.md](../zero-copy.md) |
| Buffer limits and the `caching` event | implemented | unit and protocol tests | [buffer-budget.md](buffer-budget.md), [caching-event.md](caching-event.md) |
| DRM lease | implemented | `lease-vkms` (opt-in, no recorded run) | [lease.md](../lease.md), [drm-lease.md](drm-lease.md) |
| Power profiles, dimming, idle inhibit | implemented | unit tests, `power-e2e` | [power.md](../power.md), [idle-inhibit.md](idle-inhibit.md) |
| Touch calibration, tap-and-hold, per-app and per-layer rules | implemented | unit tests | [README](../../README.md#touch-input), [per-app-hold.md](per-app-hold.md) |
| Two-app tiled layout, keyboard pan, `second_min` (default 50) | implemented | `tile`, `tile-e2e`, `pan-e2e-*` | [README](../../README.md#tiled-layout) |
| Screen capture, text input and input method relay | implemented, without the input method keyboard grab | `smoke` | [capture.md](../capture.md), [osk.md](osk.md) |
| On-screen keyboard supervision and the wvkbd-ipaq series (11 patches) | implemented | `osk-e2e`, wvkbd-ipaq tests | [osk.md](osk.md) |
| picowl-panel: bar, slider rows, date and time-left rows, translucency | implemented | `panel`, `panel-e2e` | [README](../../README.md#panel), [panel.md](../panel.md) |
| Crisp style, `--crisp-font fixed\|dejavu` (default `fixed`) | implemented | `panel-e2e` section U; host renders only | [panel-text.md](panel-text.md) |
| Panel strip on the short edge (`--edge`), `picowl-rotation-v1` | implemented | `panel-e2e` sections V and W (hardware rotation emulated) | [README](../../README.md#panel) |
| Density rule (`--style auto`, `--height auto`, `--dpi`, `[output] size_mm`) | implemented | `panel`, `panel-e2e` section X | [ipaq-displays.md](ipaq-displays.md) |
| havoc patches 0001 (text-input-v3) and 0002 (per-cell damage) | implemented | havoc's tests against a fake compositor | [havoc README](../../oe/recipes-graphics/havoc/README.md) |
| OpenEmbedded layer (picowl, wlroots, wvkbd-ipaq) | written | parse-checked once with bitbake before later changes; not built from this layer | [oe/README.md](../../oe/README.md) |
| MediaQ C8 output and the 2D engine | design and research only | board measurements of the engine | [mediaq-c8.md](mediaq-c8.md) |
| Remote access (VNC) | design only | none | [remote-access.md](remote-access.md) |

Findings that shape the plans, with their sources:
- **The bus is the constraint.** Every host path into the MQ1188's video memory runs at 7.9 to 9.1 MB/s; a full 240x320 RGB565 frame is 153,600 bytes, about 17.5 ms. The 2D engine fills at 185 to 194 MB/s and copies inside video memory at 135 to 143 MB/s ([mediaq-c8.md](mediaq-c8.md)).
- **Hardware rotation saves CPU, not bus bytes:** about a quarter less compositor CPU at 90 degrees with a full-surface client, equal uploaded bytes ([rotation-results.md](../rotation-results.md)).
- **Terminal damage:** before havoc patch 0002 one typed character damaged the whole surface; with it, two cells (1,440 bytes in the 80x24 test, 960 times fewer) ([havoc README](../../oe/recipes-graphics/havoc/README.md)).
- **Windows CE text:** bi-level, bytecode-hinted Tahoma 12, ClearType off, only 1-bit text on the graphics engine. The closest free match is DejaVu Sans with the v35 interpreter in monochrome ([panel-text.md](panel-text.md)).
- **The MediaQ engine** has no alpha, no scaler, no overlay window, one depth and one palette for the whole chip, and cannot read system RAM; only fills and copies have run on the board ([mediaq-c8.md](mediaq-c8.md) section 8.6).
- **Remote access:** the h2200's Bluetooth is version 1.1 with 10 to 25 KB/s of PAN goodput measured on comparable hardware, so 2 to 5 changed frames per second at best; screencopy only works on an enabled output ([remote-access.md](remote-access.md)).
- **Displays:** QVGA iPAQs are 106 to 115 ppi, the hx4700 about 203 ppi; no driver sets the subpixel order ([ipaq-displays.md](ipaq-displays.md)).

## Next

In this order:

- [ ] **Build and install packages of picowl, picowl-panel and wvkbd-ipaq from the current head.** Why: nothing since the tiled layout (the strip, the DejaVu font, the rotation hint, the density rule, `second_min`) has run on a board. Fetch, compile and package all three recipes of `oe/` with bitbake, and check that the wlroots patches apply in `do_patch` and that `libwlroots-0.19.so` is packaged ([oe/README.md](../../oe/README.md)).
- [ ] **Run the board checks below** (Needs a board to verify). Why: the hardware paths are only emulated headless.
- [ ] **Decide the crisp default font** after seeing `fixed` and `dejavu` on the real panel. Why: only host renders exist ([panel-text.md](panel-text.md)).
- [ ] **Measure the real battery and tune the time-left estimator.** Why: the constants are a synthesis and untuned; the cell is labelled 1000 mAh (2014) and unmeasured. Log a discharge read-only first; write a measured capacity into the gauge chip only with the project owner's go (README, Time left).
- [ ] **Finish the rotation measurement:** workloads W1 (idle), W2 (terminal) and W5 (media player with its dither kernels), the 270 degree transform and other boards. Why: the media player workload was the original doubt about hardware rotation at 320 wide ([rotation-measurement.md](../rotation-measurement.md)). Also log the kernel release at picowl start-up, which the plan asked for and the code does not do.

## Needs a board to verify

- [ ] Hardware rotation commit succeeds at 90, 180 and 270, with no `hw rotation commit failed` in the log; the `rotate` key changes it at run time.
- [ ] After a rotation at run time under hardware rotation, no black rows at the bottom of the frame (see Known issues).
- [ ] Touch calibration: the four corners of the screen give the pointer at the matching corners in all four rotations, with software and with hardware rotation; with no pointercal, no `[touch] calibration` and no udev matrix, the one-time error and a dead touchscreen.
- [ ] Panel strip with `[output] * = 90` (and 270) under hardware rotation: on the physical top of the device via `picowl-rotation-v1` (`picowl-panel --dump-state` prints `rotation hint=hardware`), upright, taps work, the subpixel fringes have the right order, the `rotate` key moves it. The pixel path is tested headless with the plane emulated; the real plane and the touch matrix are not.
- [ ] The panel's look at the real density: crisp on the h2200 with both crisp fonts, the date and battery rows (no device capture yet), and on the hx4700 the smooth style, the densities of the real kernels' sizes and a live mode switch.
- [ ] The h2200's stripe order (a loupe test with single red, green and blue pixels on black); the value in `data/picowl.ini.example` is the owner's, not a datasheet's.
- [ ] Copy-type single buffer: the swapchain stays at one slot, no `single-buffer off` log line.
- [ ] Direct scanout: the scene logs it, the render list is one entry, no composition; it stops while two tiled windows are visible.
- [ ] Damage clipping: only changed regions are copied to video memory.
- [ ] The zero-copy hardware checklist in [zero-copy.md](../zero-copy.md): `DRM_IOCTL_MODE_CLOSEFB`, `FB_DAMAGE_CLIPS`, VmHWM on the device, the buffer limits, the caching log line, a version 1 client, the `copy_type` re-send, a kernel without CLOSEFB, direct scanout from CMA on pxa-lcdc.
- [ ] Whether the mq11xx and w100 buffers are really cacheable and the sa1100-lcdc ones CMA (`pw-test-client --readback`), and whether seven QVGA buffers fit the sa1100-lcdc CMA pool ([caching-event.md](caching-event.md), [buffer-budget.md](buffer-budget.md)).
- [ ] DRM lease: the checklist in [lease.md](../lease.md) and [drm-lease.md](drm-lease.md); the objects the out-of-tree drivers expose (`modetest -p`: CRTCs, overlay `possible_crtcs`); the media player surviving a lease revocation.
- [ ] VT switch with the media player's Path B: seatd releases the VT and DRM master on an external `VT_ACTIVATE`, and picowl's outputs and state (hardware rotation, copy-type swapchain, dim state, cursor) come back ([mediaplayer-integration.md](../mediaplayer-integration.md)).
- [ ] The media player acceptance measurements in [mediaplayer-integration.md](../mediaplayer-integration.md) section 5.
- [ ] Tiled layout with `[layout] stack = mediaplayer, havoc` in portrait and landscape: placement, touch and alt+Tab move the keyboard, the keyboard pans the pair without the player rescaling, rotating tiles again, closing one maximizes the other, no direct scanout while both are visible.
- [ ] Panel with `panel_autohide = false`: the sliders change the backlight and the volume while dragging and keep the level of the release, the clock and battery rows open, `amixer` and the thumb agree both ways, the backlight keeps the slider's level through a dim, rotating redraws the panel.
- [ ] Power: the checklist in [power.md](../power.md) (profiles, dimming, LOW cap, inhibitors).
- [ ] The DS2760 driver on the h2200's kernel exports the attributes the estimator expects, and what its numbers look like.
- [ ] The on-screen keyboard uses layer-shell keyboard interactivity `none`, so tapping a key keeps the focus in the application; wvkbd-ipaq with picowl's `[osk]` on the board, including the [osk.md](osk.md) hardware checklist.
- [ ] havoc patch 0002 against picowl's real compositor: wlroots accepts the damage as sent, whether it releases shm buffers at commit, how it rounds the one pixel the patch adds under a fractional scale, and what the damage costs on the bus ([havoc README](../../oe/recipes-graphics/havoc/README.md)).
- [ ] A tablet through `zwp_tablet_manager_v2` (never tested).

## Decisions for the project owner

- [ ] The default crisp font (`fixed` or `dejavu`), and whether runtime baking (Ideas) replaces the baked DejaVu bitmaps.
- [ ] Kernel edits: a `subpixel_order` for the iPAQ drivers and the stripe direction of each panel, which need a source for the value; or should picowl advertise `horizontal_rgb` when the backend says `unknown` ([ipaq-displays.md](ipaq-displays.md)).
- [ ] When to build and deploy packages to the board.
- [ ] Remote access: start with `wayvnc` (recommended in [remote-access.md](remote-access.md)) or an embedded server.
- [ ] `data/picowl.ini.example`, which OE installs as `/etc/picowl.ini`, enables `[capture]` and sets the legacy `[idle] timeout_ms = 60000`, so a fresh image can be screen-read by any client and blanks after 60 s even on AC (the code defaults are capture off and 600 s on AC). Keep that for development images, or ship a production default.
- [ ] Offer `ext_image_copy_capture_v1` next to screencopy (needs wayland-protocols 1.37 or newer; keeps direct scanout off for the whole recording) ([capture.md](../capture.md)).
- [ ] Take the output transform from the DRM connector's `panel orientation` property instead of `[output]` (on the h3900 it reads Right Side Up) ([mediaplayer-integration.md](../mediaplayer-integration.md)).
- [ ] With the media player owner: does Path A replace `mediaplayer-x11` once GPE runs on picowl; should `--vo auto` pick Path B on the hx4700; hardware or software rotation for video on the MediaQ boards ([mediaplayer-integration.md](../mediaplayer-integration.md)).
- [ ] Should picowl set `oom_score_adj` for itself (needs CAP_SYS_RESOURCE) so that the shmem buffer pools are not blamed on it ([buffer-budget.md](buffer-budget.md)).

## Ideas not started

- [ ] **Pixel-aligned fonts in havoc** as a patch 0003 behind a config switch: a bitmap backend that returns 0 or 255 per pixel behind the existing glyph interface; first check the coverage and size of the full misc-fixed fonts ([panel-text.md](panel-text.md) section 8).
- [ ] **Runtime baking of hinted fonts with a cache:** rasterise hinted DejaVu glyphs at start-up with FreeType (v35 interpreter, monochrome, no auto-hinter) instead of shipping baked bitmaps. Then no DejaVu-derived data ships with picowl, so the Bitstream Vera notice goes away, and the bar height and font are free; a disk cache keyed by font file, size and interpreter version saves only start-up time, because the library is still linked. Costs: FreeType becomes an optional dependency of the panel, and the board's FreeType must have the v35 interpreter, which is unchecked. The offline generator already uses these settings.
- [ ] **Remote access:** stage 0 measurements (encode cost on real UI frames, the radio's UART speed, `iperf3` over `bnep0` and `usb0`, whether the backlight goes dark when the display is disabled); stage 1 picowl headless with `wayvnc` and several viewers on a PC; stage 2 the remote-only mode (`[remote]`, a headless capture output, the DRM output parked, the lease revoked, a forced full upload on disconnect); stage 3 an embedded `neatvnc` only if stage 2 measures too much ([remote-access.md](remote-access.md) sections 2 and 5).
- [ ] **MediaQ engine and C8:** experiments E9 to E14 (engine at 8 bpp, mono expansion, colour key, command issue cost, engine against scanout, idle interrupt), then driver-side scroll and unchanged-tile detection with engine copies (E-a), the kernel items K1 to K6 as the measurements justify, and a compositor C8 mode (the proposed `[output] depth` and `[app.*] output_depth`) only after the quantisation benchmark; stages 0 and 2 to 5 and the open questions are in [mediaq-c8.md](mediaq-c8.md) sections 8 to 12.
- [ ] **Vertical-stripe LCD text for the h39xx**, if ever wanted: the panel draws subpixel text only when the stripes run across the bar, so on the h39xx's landscape panel (advertised as `vertical_rgb`) shown in its native orientation it draws greyscale today ([ipaq-displays.md](ipaq-displays.md), README, Subpixel text).
- [ ] **hx4700 panel size:** measure the glass; the kernel source carries 60 x 80 mm provisionally. Also whether the h2200 glass is the same as the h3xxx and h5xxx ([ipaq-displays.md](ipaq-displays.md)).
- [ ] **The rest of the panel design:** the app list, network, Bluetooth, keyboard, rotation and storage widgets, tap-and-hold menus as xdg popups, `panel.ini`, and `picowl-control-v1` (brightness, blanking, rotation and `osk(show|hide|toggle)` with an `osk_visible` event for a trusted client) ([panel.md](../panel.md)).
- [ ] **Input method keyboard grab** (`zwp_input_method_v2.grab_keyboard`, logged and inert today) and touch hold rules for input method popups; a `tests/osk.sh` against picowl with the real wvkbd-ipaq; accent layouts beyond nl, de and fr; whether the panel reserves space while the keyboard shows ([osk.md](osk.md)).
- [ ] **Idle inhibit extras:** a maximum inhibit duration against hung clients (the proposed `[power] inhibit_max_s`), panel applets that inhibit while the panel is hidden (a `PW_INHIBIT_USER` reason and a key action), a `dim` value for `inhibit` on LOW, and removing the unused `server->idle_timer` ([idle-inhibit.md](idle-inhibit.md)).
- [ ] **Buffer extras:** a `budget(max_buffers, bytes)` event, trusted spawn over a socketpair with a tagged client (which also fixes the `WAYLAND_SOCKET` limit of `exe`), and a per-buffer `caching` event (version 3) if a second allocator appears ([buffer-budget.md](buffer-budget.md), [caching-event.md](caching-event.md)).
- [ ] **wlroots core cost:** `wlr_client_buffer` is allocated on every commit because the pixman texture has no `update_from_buffer`; implement it and let the scene drop its lock before the apply so commits recycle the client buffer ([zero-copy.md](../zero-copy.md)).
- [ ] **Lease:** backport an upstream fix of the lease grant use-after-free if one appears after 0.19.0, instead of carrying that part of patch 0004; a CI runner with root and vkms for `lease-vkms` ([drm-lease.md](drm-lease.md)).
- [ ] **Hold rules:** prefix or glob matching of app_ids, a hold override for a region a client asks for, and reloading the config on SIGHUP ([per-app-hold.md](per-app-hold.md)).
- [ ] **Video offload handshake:** apps signal that a raw buffer can go to the framebuffer directly (carried over from an earlier plan; no design exists).
- [ ] **Packaging:** switch `SRC_URI` and drop the `S` line once picowl has its own repository; a test that keeps the two copies of the wlroots patches in sync, like `wvkbd-patches-sync`; turn the `tools` option off in the recipe so build-host tools are not packaged for the target ([oe/README.md](../../oe/README.md)).

## Known issues

- [ ] **Possible black rows after a rotation at run time under hardware rotation.** Found only in the headless emulation (`PICOWL_TEST_HW_ROTATION`): after a rotation at run time that changes the mode size, one screencopy frame came back with its last quarter of rows black. Not established whether this is the test seam, screencopy, or a real defect; check with the `rotate` key on a board.
- [ ] **One unexplained flake** in `panel-e2e`: the pixel comparison of the date row with subpixel text at 90 degrees failed once in about a dozen runs and did not reproduce. The suspect is the clock's minute changing between the capture and the dump.
- [ ] The picowl recipe fetches `branch=main` with `subpath=picowl`; in this repository picowl lives on the `picowl` branch, and the local `main` has no `picowl/` directory. Not verified against the published repository.
- [ ] `picowl -h` says `-c CONFIG ... (default: /etc/picowl.ini)`, while without `-c` the file is searched in `$XDG_CONFIG_HOME`, `$HOME/.config` and then `/etc` (README, Configuration).
- [ ] An installed wlroots is probed only for patches 0003 and 0004 at configure time, so one without 0001, 0002 or 0005 passes `meson setup` ([wlroots patches README](../../subprojects/packagefiles/wlroots/README.md)).
- [ ] The h2200 kernel logged `vblank wait timed out` 18 times in 100 minutes of the rotation measurement, without stopping a run; it looks like an occasional lost frame interrupt in the driver ([rotation-results.md](../rotation-results.md)).
- [ ] picowl's guess of the keyboard's visibility is wrong when the keyboard is shown or hidden other than by picowl's own keys (README, `[osk]`).

## How to verify

1. Headless, for every change: run the suite in a clean container as README.md, Testing, shows (stream `git archive HEAD picowl` into the container and unpack it before `apt-get`, install `fonts-liberation`, build with `-Dtests=true -Dosk_tests=enabled`, let it run to the end; about 4 to 10 minutes). Expect no failure, no compiler warning and only `lease-vkms` skipped.
2. Headless with emulated hardware: `PICOWL_HEADLESS_SIZE=240x320`, `PICOWL_TEST_HW_ROTATION=1`, `[output] size_mm` and the panel's `--inject` tokens reproduce what `panel-e2e` checks, for a look at one case by hand.
3. On a board: install the packages, keep a copy of `/etc/picowl.ini`, run picowl with `-d 3` and work through Needs a board to verify; record the kernel release, the picowl commit and the result in the document the item links to, then tick it here.
