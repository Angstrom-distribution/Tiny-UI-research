# Text on the handheld: what WinCE did, what we can match, and what is worth accelerating

**Status:** research and a decision record, partly implemented. Implemented: the compiled-in pixel fonts of `picowl-panel --style crisp` (`panel/panel-pixfont-data.c`) and the hinted DejaVu bake of section 5 as `--crisp-font dejavu` (`panel/panel-pixfont-dejavu.c`, the default stays `fixed`); see [README.md#panel](../../README.md#panel). Not implemented: the engine mono expansion (section 6) and the havoc bitmap-font backend (section 8). Nothing here was measured on a board unless it says so.

What is scheduled from this document is tracked in the [roadmap](roadmap.md).

## 1. Evidence tags

| Tag | Meaning |
|---|---|
| **[decomp]** | Read from the decompile of the WinCE (Pocket PC 2003, CE 4.20 build 13100) ROMs of the h2200 and the h5550: `gwes.exe`, the MediaQ `ddi.dll`, the registry fragments and the font files. Nothing was run on a device |
| **[doc]** | Stated in the MediaQ MQ1132 programming documentation, as read by the MediaQ revival work |
| **[meas]** | Measured: on a board by the MediaQ revival work, or (where the line says so) by a harness of this repository in a container |
| **[host]** | Rendered on a development host in a debian:trixie arm64 container and looked at; not the device |
| **[inf]** | Inference from the above |
| **[est]** | Estimate, not measured |

## 2. What WinCE did on this class of device

Both ROMs carry the same font code and byte-identical fonts [decomp].

- **The default is bi-level, fully hinted text.** `gwes.exe` has three per-font modes, chosen from the font's `lfQuality` and two global switches: ClearType, 16-level gray, else bi-level. The registry keys `SYSTEM\GDI\ClearType`, `SYSTEM\GDI\FONTSMOOTHING` and `ForceGRAY16` are absent in both shipped boot registries, so the shipped default is bi-level [decomp]. The system font is Tahoma, height -12, weight 400, 96 dpi [decomp].
- **The hinting is real TrueType bytecode.** The rasteriser is inside `gwes.exe` (a 256-entry opcode dispatch table); `GETINFO` reports rasteriser version 37 (0x25) [decomp]. Bi-level and ClearType text are always hinted. Gray text follows the font's `gasp` table [decomp].
- **The fonts.** Tahoma and Tahoma Bold 1.07, Courier New 2.50, Frutiger Linotype regular, bold and italic, and Bookdings. All are 2048 units per em with full bytecode (`fpgm`, `prep`, `cvt`, instructions on more than 90 percent of the glyphs). Tahoma's `gasp` says: up to 8 ppem gray, up to 16 hinted, above both, so Tahoma renders bi-level at 9 to 16 ppem, which is the system font size, even with smoothing on [decomp, inf from the ppem bands]. Only Courier New carries embedded 1-bit strikes (4 to 21 ppem) [decomp].
- **Gray** is 4x4 oversampling into a 4 bpp mask with 16 levels [decomp].
- **ClearType exists on this build but is off by default.** It is hinted at 1x, the outline is scaled by 6 horizontally, the subpixel order is fixed RGB (the BGR and vertical flags are never set, there is no rotation), and the filter is exactly a one-pixel box: R = Bprev+R+G, G = R+G+B, B = G+B+Rnext, checked against all 243 inputs. The blend is per channel in linear light through gamma tables; the h2200 registry holds gamma 1.3, and the h5550 driver creates 1.5 [decomp].
- **Glyph cache:** per realised font, 256 hash buckets, chained, least recently used; the budget is 0x2000 bytes times 1 (bi-level), 4 (gray) or 8 (ClearType) [decomp].
- **The path to the driver.** `ExtTextOutW` ends in the driver's `DrvBitBlt` with ROP4 0xAAF0 and the text colour as the brush. Bi-level and gray text is one glyph per call (a 1 bpp or 4 bpp mask); ClearType is one 8 bpp mask per string. There is no `DrvTextOut` and no alpha entry in the CE driver interface [decomp].
- **Only 1 bpp text reaches the graphics engine.** In `ddi.dll`, 1 bpp masks go to a routine that is most likely the MediaQ mono expansion; 4 bpp and 8 bpp masks fall to the GPE software emulation, so gray and ClearType text are blended by the CPU [decomp, inf for the identification]. The engine therefore only ever drew the shipped default.

Not established: whether Courier New's embedded bitmaps are used (the substitution path was not traced); what the driver escapes 0x1806 and 0x1807 do; how Pocket IE and MS Reader draw text (they render it themselves); the panels' physical stripe order; the live gamma on a running device.

## 3. What this means for picowl

- **picowl's `--subpixel rgb` already matches WinCE ClearType's order and filter shape.** A one-pixel box over RGB is what FreeType's light LCD filter computes [inf]; the order is fixed RGB in both. Nothing to change.
- **"Crisp" in WinCE terms is bi-level and hinted, not smooth text with sharper edges.** That is what `--style crisp` does.
- **WinCE's gray path honours `gasp` per size; FreeType does not.** A port has to choose bi-level or gray per size itself.

## 4. Hinted TrueType through FreeType: what was compared

A standalone prototype (not part of the panel build) rendered Liberation Sans, DejaVu Sans and Sans Mono, Terminus (TTF) and Noto Sans at 9 to 14 px with: the v35 interpreter with `FT_LOAD_TARGET_MONO | FT_LOAD_NO_AUTOHINT`, the v40 interpreter in mono, the autohinter in mono, and light gray as the blurry reference. These are [host] renders, 1152 in all, looked at by eye.

- **Best: DejaVu Sans, v35, mono, no autohint.** Regular at 12 px and Bold at 11 or 12 px give uniform 1 px stems (2 px for bold), even spacing, clean digits, a steady x-height, and `m`, `W` and `%` do not collapse. DejaVu is Verdana-like, so Regular at 12 px is the closest free match to bi-level Tahoma 12 [host, inf]. The v40 interpreter in mono is almost the same and slightly less regular; the WinCE side makes v35 the closer match [inf]. The autohinter in mono is uniform but heavy and wide.
- **Liberation Sans Bold** has good digits, but its lowercase falls to 1 px stems and `%` blobs at 12 px. **Noto Sans** is ragged and `%` collapses. **Terminus** is sound only at its native 12 px. **DejaVu Sans Mono** is fine but wide.
- **For the 18 px bar** (digits and `%`), best to worst: DejaVu Bold 11 (v35), DejaVu Regular 12 (v35), the compiled-in misc-fixed 7x13 bold, Terminus 12, Liberation Bold (v35), Noto.
- **For the 36 px rows** the compiled-in 10x20, 9x15 bold and 7x14 bold beat every hinted TrueType: they are larger and hand-drawn. Among the TrueType candidates DejaVu 12 (v35) at 1x is best.
- **What the compiled-in pixel fonts get wrong** (`--style crisp` today): the colon of 7x13 bold sits one pixel low, so the clock looks dropped at the colon; `m`, `M` and `W` are blobby in 7x13 bold and 7x14 bold; 7x14 bold draws `0` and `O` identically (the bar only prints digits and `%`).

## 5. Decision

1. **Keep the compiled-in X11 misc-fixed fonts as the default of the crisp style.** They are public domain (each BDF header carries the `COPYRIGHT` line and the generator refuses a font without one), take about 10.2 KB, have no dependency, and are exactly pixel-aligned. This is what is implemented.
2. **If the bar text should look like WinCE's, bake DejaVu Sans Bold 11 px (v35, mono) offline into const bitmaps** for the bar and keep the pixel fonts for the rows. The panel then needs no FreeType at run time. The alternative, linking FreeType (optional dependency) and rasterising about 70 glyphs per face once at start-up into 1-bit masks, costs a dependency and FreeType's resident memory for the same per-frame cost [est]; FreeType is already on the board for fontconfig.
3. **Licence of the bake.** DejaVu's own changes are public domain, but the fonts derive from Bitstream Vera: "Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is a trademark of Bitstream, Inc." The Vera licence requires the copyright and trademark notices and the permission notice in all copies of the typefaces, allows modified fonts only if renamed to names without "Bitstream" or "Vera", and does not allow selling a typeface by itself. Bitmaps derived from DejaVu would need the notice shipped with them and, if distributed as a font, the rename. This is a reading of the licence text, not legal advice.
4. **What was built** (`--crisp-font dejavu`, `panel/fonts/gen-dejavu-bitmaps.c`, `panel/panel-pixfont-dejavu.c`). The bake covers the rows too, not only the bar: Bold 11 for the bar and the slider value, and Bold 12, 11, 10 and 9 for the text rows, as the size ladder the fit logic steps down. The rows are Bold and not Regular because, in [host] renders on the dark theme, Regular 12 has 1 px stems and looks thin in the 36 px row while Bold 12 has the weight of the compiled-in bold fonts; the longest date takes 222 of the 224 px of the text area in Bold 12 and fits the largest face, where misc-fixed falls to 7x14 bold. The bar's colon is centred (misc-fixed 7x13 bold's sits low), `m`, `W` and `%` are legible at 11 and 12 px. Against that, the DejaVu rows are smaller than 10x20 bold in the 36 px row (digits 9 px high against 13), the bar's digits are 8 px high against 9 (4 rows above and 5 below in the 17 of the bar), and the proportional widths move the battery and the buttons 7 px left. The default stays `fixed` because both looks were only seen on a host.

