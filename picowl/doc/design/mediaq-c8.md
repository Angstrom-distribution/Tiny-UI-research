# MediaQ C8 (8 bpp palettised) output: what it is, what it buys, how picowl could use it

**Status:** design only, nothing implemented in picowl. Line references are against picowl commit 8b0aaf8, wlroots 0.19.0 as vendored in `subprojects/wlroots`, havoc commit 73e3467 (the revision the meta-handhelds recipe builds), and the WinCE decompile `h22xx-ddi.c` named in the Sources.

## 1. Summary and recommendation

The MediaQ chips on the h2200 (MQ1188) and h5550 (MQ1132) can scan out an 8 bpp image through a 256-entry, 18-bit hardware palette. The kernel driver `mq11xx` on its current development branch already exposes this as `DRM_FORMAT_C8` with the palette as the CRTC `GAMMA_LUT`, and double-buffers C8 frames with a tear-free window-start flip, because two 8 bpp frames fit in the 256 KiB of video memory where two RGB565 frames do not. A full C8 frame crosses the roughly 8 MB/s host bus in 8.6 to 9.4 ms instead of about 17.5 ms for RGB565. The question is where that halving is worth a palette, a quantisation pass and the loss of colour.

Recommendation, in priority order:

1. **Video: use C8 in the media player over the DRM lease, not through picowl.** The lease already hands the player the CRTC, the plane and its `GAMMA_LUT`. A fixed palette with ordered dither and a small YUV-to-index lookup table is the first step; per-title or per-scene palettes are an option for later. picowl's part is done (the lease); the work is in the player and in two kernel items (pixel-doubled C8 and C8 under rotation are refused on the MQ1188 today).
2. **Terminal and UI: fix the bytes before changing the depth.** havoc damages its whole surface on every redraw and draws ARGB8888 without an opaque region, so a single typed character uploads the full 153,600-byte frame (about 17.5 ms of stalled bus). Per-cell damage in havoc cuts a keystroke to a few hundred bytes, which C8 can never match. A scroll is a full repaint in havoc's terminal core; there C8 halves the cost, and a driver-side scroll or unchanged-tile detector (with the measured 135 MB/s engine copy) would do better still at RGB565.
3. **picowl C8 output mode, opt-in, after a board benchmark.** If the quantise benchmark (experiment E1) shows a full-frame RGB565-to-C8 pass at 4 ms or less on the PXA255, add an opt-in mode: pixman renders RGB565 into a RAM buffer as today, picowl quantises only the damaged boxes into a C8 buffer, and the primary plane scans that buffer with one fixed, compositor-owned palette: the xterm 256-colour palette (16 ANSI colours, a 6x6x6 cube and a 24-step grey ramp), which makes 256-colour terminal output exact and is a reasonable general palette with 4x4 ordered dither. Enter the mode only by configuration or a per-app rule for a full-screen client; never by default.
4. **Client-rendered indices last.** A C8 format in picowl-buffer-v1 plus a palette event would let havoc (or a game) render indices directly and keep direct scanout. It is the most efficient path and the most work; do it only if stage 3 shows that the compositor-side quantise pass is the remaining cost.

What would kill C8 in picowl: a quantise pass that costs more than the 8 to 9 ms of bus time it saves per full frame; workloads that are dominated by small damage (where per-commit overhead of about 4 ms dwarfs the 0.06 ms of bus time); and the loss of the driver's engine solid-fill shortcut, which today uploads nothing for a solid RGB565 damage rectangle but is not enabled for 8 bpp on the MQ1188.

## 2. Evidence tags and sources used

Every factual statement carries one tag:

| Tag | Meaning |
|---|---|
| **[read]** | I read it in the named file or URL (path and lines given) |
| **[decomp]** | I read it in the Ghidra decompile of the h2200 WinCE display driver `ddi.dll` (`h22xx-ddi.c`, function and line given) |
| **[meas\*]** | a measurement on a real board reported by a source (the revival MediaQ reference, or picowl's own `doc/rotation-results.md`); not repeated by me |
| **[datasheet\*]** | an MQ-1100/1132 datasheet statement as quoted by the revival MediaQ reference; I did not see the datasheet itself |
| **[code\*]** | the current `mq11xx.c` on the kernel development branch as described by the revival MediaQ reference; I did not read that source (the patch in the meta-handhelds checkout is an older version, see 3.6) |
| **[inf]** | my inference from the above |
| **[est]** | my estimate, with the arithmetic shown |
| **[unverified]** | a claim I could not check; do not rely on it |

The revival MediaQ reference ("MediaQ MQ1188 and MQ1132", h2200-revival project, cited below as *the MediaQ reference* with its section numbers) is the primary register-level source. It tags its own claims as datasheet, decompile, code, measurement or inference; the tags above preserve that distinction.

## 3. Facts

### 3.1 What C8 is on these chips

| Fact | Tag and source |
|---|---|
| GC00 (`0x180`) bits [7:4] select the depth: `0x0`, `0x1`, `0x2`, `0x3` are 1, 2, 4, 8 bpp through the palette, `0x8`-`0xb` the grey (palette-bypassed monochrome) codes, `0xc` is 16 bpp RGB565 with the palette bypassed | [read] linux-hnd `include/linux/mfd/mq11xx.h` lines 229-238; [decomp] `FUN_02571778` lines 123-141; [datasheet\*] MediaQ reference §3.2.1 |
| The palette is 256 entries at register offset `0x800 + 4 x index` (`0x800`-`0xbff`) | [read] `mq11xx.h` lines 630-639; [read] linux-hnd `drivers/mfd/mq11xx_base.c` lines 70-74; [decomp] `FUN_025766e4` line 4312 |
| Each entry stores 18 bits: red [7:2], green [15:10], blue [23:18]; writing `0xffffffff` reads back `0x00fcfcfc` on the MQ1188 | [datasheet\*] and [meas\*] MediaQ reference §3.4 |
| Palette reset contents are undefined | [datasheet\*] MediaQ reference §3.4 |
| The palette is one per chip, for the one image window; the MQ1132 has one image window and one cursor, no overlay or YUV window. So C8 and RGB565 cannot be mixed on screen: the depth is a property of the whole window | [datasheet\*] MediaQ reference §3.2.4, §5.3; for the MQ1188 "unknown" per the same source, and nothing in the decompile suggests a second window [decomp, inf] |
| The panels are 18-bit TFTs fed through an 18-bit interface, so a 6-bit-per-channel palette loses nothing the panel could show | [datasheet\*] MediaQ reference §3.3.1 (FP00 decode) |
| Palette writes are not documented as latched. WinCE's `SetPalette` waits for the beam to leave the image window and then writes all entries in one loop | [decomp] `FUN_025766e4` lines 4301-4317 (vtable `+0x30` call at line 4310 is the window-end wait `FUN_02572c94` at line 1298, which calls `FUN_02571c28` at lines 290-305); [datasheet\*] only the cursor registers are documented as VSYNC-latched (MediaQ reference §3.6) |
| The window start GC0C (`0x1b0`) is latched at the frame boundary on the MQ1188: five writes at scan lines 117, 15, 150, 92 and 70 each took effect at line 1 of the next frame | [meas\*] MediaQ reference §3.2.4 |
| The stride GC0E (`0x1b8`) is signed; C8 at 240 wide uses 240 bytes per line; with GC00[7:4] = 3 the fetch stepped 240 bytes per line on the MQ1188 | [datasheet\*] and [meas\*] MediaQ reference §3.2, §3.4 |
| Video memory is 256 KiB. Two 240x320 RGB565 frames (307,200 bytes) do not fit; two C8 frames (153,600 bytes) or two pixel-doubled RGB565 frames do | [read] `mq11xx.h` line 16 (`MQ11xx_FB_SIZE`); arithmetic; MediaQ reference §1.4 |
| Pixel doubling is GC00[14] (horizontal) and GC00[15] (vertical); it applies to the whole window and the cursor | [read] `mq11xx.h` lines 243-244; [datasheet\*] and [meas\*] (fetch level, MQ1188) MediaQ reference §3.2.3 |
| Scanout rotation uses GC00[13:12] plus the sign of the stride; on the MQ1188 seven of eight transforms were measured at the fetch level. The datasheet gives the 8n+2 stride rule for quarter turns at 16 bpp only, and a GC0C bit-0 rule for decrementing-X scans at 8 bpp or less | [datasheet\*] and [meas\*] MediaQ reference §3.2.3, GC0C row of §3.2 |
| The hardware cursor (64x64, 2 bpp AND/XOR) takes its two colours from GC12/GC13 as direct 18-bit RGB, not from the palette | [datasheet\*] MediaQ reference §3.2 GC12/GC13 rows; [read] `mq11xx.h` lines 338-339 |
| The 2D engine draws at 8 or 16 bpp (GE0A[31:30] = 00 for 8 bpp); its fill and copy were measured on the MQ1188 at 16 bpp only (fill 185-194 MB/s, copy 135-143 MB/s, against about 8 MB/s for host writes); 8 bpp engine operation on the MQ1188 is unmeasured | [datasheet\*] MediaQ reference §4.2; [meas\*] §4.1, §5.2 |
| The engine has no colour-space conversion, no scaler, no alpha blending (MQ1132 datasheet); none is used by the MQ1188 WinCE driver | [datasheet\*] and [decomp\*] MediaQ reference §4.8, §5.3 |
| Measured refresh on the h2200: 17.747 ms by register poll, 17.594 ms (56.84 Hz) through the driver's timing file; the window-end-to-frame-start gap is 21 lines, about 1.09 ms | [meas\*] MediaQ reference §3.6, §5.2 |
| Every whole-transfer host path into video memory, CPU or DMA, runs at 7.9-9.1 MB/s on both boards; while anything writes the window, the core makes almost no progress (decode slowed by the factor 1/(1 - duty)) | [meas\*] MediaQ reference §5.2 (contention experiment) |

### 3.2 What the kernel driver supports today

From the description of the current `mq11xx.c` (kernel branch `unified-mainline-port`) and its runs on an h2200 [code\*, meas\*, MediaQ reference §5.1]:

- The driver logs `chip MQ1188, capabilities: engine@0x1400 rotation pixel-doubling palette-8bpp cursor frame-latch start-latch` (no alternate window).
- The primary plane offers `RG16` (RGB565) and `C8`, linear only. The CRTC offers a 256-entry `GAMMA_LUT`; without a LUT the palette is a grey ramp. The palette is written once the beam has left the window, when the LUT changed, when the depth switches to 8 bpp, or after chip power was cut.
- A C8 commit with a ramp set GC00[7:4] = 3, GC0E = `0xf0`, GC0C = `0x2d000` (the back buffer); all 256 palette words read back right. A full C8 upload was 76,800 bytes in 8.6-9.4 ms. Returning to RGB565 restored GC00[7:4] = `0xc`, GC0E = `0x1e0`, GC0C = 0.
- **Refused on the MQ1188 with `-EINVAL`:** C8 with `ROTATE_180` or `REFLECT_X`, a 16-entry LUT, and pixel-doubled C8 ("upright frames only"; only the combinations exercised on silicon are enabled). The MQ1132 description allows C8 with the row-scan transforms, doubled C8 and 8 bpp engine fills, but that is build-verified only (no MQ1132 unit available).
- Double buffering: C8 and doubled frames alternate between GC0C = 0 and a back buffer at the top of the pool; the previous commit's damage is first copied into the back buffer by the engine (a 16 bpp view at the frame's pitch, so it also serves C8). A C8 flip loop ran at 54.8-56.9 Hz; a full RGB565 upload (about 17.5 ms) completes only every other frame (27.6-28.4 Hz).
- Solid damage rectangles become engine fills at 16 bpp; **C8 rectangles are uploaded, not filled**, because 8 bpp engine commands were not exercised on the MQ1188.
- A plane format change sets the driver's "ignore damage clips" flag for that commit, so a depth switch costs one full upload; the fix `daf8e6712ae1` clears the flag again afterwards.
- The cursor plane refuses rotation and doubling; nothing says it refuses C8 [inf: not listed among the refusals].
- Nothing was looked at on the panel: every result is register and fetch-address read-back.

