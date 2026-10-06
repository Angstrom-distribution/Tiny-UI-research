# iPAQ display facts: size, density, stripe order, modes, and what picowl does with them

**Status:** reference, last updated 2026-10-06. The compositor and panel behaviour of section 4 is implemented: `[output] size_mm` in `src/config.c` and `src/output.c`, the density rule in `panel/panel-logic.c` (`pl_density_ppi`, `pl_look_resolve`), which accepts 60 to 400 ppi and otherwise falls back by mode class. It is documented in [README.md#configuration](../../README.md#configuration) and [README.md#panel](../../README.md#panel). The kernel side (section 3) is not verifiable from this repository. Sizes and stripe orders marked [user] were stated by the project owner and are not measured by any tool in this repository; [src] means read in the committed kernel source of the unified mainline port (nothing here was queried on a running board). Where two sources disagree the table says so.

What is scheduled from this document is tracked in the [roadmap](roadmap.md).

## 1. Evidence tags

| Tag | Meaning |
|---|---|
| **[user]** | stated by the project owner; the hx4700 size is approximate and is to be measured |
| **[src]** | read in the kernel source (the `mq11xx`, `w100`, `pxa-lcdc` and `sa1100-lcdc` DRM drivers and the iPAQ panel drivers of the unified mainline port); not measured on a board |
| **[inf]** | inference from the above, with the arithmetic shown |
| **[?]** | not known |

## 2. The panels

| Model family | Resolution | Panel size | Density | Stripe order | Notes |
|---|---|---|---|---|---|
| h2200 | 240x320 portrait | 53 x 71 mm in the driver [src] (the nominal 3.5 in panel, no datasheet value recorded) | about 115 ppi [inf] | horizontal RGB [user: every iPAQ but the h39xx] | QVGA, MediaQ MQ1188 |
| h3xxx (h3800, h3900 and relatives) | 240x320 glass | 57 x 77 mm [user] | about 106 ppi [inf] | RGB along the panel's native scan [user]; the **h39xx scans 320x240 landscape** and is vertical RGB in portrait use | SA-1100 / PXA250 LCD controller |
| h5xxx (h5550) | 240x320 portrait | 57 x 77 mm [user], the same panel as the h3xxx | about 106 ppi [inf] | horizontal RGB [user] | MediaQ MQ1132, two panel tables (Sharp and Philips) |
| hx4700 | 480x640 portrait | about 60 x 80 mm [user, approximate, to be measured] | about 203 ppi [inf] | horizontal RGB [user] | ATI W3220, XScale PXA270 |

Density is the pixel diagonal over the millimetre diagonal: 240x320 over 57x77 mm is 400 px over 95.8 mm, 106 ppi; over 53x71 mm it is 115 ppi; 480x640 over 60x80 mm is 800 px over 100 mm, 203 ppi.

## 3. What the kernel drivers report today

The compositor passes the DRM connector's physical size (`mmWidth`, `mmHeight`) on as `wl_output` `physical_width` and `physical_height`, and the connector's subpixel order as `wl_output` `subpixel`.

| Driver | Size reported | Subpixel | Modes | Notes |
|---|---|---|---|---|
| `mq11xx`, h2200 | 53 x 71 mm [src] | unknown [src] | one, 240x320 | the size is hard-coded in the driver's mode table, not read from the device tree |
| `mq11xx`, h5550 | 57 x 77 mm in the new source [src]; the older source said 53 x 71 mm, which was wrong for the h5xxx [user] | unknown | one, 240x320 (Sharp or Philips table chosen at probe) | the new values are source only until a board runs that image |
| h3800, h3900 panel drivers | 77 x 57 mm, in the 320x240 landscape scan frame, in the new source [src]; 0 before | unknown | one, 320x240 | the glass is 57 x 77 mm portrait; the h3900 DTS declares it mounted a quarter turn (panel orientation RIGHT_UP) [src] |
| `w100`, hx4700 | 60 x 80 mm for **both** modes in the new source [src, provisional]; 0 before | unknown | two: 480x640 and 240x320 | because both modes carry the same size, mode resolution over size gives the density of the mode in use |
| `pxa-lcdc`, `sa1100-lcdc` | see the panel drivers above | unknown | single mode | |

No driver sets the subpixel order, and no source in the tree records the stripe direction of any iPAQ panel. Until a driver or a default does, `[output] subpixel` has to be set per board in `picowl.ini`.

Other facts about the connectors:
- The MQ1188's pixel doubling is a plane scaling feature (a 120x160 framebuffer on the primary plane covers the screen 2x): the connector still reports a single 240x320 mode with the native size, so mode and millimetres always give the native density [src].
- No EDID exists on any of these panels. `fbdev` emulation copies the connector's millimetres into `var.width` and `var.height` [src].
- The kernel changes above (h5xxx table values, h3xxx panel sizes, hx4700 sizes) were made after the project owner gave the sizes. They are source only: no boot image contains them and no board has run them.

## 4. What picowl and picowl-panel do with this

- **Subpixel layout.** `[output] subpixel = unknown|none|horizontal_rgb|horizontal_bgr|vertical_rgb|vertical_bgr` is the layout of the panel in its **native** orientation, as the protocol defines it; picowl derives what a client sees from it and the transform. For every iPAQ it is `horizontal_rgb`. On the h39xx the native frame is the landscape scan, so `horizontal_rgb` with a 90 degree transform reaches clients as vertical RGB. The panel's LCD text handles only horizontal stripes, so an h39xx in portrait gets no LCD text; QVGA uses the crisp style anyway, which draws none.
- **Style and bar height from the density, at run time.** `picowl-panel --style auto` (the default) computes the density from the current `wl_output` mode and physical size, as a diagonal ratio so that rotation and a landscape scan frame do not matter: below 150 ppi it draws crisp text, from 150 ppi smooth text with the subpixel rule. `--height auto` is 18 px for crisp and scales with the density for smooth (about 34 px at 203 ppi). It re-evaluates when the mode or the output changes. Overrides: `--style crisp|smooth`, `--height N`, `--dpi N`, and in `picowl.ini` `[output] size_mm = WxH` (all outputs) or `NAME.size_mm = WxH` to say what `wl_output` reports for a board whose kernel reports 0 mm or an old wrong size. A density outside 60 to 400 ppi, from the compositor or from `size_mm`, is not believed (a typo such as `5x7` would give about 1180 ppi); `--dpi` is the way to go outside that range. With no size, or none that is believed, the mode class stands in: a long side of 640 or more counts as 200 ppi, anything smaller as 110 ppi. The 150 ppi threshold sits between the QVGA panels (106 to 115 ppi) and the hx4700 (about 203 ppi); the 7 percent disagreement between 53 x 71 and 57 x 77 mm does not move any panel across it.
- **hx4700 modes.** The 480x640 mode comes out at about 203 ppi (smooth) and the 240x320 mode at about 101 ppi (crisp), by the same rule.

## 5. Open items

- The hx4700's true size (to be measured) and whether the h2200 panel is the same glass as the h3xxx and h5xxx (the source says 53 x 71 mm for the h2200; the project owner has not said).
- The stripe direction of the individual panels is recorded nowhere in the tree; the rule "RGB in the native frame for every iPAQ" is the project owner's statement.
- Whether the kernel should set `subpixel_order` itself (a driver change, the project owner's decision) or picowl should default to `horizontal_rgb` when the backend reports unknown. Today it is a per-board config key.