## 6. Where text time really goes, and what is worth accelerating

- **Bytes first.** Before havoc's per-cell damage a typed character sent the whole frame through the bus-limited copy path (7.9 to 9.1 MB/s [meas]); with it, one character damages 1,440 bytes at 80x24 cells of 10x18 pixels [meas, in a container; `oe/recipes-graphics/havoc/README.md`]. That is a far bigger win than any faster glyph blit.
- **pixman on this CPU.** On ARMv5TE pixman runs only its portable C paths, and havoc does not use pixman for its glyphs [read: pixman and havoc sources]. Patching pixman would help the compositor's blends, not terminal text.
- **Order of work, by expected payoff** [est]: per-cell damage (done) first; then opaque cell blits and a flat glyph array (no blending when the background is a solid colour); then a 1-bit fast path (bi-level text needs no blend at all); a pre-blended cell cache last.
- **The engine's mono expansion** is the same path WinCE used for its default text. Evidence from the MediaQ revival work: the MQ1132 has no alpha blending [doc], and the h2200's MQ1188 driver shows none [decomp]. On the h2200 under Linux, solid fills of 240x224 ran at 185 MB/s and VRAM-to-VRAM copies of 240x112 at 135 MB/s against about 8 MB/s for host writes (7.84 MB/s for the CPU in the revival measurement; [mediaq-c8.md](mediaq-c8.md) section 3.1 quotes the same figures) [meas]. **Not run under Linux, so unverified:** host-to-screen blits through the source FIFO, the mono expansion, an off-screen mono source, colour key. A cell of 6x11 pixels is 66 bits, 3 words, about 8 bus writes per glyph on the same bus that limits CPU copies [inf]; the engine wins only if a register write on that 16-bit bus costs about 1 microsecond or less, which nobody has measured. Needs a driver-side path (DRM has no 2D interface), bounded waits and a read-cache setting [inf]. Test it before designing around it.