### 3.3 Upstream DRM and Wayland semantics

| Fact | Tag and source |
|---|---|
| `DRM_FORMAT_C8` is `fourcc_code('C', '8', ' ', ' ')`, "[7:0] C", that is 0x20203843; `DRM_FORMAT_RGB332` is `fourcc_code('R', 'G', 'B', '8')` | [read] torvalds/linux `include/uapi/drm/drm_fourcc.h` lines 99 and 143 (fetched through the GitHub API, October 2026) |
| The CRTC `GAMMA_LUT` "is also used to store the color map ... for indexed formats like DRM_FORMAT_C8" (historical Xorg convention). Helpers `drm_crtc_load_palette_8()`, `drm_crtc_fill_palette_8()` and `drm_crtc_fill_palette_332()` exist | [read] torvalds/linux `drivers/gpu/drm/drm_color_mgmt.c` lines 87-89, 802-876 (master at 2e05544, 2026-06-21) |
| fbdev `FBIOPUTCMAP` on DRM fbdev emulation is turned into a `GAMMA_LUT` blob by `setcmap_atomic()` | [read] `drivers/gpu/drm/drm_fb_helper.c` lines 806-895 (same fetch) |
| Wayland `wl_shm` has `c8` = 0x20203843 ("8-bit color index format") and `rgb332` = 0x38424752, but **no request or event anywhere in wayland.xml carries a palette**: a C8 wl_shm buffer has no defined colours | [read] `subprojects/wayland/protocol/wayland.xml` lines 336-339 and a search for palette/colormap/indexed (no hits) |
| wlroots 0.19's pixman renderer has no C8 or RGB332 entry in its format table (`render/pixman/pixel_format.c` lines 1-98), so it can neither import a C8 buffer as a texture nor render into one; `get_pixman_format_from_drm()` logs "has no pixman equivalent" | [read] `render/pixman/pixel_format.c` lines 102-111 |
| The wlroots generic pixel-format table has `DRM_FORMAT_R8` but no `C8`; the dumb and shm allocators size buffers from that table, so they cannot allocate a C8 buffer today | [read] `render/pixel_format.c` lines 43-46; `render/allocator/drm_dumb.c` lines 41-62; `render/allocator/shm.c` lines 61-75 |
| wlroots commits `GAMMA_LUT` as a blob in atomic mode (or the legacy `drmModeCrtcSetGamma` when the property is absent); the ramp is three `uint16_t` arrays of `wlr_output_get_gamma_size()` entries, set with `wlr_output_state_set_gamma_lut()` | [read] `backend/drm/atomic.c` lines 267-283; `backend/drm/drm.c` lines 1043-1068; `include/wlr/types/wlr_output.h` lines 125-126, 531-539 |
| pixman 0.46 has `PIXMAN_c8` with `pixman_indexed_t { color; rgba[256]; ent[32768]; }` (a 15-bit RGB to index table for stores), `PIXMAN_r3g3b2`, and destination dithering (`PIXMAN_DITHER_ORDERED_BAYER_8`, `PIXMAN_DITHER_ORDERED_BLUE_NOISE_64`, `pixman_image_set_dither()`) | [read] `/opt/homebrew/include/pixman-1/pixman.h` (0.46.4) lines 349-355, 985-996, 1103, 1108, 1223-1227. How the c8 store uses `ent[]` and how fast the dither path is: [unverified], the source fetch failed |

### 3.4 What picowl and its clients do today

| Fact | Tag and source |
|---|---|
| picowl renders RGB565 (configurable) with pixman, gives copy-type outputs a one-slot picowl-owned swapchain, and commits only frame damage | [read] `src/output.c` lines 82-111, 213-252, 292-317 |
| picowl-buffer-v1 accepts only RGB565 (`create_buffer`) and has no palette concept | [read] `protocols/picowl-buffer-v1.xml` |
| Hardware rotation is the default on MediaQ (`[rotation] auto`); measured at 90 degrees with a full-surface client at 30 fps: picowl CPU 16.5 ticks/s (hw) against 21.9 (sw), 25.4 frames per second presented; with a 16x16 blinking square 12.8 against 13.1 ticks/s; small damage uploads nothing because the driver fills it with the engine | [meas\*] `doc/rotation-results.md` |
| picowl-panel draws subpixel (LCD) text when `wl_output` advertises a subpixel layout | [read] `README.md` ([output] `subpixel`) |
| havoc creates its buffers as `WL_SHM_FORMAT_ARGB8888`, two of them | [read] havoc `main.c` lines 598-600, 618-641 |
| havoc skips cells whose age is not newer than the buffer's, but then damages the **whole surface** on every redraw: `wl_surface_damage_buffer(term.surf, 0, 0, term.width, term.height)` | [read] havoc `main.c` lines 707-708, 776-799 (line 791) |
| havoc sets no opaque region (no `wl_region` use), so the compositor must treat the ARGB8888 surface as translucent and blend it | [read] havoc `main.c` (no occurrence of `set_opaque_region` or `wl_region`) [inf for the compositor consequence] |
| havoc blends glyphs from 8-bit coverage per pixel between foreground and background (`print()`), with an opacity setting that defaults to 255 | [read] havoc `main.c` lines 643-697, 211 |
| havoc's terminal core (libtsm) resets the screen age on every scroll ("TODO: more sophisticated ageing"), so a scroll redraws every cell | [read] havoc `tsm/tsm-screen.c` lines 256-265 |
| havoc's default palette is the xterm 256-colour layout (16 ANSI, then a 6x6x6 cube built by a `cube6` macro, then greys); SGR 38/48 accept `;5;n` (256 colours) and `;2;r;g;b` (truecolour, kept as RGB) | [read] havoc `tsm/tsm-vte.c` lines 199-215, 1120-1166 |

### 3.5 Other iPAQ display chips

