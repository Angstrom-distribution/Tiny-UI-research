# Picowl Cursor Requirements and Configuration

## Platform Cursor Rules

Every cursor frame displayed by picowl must comply with hardware cursor constraints to avoid frame commit failures on targets with cursor-plane support.

### Why These Rules Exist

**MediaQ MQ1132/MQ1188 Hardware Cursor:**
- These 2 bpp AND/XOR bitmap cursors are limited to exactly two colour registers
- The hardware applies a bitwise AND mask followed by an XOR operation with colour registers
- This physically enforces the 2-colour per-frame limit at the register level

**mq11xx DRM Cursor Plane:**
- The mq11xx kernel driver validating cursor buffers before commit enforces:
  - ARGB8888 format (4 bytes per pixel)
  - Size 1..64 pixels in both dimensions
  - Fully transparent pixels (alpha `0x00000000`) or fully opaque (alpha `0xff`) — no translucency
  - At most 2 distinct opaque colours per frame (compared at 6 bits per component, i.e. `colour >> 2`)
- Violations are rejected with `-EINVAL`, which fails the entire output commit and drops the frame

**wlroots 0.19 Behaviour:**
- wlroots does not test-commit the cursor image before the next output update
- A non-compliant image will not be caught until the next frame render
- This means a bad cursor causes sudden frame drops rather than graceful fallback

**Software Cursor Fallback Boards:**
- Target boards without a cursor plane (W3220, PXA LCDC, SA-1110) use the wlroots software cursor
- The same rules apply to keep compositing cost low on CPU-constrained devices
- Software drawing of large, multi-coloured cursors on top of animated content is expensive

### Compliance Checklist

Every picowl cursor frame must satisfy:

1. **Format:** ARGB8888 (32-bit pixels, stride = width × 4 bytes)
2. **Size:** 1..64 pixels wide, 1..64 pixels tall
3. **Transparency:** Every pixel is either:
   - Fully transparent: exactly `0x00000000` (all 32 bits zero)
   - Fully opaque: alpha = `0xff` (any RGB)
4. **Colour Limit:** At most 2 distinct opaque colours per frame
   - Colours are compared at 6 bits per channel: `(R >> 2, G >> 2, B >> 2)`
   - e.g., `#2050c0` and `#2353c3` differ only in the low 2 bits of each channel → same colour

## Picowl Cursor Behaviour

Picowl displays a cursor **only during the tap-and-hold animation**. No cursor is drawn otherwise.

### Configuration Keys

All cursor configuration lives in the `[cursor]` section of the INI config file (first found of `$XDG_CONFIG_HOME/picowl/picowl.ini`, `~/.config/picowl/picowl.ini`, `/etc/picowl.ini`):

| Key | Default | Range/Format | Meaning |
|-----|---------|--------------|---------|
| `hold_animation` | `builtin` | `builtin` or file path | Animation to display during hold. `builtin` = Pocket PC 2003 rotating circle of circles. File path = PAM or PPM strip of frames. |
| `fill` | `#2050c0` | `#RRGGBB` | Body colour of every dot in the builtin animation. Compared at 6 bits per component. |
| `outline` | `#ffffff` | `#RRGGBB` | 1-pixel ring around the lead dot and its two trail dots in the builtin animation. Compared at 6 bits per component. |
| `frame_interval_ms` | `83` | `20..1000` | Frame duration in milliseconds (~12 fps at default). |

### Related Touch Configuration

Tap-and-hold timing is configured in the `[touch]` section:

| Key | Default | Meaning |
|-----|---------|---------|
| `hold_action` | `right-click` | `right-click` (send BTN_RIGHT) or `none` (no hold detection) |
| `hold_delay_ms` | `300` | Milliseconds before the animation starts |
| `hold_ms` | `900` | Milliseconds from touch-down to send the right-click |
| `slop_px` | `8` | Movement tolerance in pixels; exceeding this cancels the hold |

### Per-App Hold Overrides

You can configure different tap-and-hold behaviour for specific apps and layer-shell surfaces using `[app.<app_id>]` and `[layer.<namespace>]` sections:

```ini
[app.mediaplayer]
hold_action = none
hold_delay_ms = 0

[layer.osk]
hold_delay_ms = 500
```

All keys from the table above are available. Unset keys inherit from the global `[touch]` configuration. This is useful for apps that need immediate input without the hold delay animation (e.g., media players that handle their own long-press logic).