## 7. Open questions

- Does the engine mono expansion work on the MQ1188 under Linux, and at what cost per glyph? (Section 6.)
- How does the hinted bi-level DejaVu bar look on the real panel, compared with misc-fixed? Only host renders exist; the choice between `--crisp-font fixed` and `dejavu` as the default waits for it.
- Which look is preferred for the rows: the large hand-drawn pixel fonts or hinted DejaVu Bold at 12 px?

## 8. Idea, not built: pixel-aligned fonts in havoc

**Status:** recorded, nothing implemented. Source read: havoc at commit 73e3467149 (`glyph.c`, `main.c`, `font_scale`) [read].

- **Today.** havoc has its own rasteriser derived from stb_truetype in `glyph.c`. `get_glyph(id, codepoint, cwidth)` returns an 8-bit coverage bitmap the size of one cell (width x cwidth, height), cached per glyph; `main.c` blends the foreground and background colours with that coverage. `font_scale(size)` scales the outline, so the text is unhinted and anti-aliased, and the cell size follows the configured `size` [read].
- **The idea.** A bitmap-font backend behind the same `get_glyph` interface that returns only 0 or 255. Cells then contain exactly the two theme colours, no blending, no fringes, which is the crisp look the panel has. Cell sizes become fixed by the font (6x10, 6x12, 6x13, 7x13 and so on), so `size` picks the nearest font and the zoom keys step through them. Wide (CJK) cells fall back to the current rendering at first.
- **Font source.** The X11 misc-fixed fonts: public domain, made for terminals (xterm's classic default is 6x13). The panel's copies are cut to printable ASCII; a terminal also needs Latin-1, Greek, Cyrillic and box drawing. That the full BDFs cover them, and how large they are as const data, is [unverified]; check before choosing sizes. Hinted DejaVu Sans Mono was only "fine but wide" in the prototype (section 4) [host].
- **Speed.** Two colours turn a cell into a 1-bit lookup, the fast path of section 6, and a solid-background cell paint needs no blend [est].
- **Delivery.** A new havoc patch 0003 (the series is 0001 text-input, 0002 per-cell damage), behind a config switch so the current look stays the default. Tests: the damage and golden harness in `oe/recipes-graphics/havoc/tests` does not depend on the font; add a check that every pixel of every painted cell is one of two colours, as the panel's crisp end-to-end test does.
- **Open:** which sizes to ship; how `size` maps to a font; whether bold and italic are needed (not checked in havoc); the look at 10 px or so on the real panel, which is hard-edged and old-school.