| Board | Controller | C8 relevance | Tag |
|---|---|---|---|
| h2200 family | MediaQ MQ1188, `mq11xx` | C8 implemented and passing at register level, upright only | [code\*, meas\*] §3.2 above |
| h5550 | MediaQ MQ1132, `mq11xx` | C8 with row-scan transforms, doubled C8 and 8 bpp engine fills in the driver, build-verified only; no frame interrupt, so the vblank comes from a timer and GC0C is written only after the beam has left the window | [code\*] MediaQ reference §5.1 |
| hx4700 | ATI W3220, `w100` | No palette mode is known to the project; the useful hardware path is the YUV overlay through the lease. Treat as RGB565 only | [unverified] for any 8 bpp mode; overlay per the research `hardware.md` §4 [read] |
| h3870 | SA-1110 LCD controller, `sa1100-lcdc` | The SA-1110 LCD controller has palettised 8 bpp modes in general, but whether the driver offers C8 is unknown; the copy there is a memcpy into uncached DMA memory, which C8 would halve | [unverified] |
| h3970 | PXA250 LCD controller, `pxa-lcdc` | Same: the PXA25x controller supports 8 bpp with a palette in general; the driver's formats are unknown. Direct scanout means no bus copy to save, only DMA fetch and CMA memory | [unverified] |

No board in the project uses an MQ1100: the hackndev name "mq1100" for the h5xxx chip is wrong, the chip identifies as `0x0120`, an MQ1132 [meas\*, MediaQ reference §1.1]. The h3xxx boards use the SoC LCD controllers, not a MediaQ [read: research `hardware.md` §1].

### 3.6 Where sources disagree

- The meta-handhelds checkout carries `0076-drm-tiny-add-driver-for-the-MediaQ-11xx-display-cont.patch`, an early version of the driver: RGB565 only (`mq11xx_primary_plane_formats[]` at lines 875-877 of the patch), no vblank, no engine, no rotation [read]. The capability line and the C8 support described in 3.2 belong to the later driver on the kernel development branch [code\*]. Anything that depends on C8 needs that later driver in the image.
- The patch's register table labels `0x228`, `0x22c` and `0x244` as ge0a, ge0b and ge11 (patch lines 509-514) [read]; on the MQ1188 the engine is at `0x1400` and those offsets are not engine registers [decomp, meas\*, MediaQ reference §4.1].
- linux-hnd's `mq1100fb.c` advertises 6-bit channels at 8 bpp (`mq1100fb_check_var()`, lines 209-217) but its `setcolreg` never writes the palette registers at 8 bpp (lines 268-270, `case 8: break`) [read]: the old fbdev 8 bpp path was incomplete and is no evidence either way.
- WinCE puts the frame at video memory `0x2000`; the Linux driver at 0 [decomp `FUN_02572b54` line 1216; code\*].

## 4. Windows CE driver evidence

The h2200 `ddi.dll` (built from `mq1188ddi.pdb`) was decompiled with Ghidra into `h22xx-ddi.c` (330 functions). What it confirms or contradicts for C8:

| Question | Answer from the decompile | Lines |
|---|---|---|
| Depth codes written for 8 bpp | `FUN_02571778` maps bpp 1, 2, 4, 8, 16 to GC00[7:4] = `0x00`, `0x10`, `0x20`, `0x30`, `0xc0`; any other value falls to `0x30`. With FP00 bit 4 (monochrome panel) set it ORs in `0x80`. GC00 is updated read-modify-write on bits [7:4] only (`& 0xffffff0f`), so the doubling, scan and buffer-select bits are left as they were. Confirms the documented codes | `h22xx-ddi.c` 123-141 |
| Window start and stride | `FUN_02571778` writes GC0C (`+0x1b0`) = its first argument and GC0E (`+0x1b8`) = the stride; if no stride is configured it computes `width x bpp / 8` (8 bpp: 240 bytes) | 63-73 |
| Window size and centring | GC08 and GC09 are written through offsets held in the configuration block (`+0x138`, `+0x148`), centring a mode narrower or shorter than the panel | 74-96 |
| Engine surface follows scanout | `FUN_02571ab8(1)` sets GE0A (`+0x1428`) = GC0E, ORing `0x40000000` only at 16 bpp, so at 8 bpp the engine depth field is 00 (8 bpp); GE0B (`+0x142c`) = GC0C; then DC05 \|= 1 | 142, 215-227 |
| Is the depth change beam-synchronised? | No wait inside `FUN_02571778`; it writes GC0C, GC0E, GC08, GC09, two FP tables and GC00 directly | 34-144 |
| Mode set as shipped | `FUN_02572b54` calls `FUN_02571778(0x2000, stride, config byte +0x16)`; the configuration block carries 16 there (MediaQ reference §3.7), so the shipped mode is 16 bpp; the default palette routine runs only when GDI passes a palette pointer | 1208-1227 (1216, 1220-1222) |
| Default palette at 8 bpp | `FUN_0257677c` writes 2, 4 or 16 hardware entries at 1, 2 and 4 bpp, but at 8 bpp only creates a 256-entry GDI palette object and writes **nothing** to the chip | 4322-4397 (4372-4375) |
| Palette write sequence and format | `FUN_025766e4` (`SetPalette`) refuses `start + count > 256`, calls the method at vtable `+0x30` once, then stores each 32-bit source word unchanged to `regs + 0x800 + 4 x index`; no read-back, no per-entry wait, no latch bit. The source words are GDI `PALETTEENTRY` dwords (red in [7:0], green [15:8], blue [23:16]), whose high six bits land in the 18 stored bits [inf, consistent with the measured read-back] | 4301-4317 |
| The wait used before palette writes | vtable `+0x30` is `FUN_02572c94`, which calls `FUN_02571c28`: read GC1D (offset 500 = `0x1f4`), spin while the line counter [9:0] is past the window end [21:12], then spin until it reaches it; no timeout | 1298-1303, 290-305 |
| Pixel doubling | Not referenced by `FUN_02571778`, `FUN_025719b4` or the mode set; the h2200 driver has no doubling path (the h5550 driver reads a `PIXEL_DBL` key but no code sets the bits, MediaQ reference §3.2.3) | 34-202 |
| Alternate window, GC00[11:10] | Never written by these functions | 34-231 |
| Depth read-back | `FUN_025719b4` with a zero first argument decodes GC00[7:4] back to 1, 2, 4, 8 or 16; with a non-zero one it sets the depth and GE0A together | 149-202 |

**What this adds to the plan:** WinCE treats the palette as unlatched and times it to the window end, exactly as the Linux driver does; it gives the 8 bpp engine depth as a supported configuration on the MQ1188 (the engine bank `+0x1400` is reprogrammed to depth 00 at 8 bpp), which supports enabling 8 bpp engine fills in the kernel after a guarded test; and it never exercised 8 bpp in the shipped configuration, so no WinCE behaviour vouches for colours on the panel.

**Questions the decompile should answer next** (for a human with the corpus):

1. Who calls `SetPalette` (`FUN_025766e4`) and the window-end wait (vtable slots 12 and 31) from `gwes.exe`, and does any palette animation happen while scanout runs (MediaQ reference §7 question 40)?
2. The 8 bpp host blit `FUN_025752d4` translates four indices per word through a lookup table: is that table the GDI palette translation (so WinCE supported 8 bpp DIBs on a 16 bpp screen only), and does the dispatcher `FUN_02575f78` ever send an 8 bpp destination to the engine on this board?
3. In `GETGXINFO` (`FUN_02576b28`), is there a code path that reports `kfPalette` (`0x10`) with a palette pointer, which would show how GAPI games were meant to use 8 bpp?
4. In the configuration block at `0x0187a2b8`, which bytes at `+0x1e` (3, 4, `0x1c`) and `+0x16` would a registry override change, and is there a registry value that selects 8 bpp at boot?
5. Does any routine write GC00[15:14] or GC0D/GC00[11:10] by computed offset, which the static scan could have missed (MediaQ reference §8.3 lists unresolved accesses)?
6. The h5550 `ddi.dll` equivalents (`FUN_02551a90`, `FUN_02556d48`, `FUN_02551e84`): does its rotation routine write the `- 3` start adjustment only at 16 bpp, and what does it do at 8 bpp (the GC0C bit-0 rule for decrementing-X scans)? This decides how C8 under rotation should be programmed.

## 5. Benefit estimates

Constants: a frame is 240 x 320 = 76,800 pixels. RGB565 = 153,600 bytes, C8 = 76,800 bytes. Host write rate 7.9-9.1 MB/s [meas\*]. Frame period 17.594 ms [meas\*]. CPU 400 MHz.

### 5.1 Bus time per update

| Update | RGB565 bytes | C8 bytes | RGB565 time | C8 time | Tag |
|---|---|---|---|---|---|
| Full frame | 153,600 | 76,800 | 16.9-19.4 ms (measured about 17.5) | 8.4-9.7 ms (measured 8.6-9.4) | [est] 153,600 / 9.1e6 = 16.9 ms, / 7.9e6 = 19.4 ms; [meas\*] |
| Pixel-doubled full frame (120x160) | 38,400 | 19,200 | 4.2-4.9 ms (measured 4.3-4.6) | 2.1-2.4 ms | [est]; [meas\*]; doubled C8 refused on the MQ1188 today |
| Terminal cell 6x12 | 144 | 72 | 16-18 us | 8-9 us | [est] |
| Cursor-sized square 16x16 | 512 | 256 | 56-65 us | 28-32 us | [est] |
| Video 240x136 letterbox (rotation 0) | 65,280 | 32,640 | 7.2-8.3 ms (measured present 9.2-9.4 ms) | 3.6-4.1 ms | [est]; [meas\*] MediaQ reference §5.2 |
| Video 181x320 (rotation 90 geometry) | 115,840 | 57,920 | 12.7-14.7 ms (measured present 15.2 ms) | 6.4-7.3 ms | [est]; C8 under quarter-turn rotation is refused on the MQ1188 today |
| Solid rectangle, any size | 0 (engine fill) | full upload | 0 | W x H / 8.5 MB/s | [code\*]: 8 bpp engine fills not enabled on the MQ1188 |

