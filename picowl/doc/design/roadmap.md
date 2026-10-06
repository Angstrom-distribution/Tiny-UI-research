# picowl: state of the work, findings and plans

**Status:** a working record, last updated 2026-10-06. It says what is done, what was found, what is planned in which order, what only a board can verify, and which decisions belong to the project owner. It is the entry point; the detail is in the documents it names. Nothing in the "planned" sections is implemented.

## 1. What exists and where it was verified

Verification so far means the headless test suite in a clean `debian:trixie` arm64 container (57 tests, one opt-in skip, no compiler warnings) and, for some items, earlier captures from an h2200. No package built from the work listed under "recent" has been installed on a board yet.

| Area | State | Detail |
|---|---|---|
| Compositor, protocols, rotation, zero-copy, lease, power | implemented, hardware rotation measured on an h2200 | README, `doc/rotation-results.md`, `doc/zero-copy.md`, `doc/lease.md`, `doc/power.md` |
| Panel (18 px bar, slider rows, date row, battery time-left row, translucent popup) | implemented | README "Panel", `doc/panel.md`; wlroots patch 0005 fixes the stale opaque region behind a layer surface that becomes translucent |
| Crisp panel style (no anti-aliasing), `--crisp-font fixed\|dejavu` | implemented, default font `fixed`; containers only | README "Crisp style", `doc/design/panel-text.md` |
| Tiled pair: second window gets at least half (`[layout] second_min`, default 50) | implemented | README "Tiled layout" |
| Panel on the short edge of a rotated output (`--edge auto\|top`) | implemented; with software rotation pixel-identical to the portrait bar | README "Rotated output", `doc/panel.md` |
| Hardware-rotation hint (`picowl-rotation-v1`) so the strip also works under hardware rotation | built, local branch, review fixes in progress | section 3 |
| Runtime density-based style and bar height (`--style auto`, `--dpi`, `--height auto`, `[output] size_mm`) | built, local branch, review fixes in progress | `doc/design/ipaq-displays.md` |
| havoc per-cell damage (patch 0002) | implemented; fake compositor only | `oe/recipes-graphics/havoc/README.md` |
| Hold animation, panel hold timings | implemented | README "Tap-and-hold", `doc/cursors.md` |
| Research: MediaQ C8 and the 2D engine | design only | `doc/design/mediaq-c8.md` (section 8.6, experiments E9 to E14) |
| Research: remote access (VNC) with a remote-only mode | design only | `doc/design/remote-access.md` |
| Research: text rendering, WinCE, FreeType, pixel fonts | decision record | `doc/design/panel-text.md` |

## 2. Findings that shape the plans

- **The bus is the constraint.** Every host path into the MQ1188's video memory runs at 7.9 to 9.1 MB/s, and the core barely runs while it is written; a full 240x320 RGB565 frame is 153,600 bytes, about 17.5 ms. The engine fills at 185 to 194 MB/s and copies inside video memory at 135 to 143 MB/s (measured, 16 bpp, scanout running). Hence damage tracking everywhere.
- **Hardware rotation saves CPU, not bus bytes** (about a quarter less compositor CPU at 90 degrees with a full-surface client; uploaded bytes equal). It had never been applied before the measurement found that bug.
- **Terminal damage.** Before the havoc patch one typed character damaged the whole surface; with it, two cells (960 times fewer bytes in the 80x24 test, 1,440 bytes).
- **Windows CE shipped bi-level, bytecode-hinted Tahoma 12 text, ClearType off, and drew only 1-bit text with the graphics engine.** The best free match is DejaVu Sans with the v35 interpreter in monochrome; Liberation Sans looks poor in that mode. Details and licence notes in `doc/design/panel-text.md`.
- **The MediaQ engine has no alpha, no scaler, no overlay window, one depth and one palette for the whole chip, and cannot read system RAM.** An engine copy of a keyboard pan or popup would cost about 1 ms against 9 to 17 ms of upload; only fills and copies have run on the board. `doc/design/mediaq-c8.md` section 8.6.
- **Remote access.** The h2200's Bluetooth radio is version 1.1; measured bt-pan goodput on comparable hardware was 10 to 25 KB/s, so a viewer gets 2 to 5 changed frames per second at best. screencopy only works on an enabled output, and `wayvnc` forces the display power on, so the "no upload" mode needs a headless output as the capture source while the DRM output is parked. `doc/design/remote-access.md`.
- **Display facts.** QVGA iPAQs are 106 to 115 ppi, the hx4700 about 203 ppi; no driver sets the subpixel order. `doc/design/ipaq-displays.md`.
- **Container testing recipe that works.** Use Apple `container`, always `--rm`; stream `git archive HEAD picowl` into the container's own filesystem and extract it first, before any `apt-get` (apt otherwise consumes the stream and `tar` fails with "Unaligned block"); never build on a bind mount; install `fonts-liberation` (`panel-e2e` fails without a TrueType file); do not end a verification before the container has finished (the suite takes 6 to 10 minutes).

