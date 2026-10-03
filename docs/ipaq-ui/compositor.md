# Display server and compositor

[← index](README.md) · constraints and design rules: [hardware.md](hardware.md)

## 1. Evaluation

| | X (`modesetting`) | Weston 16+ | Custom wlroots | LVGL DRM / EFL drm |
|---|---|---|---|---|
| RGB565 output | yes | with `gbm-format=rgb565` | must be set explicitly; no fallback from XRGB8888 (~5 lines) | see [toolkits.md](toolkits.md) |
| `FB_DAMAGE_CLIPS` | via DIRTYFB | yes, but **empty damage becomes a full upload** (small fix) | yes, correct with buffer age (atomic path only) | neither sends it |
| Extra copies | +1 (pixmap) | `pixman-shadow` is on by default and adds a full-size 8888 pass; turn it off | none beyond composition | — |
| Direct scanout | n/a | not with pixman (no dmabuf import) | yes, for dmabuf clients | — |
| Shell fit | matchbox | kiosk-shell lacks an OSK panel and idle handling; text-input-v1 only; no layer-shell | layer-shell, virtual-keyboard, input-method-v2 | — |
| Verdict | baseline to beat | feasibility test | **chosen** | — |

**Rejected bases:**

| Base | Why rejected |
|---|---|
| swc/wld | Legacy KMS, no damage clips, XRGB8888 hard-coded |
| Smithay | Best plane support of the lot, but no Rust target for ARMv4 (non-T) and a multi-MB binary |
| phoc | GLES2 plus gnome-desktop build dependencies |
| labwc | About 38k lines |
| cage, dwl | Used as references only, not forked |

## 2. Weston

**Configuration that runs on all five boards:**
- DRM backend with pixman, `gbm-format=rgb565`, `pixman-shadow=false`, kiosk-shell.
- `repaint-window` around 15 ms.
- Patch the empty-damage case: send one zero-area rect, or leave the primary plane out of the commit.

**Why it isn't the long-term base:**
- No layer-shell and no virtual-keyboard. Only text-input-v1, which a GDK2 backend would then have to speak.
- The primary plane is always `ROTATE_0` and never scaled.
- No dmabuf import in pixman, so overlays and direct scanout need large patches.
- Atomic KMS is mandatory, and the fbdev backend has been gone since Weston 11.0.

Source details are in §A.

## 3. wlroots

- **Rendering:** no GBM or EGL. A `-Drenderers=[]` build picks pixman automatically on these render-node-less drivers. It uses the dumb-buffer allocator, which requires PRIME export. `wlr_scene` handles damage and visibility.
- **Planes:**
  - The cursor plane is used if it accepts ARGB8888.
  - Plane scaling is reachable through `buffer_src/dst_box`.
  - The `rotation` property is read but never written.
- **Atomics:** one C11 atomic in `wlr_shm.c` must be lock-free. On ARMv4/v5 that comes from the kernel user helpers [check on SA-1110].

Source details are in §B.

## 4. pixman: where the time goes

Every scene node with damage becomes one `pixman_image_composite32()` call. Each call has a fixed cost before any pixel moves:
- **On the wlroots side:**
  - soft-float `roundf` and colour conversion;
  - a malloc'd solid-fill image per rect;
  - the clip region set and reset around every call, each a region copy that marks the destination dirty.
- **On the pixman side:**
  - re-validation of the image flags;
  - region intersect and extent analysis;
  - a fast-path lookup through an 8-entry thread-local cache;
  - then one call per clip box.

Fragmented damage (text, clocks) can make the fixed cost exceed the pixel cost.

| Request | Path on ARMv4/v5 (no SIMD) | Cost |
|---|---|---|
| SRC 565→565, no transform | `fast_composite_src_memcpy` | memcpy |
| Opaque rect | `pixman_fill` | cheap |
| SRC 8888→565, identity | No C fast path in the 2013 tree, so the general path runs: three passes [check your tree] | several × memcpy |
| OVER 8888→565, or any opacity | C fast path; per pixel: read, expand, blend, pack | heavy |
| 90/270, 565 SRC | Tiled rotate; the tile size assumes 64-byte lines (SA-1110 and XScale have 32) | ~1.5–3× memcpy [est] |
| Real scaling | Stays bilinear; pixman downgrades only identity, integer-translate and 90° steps | prohibitive |

On top of that come four multipliers: buffer age (×2–3), Weston's shadow, background clears under translucent surfaces, and many small boxes.

**Mitigations, in order:**
1. **Opaque RGB565 clients only.** The GDK2 backend and LVGL do this; wvkbd needs a patch.
2. **No scaling.** No fractional-scale, output scale 1, NEAREST filter on every scene buffer.
3. **No software rotation where avoidable.** Landscape on the h3870/h3970, hardware rotation on the MediaQ.
4. **Single-buffer composition on copy-type drivers,** so buffer age is 1.
5. **A ~60-line bypass in wlroots `render/pixman/pass.c`:**
   - same-format SRC, no transform, no mask: iterate the clip boxes and call your asm memcpy directly;
   - opaque rects: call `pixman_fill` directly.
6. **An identity XRGB8888→RGB565 fast path,** if any 8888 client remains.
7. **Damage-merge tuning:** trade call count against pixel count.

The biggest pixman consumer is the clients' own cairo rendering, and that cost is the same under X. The compositor must add nothing beyond the copy budget, and with §5.3 it adds nothing in the common case.

**Profiling tools:**
- `perf` with the XScale PMU on PXA; timer sampling on the SA-1110.
- `PIXMAN_DISABLE` to A/B implementations.
- `WLR_SCENE_DEBUG_DAMAGE=highlight`.
- Composite-call and box counts per frame, via an LD_PRELOAD shim or a uprobe.

## 5. Compositor design

### 5.1 Scope
**In:**
- Full-screen stacked toplevels, with switching through the panel.
- A layer-shell panel and OSK.
- Touch and gpio-keys.
- Idle blanking and rotation.
- Video handoff.

**Out:**
- Move/resize, decorations and a drawn cursor.
- Xwayland and effects.

tinywl's interactive move/resize and xcursor code is deleted.

### 5.2 Components

