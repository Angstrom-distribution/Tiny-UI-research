# Picowl Cursor Requirements and Configuration

Reference for the implemented cursor support: the rules every cursor frame must meet, what picowl draws (the tap-and-hold animation and the `wait` and `progress` cursor shapes), the `[cursor]` and `[touch]` keys that control it, the strip format for custom animations, and the `picowl-cursor-convert` tool. The README has a short summary in its [cursor section](../README.md#cursor) and the full key list in its [configuration section](../README.md#configuration). The rules in "Platform Cursor Rules" are enforced in `src/cursorfit.c`; the statements about the mq11xx kernel driver, the MediaQ hardware, wlroots 0.19 and the other boards come from the kernel port and wlroots 0.19.0, not from this repository, and are not verified here.

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
  - Fully transparent pixels (alpha `0x00000000`) or fully opaque (alpha `0xff`); no translucency
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

Picowl displays a cursor **only during the tap-and-hold animation**, and while the client with pointer focus asks for a `wait` or `progress` shape (next section). No cursor is drawn otherwise.

### Client Cursor Shapes

`wp_cursor_shape_manager_v1` lets a client name a cursor instead of attaching an image. picowl has no cursor theme, so the shape is mapped to what it can draw (`src/cursorshape.c`):

| Shape | Result |
|-------|--------|
| `wait`, `progress` | the hold animation, centred on the cursor position |
| every other shape, including `default` | no image (picowl draws no pointer) |

Requests from clients without pointer focus are dropped; a tablet tool request is heard from the client whose surface the tool is in. The animation a client started stops when the client asks for any other shape or when the pointer focus changes. A client wait request does not replace a running animation, and a tap-and-hold animation is never stopped by a client, even one that fired while a client's own wait animation was running (the touch takes it over); the touch state machine alone ends it. Image cursors set through `wl_pointer.set_cursor` remain ignored.

### Configuration Keys

All cursor configuration lives in the `[cursor]` section of the INI config file (first found of `$XDG_CONFIG_HOME/picowl/picowl.ini`, `~/.config/picowl/picowl.ini`, `/etc/picowl.ini`). Unknown keys in `[cursor]` are silently ignored. An invalid colour logs an error (`Invalid color format: ... (use #RRGGBB)`) and keeps the default. `frame_interval_ms` is read with `atoi`; a value outside `20..1000` logs an error and uses 83. `hold_animation` is not checked when the config is read, only when picowl loads the file at startup.

| Key | Default | Range/Format | Meaning |
|-----|---------|--------------|---------|
| `hold_animation` | `builtin` (unset) | `builtin` or file path | Animation to display during hold. `builtin` = Pocket PC 2003 rotating circle of circles. File path = PAM or PPM strip of frames. |
| `fill` | `#2050c0` | `#RRGGBB` | Body colour of every dot in the builtin animation. Compared at 6 bits per component. |
| `outline` | `#ffffff` | `#RRGGBB` | 1-pixel ring around the lead dot and its two trail dots in the builtin animation. Compared at 6 bits per component. |
| `frame_interval_ms` | `83` | `20..1000` | Frame duration in milliseconds (~12 fps at default). |

### Related Touch Configuration

Tap-and-hold timing is configured in the `[touch]` section. `hold_action` and `hold_button` with an unknown value log an error and keep the default. `hold_delay_ms`, `hold_ms` and `slop_px` are read with `atoi`. If `hold_ms` is not greater than `hold_delay_ms`, an error is logged and both return to their defaults (300 and 900). A `slop_px` outside `0..64` logs an error and returns to 8.

| Key | Default | Meaning |
|-----|---------|---------|
| `hold_action` | `right-click` | `right-click` (send BTN_RIGHT) or `none` (no hold detection) |
| `hold_button` | `right` | `right` (BTN_RIGHT) or `middle` (BTN_MIDDLE, pastes the primary selection in a terminal); unused with `hold_action = none` |
| `hold_delay_ms` | `300` | Milliseconds before the animation starts |
| `hold_ms` | `900` | Milliseconds from touch-down to send the click |
| `slop_px` | `8` | Movement tolerance in pixels, `0..64`; exceeding this cancels the hold |

### Per-App Hold Overrides

You can configure different tap-and-hold behaviour for specific apps and layer-shell surfaces using `[app.<app_id>]` and `[layer.<namespace>]` sections:

```ini
[app.mediaplayer]
hold_action = none

# built in: picowl ships this rule for the on-screen keyboard (wvkbd) and for the panel
[layer.wvkbd]
hold_action = none
```

picowl ships `hold_action = none` for the layer namespaces `wvkbd` and `panel`: the keyboard times its own long presses and the panel's sliders are dragged, so a hold must not delay their presses or turn a slow drag into a right click. A `[layer.wvkbd]` or `[layer.panel]` section of your own merges over these. All hold keys from the table above are available (the `[app.<app_id>]` sections take further, unrelated keys, see the [README configuration section](../README.md#configuration)). Unset keys inherit from `[touch]`, wherever `[touch]` appears in the file. If a rule's timings are bad (`hold_ms` not greater than `hold_delay_ms`, or `slop_px` outside 0..64), an error is logged and its three timing keys revert to the `[touch]` values; its `hold_action` and `hold_button` are kept. The rule is chosen at touch-down from the surface under the finger and stays fixed until the finger lifts.

`hold_action = none` is useful for apps that time their own long press (e.g. media players): the left press is sent at touch-down, motion is forwarded, and there is no right-click and no hold animation. `hold_delay_ms` has no effect in that mode.

## Custom Cursor Format: PAM Strips

A custom cursor is a horizontal strip of square frames in PAM or PPM format.

### File Format

- **PAM:** `P7` magic, `TUPLTYPE RGB_ALPHA` or `RGB`, maxval 255 (8-bit channels)
- **PPM:** `P6` binary format, maxval 255
- **Frame Layout:** Frames are side-by-side horizontally; frame width = strip height (square frames)
- **Hotspot:** Always the centre of the (possibly cropped) frame; the PAM strip stores no hotspot and it cannot be overridden
- **Maximum:** 64 frames per strip. picowl rejects a strip whose height is over 256 pixels, whose width is not a multiple of its height, or that has more than 64 frames (bad strip geometry, builtin fallback). A frame larger than 64×64 is centre-cropped to 64×64 by the fitting step (see "Runtime Compliance Checking"). A 64×64 frame strip with 64 frames is 4096 pixels wide.

### Example: Creating a Strip

A 32×32 pixel 8-frame animation as a horizontal strip would be 256 pixels wide (8 × 32) and 32 pixels tall.

### Non-Compliant Handling

If a loaded `hold_animation` file is non-compliant:
- Picowl automatically converts it at load time using the cursorfit library
- A line starting `cursor: warning:` is logged, at error level, describing what was fixed
- If the file cannot be read or is unusable after conversion, picowl falls back to the builtin animation

## Converting Cursor Images: picowl-cursor-convert

The `picowl-cursor-convert` tool helps create or validate platform-compliant cursor strips. It is built and installed with picowl unless meson is run with `-Dtools=false` (the default is `true`).

### Command-Line Syntax

```
picowl-cursor-convert [--frame-width N] [--fg #rrggbb] [--bg #rrggbb]
                      [--premultiplied] [--hotspot X,Y] in.pam|ppm out.pam

picowl-cursor-convert --check [--frame-width N] in.pam|ppm

picowl-cursor-convert --export-builtin [--fg #rrggbb] [--bg #rrggbb] out.pam
```

### Options

- `--check`: Validate only; do not write output. Exits 0 if compliant, 1 if non-compliant. Prints one line per frame: `frame N: compliant` or `frame N: non-compliant: <reason>`.
- `--export-builtin`: Write the built-in Pocket PC 2003 animation as a PAM strip to `out.pam` (8 frames of 32×32, so 256×32). Useful as a template for custom animations. Here `--fg` is the fill colour and `--bg` the outline colour (defaults `#2050c0` and `#ffffff`, the `[cursor]` defaults).
- `--frame-width N`: Width of each frame in pixels. Default (0) = frame width equals strip height (square frames).
- `--fg #rrggbb`: Force the first of the two output colours (the lead colour). Giving `--fg` or `--bg` switches on colour forcing; without either, the two most frequent colours of each frame are kept. The value must be `#` and six hex digits, else the tool exits 2.
- `--bg #rrggbb`: Force the second of the two output colours. Without `--fg`, the first colour defaults to `#2050c0` and the second to `#ffffff` when only one of the two is given.
- `--premultiplied`: Input is premultiplied alpha (divide RGB by alpha before processing).
- `--hotspot X,Y`: Accepted but ignored: no hotspot is stored in the PAM strip and picowl always uses the frame centre.
- `--help`, `-h`: Print the usage to stderr and exit 2.

### Exit Codes

- `0`: Success (compliant or successfully converted)
- `1`: Input non-compliant in `--check` mode (output not created)
- `2`: Usage error or I/O failure (file not found, cannot write, invalid arguments)

### Output Report

When writing `out.pam`, picowl-cursor-convert prints one `frame N: WxH ...` line per frame, either `(already compliant)` or the counters `alpha_clipped`, `colours_merged`, `cropped_to_WxH`, `remapped_pixels`, `input_colours`, then `wrote <out>: N frames`. Each fitted frame is placed centred in a square cell of the size of frame 0, so the output strip stays square-celled. There is no separate warning output.

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
     - Colour reduction: Keep the two most-frequent 6-bit-quantised colours (if only one dominates, the second is chosen by splitting on luminance); map all opaque pixels to the nearer one in RGB distance
3. **Logging:** If any frame was fixed, log a warning (at error level) with the changes made
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
