# Toolkits

[← index](README.md) · design rules: [hardware.md](hardware.md) · compositor: [compositor.md](compositor.md)

## 1. GTK+ 2 Wayland backend

### 1.1 Why it's tractable
- GDK2 already has non-X backends; DirectFB is the closest model.
- GTK 2.18's client-side windows remove the subwindow problem.
- GTK3's `gdk/wayland` serves as the protocol reference (LGPL).
- Every toplevel is full-screen at (0,0), so root coordinates are true. That satisfies GTK2's assumptions about absolute positions.

Upstream never planned a GTK2 Wayland backend, and Xwayland is not an option at 64 MiB.

### 1.2 Work list (base: 2.24.33)
1. **Core.**
   - One fixed screen per output, xdg-shell toplevels, a 16 bpp `GdkVisual`.
   - Render straight into the compositor-allocated RGB565 buffer ([compositor.md §5.3](compositor.md#53-buffer-model-the-zero-copy-path)). The window's cairo surface *is* the buffer, and GDK's double-buffer pixmap is disabled.
   - Single buffer with release-on-flip on copy-type boards.
   - Frame callbacks, and `damage_buffer` per expose.
2. **Input.**
   - xkbcommon keysyms map to GDK keyvals almost one-to-one (both are X keysyms).
   - Pointer only: touch arrives as pointer from the compositor.
   - Handle keymap changes when input switches between wvkbd and the hardware keys [check].
3. **Popups.**
   - Menus, combos and tooltips become client-side child windows of the full-screen toplevel, not `xdg_popup`s, and grabs stay in-process.
   - This keeps the screen to one surface, so direct scanout survives an open menu.
   - `xdg_popup` is the fallback for toplevels that aren't full-screen.
4. **Clipboard** via `wl_data_device`. DnD later; GPE barely uses it.
5. **Text input.** The compositor's `virtual-keyboard-v1` needs no client support. A text-input-v3 immodule is optional, only for preedit and auto-show. (Under Weston it would have to be text-input-v1.)
6. **Smaller items.**
   - Titles and app_id for the switcher; xdg-activation.
   - Stub `GtkPlug`/`GtkSocket`.
   - Scrolling uses CSW's in-buffer move. That is still a full-area upload, because no "moved" hint reaches the driver.

### 1.3 GPE's other X dependencies

| X dependency | Replacement |
|---|---|
| matchbox WM | the compositor |
| matchbox panel / XEmbed tray | GTK2 layer-shell panel (a small private hook in the GDK2 backend) |
| XSettings | RC file / env |
| XRandR rotation | output transform |
| screensaver / DPMS | compositor idle |
| gpe-dm | compositor session |

**Font size:** derive it from measured DPI at session start. The h2200 keeps `Sans 7` at ~114 DPI; the hx4700's Sans-5 patch must not be applied blindly (`h2200.md` §7).

### 1.4 Build-system decision
GTK+ 2.24 is autotools, but everything we write uses meson. The options are:
- **(a)** Accept autotools for that one tree.
- **(b)** Build the backend out of tree with meson, if GDK2's backend selection allows it [check].
- **(c)** Port GTK+ 2.24's build to meson: large but mechanical.

Decide before backend work starts.

## 2. LVGL

**Fit:** integer-only RGB565 renderer. About 0.4–0.8 MiB of code and 0.5–2 MiB of heap for a PIM UI [est].

**DRM driver patch (~150 lines):**
- Send damage clips built from the flushed areas.
- Add a single-buffer mode for copy-type drivers.
- Rotation: MediaQ plane rotation where available, software rotation of damaged areas elsewhere.
- Check that the plane it picks is the primary.

On the h3970 the stock driver works, apart from rotation.

**Zero-code experiment:** the fbdev driver with `LV_LINUX_FBDEV_MMAP=n` writes with `pwrite()`. That probably skips the 20 Hz deferred-I/O delay [check on 7.2].

**As a Wayland client:**
- The shm client works today, with RGB565, per-rectangle damage and xdg-shell.
- To fit the zero-copy model, use one buffer in DIRECT mode, rendered into the compositor-allocated buffer. That drops its two-buffer sync copy.

**Floats:** none in the software renderer's hot paths. Keep `LV_USE_FLOAT`, vector graphics, ThorVG and Lottie off. ARMv5 asm can be hooked in through `LV_DRAW_SW_ASM_CUSTOM`.

**Gaps:**
- evdev calibration is linear only; libinput applies the udev calibration matrix.
- No virtualised list (use `table`).
- The calendar is a month grid only.
- `textarea` has no clipboard.
- No HarfBuzz.

**App models:**
- One process hosting all apps on bare DRM: best for the h3870.
- LVGL apps as Wayland clients beside GTK2.
- One process per app with a DRM-master handover: this re-invents a compositor.

**Build:** LVGL builds with CMake but installs `lvgl.pc`, so our apps use meson's `dependency('lvgl')`. The meta-oe `lvgl_9.5.0.bb` recipe needs a bbappend for RGB565, because its DRM fragment selects 32-bit colour.

Source details: §B.

## 3. EFL / Evas / Enlightenment: rejected

### 16-bit engine: don't revive it
- It was removed on 2012-09-24 (`bb4ee2174c`). The last release with it was 1.7.x.
- It was about 8.2k lines, with 5-bit alpha, and had no map, smooth scaling or gradients.
- Evas has since added threaded rendering, masks, Ector, filters and data map/unmap, all 32-bit only.
- Reviving it would take about 4–8k lines and 2–4 person-months, double the image memory, and have no upstream.

### The middle path that already exists
- Render 32-bit, then convert to RGB565 with dithering and built-in rotation (`evas_convert_rgb_16.c`), damaged tiles only.

### drm engine
- Needs three fixes:
  - RGB565 colour masks (it currently draws nothing);
  - real `FB_DAMAGE_CLIPS` (DirtyFB is called before render, with wrong maths);
  - plane rotation (`#if 0`).
- `fb` works at RGB565 but tears.
- `wayland_shm` supports 8888 only.

### Enlightenment
- It inherits those drm-engine bugs.
- ARGB/XRGB8888 shm clients only, and no KMS planes.
- No mobile shell (Illume was removed 2013–15).
- Doesn't fit in about 50 MiB.

### Other costs
- Edje uses `double` throughout, so it is soft-float bound.
- `native-arch-optimization` forces NEON unless disabled.

Source details: §A.

---

## A. Source notes: Evas / EFL / Enlightenment

**Sources.**
- EFL git at HEAD `e303767c34` (`v-1.27.0-340`, 2026-09-20).
- Enlightenment git at HEAD `ef0af3735` (2026-07-08).
- "OLD" means `bb4ee2174c^`, the last tree that still had the 16-bit engines.

Claims marked UNVERIFIED were not checked in code or history. Nothing was built, so sizes and build failures are estimates.

### Verdicts

- **16-bit engine.** Added 2007-04-29 (`16df9e0047`), removed 2012-09-24 (`bb4ee2174c`, "and remove 16bit engines/code as planned."). The last release with it was Evas 1.7.x. It was about 8.2k lines and very limited: no map, no smooth scaling, no gradients, and only 5-bit alpha.
- **Don't revive it.** Evas internals have grown a lot since the removal:
  - the engine function table went from 127 to 173 entries;
  - there is now a threaded render queue;
  - masks, Ector, filters and data map/unmap all assume 32-bit pixels.
- **The middle path already exists.** Render in 32-bit and convert to dithered RGB565, rotating at the same time, in `evas_convert_rgb_16.c`.
- **The drm engine is enabled on master.** The "Disable building evas drm engine" commit (`1c369dbef6`, 2022) is only on the unmerged `devs/devilhorns/apos` branch, which re-enabled drm in 2025.
- **The master drm engine has three blocking problems on these boards. All are small, well-scoped fixes, and the damage fix matters most.**
  1. RGB565 output is never drawn (black screen), because the colour masks are hard-coded for 32-bit.
  2. Damage is sent only through `drmModeDirtyFB`, which is called before rendering and gets the clip maths wrong. The atomic flips carry no `FB_DAMAGE_CLIPS`, so mq11xx and w100 do a full-frame upload on every flip.
  3. Plane rotation is `#if 0`'d out, the cursor plane is unused, and planes are never scaled.
- **fb engine.** Works with RGB565 today, single-buffered, with tearing.
- **wayland_shm.** XRGB8888/ARGB8888 only.
- **Enlightenment.** Runs without GL: its `wl_drm` module falls back to the software "drm" engine, so it inherits the drm bugs above. Beyond that:
  - clients can only send ARGB/XRGB8888 shm buffers;
  - E makes no direct use of KMS planes;
  - the `wl_fb` module is disabled;
  - Illume, the mobile shell, is gone.

  Too heavy for about 50 MiB of RAM.
- **Build.** You need `-Dnative-arch-optimization=false`; otherwise meson forces `-mfpu=neon` on any 32-bit ARM. Pixel loops are integer/fixed-point, but Edje layout and animation use `double` throughout, and the fixed-point build flag can't be set from meson. Elementary pulls in almost all of EFL.

### 1. History of the 16-bit software engines

| Event | Commit | Date / author |
|---|---|---|
| Added: "add in the work I did for a 16bit engine - for now, a dormant project until i can be convinced it provides real and significant speedups that warrant the significant effort" | `16df9e0047` | 2007-04-29, raster |
| Main development: unscaled ops, XShm, fonts, ecore_evas support, premultiplied rework, lines and polygons, cache | `f70b4e9dcf`, `0cf249a46f`, `59d7929e73`, `dfcde9bcaa`, `e12c298619`, `55f6c5f046` | 2007-06 to 2007-10, Gustavo Barbieri |
| DirectDraw 16 engine added | `2898660d8a` | 2007, Vincent Torri |
| DirectDraw 16 engine removed ("Windows users (>= XP) use only 32 bits depth color") | `e68d4430d9` | 2012-06-19, Vincent Torri |
| 16-bit engines no longer built by default | `0667809063` | raster |
| **Main removal**: `common_16`, software_16, software_16_sdl, software_16_wince (35 files, −7402 lines) | **`bb4ee2174c`** | 2012-09-24, raster (SVN r77030) |
| Spec file entries removed | `8f1c793565` | 2012-09-24 |
| software_16_x11 removed again, after being re-added by mistake (−945 lines) | `f91826870d` | 2012-10-10 |
| ecore_evas 16bpp support removed (−1631 lines) | `890e35cb11` | 2012-10-15 |

**Why it was removed.** The commit message gives no reason beyond "as planned". The plan isn't in git; it was probably discussed on e-devel (UNVERIFIED).

**Last release.** When it was removed, `legacy/evas/NEWS` already listed Evas 1.7.0 as released, and tag v1.8.0 (`cd60144d87`, 2013-12-01) has no soft16 files. So the last release was Evas 1.7.x; the exact point release is UNVERIFIED.

**Size at OLD.**

| Part | Lines |
|---|---|
| `common_16` | 3354 |
| software_16 | 438 |
| software_16_sdl | 1487 |
| software_16_wince (GAPI, DDraw, GDI, raw fb) | 1888 |
| software_16_x11 | 944 |
| `evas_common_soft16.h` | 100 |
| **Total** | **about 8.2k** |

**What it implemented** (`evas_common_soft16.h` at OLD).
- `Soft16_Image` stored RGB565 pixels as `DATA16 *pixels`, plus an optional `DATA8 *alpha` plane right after them (h:35-55). Stride was padded to 4 pixels (h:27-33).
- The colourspace was `EVAS_COLORSPACE_RGB565_A5P` (software_16/evas_engine.c:97). Alpha was stored in 8 bits but blended in 5: the blend macros shift by 5 (h:17-20), and rectangles use `A_VAL(...) >> 3` (evas_soft16_rectangle.c:62).
- R, G and B were blended in a single 32-bit operation using the "unpacked" 0x07e0f81f trick.
- Images were converted to 565 once, at load, with a 128x128 ordered dither (evas_soft16_dither_mask.c:3-9; convert functions at :216 and :282).
- Primitives:
  - rectangles, lines and polygons;
  - images, unscaled or sampled-scaled only, with no smooth scaling (evas_soft16_main.c:463-465);
  - fonts, through glyph hooks (software_16/evas_engine.c:295-310);
  - `pld` prefetch.
- It inherited software_generic and overrode only the rect, line, poly, image and font functions (evas_engine.c:343-372).

**What it lacked.**
- Map/transform: `image_map_draw` is an empty function in software_16_sdl (evas_engine.c:845-847).
- `colorspace_set`, `native_set` and `format_get` are `NOT_IMPLEMENTED()` (evas_engine.c:89-116).
- Gradients, smooth scaling, YUV, and surfaces with alpha.

**Known problems.**
- Alpha banding from the 5-bit alpha.
- Already-dithered images got re-scaled.
- `image_data_get` exposed engine internals to applications ("FIXME: That's bad, the application must be aware of the engine internal", evas_soft16_main.c ~243).
- Output rotation went through a temporary buffer and `_blit_rot_90/180` (software_16_x11/evas_engine.c:283-382).

### 2. Cost of resurrecting it, and the middle path

The engine function table (`Evas_Func`) grew from 127 pointers (OLD `evas_private.h`) to 173 (src/lib/evas/include/evas_private.h:870). A revived engine would have to adapt to:

- **Engine/output split** (`engine_new`, `output_setup`): `28397e7206` and `1e2bbf8fea`, 2017-08-25.
- **Threaded render queue** (`9b2b121e6f`, 2012-12-18, after the removal). software_generic's draw calls take `do_async` (22 uses) and queue commands that hold image references.
- **Image masks** (`context_clip_image_*`, `a9630a77b6`, 2014-11-13).
- **Ector vector rendering** (`f2380b0920`, 2015-04). Software Ector buffers are ARGB8888 or GRY8 only (ector_software_buffer.c:94-95, 152-154).
- **Filters** (`0740010a06`, 2017-01). They require ARGB8888 or alpha buffers (evas_filter.c:290-297).
- **Image data map/unmap** (`326ff9ae82`, 2016-03).
- **Everything else image-related:** slices, stretch regions, animated images, orientation, map surfaces, proxies and planes all assume 32-bit RGBA images. The image cache is still recognisable; soft16 had its own cache instance.

**Effort (judgement, UNVERIFIED).** A "soft16 v2" would override only the hot leaf operations on 565 surfaces and fall back to 32-bit for everything else: roughly 4-8k lines and 2-4 person-months. The hard parts:
- Every fallback (map, masks, filters, Ector, proxies, alpha surfaces) needs 565↔8888 conversion or a 32-bit offscreen buffer.
- Image lifetime rules under the threaded render queue.
- Keeping both 565 and 8888 copies in the cache doubles image memory, which defeats the point on 64 MiB.
- With 5-bit alpha and sampled-only scaling, Edje themes look visibly worse.
- Upstream removed the engine on purpose and nobody maintains it.

**Middle path (recommended).** Keep 32-bit rendering and output dithered RGB565. It already exists:
- `src/lib/evas/common/evas_convert_rgb_16.c` (1843 lines) has `..._to_16bpp_rgb_565_dith` with `_rot_90/180/270` variants (lines 13-487), using 128x128, 4x4 or line dithering (`6dfb74c54a`).
- `evas_common_convert_func_get` picks them when depth is 16 and the masks are 0xf800/0x07e0/0x001f (evas_convert_main.c:190-222). 8bpp palette converters exist too (:164-185).
- Only damaged tiles are rendered in 32-bit and then converted (and rotated) into the scanout buffer. Rotation is therefore nearly free, and no full 32-bit frame sits in RAM.

### 3. Current 32-bit software outputs

#### drm (evas `drm` + ecore_evas `drm` + Ecore_Drm2)

**Status.**
- Enabled on master (src/modules/evas/engines/meson.build:19; src/modules/ecore_evas/meson.build:3). Requires `-Ddrm=true`, which defaults to false (meson_options.txt:43-47).
- Recent fixes on master by Cedric Bail, 2026-08: `ce34c0c5d7`, `2fb70593c1`, `ddd08757a2`, `8225859bb6`, `b798c474e9`.
- `1c369dbef6` (2022-08-16, Christopher Michael: "For now we will disable building these as the ecore_drm2 API/ABI is about to become majorly broken") exists **only on `origin/devs/devilhorns/apos`**. It is not an ancestor of master.
- That branch:
  - rewrote and re-enabled the engines in 2025-08 (`b4508926e9`, `99e3ed2cc1`, `5b83d4eada`, `e36362225a`); GL drm stays disabled (`f4ff060cda`);
  - adds threaded atomic state, IN_FORMATS parsing (`25b2c76331`), and AddFB2WithModifiers;
  - stores the `FB_DAMAGE_CLIPS` property id (`38110cfc16`);
  - is 199 commits ahead of master (head `12fc707095`, 2026-02-20), unmerged, and breaks the API.

**Buffers.**
- Dumb buffers: CREATE_DUMB, MAP_DUMB, mmap (ecore_drm2_fb.c:42-80). AddFB2 is tried first, with legacy AddFB(depth, bpp) as fallback (:54-63).
- Up to 10 buffers. The oldest free buffer is reused, and buffers idle for 100 frames are trimmed (evas_outbuf.c:8-9, 48-91).
- Buffer age drives copy, double, triple or quadruple partial redraw (evas_outbuf.c:303-320).
- On pxa-lcdc, 2-3 buffers fit the 1 MiB CMA pool.

**Pixel format.**
- Depth and bpp come from `DRM_CAP_DUMB_PREFERRED_DEPTH`. The default is 24, giving 32 bpp; a depth of 16 or less gives 16 bpp (ecore_drm2_device.c:894-919).
- The format is hard-coded to `DRM_FORMAT_XRGB8888` (ecore_evas_drm.c:1034).
- **RGB565 is broken.**
  - `_outbuf_update_region_push` always passes masks 0xff0000/0x00ff00/0x0000ff (evas_outbuf.c:3-6, 391-394).
  - At 16 bpp no converter matches (evas_convert_main.c:190-280), so it returns NULL (:445) and the draw returns early (`if (!func) return;`, evas_outbuf.c:405). Nothing is drawn.
  - With preferred depth 16, the legacy AddFB(16,16) fallback does produce an RGB565 framebuffer, but it stays black. With depth 24, an RGB565-only plane rejects the framebuffer.
  - Fix, about 20 lines: derive the masks and format from bpp (UNVERIFIED on hardware).

**Damage.**
- The path is `_outbuf_damage_region_set` → `ecore_drm2_fb_dirty` → `drmModeDirtyFB` (evas_outbuf.c:567-591; ecore_drm2_fb.c:214-239). It has three problems:
  1. It is called **before** rendering (from software_generic/evas_engine.c:4180) and on the back buffer.
  2. The clip maths are wrong: `clip[i].x2 = rects[i].w; clip[i].y2 = rects[i].h` (fb.c:231-232) should be x+w and y+h.
  3. The atomic commit carries no `FB_DAMAGE_CLIPS` (fb.c:373-437 adds only CRTC, FB, source and destination properties).
- Atomic is the default (`_ecore_drm2_use_atomic = EINA_TRUE`, device.c:13); `ECORE_DRM2_ATOMIC_DISABLE` turns it off.
- Result: every flip counts as full damage. mq11xx and w100 upload a full frame (17-19 ms and 22-25 ms) on every frame, however little Evas redrew.

**Planes and rotation.**
- `ecore_drm2_plane_assign` (plane.c:41-139) can pick a cursor plane (if the framebuffer size equals the cursor capability), an overlay ("no size checks") or the primary plane. The loop doesn't break, so the last matching plane wins.
- Source size always equals destination size (:106-114). `ecore_drm2_plane_destination_set` (:165-176) exists, but the engine never calls it.
- The cursor plane is unused: the cursor size is queried (ecore_evas_drm.c:164), but the cursor is drawn as an Evas object.
- The `rotation` property is parsed (device.c:467-493), but applying it is `#if 0`'d out ("Disable hardware plane rotation for now as this has broken recently...") in both `ecore_drm2_output_rotation_set` (outputs.c:1601-1650) and `_fb_atomic_flip_test` (fb.c:439-456).
- Rotation is done in software, in the convert pass (evas_outbuf.c:389-488; `7198fe568b`, `9efac538cc`).

**Vblank.** The animator runs from page-flip events, plus `drmWaitVBlank`/blanktime when vblank is supported (ecore_evas_drm.c:713-821, 1051-1060).

#### fb engine
- Still present (src/modules/evas/engines/fb, `-Dfb=true`, defaults to false).
- Builds its masks from the fbdev `fb_var` fields, so the dithered 565 converter is picked correctly. Supports 12/15/16/32 bpp and rotation (outbuf.c:22-29, 61-71, 183-215).
- Writes straight into the mmapped framebuffer: no back buffer (outbuf.c:143-144), no vsync, no damage ioctl. Damage relies on the kernel's deferred I/O for fbdev emulation (page-granular; UNVERIFIED on these drivers).
- Works with RGB565 today, with tearing.

#### wayland_shm
- `WL_SHM_FORMAT_ARGB8888/XRGB8888` only (ecore_wl2_buffer.c:683-685).
- Supports buffer age (wayland_shm/evas_outbuf.c:213-223) and per-rectangle `wl_surface_damage_buffer` (ecore_wl2_window.c:1746-1765).

### 4. Enlightenment as a software Wayland compositor
- **Backend.** The `wl_drm` module is E's KMS output backend, not the old wl_drm protocol, and it is still built (src/modules/meson.build:69).
  - It tries `gl_drm` only when the compositor engine is GL. Otherwise it calls `ecore_evas_new("drm")` with accel forced to "none" (wl_drm/e_mod_main.c:956-978).
  - The default engine is software (`E_COMP_ENGINE_SW`, e_comp_cfdata.c:73).
  - So E **runs without GL**, carrying every EFL drm problem from §3.
- `wl_fb` is disabled (`### XXX: disabled for now #  'wl_fb'`, modules/meson.build:73-74).
- **Client buffers.**
  - Only the default shm formats, ARGB/XRGB8888 (`wl_display_init_shm`, e_comp_wl.c:2841).
  - The pixmap code only tells ARGB from non-ARGB (e_pixmap.c:629-650).
  - linux-dmabuf formats come from EGL queries (e_comp_wl_dmabuf.c:509-512), so there are none without GL.
- **KMS planes.** E never calls `ecore_drm2_plane_*` itself; planes are reachable only through Evas `image_plane_assign` for dmabuf clients.
- **Rotation.** E's call to `ecore_drm2_output_rotation_set` (e_mod_main.c:740) does nothing, so rotation is done in software via `ecore_evas_rotation_with_resize_set`.
- **Mobile shell.**
  - Illume was disabled on 2013-08-20 (`1be76d599`, "illume 100% requires X, thus illume is dead") and removed on 2015-03-18 (`bc087bd6f`); its configs went in 2016 (`b2138ad5a`).
  - Only the `vkbd` and `convertible` modules remain, so there is no Wayland mobile shell.
- **Memory.** E is itself a full Elementary app. Its RAM use (UNVERIFIED) rules it out on 64 MiB boards.

### 5. Soft-float, SIMD and footprint

**SIMD.**
- `native-arch-optimization` defaults to true (meson_options.txt:343-347). For `cpu_family()=='arm'` it adds `-mfpu=neon -ftree-vectorize` and `BUILD_NEON` to the whole project (meson.build:192-197).
- `evas_scale_smooth.c:4-5` then includes `<arm_neon.h>`. GCC is expected to reject that on a soft-float ABI (UNVERIFIED, not built). The `*_neon.c` blend and copy operations also contain NEON inline asm.
- **Use `-Dnative-arch-optimization=false`.**
- There is no iWMMXt code (`grep -i iwmmxt` finds nothing).
- `pld` is emitted only for `__ARM_ARCH__ >= 52` (evas_common_private.h:304-311): fine on PXA, compiled out on SA-1110.

**Floating point.**
- No floats in the per-pixel hot paths:
  - the per-pixel map renderer uses fixed point (`typedef int FPc`, evas_common_private.h:396, 858-862);
  - `evas_map_image.c` has only two float declarations, both in perspective setup (:201-202);
  - smooth and sampled scaling and font drawing use no float or double at all.
- Float and double show up per object or per frame instead:
  - `evas_map.c`: 47 hits;
  - textblock layout: 53;
  - **Edje uses `double`**: fixed-point `BUILD_EDJE_FP` (Eina_F32p32) exists in edje_private.h:140-166, but meson never sets it;
  - Ecore timers and animators use double.
- So animated Edje UIs will be soft-float-bound.

**Footprint.**
- Elementary hard-depends on eina, eo, efl, eet, emile, ecore, ecore-evas, ecore-file, ecore-input, ecore-imf, ecore-con, edje (which needs Lua), ethumb-client, emotion, eldbus, efreet and eio (meson.build:362).
- Source size: elementary 220k lines, evas 133k, edje 68k, eina 56k, ecore_con 29k, ecore 26k; about 650k lines in total.
- Estimated install size: 15-25 MiB of stripped ARM shared libraries, modules and the default theme (UNVERIFIED).
- Meson options that shrink it:
  - **Outputs:** `x11=false`, only one of `drm`/`fb`/`wl`, `opengl=none`.
  - **Media:** `audio=false`, `pulseaudio=false`, `pipewire=false`, `gstreamer=false`, `v4l2=false`, `edje-sound-and-video=false`.
  - **System integration:** `physics=false`, `avahi=false`, `systemd=false`, `libmount=false`, `network-backend=none`.
  - **Text:** `hyphen=false`; `harfbuzz`, `fribidi` and `fontconfig` can be turned off.
  - **Loaders:** `evas-loaders-disabler` (keep only png, jpeg and eet), `ecore-imf-loaders-disabler`.
  - **Build extras:** `bindings=[]`, `build-tests=false`, `build-examples=false`, `nls=false`, `install-eo-files=false`, `eina-magic-debug=false`, `elua=false`.
  - **Arch and packaging:** `native-arch-optimization=false`, `efl-one` (one shared library), `embedded-lz4`, `embedded-libunibreak`.
  - `eeze` is required for drm.

### 6. Using the MediaQ and W3220 features

| Feature | EFL today | What has to be written |
|---|---|---|
| **FB_DAMAGE_CLIPS** (the most important item on these buses) | Only a buggy DirtyFB call (fb.c:214-239) | A damage blob on every atomic commit, built from the post-render rectangles; fix x2/y2 |
| **RGB565 primary plane** | Dithered 565 converters exist | Pick masks and format from bpp in the drm outbuf (about 20 lines) |
| **mq11xx 64x64 cursor plane** | The plane picker handles cursor planes (plane.c:60-72); the cursor capability is queried | Draw the cursor into a cursor-sized dumb buffer, assign the plane, move it on motion. Cursor format is unknown. On a touchscreen, hiding the cursor is simplest |
| **Plane `rotation`** | Property and supported rotations are parsed (device.c:467-493); applying them is `#if 0` | Re-enable, fix `rotation_map`, include the property in every commit, and set engine rotation to 0 when the hardware rotates. sa1100 and pxa keep software rotation in the convert pass |
| **2x plane scaling** | `ecore_drm2_plane_destination_set` (plane.c:165-176) | An ecore_evas mode with a half-size canvas (source 120x160, destination 240x320, full screen only), plus Elementary scale 0.5 |
| **C8 with a gamma-LUT palette** | Dithered 8bpp palette converters (evas_convert_main.c:164-185) | Map to `DRM_FORMAT_C8`, load the palette into the gamma LUT, choose the palette mode. Gives tear-free flips and half the bandwidth |
| **W3220 YUV overlay** | Evas `image_plane_assign` puts `WL_DMABUF` native surfaces on an overlay (drm/evas_engine.c:151-194; evas_render.c:3212-3240) | Multi-planar import (bpp is hard-coded to 32, evas_engine.c:111-114), 1-4x scaling (source always equals destination today), colour-key properties, a dmabuf allocator for clients (these drivers have no render node), and E advertising YUV formats without EGL. For a single non-Wayland app: an emotion/gstreamer sink writing into an overlay dumb buffer (new code) |
| **Vblank events** | The animator runs from page flips and `drmWaitVBlank` (ecore_evas_drm.c:713-821, 1051) | Nothing; check that `ecore_drm2_vblank_supported` works with the DRM vblank timer |

Source trees: [Enlightenment/efl](https://github.com/Enlightenment/efl) and [Enlightenment/enlightenment](https://github.com/Enlightenment/enlightenment) (GitHub mirrors).

## B. Source notes: LVGL

**Sources read:**
- lvgl master `389dbdee` (2026-10-02, version header 10.0-dev). Line numbers below refer to it, and match v9.6.0 for the DRM driver.
- Tag **v9.6.0** (2026-09-16), the latest release. Between it and master, `lv_linux_drm.c`, `lv_evdev.c` and `lv_libinput.c` differ only in header moves. fbdev and Wayland have small edits.
- `lv_port_linux` at `5ae67b9`.
- meta-openembedded master at `ca3cf5f` (2026-10-01).

Anything marked "UNVERIFIED" is either inference or kernel-side behaviour I did not check in the source.

### Verdicts

1. **Fits the CPU and RAM budget.** The RGB565 software renderer is integer-only, fills are fast, and the footprint is small.
2. **The stock DRM driver is wrong for shadow-plane drivers.** It does a full-frame commit every frame, sends no damage clips, and cannot rotate. Plan on a patch of about 150 lines (damage clips, a single-buffer mode, plane or software rotation) before using it on the h2210, h5550, hx4700 or h3870. On the h3970 (pxa-lcdc) it works as is, except for rotation.
3. **The Wayland shared-memory client is usable today.** It supports RGB565, per-rectangle damage and xdg-shell. wlroots carries the damage on down to KMS.
4. **Hardware features:**
   - Small driver patches: C8 (as L8 greyscale), mq11xx plane rotation, 2x scaling.
   - Medium new code: W3220 overlay video.
   - Not viable: a MediaQ draw unit, because of the shadow plane.
5. **App model.** On bare DRM, run one process that hosts all apps. If you want isolation or mixed toolkits, run LVGL apps as Wayland clients. Handing DRM master between processes needs new suspend/resume code plus libseat.

### 1. Display

#### 1a. DRM driver (`src/drivers/display/drm/lv_linux_drm.c`, 1218 lines)

The driver has three backends: dumb buffers (the default, confusingly named "FBDEV" in Kconfig), GBM and EGL (`drm/Kconfig`). Only dumb buffers matter here.

**Buffer allocation**
- Always **2 full-screen dumb buffers** (`BUFFER_CNT 2`, :43).
- Each buffer goes through `DRM_IOCTL_MODE_CREATE_DUMB` with bpp = `LV_COLOR_DEPTH`, then `MAP_DUMB`, `mmap`, a zero fill and `drmModeAddFB2` (:869-925, :1013-1041).

**Pixel format**
- `LV_COLOR_DEPTH` 16 maps to `DRM_FORMAT_RGB565`, and 32 to XRGB8888.
- Any other depth hits `#error LV_COLOR_DEPTH not supported` (:34-41). There is no C8 or L8 output.
- The GBM path refuses anything but 32 bpp (:939).
- `LV_COLOR_DEPTH` is now derived from `LV_COLOR_FORMAT_DEFAULT`, whose default is RGB565 (`lv_conf_template.h:142`, `lv_conf_internal.h:160-180, 5001-5016`).

**Render mode: DIRECT**
- Set via `lv_display_set_buffers_with_stride(..., LV_DISPLAY_RENDER_MODE_DIRECT)` with both dumb buffers (:235-236).
- LVGL renders only the invalidated areas, at their real positions in the full-screen buffer.
- With two buffers, the core records each frame's areas (`core/lv_refr.c:430-442`). Before the next frame it memcpy's them into the other buffer (`refr_sync_areas`, `lv_refr.c:426, 673-785`). That is an extra CPU copy of all damage on every frame.

**Modesetting**
- **Atomic only.** If `drmSetClientCap(DRM_CLIENT_CAP_ATOMIC)` fails, init fails; there is no legacy fallback (:760-764).
- The first commit adds `CRTC_ID`, `MODE_ID`, `ACTIVE` and `ALLOW_MODESET`. The "first commit" flag is a function-level `static int first`, shared by all displays and never reset (:445, :461-471).
- The mode is always `conn->modes[0]` (:610-619). `lv_linux_drm_set_mode_cb` does nothing but print a warning on the dumb-buffer path (:252-257).

**Page flip and sync**
- Each frame does `drmModeAtomicCommit(NONBLOCK | PAGE_FLIP_EVENT)` with plane `FB_ID`, `CRTC_ID`, `SRC_X/Y/W/H` and `CRTC_X/Y/W/H`. Source size equals CRTC size equals the mode size (:473-484).
- `drm_flush_wait` polls the fd and calls `drmHandleEvent` until the flip handler frees the request (:1043-1064, :307-320).
- So there is one flip per LVGL frame, paced by the flip event. LVGL's refresh timer (`LV_DEF_REFR_PERIOD 33`, template :153) is not tied to vblank.

**Damage: none**
- There is no `FB_DAMAGE_CLIPS` and no `DRM_IOCTL_MODE_DIRTYFB` (grep finds 0 hits).
- `drm_flush` returns early unless `lv_display_flush_is_last()`, and it explicitly marks `area` and `px_map` unused (:1066-1083).

**Planes**
- `find_plane()` returns the first plane whose `possible_crtcs` and format list match. **It does not check the plane type** (:494-549). The atomic client cap implies universal planes, so a cursor plane that lists RGB565 could in principle be picked.
- No cursor plane and no second plane are ever used.

**Rotation**
- There is no KMS `rotation` property and no software rotation in the flush.
- `lv_display_set_rotation()` only stores the rotation and swaps the logical resolution (`display/lv_display.c:1106-1116`).
- The only DIRECT-mode rotation in the core is `lv_display_set_matrix_rotation`. It requires `LV_DRAW_TRANSFORM_USE_MATRIX` (`lv_display.c:1128-1141`), and `LV_USE_MATRIX` selects `LV_USE_FLOAT` (`src/draw/Kconfig:117-125`).
- Matrix transforms are implemented only by the VG-Lite and NanoVG renderers. The files that use `LV_DRAW_TRANSFORM_USE_MATRIX` live under `draw/vg_lite` and `draw/nanovg`; none are in `draw/sw`.
- **So rotation does not work today with DRM and the software renderer.**

**Session handling**
- There is no drop or set master, no VT switching and no hotplug handling.
- `saved_crtc` is never filled, so the restore on delete (:1094-1101) is dead code.
- The public API (`lv_linux_drm.h:58-130`) has no getter for the fd.

**History** (`git log -- src/drivers/display/drm`):
- `fa305054f` (2025-01-29): GBM buffers.
- `35ecac5c0` (2025-10-06): mode selection, EGL only.
- `d67aaab16` (2026-01-19): stride for draw buffers.
- `9f4e962f5` (2026-09-03): libdrm fallback for device probing.

No commit touches damage or rotation.

**What this means for each driver**
- **mq11xx and w100 (shadow plane with damage clips):** a commit without damage clips is treated as a full-plane update. That is standard `drm_atomic_helper_damage_iter_init` behaviour (UNVERIFIED for these drivers). So **every LVGL frame becomes a full VRAM copy**: 17-19 ms with the bus stalled on the MediaQ boards, 22-25 ms on the W3220, even when only a text cursor blinks. The second buffer and `refr_sync_areas` add RAM traffic for no gain.
- **sa1100-lcdc (persistent scanout buffer plus damage memcpy):** a full 150 KiB memcpy every frame, and it tears.
- **pxa-lcdc (scans GEM buffers directly):** matches the stock design. Two 150 KiB dumb buffers fit the 1 MiB CMA pool, and page flips are real. Only rotation is missing.

**Patches needed (each small, roughly 150 lines in total)**
1. **Damage clips.**
   - In DIRECT mode the flush callback runs once per joined invalid area, and `flush_is_last` marks the final one (`lv_refr.c:858-863, 1406-1446`).
   - Collect those areas into a `struct drm_mode_rect[]`, call `drmModeCreatePropertyBlob`, and add `FB_DAMAGE_CLIPS` to the commit.
   - With two synced buffers, "areas drawn this frame" is the correct damage relative to what VRAM already holds.
   - Watch `LV_INV_BUF_SIZE`: if the invalidated areas overflow it, LVGL falls back to a full-screen invalidate (`lv_refr.c:330-339`).
2. **Single-buffer mode for shadow-plane drivers.**
   - Use one dumb buffer and re-commit the same `FB_ID` with damage clips, or use `drmModeDirtyFB` if the driver's framebuffer implements `dirty` (UNVERIFIED).
   - This drops the sync copy and saves 150 KiB of RAM (600 KiB at VGA).
   - `flush_wait` already blocks until the commit completes, so rendering does not overlap the kernel's copy.
3. **Plane rotation on mq11xx.** Allocate the framebuffer with swapped dimensions, set `SRC_*` to the framebuffer size and `CRTC_*` to the mode size, and add the plane `rotation` value (`DRM_MODE_ROTATE_90/270`) if the plane has that property.
4. **Software rotation for sa1100, pxa and w100.**
   - Port the fbdev driver's rotate-in-flush, preferably in PARTIAL mode so that only damaged rectangles are rotated.
   - `lv_draw_rotate` walks columns naively (`draw/lv_draw_utils.c:463-521`), which is hostile to the SA-1110's 8 KiB D-cache.
   - It can be replaced through the `LV_DRAW_ROTATE90_RGB565` / `LV_DRAW_ROTATE270_RGB565` hooks with `LV_USE_DRAW_SW_ASM = LV_DRAW_SW_ASM_CUSTOM`.
   - Several ms per full 320x240 frame on the SA-1110 is an estimate (UNVERIFIED).
5. **Hygiene.** Require `DRM_PLANE_TYPE_PRIMARY` in `find_plane`, make the first-commit flag per device, fill `saved_crtc`, and add `lv_linux_drm_get_fd()`.

#### 1b. fbdev driver (`src/drivers/display/fb/lv_linux_fbdev.c`)

**Rendering and output**
- The default render mode is PARTIAL (`fb/Kconfig`). DIRECT and FULL are optional, with 1 or 2 buffers.
- The flush copies row by row, with `lv_memcpy` into the mmap'd framebuffer or with **`pwrite()` when `LV_LINUX_FBDEV_MMAP=n`** (`write_to_fb` :382-391; loops :543-580).
- There is no explicit damage ioctl; damage is implicit in which bytes get written.
- Options: `FBIO_WAITFORVSYNC` once per frame (:434-441), software rotation per flushed area or of the whole frame in DIRECT mode (:459-507), an R/B swap (:525-541), and `force_refresh` via `FBIOPUT_VSCREENINFO` (:582-587).

**On DRM's fbdev emulation**
- The mmap path goes through deferred I/O: page-granular tracking, a delay of HZ/20 (the 20 Hz cap), and damage rounded to full-width line bands.
- **The `pwrite` path probably avoids that delay.** In fbdev-shmem the write helper calls the damage-range helper, which schedules the damage worker immediately (`FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS`, `drm_fb_helper_damage_range`). UNVERIFIED against the 7.2 tree.
- That makes it a zero-code experiment: `LV_LINUX_FBDEV_MMAP=n` in PARTIAL mode, with damage arriving as row bands. The patched DRM driver is still the better end state: exact rectangles, plane rotation and flip events.

### 2. Input

#### evdev (`src/drivers/evdev/lv_evdev.c`)

**Device handling**
- Handles absolute pointers (`ABS_X/Y`, multitouch slots, `ABS_MT_TRACKING_ID`, `BTN_TOUCH`/`BTN_MOUSE`), relative mice and keys (:152-310).
- Single-touch resistive panels such as ads7846 and resistive-adc-touch go through the `ABS_X/Y` + `BTN_TOUCH` path.
- Devices are discovered with inotify on `/dev/input`, and the type is detected automatically (:393-510, :512-616).

**Calibration**
- Linear only: per-axis min/max scaled to the display (`_evdev_calibrate` :119-123, `_evdev_process_pointer` :125-144).
- min/max come from `EVIOCGABS` at creation. `lv_evdev_set_calibration(min_x, min_y, max_x, max_y)` (:700) overrides them, and min > max inverts an axis. `lv_evdev_set_swap_axes` (:692) swaps X and Y.
- There is no affine or 7-point tslib matrix, no tslib backend in the tree (grep finds none), and no pressure threshold or jitter filter.
- Rely on kernel filtering and the DT `touchscreen-*` properties, or wrap `read_cb` with your own affine transform (about 20 lines).
- The core rotates pointer coordinates to match the display rotation (`indev/lv_indev.c:756-765`).

**Keys**
- `KEY_UP/DOWN/LEFT/RIGHT/ESC/DELETE/BACKSPACE/ENTER/TAB/NEXT/PREVIOUS/HOME/END` map to `LV_KEY_*`. Any other key arrives as `code + 0xFFFF` (:86-117).
- A gpio-keys d-pad therefore works as `LV_INDEV_TYPE_KEYPAD` with `lv_group` focus navigation. The app has to handle the application buttons itself (KEY_F*, KEY_PROG*, KEY_CALENDAR and so on).

**Polling**
- By default devices are read every 33 ms by a timer.
- For battery life:
  - Open the fd yourself and use `lv_evdev_create_fd()`.
  - Set `lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT)` (`lv_indev.c:595-610`).
  - Poll the fd in your own loop and call `lv_indev_read()` when it is readable.
- The display refresh timer already pauses itself when nothing is invalid (`lv_refr.c:390-397`; resumed at `lv_display.c:1725-1735`).

#### libinput (`src/drivers/libinput/lv_libinput.c`)
- Touch uses `libinput_event_touch_get_x_transformed` (:497-512). libinput therefore applies the udev `LIBINPUT_CALIBRATION_MATRIX`, which gives **proper affine calibration** for resistive panels. That comes from libinput; LVGL itself has no calibration API here.
- It runs one pthread per device (:210-218) with a path context (:167).
- Dependencies: libinput, libudev, libevdev and mtdev, plus xkbcommon with `LV_LIBINPUT_XKB`.

#### On-screen keyboard
- `lv_keyboard` is a buttonmatrix with modes TEXT_LOWER/UPPER, SPECIAL, NUMBER, TEXT_ARABIC and USER_1..4.
- Custom maps go through `lv_keyboard_set_map` and popovers through `lv_keyboard_set_popovers` (`lv_keyboard.h:91, 102`).
- It works with a stylus. The default layout covers about 40-50% of a 240x320 screen (estimate), so a PDA needs a compact custom map and a small font.
- There is no handwriting input (Graffiti or xstroke style) and no word prediction. `LV_USE_IME_PINYIN` exists.

### 3. Wayland client (`src/drivers/wayland/`)

**Backends and formats**
- Backends: SHM (the default), DMA-BUF via gbm, EGL, and NXP G2D (`wayland/Kconfig`).
- LVGL depths 8 and 1 are rejected with `#error` (`lv_wayland.c:13-15`).
- In SHM, `LV_COLOR_FORMAT_RGB565` maps to **`WL_SHM_FORMAT_RGB565`**. ARGB8888 and XRGB8888 are also supported, and unknown formats fall back to XRGB8888 (`lv_wayland_backend_shm.c:139-145, 184-190`).

**Buffers and damage**
- Two `wl_buffer`s in one shm pool, in DIRECT mode (:197-251).
- Each flushed area becomes a `wl_surface_damage` call in surface coordinates (:441-446).
- On the last area it requests a `wl_surface_frame` callback, then attaches and commits (:448-466).
- If the next buffer is still held by the compositor, it only logs "Failed to acquire a non-busy buffer" (:452-455). There is no third buffer and no wait. That is probably fine with pixman compositors, which release the buffer after copying it (UNVERIFIED).
- Rotation is a full-frame `lv_draw_rotate` into the `wl_buffer` that damages the whole surface (:423-441). `wl_surface.set_buffer_transform` is not used.

**Shell and windows**
- xdg-shell only (`lv_wayland_xdg_shell.c`), with fullscreen, maximize and minimize (:86-116). Client-side decorations were removed in 9.5 (`lv_wayland.c:17-22`).
- One process can open several windows: each `lv_wayland_window_create` call (`lv_wayland_window.c:46`) creates its own `lv_display`.
- `lv_wayland_get_fd()` (`lv_wayland.c:156`) allows a poll-driven main loop.
- Dependencies: wayland-client, wayland-cursor and xkbcommon.

**Maturity**
- The directory has 78 commits and is actively fixed. Recent examples:
  - `73ba5865d`: input goes to the focused window.
  - `29f7dfcd2`: keyboard events are buffered.
  - `140f3223b`: missing wl_pointer axis listeners added.
  - `4c5330fbc`: waits for buffer release (G2D only).
  - `2110b9fac`: outputs identified by connector name.
- Under cage or wlroots with pixman, the compositor makes the damage-tracked KMS commit; wlroots sends `FB_DAMAGE_CLIPS` (wlroots `46c42e55`). The price is one extra compositor copy per damaged rectangle and one more process.

The SDL, X11 and GLFW backends exist but don't matter here.

### 4. Performance on ARMv4/ARMv5 without an FPU

**Floats**
- `LV_USE_FLOAT` defaults to 0 (template :775), and coordinates are int32.
- In `src/draw/sw/*.c` and blend, floats appear only in the vector-font glyph path (`lv_draw_sw_letter.c:238-272`, FreeType outline fonts) and in `lv_draw_sw_vector.c` (ThorVG).
- `lv_draw_vector.c` (98 float uses), the matrix code, ThorVG and Lottie are float-heavy, so keep `LV_USE_VECTOR_GRAPHIC`, `LV_USE_MATRIX`, ThorVG and Lottie off.
- Fills, borders, arc masks, image blending and widget transforms (`lv_draw_sw_transform.c`) are fixed-point. The core's float matrix code is compiled only under `#if LV_DRAW_TRANSFORM_USE_MATRIX` (`lv_refr.c:1230-1275`).

**RGB565 fast paths**
- Solid fills write 2 pixels per 32-bit word, unrolled 8 times (`blend/lv_draw_sw_blend_to_rgb565.c:291-331`). Blending uses integer `lv_color_16_16_mix`.
- It is all plain C that compiles fine for ARMv4, and the unrolled stores can become STM bursts.

**Assembly**
- The options are NONE, NEON, HELIUM, RISC-V V, SVE2 and CUSTOM (template :307-314). None of the built-in ones applies to ARMv4/v5.
- `LV_DRAW_SW_ASM_CUSTOM` plus `LV_DRAW_SW_ASM_CUSTOM_INCLUDE` let you supply your own routines for each `LV_DRAW_SW_COLOR_BLEND_TO_RGB565*` and rotate hook (they default to no-op macros, e.g. `blend_to_rgb565.c:81-82`).
- ARMv5TE versions, or iWMMXt ones for the hx4700 under GCC 15, would be new but well-scoped code.

**Trimming**
- Set `LV_DRAW_SW_COMPLEX 0` (template :284). It turns off rounded corners, shadows and anti-aliased masks.
- Keep `LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0` and use opaque themes.
- Avoid `style_opa` layers: each allocates an intermediate buffer of `LV_DRAW_LAYER_SIMPLE_BUF_SIZE` (24 KiB, template :180).
- Disable unused `LV_DRAW_SW_SUPPORT_*` formats (template :231-261) to save code.

**Memory**
- The docs ask for >64 kB of flash (180 kB recommended), >2 kB of stack (8 kB recommended), and >48 kB of heap "if using many widgets" (`docs/src/introduction/requirements.mdx`).
- On Linux, use `LV_STDLIB_CLIB` malloc instead of the fixed `LV_MEM_SIZE 65536` pool (template :37, :59).
- For a PIM UI on ARM, estimate (UNVERIFIED):
  - 400-800 KiB of code, depending on widgets and fonts.
  - 0.5-2 MiB of heap for a few hundred live objects (about 100-200 B each, with styles).
  - 1-2 frame buffers of 150 KiB each (600 KiB each at 480x640).
- That is far below the 50 MiB budget and far below GTK2.

**Threading**
- `LV_USE_OS LV_OS_PTHREAD` adds draw threads (`LV_DRAW_SW_DRAW_UNIT_CNT`, template :269-273) and locking, which gain nothing on a single core.
- Use `LV_OS_NONE` with a single-threaded poll loop. lv_port_linux's loop is `lv_timer_handler()` + `usleep` (`src/main.c:204-208`).

**External data point.** A Nuvoton N9H30 (ARM926EJ-S at 300 MHz, 64 MB, 800x480, *with* a 2D GPU) ran the LVGL benchmark at about 22 fps ([LVGL blog review](https://blog.lvgl.io/2022-02-18/numaker-hmi-n9h30-review)). 240x320 is about a fifth of those pixels, so interactive rates on a CPU-only PXA25x look plausible (UNVERIFIED). On mq11xx and w100 the VRAM bus dominates unless damage is fixed.

### 5. App model, text and widgets

#### App models

**A. One process, many apps (recommended for bare DRM)**
- A shell owns the display. Apps are screens or `lv_fragment`s (`src/others/fragment`) or tabview/tileview pages, compiled in or loaded with `dlopen`.
- Least RAM, instant switching.
- No isolation: one crash takes everything down.

**B. One LVGL process per app, with a launcher handing over DRM master**
- Needs **new code** in the driver: a suspend/resume API.
  - Suspend: stop flushing, `drmDropMaster`, close or ungrab the evdev devices.
  - Resume: `drmSetMaster`, a forced modeset commit (the one-shot first-commit logic gets in the way), and a full invalidate.
- Since kernel 5.8, a non-root `SET_MASTER` works only for the fd's original opener (UNVERIFIED detail). A handover between processes therefore needs seatd/logind via libseat, root, or fd passing.
- This effectively re-invents a compositor.

**C. LVGL apps as Wayland clients of cage/wlroots with pixman, or of a tiny custom compositor**
- Works today, with RGB565 shared-memory buffers and per-rectangle damage.
- Costs one more compositor process and one extra damage copy.
- Gives process isolation, and lets GTK2 apps (once the GDK2 Wayland backend exists) run next to LVGL apps under one shell. If that backend happens, this is the most coherent route.

#### Text
- **Fonts:**
  - Bitmap fonts in `src/font/`: Montserrat 8-48, unscii 8/16, DejaVu 16 Persian/Hebrew, Source Han Sans SC 14/16 CJK subsets.
  - Custom subsets via the font converter; `binfont_loader` loads fonts at runtime.
  - Fallback chains through `lv_font_t.fallback` (`lv_font.h:94`, resolved in `src/font/lv_font.c:153`).
  - Scalable fonts through FreeType (`LV_USE_FREETYPE` with a glyph cache, template :1180-1196) or tiny_ttf, i.e. stb_truetype (template :1199-1211).
- **Scripts and i18n:**
  - UTF-8 (`LV_TXT_ENC`, template :957).
  - BiDi (`LV_USE_BIDI` :983) and Arabic/Persian shaping (:999).
  - String tables via `LV_USE_TRANSLATION` (:781).
- **No HarfBuzz**, so Indic and other complex scripts will not render properly. This is adequate for a European or CJK PIM.
- Prefer pre-rendered bitmap fonts for speed and keep FreeType as a fallback. FreeType rasterises in fixed point, but LVGL's outline-glyph path uses floats (section 4).

#### Widgets (`src/widgets/`)
- **`calendar`:** a month grid with highlighted dates and arrow or dropdown headers, plus a Chinese calendar variant. There is **no day, week or agenda view**.
- **`textarea`:** cursor, selection, password mode, one-line mode, accepted characters, maximum length and placeholder.
  - It is built on `lv_label`, which re-lays out the whole text on every edit. That's fine for notes and poor for large documents.
  - No clipboard.
- **`list`:** **not virtualised**; every row is a full object. For hundreds of contacts use `table` (cells are not objects) or a custom list that recycles rows.
- **Also available:** roller, spinbox, dropdown, menu, tabview, tileview, msgbox, keyboard, chart, span, win, and `file_explorer` (`src/others`).
- Data storage, sync and PIM logic are entirely up to you.

### 6. Using the MediaQ and W3220 features

| Feature | With LVGL | Effort |
|---|---|---|
| Damage to the shadow plane (mq11xx, w100, sa1100) | Not sent today. Add `FB_DAMAGE_CLIPS` from the flushed areas, plus a single-buffer mode | **Small patch**, highest value |
| MediaQ hardware rotation (plane `rotation` property) | Not used. Swap the framebuffer dimensions and set the property. Better than software rotation, which DRM with the software renderer cannot do at all today | **Small patch** |
| Software rotation (sa1100, pxa, w100) | Missing in the DRM driver. Port fbdev's rotation; ideally a tiled ARM rotate via `LV_DRAW_SW_ASM_CUSTOM`, applied only to damaged rectangles | Small to medium |
| MediaQ hardware cursor / DRM cursor plane | Not used. The LVGL cursor is a software `lv_obj` (`lv_indev_set_cursor`). Irrelevant with a stylus; using it for a mouse would need a raw cursor-plane commit | n/a, or new code |
| 2x pixel doubling (120x160 source to the full screen) | The driver must set different source and CRTC sizes. Full-screen only, so useful for a dedicated video or game mode, not the UI | Small patch |
| C8 / 8 bpp | LVGL cannot render into a paletted I8 target; I8 exists only as an image format. It can render **L8 greyscale** (`LV_COLOR_FORMAT_L8`, `blend_to_l8.c`). Accepting `DRM_FORMAT_C8` with L8 rendering and a grey-ramp GAMMA_LUT halves the bus bytes and enables MediaQ's tear-free GC0C flips, at the cost of a monochrome UI. Colour 8-bit needs new renderer code and isn't recommended | Small patch |
| MediaQ 2D engine as an LVGL draw unit | The API exists (`lv_draw_create_unit`, `lv_draw.h:219`; `dispatch_cb`/`evaluate_cb` in `lv_draw_private.h:110-118`) but **doesn't fit**: LVGL draws into RAM, and the engine reaches only VRAM behind the shadow plane. The kernel already uses the engine for solid fills it finds by scanning the shadow, but only within the damage it is given, so the damage patch matters here too. With the private blit ioctl rejected, there is no scroll or copy offload | Not viable |
| W3220 YUV overlay for video (colour key) | LVGL owns the only DRM fd (there is no getter) and commits nonblocking every frame. Overlay plane properties (FB_ID, source/destination rectangles, colour key) need either a hook into LVGL's own commit or commits serialised with LVGL's, otherwise EBUSY. The UI side is trivial: an `lv_obj` filled with the key colour. The decoder writes YUV420 into dumb buffers. The built-in `ffmpeg`/`gstreamer` widgets convert to RGB in software, which is too slow here | **Medium, new code** |
| vblank pacing | Flips already wait for the flip event. When that event fires on the shadow-plane drivers depends on their real or simulated vblank. Driving LVGL's refresh timer from the flip event is a small patch | Partly exists |

### 7. Licensing and OpenEmbedded

**Licences**
- LVGL is **MIT** (`LICENCE.txt`, "Copyright (c) 2025 LVGL Kft"), and so is `lv_port_linux`.
- `COPYRIGHTS.md` lists the bundled third-party code:
  - FreeType: LVGL ships only the interface; FreeType itself (FTL or GPL-2.0+) is linked externally.
  - stb is partly integrated.
  - lodepng and tiny_ttf are bundled under their own licences.

**meta-oe recipes**
- meta-openembedded master carries `meta-oe/recipes-graphics/lvgl/` with:
  - `lvgl_9.5.0.bb`: branch `release/v9.5`, tag v9.5.0, SRCREV `85aa60d1`. It inherits cmake and passes `-DBUILD_SHARED_LIBS=ON -DLV_BUILD_USE_KCONFIG=ON`.
  - `lv-conf.inc`, which concatenates Kconfig fragments chosen by PACKAGECONFIG: drm (the default, depends on libdrm and libevdev), fbdev, freetype, gridnav, sdl and thorvg.
  - `files/*.cfg`.
  - `lvgl-demo-fb_9.5.0.bb`, from `lv_port_linux_frame_buffer`.
- Build dependencies are only `python3-kconfiglib-native`, `python3-pcpp-native` and libdrm/libevdev.

**What to change for this target**
- `files/drm.cfg` sets `CONFIG_LV_COLOR_DEPTH_32=y`. Add a bbappend with a `.cfg` fragment for RGB565 that also turns floats and ThorVG off and applies the section 4 trims.
- The recipe uses `UNPACKDIR`, which only recent OE releases have. On an older Angstrom/OE branch, backport the recipe or build with plain CMake.
- The upstream Yocto guide lists older OE branches and their versions: scarthgap 9.1.0, nanbield 8.3.10, kirkstone 8.0.3 (`docs/src/integration/embedded_linux/distros/yocto/lvgl_recipe.mdx:274-284`).
- Moving to 9.6.0 is a SRCREV/branch bump plus checking for Kconfig renames:
  - `LV_COLOR_DEPTH` is deprecated in favour of `LV_COLOR_FORMAT_DEFAULT`.
  - The DRM backend choice moved to `LV_LINUX_DRM_BACKEND_*`, with `LV_LINUX_DRM_AUTO_BACKEND` (`drm/Kconfig`).

### Sources
- [wlroots FB_DAMAGE_CLIPS commit](https://git.nixnet.services/blankie/wlroots/commit/46c42e55c6e0b08d9e7db989d6a10f073525e999)
- [LVGL N9H30 review](https://blog.lvgl.io/2022-02-18/numaker-hmi-n9h30-review)
- Source trees: [lvgl/lvgl](https://github.com/lvgl/lvgl) at `389dbdee` and v9.6.0, [lvgl/lv_port_linux](https://github.com/lvgl/lv_port_linux) at `5ae67b9`, [openembedded/meta-openembedded](https://github.com/openembedded/meta-openembedded) at `ca3cf5f`.