| Part | Approach | Lines |
|---|---|---|
| Output | `wlr_output_state_set_render_format(RGB565)`; per-board transform (normal, or portrait on the h3870/h3970 if chosen); atomic only, never `WLR_DRM_NO_ATOMIC` | ~60 |
| Scene | Trees, bottom to top: background (one solid rect, engine-fill friendly), apps, panel (layer "top"), OSK (layer "overlay"), lock/blank | ~80 |
| Window policy | On map: `wlr_xdg_toplevel_set_size` to the usable area, maximized or fullscreen, origin (0,0). Move/resize requests are ignored | ~150 |
| Switching | `wlr_scene_node_raise_to_top`, then activate and focus. The app list goes to the panel through foreign-toplevel (wlr and ext). A hardware button cycles apps | ~120 |
| Input | The compositor converts touch to pointer events: GTK2 has no touch model, and one place serves every client. Calibration comes from udev `LIBINPUT_CALIBRATION_MATRIX`; `wlr_cursor_map_to_output` follows the transform. No xcursor manager. gpio-keys: power blanks, app buttons launch from a table, one button cycles, a chord rotates | ~250 |
| Layer-shell | `wlr_scene_layer_surface_v1_*`; exclusive zones shrink the usable area (modelled on dwl's `arrangelayers`) | ~200 |
| OSK | `wlr_virtual_keyboard_manager_v1`. wvkbd (RGB565 patch, meson-built fork), toggled by a signal from the panel. It floats by default, because an exclusive zone forces an app relayout and full repaint on every toggle | ~60 |
| Power | On idle, `wlr_output_state_set_enabled(false)`; the drivers sequence backlight, scanout and chip power. `wlr_idle_notifier_v1`. Output-power-management for the panel. Unblank costs a full chip re-init on mq11xx | ~120 |
| Rotation | On a runtime transform change, re-arrange the layers and resize the toplevels | ~60 |
| Protocols | presentation-time, viewporter (never used for scaling), single-pixel-buffer, xdg-activation, xdg-decoration ("none"), data-device | ~40 |
| Session | Replaces gpe-dm. A systemd unit on seatd that spawns the panel, wvkbd and the autostart list | ~150 |
| **Phase 1 total** | | **~1.8k** |

### 5.3 Buffer model: the zero-copy path

**Status (picowl branch `picowl`):** implemented, but compiled and unit-tested only, because no test system has a DRM device. Pieces:
- `picowl-buffer-v1` with `copied`/`retained` events. Early release is sent only for direct scanout on copy-type outputs.
- RGB565 linear dmabuf feedback.
- Panel auto-hide.
- Single-buffer composition on copy-type outputs.
- A wlroots patch so pixman can read dmabufs.

See `picowl/doc/buffers.md` and `picowl/doc/zero-copy.md`, including the hardware-only checklist.

1. **Compositor-allocated client buffers.**
   - A small private protocol: the client asks for a w×h RGB565 buffer and gets a dmabuf fd and stride.
   - The compositor creates a dumb buffer and PRIME-exports it. The client mmaps it and attaches it through linux-dmabuf.
   - On pxa-lcdc the buffers come from the contiguous LCDC pool, so they are scanout-capable. On the shmem drivers they self-import.
   - wl_shm remains as the compatibility path.
2. **Direct scanout.**
   - `wlr_scene` scans out the single visible buffer node and sends the frame's damage as `FB_DAMAGE_CLIPS`.
   - Patch needed: advertise the primary plane's RGB565 linear format in linux-dmabuf. The pixman renderer advertises none.
3. **One surface on screen.**
   - The panel auto-hides while an app is focused. A hardware button or an edge tap shows it.
   - GDK2 draws popups inside the toplevel buffer ([toolkits.md §1](toolkits.md#1-gtk-2-wayland-backend)).
4. **Single client buffer with early release on copy-type outputs.**
   - The kernel's copy is done by the time the flip event arrives. The compositor then releases the buffer, and the client paints the next damage into it.
   - This drops both the buffer-age repair copy and GDK's double-buffer pixmap.
   - It needs a compositor patch, because wlroots otherwise holds the buffer until the next commit.
   - pxa-lcdc needs two client buffers, or you accept tearing.
5. **Composition as the fallback,** while the OSK or panel is visible.
   - It covers damage only, through the §4 bypass, into a single compositor buffer on copy-type outputs.
   - It needs pixman to read dmabuf client buffers via mmap with `DMA_BUF_IOCTL_SYNC` (~150 lines in wlroots). Without that, those surfaces vanish whenever direct scanout fails.

### 5.4 Phase 2

**Status:**
- MediaQ hardware rotation (`[rotation] mode = auto|hardware|software`, wlroots patch 0003) is implemented, untested on hardware.
- Pixel doubling, C8, video handoff and the windowed W3220 overlay are not implemented. The C8 options are written up in `picowl/doc/zero-copy.md`.
| Feature | Approach | Size |
|---|---|---|
| MediaQ hardware rotation | Write the primary-plane `rotation` property; report the swapped size; rotate touch | 50–150 lines, incl. a wlroots patch |
| MediaQ pixel doubling | Render a 120x160 swapchain and commit with a 240x320 destination box | ~100 |
| Video handoff (W3220 overlay; MediaQ C8/GC0C flip/doubling) | Lease the output to `mediaplayer-drm`. `wlr_drm_lease_v1_manager_offer_output` accepts any output; the `wlr_output` is destroyed while leased and re-issued afterwards. The player needs a small lease client. Alternative: libseat VT-style master handover, with no player change | ~150 |
| Windowed W3220 overlay | The kernel side works (the shmem helper imports udmabuf/dma-heap buffers). Needs YUV formats in linux-dmabuf, a libliftoff output layer and colour-key properties | several hundred |
| MediaQ C8 output | pixman indexed format, palette via the gamma LUT; only for the tear-free GC0C flip | new code |

## 6. Build

Everything we write builds with meson.

| Component | Build | Notes |
|---|---|---|
| our compositor | meson | |
| wlroots | meson | Pin 0.19 or 0.20 (the API differs; only 0.20-dev master was reviewed). Options and dependencies below. |
| Weston (test only) | meson | Minimal options in §A.5. cairo and libpng `.pc` files are needed at configure time but not linked. |
| wvkbd, dwl forks | Makefile; add meson | Small |
| pixman | meson upstream [check] | The 2013 snapshot read here is autotools |

**wlroots configuration:**
- Options: `-Dbackends=drm,libinput -Drenderers=[] -Dallocators=[] -Dxwayland=disabled -Dlibliftoff=disabled -Dcolor-management=disabled`.
- Dependencies: wayland-server, libdrm ≥2.4.122, xkbcommon, pixman, libdisplay-info, hwdata (build only), libinput, libudev, libseat.
- Size: about 77k lines, 1–2 MB stripped [est].

---

## A. Source notes: Weston

**Source.** The mirror `freedesktop-unofficial-mirror/wayland__weston` stops at 2015-04-14 (tag 1.7.0), so it is stale. This note uses **github.com/wayland-mirror/weston** instead:
- HEAD `6cce279a` (main, 2026-09-28), which is 16.0.0 plus 246 commits. meson reports 16.0.90, libweston-17.
- Tag 16.0.0 is `d1882b0a` (2026-07-14).
- File:line references are at `6cce279a`.

Other code consulted:
- Pixman fast-path references come from a 2013 tree. Those tables have been stable since, but recheck them against current pixman.
- Kernel references are torvalds/linux master (7.3-rc5): `drm_damage_helper.c` and `drm_property.c`.

**Summary.** Plain Weston with the DRM backend and pixman runs on all four boards, without GBM. Set `gbm-format=rgb565` and `pixman-shadow=false`. Out of the box it uses no hardware feature except page-flip/vblank events and the cursor plane. One real bug turns every commit with no primary-plane damage into a full-frame VRAM upload on the copy-type drivers.

### 1. DRM backend + pixman renderer

#### Output format
- The default is XRGB8888 (`drm.c:4849-4852`), so it fails on the MediaQ driver, which has no XRGB8888. **Set `gbm-format=rgb565`**:
  - in `[core]` (`frontend/main.c:3869`), or
  - per output (`main.c:3102-3105` → `drm_output_set_gbm_format`, `drm.c:2194`).
- The value is parsed as a DRM fourcc name (`parse_gbm_format`, `drm.c:2139`) and maps to `PIXMAN_r5g6b5` (`pixel-formats.c:374-383`).
- Weston never checks the format against the primary plane's format list. A wrong value only fails at AddFB2 or at commit.
- **Weston's format table has no C8 entry** (`pixel-formats.c`), so palette output needs a patch.

#### Buffers
- With pixman, outputs always use `drm_fb_create_dumb` (`fb.c:285-351`: CREATE_DUMB, AddFB2, MAP_DUMB, mmap).
- GBM is only for the GL and Vulkan renderers (`backend-drm/meson.build:55-68`). With both off, `b->gbm` is NULL (`drm-internal.h:1127-1131`).
- There are **exactly 2 dumb buffers** (`drm-internal.h:714-716`), alternated with `current_image ^= 1` (`drm.c:530`).
- Atomic modesetting is mandatory: legacy KMS was removed in `ddd1cf6f3` (2026-05-11). Universal planes are required too (`kms.c:2355-2387`).
- A plane without a blend-mode property is treated as premultiplied, which is fine (`kms.c:939-951`).

#### pixman-shadow
- Defaults to **on** (`main.c:3873-3874`, `drm.c:5058`).
- The shadow is **always XRGB8888 at full output size** (`pixman-renderer.c:1191-1192`): 300 KiB at 320×240, 1.2 MiB at 480×640.
- With the shadow on, repaint (`pixman-renderer.c:602-644`) first composites this frame's damage into the shadow. `copy_to_hw_buffer` (`:511-536`) then copies the renderbuffer's accumulated damage into the dumb buffer, converting 8888→565. With 2 buffers that is the last 2 frames' damage.
- With the shadow off, that same 2-frame damage is composited directly into the dumb buffer.
- Buffer age is handled correctly both ways: damage accumulates into every renderbuffer (`:612-615`), and new ones start fully damaged (`:916-917`).
- **Turning the shadow off is safe**; correctness does not change.
  - On cached shmem dumb buffers (mq11xx, w100, probably sa1100) the shadow is pure overhead.
  - On pxa-lcdc (write-combined GEM DMA memory) it only helps translucent content, where OVER blending reads the destination. Opaque RGB565 surfaces use SRC, which only writes.

#### FB_DAMAGE_CLIPS
- `drm_output_render` (`drm.c:541-645`):
  - takes the frame damage (`:573`) and converts it to framebuffer coordinates, output transform included (`:624-626`);
  - creates a property blob (`:638-640`), skipped if the plane lacks the property (`:619-620`);
  - adds it to every commit (`kms.c:1895-1897`).
- So Weston sends **frame damage**. The kernel helper has no "framebuffer changed" check (`drm_damage_helper.c:246-251`), which makes this correct for drivers that copy damage into VRAM.

**Bug: empty primary damage becomes full damage.**
- When the primary plane has no damage, Weston commits it anyway. This happens on the reuse path (`drm.c:582-589`) and when frame callbacks are pending (it renders with empty damage and still swaps buffers).
- `drmModeCreatePropertyBlob` with 0 rects fails, because the kernel rejects zero length (`drm_property.c:562`). The blob id stays 0, which the kernel treats as **full-plane damage** (`drm_damage_helper.c:246-251`).
- Result: every cursor-only move, overlay-only update, or client commit with no damage costs 17-19 ms on mq11xx and 22-25 ms on w100.
- **Small patch:** send one zero-area rect, which the kernel's damage iterator skips (`:283-290`), or leave the primary plane out of the commit.

**Optional patch:** use a single dumb buffer for the copy-type drivers, so Weston stops re-rendering 2 frames of damage. This is safe if the kernel finishes its copy before it sends the flip event (unverified).

### 2. KMS planes

| Hardware feature | Out of the box? | Notes and what a patch needs |
|---|---|---|
| mq11xx 64×64 cursor plane | Yes, with conditions | <ul><li>Without GBM, Weston uses renderer-plus-cursor mode.</li><li>Cursor buffers are 2 ARGB8888 dumb buffers sized from `CURSOR_WIDTH/HEIGHT`, default 64.</li><li>The client's cursor must be an ARGB8888 shm buffer no larger than that.</li><li>Under a 90/270 output transform with no cursor `rotation` property, it falls back to a software cursor.</li><li>Whether the driver accepts ARGB8888 is unverified.</li></ul>`state-propose.c:1722-1747`, `drm.c:2925-2965`, `kms.c:2341-2353`, `state-propose.c:614-635`, `drm.c:887-913`, `kms.c:598-602` |
| W3220 cursor | — | No hardware cursor; software cursor. |
| Overlay planes (any client buffer) | No | <ul><li>The pixman renderer has no `import_dmabuf`, so linux-dmabuf is never advertised and clients can only send shm buffers.</li><li>shm buffers can only go on the cursor plane.</li><li>The dmabuf path also needs `b->gbm`, `gbm_bo_import` and an explicit modifier.</li><li>Planes-only and mixed modes are tried only when GBM is present, so even direct fullscreen scanout is impossible.</li></ul>`pixman-renderer.c:1065-1124`, `compositor.c:11480`, `state-propose.c:775-803`, `fb.c:743-747`, `fb.c:479`, `fb.c:432-437`, `state-propose.c:1722` |
| W3220 YUV overlay | No | <ul><li>Weston already supports three-plane YUV formats, COLOR_ENCODING, zpos/underlays and plane alpha.</li><li>Still needed, a large patch: dmabuf import in pixman (pixman can't composite three-plane YUV for the fallback), a PRIME import path without GBM, and colour-key support.</li></ul>`pixel-formats.c:876-900`, `kms.c:151-160, 241`, `drm.c:2701-2719` |
| W3220 colour key | No | No colour-key property support anywhere in the DRM backend. |
| mq11xx hardware rotation | No | <ul><li>`rotation` is only set on planes that carry client buffers (`e471edb3`/`b0f23dc0`, 2023).</li><li>The rendered primary plane is always ROTATE_0, so output transforms are done in software.</li><li>The panel's orientation sets the default head transform (`72e7a1ed`).</li><li>Medium patch: render in logical orientation and set the rotation on the primary plane.</li></ul>`kms.c:591-640`, `state-helpers.c:56, 248`, `main.c:3088-3095` |
| mq11xx 2× pixel doubling | No | <ul><li>Client-buffer planes do get src/dst scaling, but the primary plane always has framebuffer size = mode size.</li><li>Weston's output `scale` is for HiDPI, the opposite direction.</li><li>Medium patch: a half-size framebuffer and a 120×160 logical mode.</li></ul>`state-helpers.c:250-259`, `drm.c:602-612` |
| Vblank / page-flip pacing | Yes | <ul><li>Atomic non-blocking commits with a page-flip event, `drmWaitVBlank` for the timestamp, and a `pageflip-timeout` option.</li><li>`repaint-window` defaults to 7 ms and is clamped below the refresh period. Raise it to about 15 ms on these CPUs.</li></ul>`kms.c:1996`, `drm.c:1066`, `main.c:3871`, `compositor.c:110`, `main.c:1215`, `compositor.c:4271-4310` |

### 3. Client formats and blit cost
- **shm formats advertised** (`pixman-renderer.c:1106-1120`): RGB565, the 8888 variants, the 2101010 variants and ABGR16161616. RGB565 is offered to clients; YUV is not.
- An RGB565 surface counts as fully opaque (`surface-state.c:476-490`). A plainly positioned window is therefore composited with SRC (`pixman-renderer.c:370-384`).
- **565 client to 565 output, shadow off:** pixman's `fast_composite_src_memcpy` (`pixman-fast-path.c:1873`), a plain memcpy per damaged rectangle.
- **With the default shadow:** two conversions per pixel (565→8888, then 8888→565), through pixman's generic path.
  - On ARMv4/5 the dedicated 8888→565 path exists only for NEON (`pixman-arm-neon.c:275-276`).
  - The hx4700 has an iWMMXt pixman build (`configure.ac:604-608`). Whether it covers this conversion is unverified.
- **Output transform 90/270:** an exact 90° rotation with nearest filtering (`compositor.c:392-398`).
  - With the shadow off and a 565 client, it goes through pixman's `fast_composite_rotate_90/270_565` (SRC only, same format; `pixman-fast-path.c:1958-1962`). Estimated cost: about 1.5-3× a memcpy.
  - With the shadow on, or translucent content, it falls to pixman's slow general transform path.
  - The cursor falls back to software.

### 4. Shells and PDA fit
- **kiosk-shell** (about 1.9k lines): makes every window fullscreen or maximized, has touch-to-activate (`kiosk-shell.c:1297-1326`) and handles transform changes (`:92, :1508`). **No input-panel (OSK) support and no idle/blanking handling.**
- **desktop-shell** (about 4.9k lines):
  - has the input panel (`desktop-shell/input-panel.c`) and idle → fade → lock → display off (`shell.c:3819-3845, 4797`);
  - needs the cairo-based `weston-desktop-shell` helper client (`shell.c:356`);
  - its full-screen fade animations are expensive on this hardware.
- **ivi-shell** has an input panel (`ivi-shell.c:1008-1132`), but its automotive model doesn't fit a PDA.
- **lua-shell** (`e91eccd7`, 2025-06-04): 2.2k lines of C plus `shell.lua`, needs Lua ≥ 5.4. No input panel and no idle handling.
- **Custom shell, recommended path:** write it as a Weston plugin. Fork kiosk-shell and add the input panel and idle handling from desktop-shell. The frontend's config, launcher, idle timer and text-input backend then come for free.
  - The alternative, a standalone libweston compositor, replaces `frontend/main.c` (5.4k lines). Estimated at about 800-1500 lines for a minimal DRM-only one (unverified).
  - There is no minimal example in the tree; `westinyplus/` is just a C++ header build test with an empty `main`.
- **Input methods:** only text-input-v1 and input-method-v1 (`text-backend.c`, `protocol/meson.build:27, 40`).
  - The keyboard client is set by `[input-method] path=` and defaults to the cairo-based `weston-keyboard`.
  - Weston spawns it, and only that client may bind the input method (`text-backend.c:893, 1068-1085`).
  - **There is no text-input-v3, input-method-v2, virtual-keyboard or layer-shell.** A GDK2 backend would have to speak text-input-v1.
- **Other protocols:**
  - xdg-shell stable, with popups through libweston-desktop.
  - Touch through libinput, plus a touch-calibration protocol and tool.
  - `idle-time` defaults to 300 s (`main.c:5202-5207`), but only desktop-shell acts on it by turning the display off.
  - `weston_output_set_transform` works on enabled outputs (`compositor.c:8494-8544`). Nothing in the tree triggers rotation at runtime, so the shell needs a binding for it.

### 5. Build footprint and CPU concerns

**Minimal meson configuration:**
```
-Dbackend-drm=true -Dbackend-{headless,pipewire,rdp,vnc,wayland,x11}=false -Dbackend-default=drm
-Drenderer-gl=false -Drenderer-vulkan=false -Dxwayland=false -Dsystemd=false
-Dshell-desktop=false -Dshell-ivi=false -Dshell-lua=false -Dshell-kiosk=true
-Dcolor-management-lcms=false -Dimage-jpeg=false -Dimage-webp=false
-Ddemo-clients=false -Dsimple-clients=[] -Dtools=[] -Dtests=false -Dperfetto=false
```

**Required dependencies:**
- wayland ≥ 1.24 and wayland-protocols ≥ 1.46 (`meson.build:158`, `protocol/meson.build:4`).
- pixman, xkbcommon, libinput, libevdev, libdrm ≥ 2.4.108, libudev (`libweston/meson.build:240`).
- **libseat ≥ 0.4** (seatd). It is the only launcher left: logind and weston-launch were removed in `a96dfc70` (2022).
- libdisplay-info 0.3-0.4, with a bundled fallback.
- Gotcha: `shared/meson.build:53-54` requires cairo and libpng unconditionally, so their `.pc` files must exist at configure time. Neither is linked into the DRM compositor itself.

**Soft-float.**
- The pixman renderer does no per-pixel floating point.
- Per window per frame there is one 4×4 matrix inversion, some double-to-fixed conversions (`pixman-renderer.c:115-129`) and double-precision coordinate maths. That comes to a few hundred to low thousands of soft-float operations per frame, which is acceptable.
- With lcms disabled, a no-op colour manager is used.
- Avoid fade animations.

**Atomics / ARMv6.** libweston, shared, frontend and shells use no `stdatomic`, `__atomic` or `__sync`; only `pthread_sigmask`. The code is gnu11 C, and Weston itself has no ARMv6+ requirement that I found.

### 6. fbdev backend
- Deprecated in `6338dbd5` (2022-01-25, Weston 10).
- Removed in `b3ba1bec` (2022-03-15), so absent from **Weston 11.0** onward.

### Verdict per board

| Board | Out of the box | Gaps |
|---|---|---|
| h3870 (SA-1110) | Works | Portrait is software rotation. Use 565 clients with the shadow off to get pixman's rotate fast path. |
| h3970 (PXA250) | Works | Double-buffered direct scanout; 2 dumb buffers fit the 1 MiB CMA pool. |
| h2210 / h5550 (MediaQ) | Works | The hardware cursor is fine. Fix the empty-damage full-upload bug first. Hardware rotation, 2× doubling and C8 each need patches. |
| hx4700 (W3220) | Works, compositing only | The YUV overlay and colour key need large patches to pixman, the dmabuf path and the backend. |

## B. Source notes: compositor bases (wlroots, swc, Smithay, tinywl/dwl/cage/labwc/phoc, wvkbd)

**Recommended base:** a new compositor of about 1.5–2.5k lines, grown from tinywl on wlroots 0.19/0.20 with `wlr_scene` and the pixman renderer. Borrow touch and virtual-keyboard handling from cage, and layer-shell and per-output rules from dwl, rather than forking either. Use wvkbd as the on-screen keyboard.

**Limits of this research:**
- No mirror of the released wlroots 0.20.x was reachable. The only current GitHub mirror (`myaiexp/wlroots-ayasa0520`) carries master up to 2025-08-12 (0.20.0-dev) and 0.19.0, so the wlroots claims below come from that master.
- The GitHub mirror of dwl is stale (v0.5, 2023).
- Nothing was tested on hardware. Items marked [unverified] need a device to confirm.

### Verdicts

1. **RGB565 must be selected explicitly.** wlroots defaults to XRGB8888 with no fallback, so every stock compositor fails on mq11xx until it calls `wlr_output_state_set_render_format(RGB565)`. That is about 5 lines.
2. **Damage works with dumb-buffer swapchains.** Per-frame damage is correct, and FB_DAMAGE_CLIPS is sent, but only on the atomic path. Never set `WLR_DRM_NO_ATOMIC`.
3. **No GBM or EGL needed**, at build time or at run time. With `-Drenderers=[]`, pixman is selected automatically on these drivers.
4. **Planes are mostly out of reach with pixman.** Pixman can't use dmabufs, so client direct scanout and overlay planes are effectively unavailable. wlroots never writes the plane `rotation` property, but it can drive plane scaling.
5. **swc + wld: rejected.** Legacy KMS, no damage clips, XRGB8888 hard-coded, and no layer-shell, virtual keyboard, input method or touch.
6. **Smithay: rejected as a base.** It has a pixman renderer that can map dmabufs, and it uses overlay planes and the `rotation` property. But there is no Rust target for the SA-1110, and the binary would be several MB.
7. **phoc and labwc: rejected.** phoc needs GLES2 and GNOME/GTK3 libraries; labwc is about 38k lines.
8. **For GTK2 apps, virtual-keyboard-v1 alone is enough.** wvkbd's input-method support only adds automatic show and hide.
9. **The MediaQ/W3220 features are reachable, but most need new code.** §7 has the split, and §7c covers the overlay.

### Sources

| Project | Where from | Version |
|---|---|---|
| wlroots | github.com/myaiexp/wlroots-ayasa0520 (mirrors the upstream gitlab branches) | master `7392b331` (2025-08-12, 0.20.0-dev, meson.build:4); branch `0.19` / tag 0.19.0 `13a62a23` (2025-05-15) |
| swc / wld | michaelforney | `e4ce167` (2026-03-25) / `beceef2` (2026-08-10) |
| Smithay | Smithay/smithay | `118e34ff` (2026-09-28), v0.7.0 |
| cage | cage-kiosk/cage | `ef6ef6a` (2026-09), needs wlroots-0.20 |
| labwc | labwc/labwc | `c2a95cf` (2026-09), 0.20.2 |
| dwl | djpohly/dwl (GitHub mirror) | `58d33b7` (2023-11), v0.5, stale |
| wvkbd | jjsullivan5196/wvkbd | `e14b53a` (2026-09), v0.20 |
| phoc | agx/phoc | `6bf71f7` (2026-09) |
| pixman | servo/pixman | 2013 snapshot, used for the fast-path logic only |
| Linux | torvalds/linux, sparse checkout | `e767a4e`, v7.3-rc5 |

wlroots 0.20.0 was released in early 2026 ([Phoronix](https://www.phoronix.com/news/wlroots-0.20-Sway-1.12-rc1)), followed by 0.20.1 and 0.20.2; the mirror has none of these. Other GitHub copies are older still: swaywm's stops in 2021, sepnic's in January 2025, and the gamescope forks are at 0.18. `freedesktop-unofficial-mirror/*` and gitlab.freedesktop.org (git, HTTPS and WebFetch) were not reachable.

### 1. wlroots (master, about 0.20-dev)

#### Pixman formats
- **Formats offered** (`render/pixman/pixel_format.c:5-96`): the 8888 variants always, plus RGB565, BGR565 and the 2101010 variants on little-endian builds (l.73-95). They are advertised with the LINEAR and INVALID modifiers (`renderer.c:332-342`).
- **Buffers it can read:** only ones with a CPU data pointer (`renderer.c:328`). Texture formats are offered only for those (`renderer.c:196-204`), so pixman cannot texture dmabufs.
- **Missing:** C8 and YUV.

#### Selecting RGB565
- The output default is XRGB8888 (`types/output/output.c:340`).
- `output_pick_format()` (`types/output/render.c:147-196`) intersects what the renderer and the primary plane support for that one format, and returns false ("Output doesn't support format") when it isn't there.
- **There is no fallback.** Swapchain creation fails (`swapchain.c:13-24, 72-117`), and the modeset fails with it. The only fallbacks are retrying without modifiers (`swapchain.c:96-108`) and dropping alpha at framebuffer import (`backend/drm/fb.c:161-176`).
- **Fix:** call `wlr_output_state_set_render_format(RGB565)` (`state.c:67`).
- labwc already tries a list of candidate formats (`src/output.c:59, 88-110`), but RGB565 isn't on it.

#### Renderer selection
- With "auto", `render/wlr_renderer.c:219-288` tries GLES2 and Vulkan (if built), and pixman only if the DRM device has no render node.
- The tiny drivers have no render node, so a `-Drenderers=[]` build ends up on pixman automatically.
- The earlier need for `WLR_RENDERER=pixman` on qemuarmv5 most likely came from a GLES2-enabled build or a virtio-gpu render node [unverified].

#### Allocator
`render/allocator/allocator.c`, `wlr_allocator_autocreate`:
- GBM is used only if both the backend and the renderer take dmabufs. Pixman doesn't, so GBM is skipped.
- The shm allocator is skipped because the DRM backend only accepts dmabufs (`backend/drm/backend.c:235`).
- That leaves the **DRM dumb-buffer allocator**, which needs DRM master.
  - `drm_dumb.c:61-98` creates a 16 bpp dumb buffer, maps it and clears it.
  - It also requires dmabuf export (`drmPrimeHandleToFD`, l.87-91), so the kernel driver must support PRIME export.

Neither GBM nor EGL is needed to build or run; both are optional in `render/meson.build` and `render/allocator/meson.build`.

#### Swapchain
- `WLR_SWAPCHAIN_CAP 4` (`include/wlr/render/swapchain.h:8`). Buffers are allocated on demand; in practice 2–3.
- Buffer sizes: 150 KiB at 240x320 RGB565, 600 KiB at 480x640.

#### FB_DAMAGE_CLIPS
- Added in `46c42e55` (2021-07-21). Later fixes: `83090de0` (don't send empty damage) and `97a6a58a` (a 2024 leak fix).
- Built at `atomic.c:354-358` from the commit's damage and attached to the primary plane at `atomic.c:549-552`.
- Not sent for the cursor plane.
- **The legacy path sends no damage**, and `drmModeDirtyFB` is never called.

#### Damage correctness with dumb buffers
Yes, damage is correct. `wlr_scene` tracks two regions:
1. **Render damage**, from the buffer-age damage ring (`types/wlr_damage_ring.c:78-125`). It unions the changes since that buffer was last drawn; a new buffer gets a full redraw. Above `MAX_RECTS` it collapses to a bounding box.
2. **Frame-to-frame damage** (`wlr_scene.c:365-379, 2216, 1610-1614`). This one is sent as FB_DAMAGE_CLIPS.

The second is exactly what KMS FB_DAMAGE_CLIPS means ("changed since the previous framebuffer"). It is what mq11xx, w100 and sa1100-lcdc need in order to copy only the changed rectangles into VRAM or into their persistent buffer.

#### Hardware cursor
- `types/output/cursor.c:166-181`: the cursor is ARGB8888 only, allocated with the same dumb allocator and drawn with pixman.
- Size and formats come from `drm.c:1242-1260`, with a software fallback at `cursor.c:429`.
- Works if the mq11xx cursor plane advertises ARGB8888 [unverified].

#### Overlay planes and libliftoff
- libliftoff ≥0.4 is optional (`backend/drm/meson.build:16-21`).
- It is used only when `WLR_DRM_FORCE_LIBLIFTOFF=1` is set (`drm.c:90-102`), and only through the `wlr_output_layer` API. `wlr_scene` never uses output layers.
- Every plane framebuffer must be a dmabuf (`fb.c:143-148`), so shm client buffers can never go on a plane.

#### Plane rotation and scaling
- The `rotation` property is read (`properties.c:69`) but never written. Output rotation is always done in software by pixman (`pass.c:74-158`).
- **Plane scaling is possible:**
  - `wlr_output_state` carries `buffer_src_box`/`buffer_dst_box` (`wlr_output.h:130-135`).
  - `drm.c:776-777` turns them into the plane viewport, and `atomic.c:458-477` writes SRC_* and CRTC_*.
  - `wlr_scene` sets them only for direct scanout (`wlr_scene.c:2005-2014`), but a compositor can set them for its own buffers.

#### Direct scanout
- `wlr_scene` (~1935-2035) only tries it when exactly one surface is visible and its transform matches the output.
- It needs a client dmabuf (`fb.c:145`). With pixman, `wlr_linux_dmabuf_v1_create_with_renderer` advertises no formats (`wlr_linux_dmabuf_v1.c:994-1006, 1105`).
- So client direct scanout never happens in a stock pixman setup.

#### Pixman performance
- **No copy on input.** shm client buffers are used in place (`renderer.c:249-263`).
- **Opaque surfaces use SRC** (`wlr_scene.c:1471-1474`).
- **Filtering.** The default filter is bilinear (`pass.h:83-85`). Pixman downgrades it to nearest for 90/180/270° (`pixman-image.c:331-370`), and has `fast_composite_rotate_90/270_565` (`pixman-fast-path.c:1961`) for SRC 565→565.
- **So software portrait rotation is cheapest with RGB565 clients**, and wl_shm advertises RGB565. XRGB8888 clients rotated onto RGB565, or blended surfaces, fall to slow generic paths.
- **No SIMD on these CPUs.** Pixman's ARMv6 SIMD paths don't apply to ARMv4/v5; only the generic C code runs.

#### KMS feature mapping

| Feature | Status with wlroots |
|---|---|
| RGB565 output | Free after the ~5-line render-format change |
| FB_DAMAGE_CLIPS | Free (atomic only) |
| vblank-paced frames | Free: page-flip events, `DRM_CAP_CRTC_IN_VBLANK_EVENT`/`TIMESTAMP_MONOTONIC` needed (`drm.c:80-88`) |
| mq11xx hardware cursor | Free if the plane offers ARGB8888 |
| mq11xx hardware rotation | Not used; small patch (§7) |
| 2x pixel doubling | Small compositor-side code (§7) |
| C8 output / GC0C tear-free flip | New code |
| W3220 YUV overlay | New code |

#### Dependencies and footprint

**Dependencies:**
- Core (`meson.build:89-130`): wayland-server ≥1.23.1, libdrm ≥2.4.122, xkbcommon ≥1.8, pixman ≥0.43, libm, librt.
- DRM backend: hwdata (`pnp.ids`, build time only) and libdisplay-info ≥0.2 (`backend/drm/meson.build:1-27`).
- Session: libudev and libseat (seatd).
- libinput backend: libinput, which pulls in libevdev, mtdev and libudev.
- Turn off xwayland, lcms2, libliftoff, xcb-errors, gbm, egl and vulkan.

**Minimal configure line:**
```
-Dbackends=drm,libinput -Drenderers=[] -Dallocators=[] -Dxwayland=disabled
-Dexamples=false -Dcolor-management=disabled -Dlibliftoff=disabled
-Dxcb-errors=disabled -Dsession=enabled
```
- **Size:** about 77k lines of C; the stripped library is roughly 1–2 MB [estimate].

**C11 atomics:**
- Used only in `types/wlr_shm.c:6-23, 224`, for one pointer read by the SIGBUS handler.
- The build errors out unless pointer atomics are lock-free, and the code checks this again at run time.
- On ARMv4/v5, GCC implements atomics through the kernel's user helpers, so the check should pass, as it did on qemuarmv5 [unverified on SA-1110 with GCC 15.3; test with a small `atomic_is_lock_free` program].
- If it fails, the check is about 10 lines to patch out.

**Floating point:**
- Nothing per pixel; pixman uses integer and fixed-point maths.
- The scene does a few dozen soft-float operations per surface per frame (`pass.c:55-60, 127-129`), which is negligible.
- The matrix code is GLES-only.

### 2. swc + wld

**wld:**
- Falls back to dumb buffers with a pixman renderer (`drm.c:95-105`, `dumb.c:79`).
- Formats: XRGB8888 and ARGB8888 only (`wld.h:45-46`).
- About 7.9k lines.

**swc:**
- **KMS:** legacy `drmModeSetCrtc`/`drmModePageFlip` only (`primary_plane.c:60, 70`), with `drmModeSetPlane` for overlays (`plane.c:63`).
- **Damage:** no FB_DAMAGE_CLIPS and no DirtyFB. On the shadow-plane drivers, every flip copies the full frame: 17–19 ms of VLIO bus time per frame on mq11xx [unverified that legacy flips mark full damage]. It does track damage internally (`compositor.c:180-235`).
- **Formats:** the screen buffer is hard-coded to XRGB8888 (`compositor.c:155`), and shm client buffers are copied (`compositor.c:286`).
- **Protocols it has:** compositor v4, subcompositor, wl_shm v1, wl_shell, xdg_wm_base v1, xdg-decoration, KDE decoration, xdg-output, linux-dmabuf v3, wl_drm, data-device, its own swc_panel and swc_screen, and XWayland.
- **Protocols it lacks:** layer-shell, virtual keyboard, input method, text input, output transform, viewporter and touch.
- **Size and activity:** about 13.7k lines. 39 commits in 2019 and 30 in 2020, then 1–4 a year, and 14 in 2026 (portability fixes).

**Verdict:** not a good base. It would mean rewriting the KMS layer and adding most of the protocols.

### 3. Smithay

- **Pixman renderer:** `src/backend/renderer/pixman/mod.rs` (1406 lines), feature `renderer_pixman`.
  - RGB565 is the first format on little-endian (`mod.rs:50-52`).
  - Unlike wlroots, it can import dmabufs by mapping them (`mod.rs:1182-1214`).
- **Allocators:** dumb buffers (`allocator/dumb.rs`, `drm/dumb.rs`) and udmabuf (`allocator/udmabuf.rs`).
- **Damage:** FB_DAMAGE_CLIPS per plane (`drm/compositor/mod.rs:487, 2306, 3980-3998`).
- **Planes:**
  - Surfaces go on primary, cursor and overlay planes, with the `rotation` property written for them (`drm/surface/atomic.rs:1080-1101, 1251-1263, 1320, 1498`).
  - Whole-output rotation is still done in software (`compositor/mod.rs:1826`).
  - No overlays on legacy KMS (`mod.rs:14`).
- **Rust targets:**
  - `armv5te-unknown-linux-gnueabi` is Tier 2 with std (soft-float, 32-bit atomics).
  - `armv4t-unknown-linux-gnueabi` is Tier 3 (std, but you build it yourself).
  - There is no target for ARMv4 without Thumb (the SA-1110). It would need a custom target plus nightly build-std, and LLVM's ARMv4 support is lightly tested [unverified] (`platform-support.md:170-171, 310-313`).
- **Size:** about 110k lines of Rust. A compositor binary would be several MB [estimate], with no code shared with the C stack.

**Verdict:** rejected as a base, but a good reference for plane rotation and overlay assignment.

### 4. Existing small wlroots compositors

| Compositor | Size | Layer-shell | Virtual keyboard | Input method | Touch | Output transform |
|---|---|---|---|---|---|---|
| tinywl | 1101 lines | no | no | no | no | no |
| dwl (GitHub v0.5) | 2904 + 394 lines | yes | yes | no | no (patch exists) | yes (per-output rules) |
| cage | ~3.1k lines | no | yes | no | yes | no |
| labwc | ~38k lines | yes | yes | yes, plus text-input-v3 | yes | yes |
| phoc | — | yes | yes | yes | yes | yes |

- **tinywl:** xdg-shell, cursor and keyboard only. It already stacks windows and does click-to-focus.
- **dwl:** also xdg-activation, idle, session-lock, screencopy, presentation, viewporter, fractional-scale and xwayland. The Codeberg version is newer and tracks wlroots 0.18/0.19 [unverified].
- **cage:** stacks fullscreen windows, but there is no way to switch between them. Also has virtual-pointer and foreign-toplevel.
- **labwc:** a stacking window manager, too big for this role, and it needs libxml2 plus cairo/pango for decorations.
- **phoc:** needs GLES2 and `gnome-desktop-3.0` (GTK3) at build time (`meson.build:32-72`). Its sources make no GL calls, but the GNOME dependencies rule it out.
- **All of them default to XRGB8888** and need the RGB565 patch.

### 5. Supporting clients

**wvkbd** (2693 lines of C):
- **Protocols:**
  - virtual-keyboard-v1 is required (`main.c:1232-1243`).
  - input-method-v2 is optional (`main.c:1256-1259`) and only used for automatic show and hide (`main.c:527, 533`).
  - It also uses layer-shell, viewporter and fractional-scale.
- **Dependencies:** wayland-client, xkbcommon and pangocairo (`Makefile:10`). Pango and cairo are already loaded for GTK2, so the extra memory is small.
- **Rendering:** draws in ARGB8888 (`drw.c:263`); switching to RGB565 is a small patch.
- **Control signals:** SIGUSR1 hides it, SIGUSR2 shows it, SIGRTMIN toggles (`README:24, 70`).

**Other keyboards and panels:**
- squeekboard is too heavy (Rust plus GTK3).
- For the panel, write a tiny pixman-only layer-shell client or build it into the compositor. yambar and dwlb are possible models [unverified].

**GTK2 apps without text-input:**
- virtual-keyboard alone is enough. The compositor adds the virtual keyboard to the seat, and its key events reach the focused app as ordinary keyboard input.
- input-method-v2 only matters for automatic show and hide, or for composition with clients that speak text-input-v3.
- Still open: how a GDK2 Wayland backend copes with keymap changes when input alternates between the virtual and physical keyboards [unverified].

### 6. ARMv4/ARMv5 concerns

The atomics, floating-point and dependency points are covered in §1. Beyond those:
- **Session:** use seatd, or libseat's built-in mode. libudev comes from eudev or systemd-udev.
- **Touch:** libinput needs `INPUT_PROP_DIRECT` on the touchscreen. Calibration goes in the udev property `LIBINPUT_CALIBRATION_MATRIX` [unverified for the iPAQ touchscreen drivers].
- **Never use legacy KMS.** It loses the damage information.
- **pxa-lcdc buffers:** its GEM DMA dumb buffers are probably write-combined or uncached. SRC copies are fine, but blending reads the destination and will be slow, so keep windows opaque [unverified].

### 7. Using the MediaQ/W3220 features

#### a. Free with wlroots
- FB_DAMAGE_CLIPS and vblank-paced frames.
- The mq11xx hardware cursor, if the plane offers ARGB8888. On a stylus device, hiding the cursor on touch is probably better anyway.

#### b. Small patches
- **RGB565 render format:** about 5 lines in the compositor.
- **2x pixel doubling:**
  - The compositor renders into its own 120x160 swapchain (scene build-state option `swapchain`, `scene.c:2257-2264`).
  - It commits with a 240x320 destination box, which the DRM backend passes on as SRC_*/CRTC_* (`atomic.c:470-477`).
  - The kernel's test commit then decides whether the driver accepts it.
- **Hardware rotation** (about 50–150 lines):
  - Make the DRM backend able to write the primary-plane `rotation` property it already reads (`properties.c:69`).
  - The compositor keeps the output transform at "normal" but reports swapped width and height, so clients lay out in portrait, and it rotates touch input to match.
  - This removes software rotation entirely. Damage rectangles are already in framebuffer coordinates [design, unverified].

#### c. New code
- **C8 output:**
  - Map DRM C8 to pixman's 8-bit indexed format and add it to the dumb-buffer size table.
  - Load the palette through the gamma path, whose plumbing already exists (`atomic.c:340-352`).
  - Pixman picks the nearest palette colour and does not dither. Only worth it for the tear-free GC0C flip.
- **W3220 YUV overlay for video:**
  - **Getting a dmabuf without a GPU.** The client uses either:
    - udmabuf, which turns a memfd into a dmabuf (`drivers/dma-buf/udmabuf.c:262-265`; mmap, vmap and CPU access are supported), or
    - dma-heap system/CMA heaps (`heaps/system_heap.c:370-373`, `cma_heap.c:273-276`).

    Dumb buffers aren't an option for clients: there is no render node, and the primary node needs authentication.
  - **Kernel side already works.** `DRM_GEM_SHMEM_DRIVER_OPS` imports with `drm_gem_shmem_prime_import_no_map` (`drm_gem_shmem_helper.h:304-306`, `.c:948`), and vmap of an imported buffer goes through `dma_buf_vmap` (`.c:397-398`). So the copy to VRAM works, assuming w100's overlay path maps buffers through the shmem helper.
  - **wlroots side:**
    1. Create the dmabuf global with a custom format list (the overlay's YUV formats), because the pixman path fails.
    2. Put the video surface on an output layer under `WLR_DRM_FORCE_LIBLIFTOFF=1`. This needs custom glue, because the scene doesn't do it.
    3. Set the colour-key plane properties through libliftoff.
    4. If the plane test fails, either mmap the buffer (with `DMA_BUF_IOCTL_SYNC`) and convert YUV→RGB in software, or refuse.

    In total, several hundred lines.
  - **Simpler alternative:** lease the plane to the video player (`wlr_drm_lease_v1`). Full-screen only.

### Recommendation

**Build:** a purpose-built compositor of about 1.5–2.5k lines, grown from tinywl, on wlroots 0.19/0.20 with `wlr_scene` and pixman.

**Protocols:**
- layer-shell (panel and wvkbd);
- virtual-keyboard-v1;
- input-method-v2 and text-input-v3 (both optional);
- touch, viewporter, single-pixel-buffer, presentation-time;
- xdg-decoration, set to no decorations.

**Settings:** RGB565 output and nearest-neighbour filtering.

**Phase 2, only if measurements justify it:**
1. Primary-plane hardware rotation.
2. A 2x pixel-doubling mode for fullscreen video.
3. A C8 or overlay video path.

**Rejected:** swc, Smithay, phoc and labwc, for the reasons in the verdicts. cage and dwl are useful references, but not worth forking.