## Custom Cursor Format: PAM Strips

A custom cursor is a horizontal strip of square frames in PAM or PPM format.

### File Format

- **PAM:** `P7` magic, `TUPLTYPE RGB_ALPHA` or `RGB`, maxval 255 (8-bit channels)
- **PPM:** `P6` binary format, maxval 255
- **Frame Layout:** Frames are side-by-side horizontally; frame width = strip height (square frames)
- **Hotspot:** Always the centre of the (possibly cropped) frame; the PAM strip stores no hotspot and it cannot be overridden
- **Maximum:** 64 frames per strip (width ≤ 4096 pixels for a 64×64 frame)

### Example: Creating a Strip

A 32×32 pixel 8-frame animation as a horizontal strip would be 256 pixels wide (8 × 32) and 32 pixels tall.

### Non-Compliant Handling

If a loaded `hold_animation` file is non-compliant:
- Picowl automatically converts it at load time using the cursorfit library
- A warning is logged describing what was fixed
- If the file cannot be read or is unusable after conversion, picowl falls back to the builtin animation

## Converting Cursor Images: picowl-cursor-convert

The `picowl-cursor-convert` tool helps create or validate platform-compliant cursor strips. It is part of the picowl build.

### Command-Line Syntax

```
picowl-cursor-convert [--frame-width N] [--fg #rrggbb] [--bg #rrggbb] 
                      [--premultiplied] [--hotspot X,Y] in.pam|ppm out.pam

picowl-cursor-convert --check [--frame-width N] in.pam|ppm

picowl-cursor-convert --export-builtin [--fg #rrggbb] [--bg #rrggbb] out.pam
```

### Options

- `--check`: Validate only; do not write output. Exits 0 if compliant, 1 if non-compliant. Prints one line per frame explaining any violation.
- `--export-builtin`: Write the built-in Pocket PC 2003 animation as a PAM strip to `out.pam`. Useful as a template for custom animations.
- `--frame-width N`: Width of each frame in pixels. Default (0) = frame width equals strip height (square frames).
- `--fg #rrggbb`: Force the first colour (foreground/lead colour in a 2-colour palette) as `#RRGGBB` hex.
- `--bg #rrggbb`: Force the second colour (background/other colour in a 2-colour palette) as `#RRGGBB` hex.
- `--premultiplied`: Input is premultiplied alpha (divide RGB by alpha before processing).
- `--hotspot X,Y`: Accepted but currently ignored: no hotspot is stored in the PAM strip and picowl always uses the frame centre.

### Exit Codes

- `0`: Success (compliant or successfully converted)
- `1`: Input non-compliant in `--check` mode (output not created)
- `2`: Usage error or I/O failure (file not found, cannot write, invalid arguments)

### Output Report

When writing `out.pam`, picowl-cursor-convert prints one `frame N: WxH ...` line per frame, either `(already compliant)` or the counters `alpha_clipped`, `colours_merged`, `cropped_to_WxH`, `remapped_pixels`, `input_colours`, then `wrote <out>: N frames`. There is no separate warning output.

### Examples

#### Validate a PNG-to-PAM conversion

Convert PNG to PAM, then validate:

```sh
# Using netpbm:
pngtopam -alphapam mycursor.png mycursor.pam
picowl-cursor-convert --check mycursor.pam

# Using ImageMagick:
convert mycursor.png pam:mycursor.pam
picowl-cursor-convert --check mycursor.pam
```

#### Convert with colour forcing

Ensure the animation uses specific colours:

```sh
picowl-cursor-convert --fg '#ff0000' --bg '#00ff00' myanimation.pam fixed.pam
```

#### Export the built-in animation as a template

Customise the built-in animation:

```sh
picowl-cursor-convert --export-builtin my-builtin-copy.pam
# Edit my-builtin-copy.pam with an image editor, then:
picowl-cursor-convert my-builtin-copy.pam my-custom-cursor.pam
```

#### Handle premultiplied alpha

Some tools produce premultiplied alpha; convert and un-premultiply:

```sh
picowl-cursor-convert --premultiplied --fg '#2050c0' --bg '#ffffff' premult.pam fixed.pam
```

## Converting Images: netpbm and ImageMagick

Picowl does not link libpng. Use standard image tools to convert PNG to PAM:

### netpbm