## 3. In progress

1. **Review fixes for the hint and density work.** An independent review found no blocking problem and eight minor ones: a tap during a pending look change can open a row that is never drawn for about 3 s; a reported density has no plausibility bound (a `size_mm = 5x7` typo gives an 80 px bar); a test seam turns the view twice after blank and unblank; two tests do not exercise the paths they claim; several documents still say the panel always reads a font file; test helper comments are misplaced; two one-off mutation scripts were committed. All are being fixed; the suite is run again afterwards.
2. After that: push, then ask the build peer for packages of `picowl` and `picowl-panel`, so that one install carries the tiling change, the strip, the DejaVu font, the hint and the density detection.

## 4. Planned, in the order proposed

1. **Verify on the board** what only a board can show (section 5).
2. **Decide the crisp default font** (`fixed` or `dejavu`) after seeing both on the real panel.
3. **Runtime baking with a cache** (requested, not built): rasterise hinted DejaVu glyphs at start-up with FreeType (v35 interpreter, monochrome, no auto-hint) instead of shipping baked bitmaps. Advantages: no DejaVu-derived data ships with picowl, so the Bitstream Vera notice problem goes away, and the bar height and font are free; the disk cache (keyed by font file, size and interpreter version) saves only start-up time, because the library is still linked. Costs: FreeType becomes an optional dependency of the panel, and the board's FreeType must have the v35 interpreter, which is unchecked. The offline generator already uses the same settings.
4. **Remote access.** Stage 0 measurements (encode cost on real UI frames, the radio's UART speed, `iperf3` over `bnep0` and `usb0`, whether the backlight goes dark when the display is disabled); stage 1 a PC run of picowl headless with `wayvnc` and several viewers; stage 2 the remote-only mode (`[remote]`, headless capture output, parked DRM output, revoke the media player lease, forced full upload on disconnect). Stage 3 embeds `neatvnc` only if stage 2 measures too much. `doc/design/remote-access.md`.
5. **MediaQ engine and C8.** Run experiments E9 to E14 (engine at 8 bpp, mono expansion, colour key, command issue cost, engine against scanout, idle interrupt); then driver-side scroll and unchanged-tile detection with engine copies (E-a); a compositor C8 mode only after the quantisation benchmark. `doc/design/mediaq-c8.md`.
6. **Pixel-aligned fonts in havoc** as patch 0003 behind a config switch: a bitmap backend returning 0 or 255 per pixel behind the existing glyph interface; first check the coverage and size of the full misc-fixed fonts. `doc/design/panel-text.md` section 8.
7. **Battery.** Measure the real cell (it says 1000 mAh, from 2014, unmeasured) with the read-only logger over a discharge, then tune the estimator constants; writing a measured capacity into the gauge chip only with the project owner's go. The `iiotool` helper labels the kernel's `time_to_*` attributes as minutes while the kernel reports seconds; not touched.
8. **hx4700 size**: measure the glass (the kernel source carries 60 x 80 mm provisionally).

## 5. Only a board can verify

- The strip is upright and on the physical top of the device under real hardware rotation, a tap on it works, and the LCD text fringe is right on the real panel.
- **Possible hardware-rotation defect:** in the headless emulation, after a rotation at run time that changes the mode size, a screencopy frame came back with its last quarter of rows black. It was not established whether this is the test seam, screencopy, or a real defect of hardware rotation at run time. Check it with the rotate key on the board.
- The new look: crisp on the h2200 at its real density, both crisp fonts, the date and battery rows (the date and battery rows have no device capture yet).
- Touch correctness at each rotation and with hardware rotation.
- Zero-copy and direct scanout paths (checklist in `doc/zero-copy.md`), the media player surviving a lease revocation.
- havoc's per-cell damage against picowl's real compositor, including the one pixel it adds under a fractional scale.
- One unexplained test flake: a pixel comparison of the date row with subpixel text at 90 degrees failed once in about a dozen runs and did not reproduce; the suspect is a clock minute change between capture and dump.

## 6. Decisions that belong to the project owner

- The default crisp font, and whether runtime baking replaces the baked bitmaps.
- Kernel edits: the display sizes are already in the source; a `subpixel_order` for the iPAQ drivers (and the stripe direction of each panel) is not, and needs a decision and a source for the value.
- When to build and deploy packages (the board is not reachable from the development machine at the moment).
- Whether the remote-access work starts with `wayvnc` (recommended) or an embedded server.

## 7. Related documents

`README.md` (features, configuration, testing), `doc/panel.md` (panel design), `doc/design/README.md` (index of the plans), `doc/design/panel-text.md`, `doc/design/ipaq-displays.md`, `doc/design/mediaq-c8.md`, `doc/design/remote-access.md`, `oe/README.md` (packaging), `oe/recipes-graphics/havoc/README.md`.