### 5.2 Small damage: C8 changes nothing

picowl spent 12.8 ticks per second on a 16x16 square with a client committing at up to 30 frames per second [meas\*], about 4.3-5.1 ms of CPU per commit [est: 128 ms / 30 to 128 ms / 25; the presented rate of that cell is not recorded]. The bus time of that square is 0.06 ms in RGB565 (and zero, because the driver filled it with the engine). Halving 0.06 ms is invisible next to 4.3 ms of fixed per-commit cost (protocol, scene, pixman, the atomic commit). For typing, cursor blinks, clocks and sliders the levers are tight damage and fewer commits, not depth [inf].

### 5.3 Full-screen updates: where the CPU goes

With the full-surface client at 25.4 frames per second, picowl itself used 16.5 ticks per second (6.5 ms per frame) [meas\*]. The 17.5 ms upload of each frame is not in that figure: wlroots commits nonblocking, so the driver's `atomic_update` copy runs in the DRM commit worker, not in picowl's process [inf: standard DRM helper behaviour, not checked on this kernel]. So the system spends about 25.4 x 17.5 = 445 ms per second, 44 % of the CPU, stalled on the bus [est], on top of picowl's 16.5 %. This is the cost C8 attacks.

Compositor-side quantise cost [est]: RGB565 in, one byte out, a fixed palette, a 4x4 ordered dither and a 4096-entry (RGB444) index table that fits in the PXA255's 32 KiB data cache:

- per pixel: load halfword, add the dither offset for (x & 3, y & 3), extract and pack 12 bits, table load, store byte: 8 to 20 cycles depending on how cleverly the dither is folded in;
- full frame: 76,800 x 8-20 = 0.61-1.54 M cycles = 1.5-3.8 ms at 400 MHz;
- memory traffic: 153.6 KB read plus 76.8 KB written in SDRAM, perhaps 1-3 ms more [unverified: PXA255 SDRAM copy bandwidth was not measured here];
- total 2.5-6.8 ms per full frame against a bus saving of 8.2-9.7 ms (17.5 minus 8.6-9.4 measured, or 8.4-9.7 by the rate).

Net, per full frame: a saving of roughly 1.4 to 7.2 ms of CPU [est: 8.2-9.7 minus 2.5-6.8], or 4 to 18 % of the CPU at 25 frames per second. That is real but not large, and the low end is close to zero. Experiment E1 settles it before any code is written.

Kernel-side conversion during the copy [est, inf]: one 16-bit bus beat takes 220-253 ns [meas\*], 88-101 core cycles at 400 MHz, and carries two C8 pixels. A conversion of 8-20 cycles per pixel fits inside the time the core would otherwise spend waiting for the bus, if the PXA write buffer lets the core run on while a VLIO write drains. The contention experiment shows the core stalls on SDRAM traffic while the bus is busy, so source cache-line fills (one per 16 pixels) would wait; the conversion might still be mostly hidden. This is the strongest argument for doing the conversion in the driver (option K in section 8), and experiment E4 measures it.

### 5.4 Terminal scroll

A scroll in havoc redraws every cell (libtsm resets the screen age) and damages the whole surface [read]. Per scroll frame: RGB565 17.5 ms of bus; C8 8.6-9.4 ms plus 2.5-6.8 ms of quantise in picowl [est]; havoc rendering in C8 natively (stage 4) 8.6-9.4 ms with no quantise. An engine scroll would upload only the new 12-pixel text line at RGB565 (240 x 12 x 2 = 5,760 bytes, 0.6-0.7 ms of bus) and move the rest inside video memory ((320 - 12) x 480 = 147,840 bytes at 135 MB/s, 1.1 ms of engine time) [est], but needs the move to be known to the driver (section 7.4).

### 5.5 Memory, caches, power

- Video memory: C8 frees nothing that matters; the pool beyond one RGB565 frame is 107,520 bytes, and C8 double buffering uses it [code\*].
- System memory for option C (compositor quantise): the existing RGB565 render buffer (150 KiB) stays and a C8 buffer (75 KiB) is added, plus a 4 KiB index table: about 80 KiB more resident [est]. For stage 4 (client indices), a C8 client buffer is half an RGB565 one and a quarter of havoc's ARGB8888 buffers (two of 300 KiB today) [est].
- Cache: the 4 KiB table and the 16-byte dither matrix stay resident in the 32 KiB data cache; a 64 KiB RGB565-indexed table would not and is rejected [inf].
- Power: the chip's scanout fetch halves (9.22 MB/s at 16 bpp to about 4.6 MB/s) [est from meas\*], an effect too small to measure next to the CPU. The real power effect is the CPU time not spent stalled; unmeasured [unverified].

## 6. Media player

### 6.1 What the player has today