```sh
# PNG to PAM with separate alpha channel (recommended):
pngtopam -alphapam image.png image.pam

# PPM (if you prefer, but alpha is lost):
pngtoppm image.png image.ppm
```

### ImageMagick

```sh
# PNG to PAM with alpha:
convert image.png pam:image.pam

# Or with explicit RGBA:
magick image.png -alpha on -define pam:format=rgb_alpha -compress none PAM:image.pam
```

## Runtime Compliance Checking

At startup, picowl loads the configured `hold_animation` file:

1. **File Read:** Attempts to read the PAM or PPM strip using cursorfit's file parser
2. **Frame Extraction:** For each frame in the strip:
   - Extract the square frame region
   - Check compliance against the platform rules
   - If non-compliant, run cursorfit's fitting algorithm:
     - Alpha threshold: alpha ≥ 128 → opaque (0xff), else → transparent (0x00000000)
     - Resize: Centre-crop to 64×64 if needed; adjust hotspot
     - Colour reduction: Keep the two most-frequent 6-bit-quantised colours; map all opaque pixels to the nearest one
3. **Logging:** If any frame was fixed, log a warning with the changes made
4. **Fallback:** If the file is unreadable or no frames can be extracted, fall back to the builtin animation

## Built-in Animation: Pocket PC 2003 Rotating Circle

The default `hold_animation = builtin` renders a 32×32 animation inspired by the classic Pocket PC 2003 wait cursor.

**Design:**
- 8 frames
- 8 small dots arranged on a ring
- The lead dot (radius 4) and the two dots trailing it (radius 3) are filled with `fill` and ringed with a 1-pixel `outline`
- The other five dots are radius-2 discs in `fill` only
- The lead advances one ring position per frame
- Rendered with integer arithmetic only (no FPU required)
- Colours: `fill` (body of every dot) and `outline` (ring on the lead and trail dots)

**Configuration:**
- `fill`: Body colour of all dots (default `#2050c0`, a medium blue)
- `outline`: 1-pixel ring colour of the lead and trail dots (default `#ffffff`, white)

**Frame Interval:**
- Controlled by `frame_interval_ms` (default 83 ms, ~12 fps)

The animation uses a precomputed integer sine/cosine table for positioning the 8 ring positions, avoiding any floating-point arithmetic and keeping CPU cost low on constrained targets.

## Troubleshooting

### A custom cursor looks wrong or a warning is logged

Picowl checks every frame of a `hold_animation` file and auto-fits non-compliant ones at load, so a non-compliant frame never reaches the cursor plane (the builtin frames are checked too). The log line is:

`cursor: warning: <path> frame N non-compliant (<why>); fixed: alpha=.. colours=.. (merged ..) remapped=.. [cropped]`

**Solution:**
1. Run `picowl-cursor-convert --check myanimation.pam` to see which frames violate the rules
2. Convert with `picowl-cursor-convert myanimation.pam fixed.pam`
3. Update your config to point to the fixed file

### Too many colours in my custom animation

Your animation uses more than 2 colours per frame.

**Solution:**
1. Redesign the animation to use at most 2 opaque colours per frame, or
2. Run `picowl-cursor-convert --fg '#colour1' --bg '#colour2' input.pam output.pam` to force specific colours (quote the colours: an unquoted `#` starts a shell comment)

### Animation looks distorted after conversion

The tool had to crop, threshold alpha, or remap colours significantly.

**Solution:**
1. Check the tool's output report: `picowl-cursor-convert myanimation.pam fixed.pam`
2. Redesign the source to be closer to the constraints (smaller, fewer colours, sharper alpha transitions)

### Animation plays jerky or stutters

The device is CPU-constrained.

**Solution:**
1. Increase `frame_interval_ms` to reduce frame rate
2. Use a smaller animation (e.g., 24×24 instead of 64×64)
3. Reduce the number of frames

### Picowl fell back to the builtin animation

The configured file could not be read or was unusable.

**Symptoms:**
- Log lines: `cursor: cannot read <path>: <reason>`, or `cursor: <path>: bad strip geometry WxH ...`, or `cursor: <path>: no frames`, followed by `cursor: falling back to builtin animation`
- Builtin animation is displayed instead

**Solution:**
1. Check that the file path is correct and readable
2. Run `picowl-cursor-convert --check` on the file
3. If it has errors, convert it with `picowl-cursor-convert input.pam fixed.pam`
4. Update the config path