The player repository was not found on this machine (scoped searches of `~/Projects` to depth 6 for `kms_state.c`, `ovplan.c` and `media-player.md`, and of the build volume's `work` directory to depth 4, found nothing), so its C8 code, palette, dither kernels, measured conversion costs and its backlog section could not be read [unverified]. What picowl's own documents say: the bare-DRM output drives "MediaQ C8 / pixel doubling / GC0C tear-free flips" and a full-frame RGB565 flip is upload-bound at about 28 Hz while C8 and pixel-doubled flips reach about 56 Hz (`doc/mediaplayer-integration.md` lines 14 and 85) [read]; the same document still says "C8 when implemented" (line 121) [read]. The player converts and dithers to RGB565 with ordered (Bayer) dither kernels in a fused convert-scale-rotate pass that costs 5.8-8.4 ms per shown frame at rotation 90 and about 2.1 ms at rotation 0 on the h2210 [meas\*, MediaQ reference §5.3; `doc/rotation-measurement.md`].

### 6.2 Path: lease, not Wayland

The lease gives the player the connector, CRTC, primary plane and its `GAMMA_LUT` (`doc/lease.md`) [read]. Everything in this section runs in the player over that fd; picowl is idle. A Wayland path for C8 video (section 9) would need a palette in the protocol and loses the GC0C flip unless the compositor scans the client buffer out directly; there is no reason to build it for video.

### 6.3 Palette strategies

| Strategy | Quality | CPU | Tearing | Recommendation |
|---|---|---|---|---|
| Fixed palette, ordered dither | Visible dither texture at 240x320; banding hidden by dither; skin tones and dark scenes suffer most | lowest: one table lookup per pixel (6.4) | none: palette loaded once | **first**: measure it |
| Per-title palette, computed offline | Good for most titles (a film's colour range is narrow) | no run-time cost beyond the table; a sidecar file per title, built by a host tool (median cut or k-means on sampled frames) | none | second, if the fixed palette looks poor |
| Per-scene palette, computed at run time at scene cuts | Best | median cut or an octree on a subsample (for example every fourth pixel of every fourth line, 4,800 samples) is a few milliseconds; rebuilding a 3D table by nearest-colour search is 4,096 x 256 distance tests = 1 M operations, about 5-10 ms [est]; must be spread over several frames | the palette and the frame that uses it must change in the same vertical blank (6.5) | only if the per-title result is not good enough |
| Error diffusion (Floyd-Steinberg) | Best per frame, but the error pattern crawls between frames (temporal noise) | 4 multiply-adds and 3 stores per channel per pixel, several times the ordered dither | none | not for video [inf] |

Fixed palette choice for video [inf]: a 6x7x6 RGB cube (252 entries) plus 4 greys, or a palette designed in YUV space (more luma levels than chroma levels, for example 16 Y x 4 U x 4 V = 256 with every entry converted to RGB) because the eye tolerates chroma error far better than luma error. The YUV-designed palette makes the conversion a direct index computation, `index = (Y' >> 4) << 4 | (U' >> 6) << 2 | (V' >> 6)` with dither added to Y', U' and V' first, and needs no table at all; its weakness is that the YUV box corners map outside the RGB gamut, so some entries are wasted.

### 6.4 YUV to index cost

- 3D table indexed by Y 5 bits, U 4 bits, V 4 bits: 8,192 one-byte entries, 8 KiB, fits in the data cache [est]. Y 6 / U 5 / V 5 would be 64 KiB and miss constantly; rejected.
- Per pixel with 4:2:0 input: the chroma part of the index is computed once per 2x2 block, so a pixel costs load Y, add the dither value, shift, OR with the chroma part, table load, store: about 6-8 cycles. Full frame 76,800 x 6-8 = 0.46-0.61 M cycles = 1.2-1.5 ms at 400 MHz [est], which is likely cheaper than the current YUV-to-RGB565 conversion with dither [inf; the player's figure for that pass alone is not available].
- With pixel doubling (once the kernel allows doubled C8): 120x160 = 19,200 pixels, 0.3-0.4 ms of conversion and 19,200 bytes of bus (2.1-2.4 ms) [est]. Decode at half resolution as well and the whole pipeline shrinks by four.
- Rotation: C8 is upright only on the MQ1188 today. A landscape clip on the portrait panel must then be rotated by the player in its conversion pass (the rotation-90 cost above) until the kernel enables C8 with the quarter turns.

### 6.5 Palette changes and tearing

The driver writes a changed palette once the beam has left the window and writes GC0C at commit time, latched at the next frame start [code\*]. If the commit arrives during the active window, both land in the same vertical gap: the palette at the window end, the new buffer at the frame start 1.09 ms later [inf]. 256 palette words are 512 bus beats, about 0.11-0.13 ms at 220-253 ns per beat [est], so the write fits inside the 1.09 ms gap. If the commit arrives after the window end, the palette lands at once (during the gap) and GC0C at the frame start: still the same gap, unless the palette write itself straddles the frame start, which happens only for commits in the last 0.13 ms of the gap [inf]. A scene-cut palette change is therefore tear-free in most frames by design; experiment E6 checks it by eye.

### 6.6 Subtitles and OSD

With a fixed palette, white, black, mid grey and the cube's pure colours exist, so subtitles and the OSD render exactly. With an adaptive palette, reserve 8-16 fixed entries (black, white, two greys, the OSD accent colours) and let the per-scene or per-title palette use the rest [inf]. Anti-aliased subtitle edges map to the nearest grey; the 24-step grey ramp of the xterm layout makes that clean.

## 7. Fullscreen terminal and other clients

### 7.1 A palette for a terminal

The xterm 256-colour layout, which havoc already uses as its default palette [read], is the natural compositor palette:

- the 16 ANSI colours are exact;
- `ESC[38;5;n` 256-colour output is exact, index for index;
- truecolour (`ESC[38;2;r;g;b`) maps to the nearest of the 256 entries; with the cube's 6 levels (0, 95, 135, 175, 215, 255) and 24 greys plus the cube's 6 greys, the worst case error is about 47 levels per channel near black (half of the first cube step, 95 / 2) and about 20 elsewhere (half of the 40-level steps) before dither [est];
- anti-aliased text: white-on-black (or any grey pair) gets 24 + 6 + 2 = 32 distinct grey levels for its coverage ramp, plenty for 8-bit stb_truetype coverage. A coloured pair such as red on black gets the cube's 6 levels along the red axis, enough for small text; a pair such as yellow on blue has intermediate colours that fall between cube entries, and its edges get the nearest cube colours [inf];
- subpixel (LCD) text, which picowl-panel draws when a layout is advertised, depends on per-channel colour fringes that a 256-colour palette destroys; picowl should advertise subpixel `none` while it scans C8 so that clients draw greyscale antialiasing [inf].

A custom per-terminal palette can do better for an unusual default pair. Entries needed for L coverage levels per pair (L - 2 intermediates plus the two solids): for the default pair plus 16 solids, 16 + 2 + (L - 2); with L = 16, 32 entries [est]. For every ANSI foreground on the default background, 16 x (L - 2) + 18; with L = 8, 114 entries; adding every ANSI background with the default foreground, another 96, 210 in total [est]. That fits in 256 but leaves nothing for 256-colour output; the xterm layout is the better general choice, with the 24-step grey ramp optionally replaced by the default foreground-to-background ramp when the configured default pair is not grey.

### 7.2 What havoc would have to change

| Change | Effect | Effort |
|---|---|---|
| Damage only the cells redrawn (`wl_surface_damage_buffer` per changed run of cells) instead of the whole surface | A keystroke uploads one cell (about 144 bytes if a cell is 6x12 pixels at RGB565 [est]) instead of 153,600 bytes: on the order of 1,000 times fewer bytes, 17.5 ms of bus saved per keystroke; independent of C8 | small: havoc already knows which cells it draws (`draw_cell` skips by age); collect their rectangles |
| Set an opaque region when `opacity` is 255, or use XRGB8888 or RGB565 buffers | pixman copies instead of blending (no OVER per pixel); RGB565 buffers also halve havoc's memory (two 150 KiB buffers instead of two 300 KiB) and remove the ARGB8888-to-RGB565 conversion in the composite | small |
| Render indices (stage 4): per (foreground, background) pair a 256-byte table from coverage to the nearest palette index, built lazily (256 nearest-colour searches over 256 entries, about 65 k distance tests, under 1 ms per new pair [est]) | glyph drawing becomes one table load per pixel, cheaper than today's three multiplies per channel; output is a C8 buffer | medium, and needs the palette protocol (section 8.4) |

**Stage 1 is implemented as havoc patch 0002** (`oe/recipes-graphics/havoc/files/`, described in the README there): havoc damages only the cells whose pixels changed since the frame the compositor shows, keeps a per-buffer record of what each of its two buffers holds, and at opacity 255 sets the opaque region and uses XRGB8888 buffers [read: the patch]. The damaged bytes are counted per commit by a fake compositor in a container, at 80x24 cells of 10x18 pixels (800x432, 1,382,400 bytes per frame); the tag **[meas, container]** means this count, not bus time and not a board. One typed character damages 1,440 bytes instead of 1,382,400, so 100 characters typed one by one damage 144,000 bytes instead of 138,240,000 (960 times fewer) and ten cursor hide-and-show cycles 14,400 instead of 27,648,000 (1,920 times) [meas, container]; a top-like refresh repainting the screen 20 times 3.8 MB instead of 55.3 MB (15 times) and `seq 1 3000` 0.10 MB instead of 2.8 MB in two commits (27 times, a figure that depends on how the output splits into commits) [meas, container]; one scrolled line 1.0 MB instead of 1.4 MB, since a scroll changes nearly every cell and the damage stays what 7.4 describes [meas, container]. On the panel a keystroke is two cells, about 290 bytes at RGB565 [est: two of the 144-byte cells above], which is the "few hundred bytes" of section 1. Not verified: picowl's wlroots with these damage rectangles, its handling of the one pixel havoc adds under a fractional scale, and any time on the board [unverified]; the opaque region only applies if the board's `opacity` is 255, which is not known (the code default is 255, the sample configuration 230) [unverified].

### 7.3 Cursor, selection and inverse video

havoc draws its text cursor and selection as inverse attributes in the cell grid [read: `tsm/tsm-render.c` lines 185-196 toggle `attr.inverse`, `main.c` `draw_cell` lines 714-733 swap the colours; patch 0002 splits that function into `collect_cell` and `paint_cell`, same swap]; in a palette the inverse of an entry is generally not an entry, so the quantiser maps it to the nearest colour (for the default pair, inverse simply swaps the two solids) [inf]. The pointer cursor is a separate surface; picowl only draws its hold animation, and the hardware cursor plane takes its colours directly, not through the palette [datasheet\*].

### 7.4 Scrolling

A scroll is a full repaint in havoc and a full upload in the driver whatever the depth. The 2D engine can move video memory at 135 MB/s [meas\*], but the shadow-plane design hides moves from the driver: it sees damage, not "this rectangle moved" [code\*, MediaQ reference §5.3]. Two ways to use the engine anyway, both independent of C8:

1. **Driver-side scroll detection [inf].** In `atomic_update`, for a damage rectangle wider than some threshold, hash each shadow row and compare with the row hashes kept from the previous upload; a constant vertical offset over most rows means a scroll: engine-copy those rows inside video memory (an upward scroll is a forward copy, measured exact on the MQ1188 [meas\*]) and upload only the new rows. Cost: reading the shadow rows and hashing, roughly 1-2 ms per full frame in SDRAM [est]; saving about 15 ms per scroll frame. Works for every client, fbcon included.
2. **Driver-side unchanged-tile skip [inf].** The same row or tile hashes detect regions whose content did not change and skip them; this alone neutralises havoc's full-surface damage for a keystroke without changing havoc. Listed as not implemented in the MediaQ reference §5.5 (X11 option 4).

Neither needs a new UAPI. Both are kernel work and must handle the double-buffered C8 path (the back buffer holds the frame from two commits ago).

### 7.5 Several clients with different needs

The hardware has one palette for the whole window [datasheet\*]. So:

- **One global palette, owned by picowl.** Clients never set the hardware palette. In option C every client renders RGB565 (or ARGB8888) as today and picowl quantises the composited result; the panel, the OSK and a video tile above a terminal all coexist, at the colour quality of the global palette.
- **Translucent surfaces** (the panel's slider rows, notifications): blending in index space is impossible, so picowl blends in RGB565 with pixman as today and quantises afterwards. No extra cost beyond the quantise pass over the damaged area.
- **Client-indexed buffers** (stage 4) are valid only under the global palette, which the compositor announces. A C8 client buffer that is composited (not direct-scanned) is read by pixman through the palette as a `PIXMAN_c8` source with `pixman_image_set_indexed()` [read: the API exists], blended in RGB565 and re-quantised; the re-quantisation must map palette colours exactly to themselves (dither only colours that are not palette entries) or the content degrades on every composite [inf].
- **Screenshots and screencopy:** in option C the RGB565 render buffer holds the pre-quantisation image; screencopy (`doc/capture.md`) would show colours the panel does not. Capturing the C8 buffer and expanding it through the palette shows the truth at an extra pass; make it a capture option [inf].

### 7.6 When to enter and leave C8

| Trigger | Assessment |
|---|---|
| `[output] depth = 8` config key | simplest; useful for kiosks and measurement; always on |
| Per-app rule, for example `[app.havoc] output_depth = 8`, active while that app is focused and full screen (panel auto-hidden) | **recommended first policy**: reuses the `[app.*]` section; the switch costs one full upload (the format change voids the damage clips for one commit, 3.2) plus the palette write, about 10 ms [est]; add hysteresis so a popup or the OSK does not flip the depth back and forth |
| Client opt-in through a protocol request | stage 4, together with client-indexed buffers |
| Automatic, when only palette-friendly content is visible | rejected: picowl cannot tell from RGB565 pixels whether quantisation will hurt [inf] |

Rotation: C8 is upright only on the MQ1188 today, so in a rotated configuration picowl must either rotate in software while in C8 (21.9 against 16.5 ticks per second for a full-surface client at 90 degrees, 5.4 ticks per second more [meas\*]) or not enter C8 until the kernel enables C8 with the quarter turns.

## 8. Compositor architecture

### 8.1 Options

| Option | Where the quantise runs | picowl change | wlroots change | Kernel change | Assessment |
|---|---|---|---|---|---|
| **K: in the driver's copy** | in `atomic_update`, RGB565 shadow to C8 video memory, fixed palette | none, or a config key that sets a plane property | none | a driver property (for example "scanout depth 8, fixed palette") on an RGB565 plane, or a module parameter; palette, dither and table in the driver | cheapest overall and possibly free CPU-wise (5.3); downstream-only, because the plane would scan a format other than the one the framebuffer declares; screencopy and every client stay RGB565. **Best first prototype**, gated on E4 |
| **K332: honest RGB332** | clients or pixman render `DRM_FORMAT_RGB332`; the driver scans it as C8 with the upstream `drm_crtc_fill_palette_332()` palette | render format RGB332 | add RGB332 to the pixman format table (`PIXMAN_r3g3b2` exists) and the generic pixel-format table | add `DRM_FORMAT_RGB332` to the plane formats with a fixed 3-3-2 palette | honest and upstreamable; but 3-3-2 has only 8 red and green and 4 blue levels, and pixman's dithered narrow-format path is likely slow [unverified]; poor for terminals (the ANSI colours are not exact) |
| **C: in picowl, after pixman** | picowl, per damaged box, RGB565 render buffer to a C8 buffer | new `src/c8out.c`; the copy-type swapchain becomes RGB565 render buffer + C8 scanout buffer; `GAMMA_LUT` committed on entry | C8 entry in `render/pixel_format.c` (for the dumb allocator and the framebuffer import) | none: C8 and `GAMMA_LUT` exist [code\*] | **recommended for picowl**, gated on E1; a compositor-owned palette, exact for palette colours; costs one RAM pass per damaged pixel |
| **P: pixman renders C8** | pixman, through `PIXMAN_c8` destination with `pixman_indexed_t` | render format C8 | C8 in the pixman format table, an indexed table attached to the render image, texture and render paths for an indexed format | none | most invasive; pixman's general path for an indexed destination is likely slower than option C's dedicated loop [unverified]; not recommended |
| **D: client indices, direct scanout** | the client | picowl-buffer-v1 C8 format and palette event; C8 in the linux-dmabuf feedback; per-surface decision whether to scan out | C8 in the format tables (pixman source with indexed table for the composite fallback) | none | best efficiency for one full-screen client; stage 4 |

### 8.2 Option C in detail

- **Buffers.** Keep picowl's one-slot RGB565 swapchain as the render target, but no longer attach it to the plane. Add a one-slot swapchain of C8 dumb buffers (`wlr_swapchain_create()` with a C8 `wlr_drm_format`, which needs the wlroots pixel-format entry) for scanout. On the copy-type MQ1188 one C8 buffer suffices: the driver copies from the shmem shadow and double-buffers in video memory itself [code\*].
- **Frame.** In `output_frame()` (`src/output.c` lines 292-317) replace `wlr_scene_output_commit()` by `wlr_scene_output_build_state()` into the RGB565 swapchain, take the state's damage, quantise those boxes from the RGB565 buffer into the C8 buffer, then replace the state's buffer with the C8 buffer (`wlr_output_state_set_buffer()`) and commit with the same damage.
- **Palette.** On entering C8, commit `wlr_output_state_set_gamma_lut()` with the 256-entry palette (16-bit components, the driver keeps the top six bits) in the same commit as the first C8 buffer; wlroots sends it as the `GAMMA_LUT` blob [read]. The scene's own gamma-control handling (`types/scene/wlr_scene.c` lines 2020-2038) must be kept out of the way: picowl does not offer `wlr-gamma-control` today, which keeps it so [read: no gamma in `src/`].
- **Quantiser.** A 4,096-byte table from RGB444 to index, built once from the palette by nearest-colour search with the colour difference weighted for the eye; a 4x4 Bayer matrix added per channel before truncation to 4 bits; a second table that marks exact palette colours so they are not dithered (needed for clean text in the 16 ANSI colours) [inf]. Loop over 32-bit words (two RGB565 pixels in, two indices out), ARM mode, no allocation per frame.
- **Direct scanout** of client buffers is impossible in C8 mode (they are RGB565), so `copied` events stop and picowl-buffer-v1 clients receive `retained`, which they already handle [read: protocol].
- **Damage.** The RGB565 render buffer has buffer age 1 like today; the C8 buffer is written only inside the same damage, so its content stays coherent [inf].
- **Leaving C8.** One commit with the RGB565 buffer and full damage (the format change makes the driver upload the whole frame anyway [code\*]).
- **Headless.** The headless backend has no C8; a debug option that quantises and expands back through the palette into the headless output lets the quality and the damage logic be tested in CI and in a container [inf].

### 8.3 wlroots patches needed

| File | Change | Needed by |
|---|---|---|
| `render/pixel_format.c` | add `{ .drm_format = DRM_FORMAT_C8, .bytes_per_block = 1 }` (and RGB332 for option K332) | C, D, K332 (dumb allocator, shm allocator, framebuffer import) |
| `render/pixman/pixel_format.c` | add `DRM_FORMAT_C8` to `PIXMAN_c8` and `DRM_FORMAT_RGB332` to `PIXMAN_r3g3b2` | D (C8 client buffers as pixman sources), K332, P |
| `render/pixman/renderer.c` | attach a `pixman_indexed_t` to C8 textures (`pixman_texture_create()`, around line 398) from a compositor-provided palette | D, P |
| `types/output/render.c` (`output_pick_format()`, line 147) | none for option C (picowl does not let wlroots pick C8) | -- |

All are small; the indexed-texture change is the only one with a new API (a palette pointer per renderer or per texture) and would be a picowl-local patch, numbered after the existing five.

### 8.4 Option D protocol sketch (stage 4)

picowl-buffer-v1 version 3: the manager advertises `format` C8 in addition to RGB565, and a new event `palette(array entries)` (256 x 4 bytes, `0x00RRGGBB`) sent on bind and whenever the global palette changes; `create_buffer` accepts `DRM_FORMAT_C8`. Semantics: a C8 buffer's indices refer to the last announced palette; picowl scans a C8 buffer out directly only while the output is in C8 mode and the surface is the only visible node, otherwise it composites it through the palette. A client must redraw after a `palette` event. No client may set the hardware palette, which keeps arbitration trivial. wl_shm C8 buffers would be accepted with the same meaning (the protocol has no palette, so the picowl event defines it).

### 8.5 Kernel work items

| # | Item | Why | Size [est] |
|---|---|---|---|
| K1 | Get the current `mq11xx.c` (with C8, `GAMMA_LUT`, vblank, engine, `daf8e6712ae1`) into the meta-handhelds patch series | the checkout's patch 0076 is RGB565 only (3.6) | packaging |
| K2 | Exercise and enable 8 bpp engine fills and copies on the MQ1188 | C8 loses the solid-fill shortcut until then; WinCE programs the engine to 8 bpp (section 4) | 1 day with the existing guarded self-test |
| K3 | Exercise and enable pixel-doubled C8 on the MQ1188 | the cheapest video mode (19,200 bytes per frame) | 0.5-1 day |
| K4 | C8 with 180 degrees, the flips and the quarter turns | needed for landscape C8 without software rotation; the 8 bpp start-address and stride rules are undocumented for quarter turns (section 4, question 6) | 1-2 days plus panel checks |
| K5 | Optional option K: fixed-palette conversion in the copy loop behind a plane property or module parameter | measure first (E4) | 1-2 days |
| K6 | Row-hash scroll detection and unchanged-tile skip in `atomic_update` (7.4) | the largest terminal win at any depth | 2-4 days |

### 8.6 An engine back end: what the 2D engine could do for picowl

The question this answers: with video memory holding more than the scanout frame, could picowl keep several buffers or surfaces there and compose them with the 2D engine, in C8 or in RGB565? Source for everything in the next two tables: the revival MediaQ reference, §1.4, 2.3, 3.2, 4.1 to 4.8 and 5.1, as sent by its session; none of it was run by me.

**Established or documented.**

| Fact | Evidence |
|---|---|
| Fills (`PATCOPY` 0xf0) and VRAM-to-VRAM copies (`SRCCOPY` 0xcc, both directions, overlap-correct) work on the MQ1188 with scanout running: 240x224 fill 551 us (185 MB/s), 240x112 copy 368 us (135 MB/s), a 16x4 fill 17.9 us including the first poll; a fill or copy is 4 register writes | [meas\*] §4.1, 5.2 |
| The engine addresses memory independently of the scanout window (base GE0B, stride GE0A), and fills and copies at rows 320 to 339, below the 240x320 frame, were exact with scanout running | [datasheet\*], [meas\*] §4.1, 3.2.4 |
| Surface limits: X and Y 12 bit, stride a 10-bit byte count (at most 1023 bytes per row: 511 pixels at 16 bpp, 1023 at 8 bpp), base address 8-byte aligned, clipping needs a stride that is a multiple of 8, depth field 00 = 8 bpp and 01 = 16 bpp | [datasheet\*] §4.1 |
| All 256 ROP3 codes; BitBLT and Bresenham line; colour transparency on the source or the destination ('blue screen') with a key (8 bpp: low byte), polarity selectable; monochrome 8x8 patterns only (no colour patterns); clip rectangle; monochrome expansion with a transparent background; horizontal and vertical mirror by copy direction (the start corner is set by software) | [datasheet\*] §4.2 to 4.8; WinCE uses ROPs, source key on host blits, mono expansion for glyphs and the 0xaaf0 mask blit, never the clip rectangle or the destination key [decomp\*] |
| No scaler, no alpha, no colour-space conversion (MQ1132 datasheet; on the MQ1188 only "not observed"). A stretch is many small replicated copies | [datasheet\*] §4.8 |
| Sources are video memory or the host source FIFO (the CPU writes 32-bit words to chip+0xc00..0xfff; FIFOs 16 entries). **No bus mastering from system RAM is documented.** `mq11xx.present_dma` is not the engine: it is the PXA255's DMA controller copying a bounce buffer into video memory, 8.1 to 8.7 MB/s, no faster than the CPU because the VLIO bus is the limit | [datasheet\*] §4.1, [code\*], [meas\*] §5.1 |
| Video memory is 256 KiB on both chips. The driver's layout: frame at 0, off-screen pool 0x25800 to 0x3fbff (107,520 bytes), the 1 KiB cursor image at 0x3fc00; the page-flip back buffer lives in the pool only when two frames fit (8 bpp 76,800 bytes) | [code\*] §5.1 |
| **At 8 bpp** the frame is 76,800 bytes, which leaves 184,320 bytes (about 2.4 more screens), of which the back buffer takes 76,800, so about 107,520 bytes remain for surfaces; at 16 bpp the pool is 107,520 bytes (under 0.7 of a screen) | [inf], arithmetic from the row above |
| One image window with one depth field and one 256-entry palette for the whole chip. No second graphics window, overlay, blend or colour key between windows is documented, none observed on the MQ1188. The "alternate window" (GC00[11:10]) is only a second buffer address with the same format (inconclusive on the MQ1188). The window start GC0C is latched at the frame boundary (tear-free flip) | [datasheet\*], [meas\*] §3.2 |
| The lower 256 KiB aperture is serialised with the engine: CPU access to video memory is held off while the engine draws, without a timeout. The upper alias (chip+0x42000..) is not serialised and can race the engine. Scanout and engine share the memory interface; the arbitration priority is unknown | [datasheet\*] §1.4, 4.7 |
| A register access on the 16-bit bus is about 0.22 to 0.25 us per 16-bit beat, so issuing one fill or copy costs roughly 2 us of CPU plus the poll reads (not measured). Busy is bit 16 of CC01; there is an idle interrupt bit, unproven | [inf], [datasheet\*] §4.7 |

**Not established, and what a back end depends on:** every engine command at 8 bpp (fills, copies, key, mono expansion) and its rate; the host source FIFO and mono expansion under Linux; colour key; the clip rectangle; auto-execute batching; the engine idle interrupt; the arbitration between scanout and engine; the command window (GE0C).

**What follows.**

1. **Different surfaces, one depth.** Video memory can hold several surfaces and the engine can compose them into the scanout frame, but all of them share the chip-wide depth and palette. In C8 every surface is C8 with the same palette (the fixed-palette options of section 7.5); no C8 window beside an RGB565 window, no hardware blending of windows.
2. **The engine cannot blend, so translucency stays in software.** The popup row's `--popup-alpha` and any other alpha would be composed by the CPU in system RAM, as now. What the engine can do is hard-edged: opaque copies, fills, source colour key (a surface with a transparent colour) and a 1-bit mask with a transparent background (text over an arbitrary background without reading it back).
3. **Content still has to cross the bus once.** A client's pixels live in system RAM and the engine cannot fetch them, so each changed pixel costs one trip at 7.9 to 9.1 MB/s (CPU or DMA). The engine saves the repeat trips: moving, raising, exposing and uncovering a surface that is already in video memory.
4. **Where that pays.** Keyboard pan and popup open and close, today a full-frame upload, become an engine copy of at most the frame: 76,800 bytes at 135 MB/s is about 0.6 ms, 153,600 bytes about 1.1 ms [est], against about 9 ms (C8) or 17.5 ms (RGB565) of upload. Scrolling is the same trade (section 7.4). Painting a solid background and drawing text from a 1-bit source never touch the bus with pixels.
5. **Space is the limit.** About 107,520 bytes of pool either way: at 8 bpp that is the room left after the double buffer, at 16 bpp there is no double buffer. That holds the popup's save-under (a 240x54 strip is 12,960 bytes at 8 bpp and 25,920 at 16 bpp), a glyph atlas, or one more small window, not a second screen.

**Three ways to build it, in order of cost.**

| | What | Interface | Verdict |
|---|---|---|---|
| **E-a** | Driver only: detect scrolls and unchanged tiles in the damage the driver already sees, engine-copy within video memory, upload the rest (section 7.4, items 1 and 2) | none | Cheapest, covers pan and popup and scroll. Needs the double-buffered C8 path handled and the 8 bpp engine enabled |
| **E-b** | picowl tells the driver what moved (a "copy this rectangle" hint with the damage), and keeps the popup save-under and similar surfaces in the pool | a new plane property, or a private ioctl | Exact where E-a only guesses. Needs a kernel interface the project does not have; DRM has no 2D interface |
| **E-c** | A full command list from clients or picowl: fills, copies, key blits, 1-bit mask blits, host blits through the source FIFO | private ioctl plus protocol | Largest work. Only worth it if a register write on this bus costs about 1 us or less and the FIFO and mono-expansion paths work, none of which is measured. A glyph costs about 8 bus writes [inf], against 132 bytes of RGB565 through the CPU (about 15 us) |

**Recommendation.** E-a first, and only after the experiments below. Do not design E-c until the host FIFO and mono expansion have been run once on the board.

**Experiments** are E9 to E14 in section 11 (all on the h2200, none run): 8 bpp engine rates, mono expansion, colour key, the cost of issuing a command, engine against scanout, and the idle interrupt.

## 9. Staged plan

| Stage | Content | Decides | Effort [est] |
|---|---|---|---|
| 0 | Experiments E1 (quantise benchmark, pure CPU) and E2 (C8 commit, palette and switch timing with a small DRM test client) on the h2200 | whether option C saves anything; switch and palette costs | 1-1.5 days |
| 1 | havoc: per-cell damage and an opaque region (or RGB565 buffers); upstream them | the terminal's largest win; baseline for everything after | 1 day |
| 2 | Player: C8 over the lease with a fixed palette and ordered dither (player work); kernel K1, K3 | video C8 frame rate and quality on the panel | player-side unknown; kernel 1-2 days |
| 3 | picowl option C behind `[output] depth` and `[app.*] output_depth`, with the wlroots pixel-format patch, the headless preview and unit tests of the quantiser and damage handling | compositor C8 in practice for a full-screen havoc | 4-6 days |
| 3b | Alternatively option K in the driver (K5), if E4 shows conversion hidden behind the bus | no picowl change at all | 1-2 days kernel |
| 4 | picowl-buffer-v1 C8 and `palette` event; havoc renders indices | removes the quantise pass for the terminal | 5-8 days |
| 5 | K2, K4, K6 as measurements justify | engine fills in C8, landscape C8, scroll detection | 4-7 days |

The smallest step with the biggest measured win is stage 1, which does not involve C8 at all. The smallest C8 step is stage 2 (the kernel already has everything the player needs on an upright panel).

What can be prototyped without a board: the quantiser and its image quality (host or container: PNG dumps of real screenshots through the palette, PSNR against the RGB565 original); option C's damage logic in picowl's headless backend with the expand-through-palette preview; the wlroots patches and unit tests. vkms cannot stand in: whether it offers C8 and a palette is [unverified], and it has no copy-type bus to measure.

## 10. Risks and alternatives

| Risk or alternative | Assessment |
|---|---|
| The quantise pass costs about as much as the bus time it saves | possible (5.3 gives 1.4-7.2 ms net saving per full frame, low end near zero); E1 decides before code is written |
| Small-damage workloads see no gain | certain (5.2); C8 is for full-screen motion and repaints only |
| C8 rectangles are uploaded where RGB565 solid rectangles cost nothing | true until K2; for flat UIs with large solid damage C8 can be worse than RGB565 |
| Colour quality: banding, dither texture, lost subpixel text | inherent; mitigated by the xterm palette for terminals and by keeping C8 opt-in |
| Rotation: C8 upright only on the MQ1188 | software rotation in C8 mode, or K4 |
| Nothing looked at on the panel | every driver result is register-level; E5 and E6 are needed before any user-facing mode |
| The deployed kernel lacks the C8 driver | K1; the checkout's patch is RGB565 only |
| Maintenance: two render paths in picowl, patches in wlroots | option C is self-contained (one file plus one wlroots line); option D touches the protocol and the renderer |
| Alternative: RGB565 with tight damage | already the design, and for UI it beats C8 (5.2) |
| Alternative: pixel doubling alone | four times fewer bytes for full-screen video at RGB565 (38,400 bytes, measured 4.3-4.6 ms); available today on the MQ1188 for RGB565; for video it beats full-resolution C8 on bytes, at half the resolution |
| Alternative: 2D-engine offload (fills, copies, scroll detection) | the engine is 16-24 times faster than the bus [meas\*]; scroll detection (K6) is the terminal's best kernel-side lever |
| Alternative: YUV overlay | none on the MediaQ (one image window) [datasheet\*]; the hx4700's W3220 has one, through the lease |
| Alternative: lower refresh | the bus copy is independent of refresh; scanout fetch is a few percent of memory bandwidth [meas\*, inf], so lowering refresh saves nothing measurable for uploads |
| Dead end: copy path CPU-bound, not bus-bound | the measurements say bus-bound: CPU and DMA copies run at the same 7.9-9.1 MB/s and the core stalls whoever writes [meas\*]; halving bytes halves the stall |
| Legal | none specific: palettes, Bayer matrices and median cut are textbook techniques; the WinCE decompile is used as reference evidence only, no code is copied |

## 11. Experiments to run on the h2200

Cheap and risk-free first. All need the current driver (K1).

| # | Experiment | Measures | Risk |
|---|---|---|---|
| E1 | Userspace benchmark (no display access): quantise a 240x320 RGB565 screenshot to C8 with (a) RGB444 table plus 4x4 Bayer, (b) the same without dither, (c) pixman `PIXMAN_r3g3b2` with Bayer-8 dither, (d) the per-pair coverage tables of 7.2; full frame and a set of damage boxes; 100 iterations, `clock_gettime(CLOCK_MONOTONIC)`, governor pinned | cycles per pixel on the real CPU; decides option C | none |
| E2 | A small atomic KMS client: commit C8 full frames and partial damage, read `mq11xx_copy_stats` before and after; time a `GAMMA_LUT`-only commit; time an RGB565-to-C8 and back switch | upload bytes and time at 8 bpp (expected 76,800 bytes in 8.6-9.4 ms); palette write latency; depth switch cost | none (display content only) |
| E3 | System-wide CPU (`/proc/stat`) during picowl's W3 workload (`picowl-commit-loop --damage full`), alongside picowl's own ticks | confirms that the upload runs in the commit worker and how much CPU it takes (5.3) | none |
| E4 | Kernel A/B: in the driver's timing file, `memcpy_toio` of 76,800 bytes against a loop that converts 153,600 bytes of RGB565 to 76,800 bytes of C8 while writing to video memory | whether the conversion hides behind the bus; decides option K | low (driver debugfs) |
| E5 | Look at the panel: a C8 test pattern (colour bars, a grey ramp, the xterm palette as 16x16 swatches) with the palette loaded | colours, palette byte order, dither appearance | none |
| E6 | Palette change at a frame flip: alternate two frames whose palettes differ, at 56 Hz, filmed with a phone at 240 fps | whether palette and GC0C land in the same vertical gap (6.5) | none |
| E7 | After K2: 8 bpp engine fill through the guarded self-test | enables C8 solid-fill shortcut | low |
| E8 | havoc before and after per-cell damage, `seq 1 3000` and typing at 10 characters per second, `mq11xx_copy_stats` bytes and picowl ticks | the terminal baseline C8 must beat | none |
| E9 | Engine at 8 bpp with scanout running: fills and copies of 16x4, 240x112 and 240x224, reading the busy bit (section 8.6) | 8 bpp engine rates; the base for every engine plan | low |
| E10 | Mono expansion through the source FIFO with a transparent background, a few glyphs at 6x11 | correctness and time per glyph (about 8 bus writes expected) | low |
| E11 | Source colour key on a copy (8 and 16 bpp) | whether a keyed surface is usable | low |
| E12 | Time 1000 back-to-back 16x4 fills, with and without the poll | the real cost of issuing a command on the 16-bit bus (about 2 us expected) | none |
| E13 | A copy of 76,800 bytes while the frame is scanned out, filmed at 240 fps | whether engine and scanout arbitration shows as tearing or stalls | none |
| E14 | Enable the engine idle interrupt (IN01 bit 12) | whether an idle interrupt arrives on GPIO14 | low |

## 12. Open questions

1. Where is the player's C8 implementation, its palette and its measured dither and conversion costs (the backlog section the owner mentioned)? Not readable here.
2. Does the palette byte order on the panel match the datasheet (red in [7:2])? E5.
3. Does the MQ1188 engine draw 8 bpp surfaces correctly? K2, E7.
4. What are the 8 bpp rules for decrementing-X and column scans (start address, stride)? Section 4, question 6; K4.
5. Is the PXA255 write buffer deep enough for conversion to hide behind VLIO writes? E4.
6. Does the cursor plane work over a C8 primary plane? Not exercised [code\*].
7. Should picowl advertise subpixel `none` automatically in C8 mode, or leave it to the configuration?
8. For the h5550 (MQ1132): all C8 paths are build-verified only; no unit is available.
9. Do the SA-1110 and PXA25x LCD drivers (h3870, h3970) offer C8, and would picowl's option C apply there (h3870: halves the memcpy into uncached memory)?

## 13. Sources

- picowl (this repository, commit 8b0aaf8): `README.md`; `doc/zero-copy.md`; `doc/mediaplayer-integration.md`; `doc/lease.md`; `doc/rotation-measurement.md`; `doc/rotation-results.md`; `src/output.c`; `protocols/picowl-buffer-v1.xml`; `data/picowl.ini.example`.
- wlroots 0.19.0 as vendored by picowl's wrap: `render/pixman/pixel_format.c`, `render/pixman/renderer.c`, `render/pixel_format.c`, `render/allocator/drm_dumb.c`, `render/allocator/shm.c`, `backend/drm/atomic.c`, `backend/drm/drm.c`, `types/scene/wlr_scene.c`, `types/output/render.c`, `include/wlr/types/wlr_output.h`.
- Wayland: `protocol/wayland.xml` (the vendored copy).
- pixman 0.46.4 public header `pixman.h`.
- Linux master (fetched October 2026 through the GitHub API, `drm_color_mgmt.c` at commit 2e05544): `include/uapi/drm/drm_fourcc.h`, `drivers/gpu/drm/drm_color_mgmt.c`, `drivers/gpu/drm/drm_fb_helper.c`.
- meta-handhelds kernel series for the h2200: `0076-drm-tiny-add-driver-for-the-MediaQ-11xx-display-cont.patch` (early driver version).
- linux-hnd (hackndev): `include/linux/mfd/mq11xx.h`, `drivers/mfd/mq11xx_base.c`, `drivers/video/mq1100fb.c`.
- "MediaQ MQ1188 and MQ1132" chip reference from the h2200-revival project (sections 1.4, 3.2-3.6, 4, 5.1-5.6, 6, 7), which in turn cites the MediaQ MQ-1100/1132 datasheet (document 12-00026 Rev D), the WinCE decompiles and board measurements.
- The Ghidra decompile of the h2200 WinCE display driver `ddi.dll` (`mq1188ddi.pdb`), `h22xx-ddi.c`, functions `FUN_02571778`, `FUN_025719b4`, `FUN_02571ab8`, `FUN_02571c28`, `FUN_02572b54`, `FUN_02572c94`, `FUN_025766e4`, `FUN_0257677c`.
- havoc (github.com/ii8/havoc, commit 73e3467): `main.c`, `tsm/tsm-screen.c`, `tsm/tsm-vte.c`; the meta-handhelds recipe `havoc_git.bb`.
- The ipaq-ui research notes, branch `docs/ipaq-ui-research`: `docs/ipaq-ui/hardware.md`.
