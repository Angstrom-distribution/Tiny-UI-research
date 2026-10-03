# Zero-copy, hardware rotation and memory

This document describes how picowl keeps the number of pixel copies and the
resident memory low on iPAQ class hardware (about 50 MiB usable RAM, VRAM bus
about 8 MB/s that stalls the CPU). The design rule is one copy per changed pixel
after rendering.

Targets:

| Driver | Kind | Notes |
|---|---|---|
| mq11xx (MediaQ, h2210/h5550) | copy-type | shmem shadow plane; the kernel copies FB_DAMAGE_CLIPS rectangles to VRAM during the commit; primary plane has `rotation` |
| w100 (hx4700) | copy-type | as above |
| sa1100-lcdc (h3870) | copy-type | persistent buffer, as above |
| pxa-lcdc (h3970) | scanout | scans GEM DMA buffers directly from a 1 MiB CMA pool |

Every display is RGB565.

## Buffer model

picowl renders with the wlroots pixman renderer. Pixman reads client pixels
when the compositor draws, so a client buffer is "in use" until the output
commit that consumed it has been rendered (copy-type: and copied to VRAM).
For this reason `wl_buffer.release` cannot be sent early with pixman: the
renderer holds the buffer through the scene (a wlr_client_buffer lock) until
the frame that reads it is done, and the commit of the next frame may read it
again while it is still lock-held. A client that wants single buffering needs a
signal that is exact, which is what `picowl-buffer-v1` `copied` (direct
scanout only) and `retained` provide.

## picowl-buffer-v1 flow

Protocol: `protocols/picowl-buffer-v1.xml`. Server code: `src/zerocopy.c`,
serial logic: `src/copyrel.c` (pure integer state machine, unit tested).

1. The client binds `picowl_buffer_manager_v1`. It receives `format` (RGB565)
   and `copy_type` (1 = the output copies damage to device memory, 0 = scanout).
2. `create_buffer(w, h, RGB565)` allocates a compositor-owned dmabuf from the
   server allocator. The client gets `dmabuf` (fd, stride, offset, modifier)
   and `done`, or `failed(reason)`. Limits (`src/zbquota.c`, wired in
   `mgr_create_buffer`): buffers per client, then the memory of the client's
   pool, then an optional ceiling over all pools. Defaults: 3 buffers, one
   2 MiB pool shared by clients without an `[app.*]` rule. A rule selected by
   the `xdg_toplevel` app_id (see the README) has its own pool. The check runs
   before the allocation, and sizes are page-rounded real sizes (`lseek` on the
   dmabuf fd).
3. The client wraps the fd in a wl_buffer with `zwp_linux_dmabuf_v1`
   (`params.add` + `create_immed`).
4. `attach_surface(surface)` makes the compositor number the commits of that
   surface from 1 (uint32 wrap, 0 skipped).
5. With `copy_type == 1` every surface commit is answered by exactly one of
   two events (unless a newer commit supersedes it):
   - `copied(serial)`: the commit was consumed by **direct scanout** on a
     copy-type output (tracked through the surface's scene_buffer
     `output_sample` event with `direct_scanout=true`), and the present of that
     output commit has arrived (the kernel copy has finished). The client may
     draw into the same buffer again.
   - `retained(serial)`: the commit is composited (pixman samples the buffer
     now and again whenever a region over the surface is redrawn: software
     cursor, touch-hold animation, popup, OSK, unblank, rotation), or it is
     occluded, off-output, idle, on a scanout-type output, or the outputs are
     blanked. The compositor keeps the buffer as the surface's content until a
     newer commit replaces it, so the client must switch to a second buffer
     (allocated on demand) and use `wl_buffer.release`.
   `copied` is never sent for a commit shown on a non-copy-type output (pxa).
   With `copy_type == 0` the client keeps double buffering and uses
   `wl_buffer.release`.

Known limit of `copied`: the compositor takes no snapshot. When a surface that
was direct-scanned and answered `copied` has to be composited before the
client's next commit, pixman re-reads the buffer for the redrawn region and may
see a partly drawn frame; the next commit replaces it. Clients commit complete
frames. `retained` never has this problem.

The idle rule: commits which no output sampled (no damage, surface occluded or
off the output, outputs disabled or blanked) are answered `retained` instead of
waiting for a present that never comes. `pw_copyrel_on_retain` computes it; a
persistent `wl_event_source` timer (`idle_timer` in `zerocopy.c`, created at
init, re-armed with `wl_event_source_timer_update`, no per-frame allocation)
and the end of every output frame run it, once no output needs a frame. A
direct commit awaiting its present is left alone unless all outputs are off.
A client therefore never waits forever.

`pw-test-client --zerocopy` (or `PW_TEST_ZEROCOPY=1`) exercises the path and
falls back to `wl_shm` when the globals are missing (headless).

## linux-dmabuf custom feedback

The pixman renderer has no DRM fd and advertises no DMABUF texture formats, so
`wlr_linux_dmabuf_v1_create_with_renderer` cannot work. picowl builds a
`wlr_linux_dmabuf_feedback_v1` by hand (formats collected into a
`struct wlr_drm_format_set` with `wlr_drm_format_set_add`/`get`; `wlr_drm_format_add`
is not public) and calls `wlr_linux_dmabuf_v1_create(display, 4, &feedback)`.
It never calls `wlr_scene_set_linux_dmabuf_v1`. The tranche lists RGB565/LINEAR
with the SCANOUT flag.

## Direct-scanout conditions

wlroots tries direct scanout in `scene_entry_try_direct_scanout`
(`types/scene/wlr_scene.c`), only when the render list has exactly one entry
and there is no color transform. It also requires:

- no MODE/ENABLED/RENDER_FORMAT change in the pending state;
- `wlr_output_is_direct_scanout_allowed`: no visible software cursor and no
  `attach_render` locks;
- buffer transform equal to the output transform;
- `wlr_output_test_state` accepts the dmabuf: `drm_fb_import` against the
  primary plane formats (RGB565, LINEAR).

picowl helps by disabling the panel scene node while an app is fullscreen
(`panel_autohide`, `pw_panel_update`), so the list has one entry. The overlay,
OSK layer, background layer still being rendered, or a visible software cursor
(for example during the hold animation) force the composition path.
`WLR_SCENE_DISABLE_DIRECT_SCANOUT=1` forces composition; the test suite is run
both ways.

## Composition fallback

When scanout is not possible the pixman renderer composes into the output
buffer. Patch 0001 lets it read DMA-BUF client buffers: it `mmap`s a
single-plane LINEAR (or INVALID modifier) dmabuf once with MAP_SHARED, caches the
mapping per buffer (unmapped on the buffer destroy event and on renderer
destroy), and brackets each access with `DMA_BUF_IOCTL_SYNC` START/END
(ENOTTY, EINVAL and ENOSYS count as success, EINTR is retried). Patch 0002 adds
fast paths: same-format, unscaled, untransformed textures are copied with one
row `memcpy` per clip rectangle; opaque rectangles use `pixman_fill`.

## Copy-type detection and override

`src/output.c` reads the driver name with `drmGetVersion` and calls
`pw_copytype_resolve` (`src/copytype.c`): mq11xx, w100 and sa1100-lcdc are
copy-type, pxa-lcdc and everything else is not. The `[copytype]` config section
overrides per output (`auto | yes | no`). The result is passed to wlroots with
`wlr_drm_connector_set_copy_type` (patch 0003), and reported to clients through
the `copy_type` event. The log line is
`output NAME: driver 'X' copy_type=yes|no rotation_mode=...`.

## Single-buffer swapchain

On a copy-type output the kernel copies out of the framebuffer during the
commit, so one persistent buffer is enough. With `[zerocopy] single_buffer`
(default true), an output that is enabled, renders RGB565 and has
RGB565/LINEAR in the primary formats gets a picowl-owned `wlr_swapchain` with
one slot, passed as `swapchain` in the commit options. If the allocator or the
renderer needs a second slot picowl logs once per output, at INFO,
"single-buffer degraded to 2 slots" and carries on with 2 slots. If the
swapchain cannot be created at all, one INFO line per output, "single-buffer off, using 2 slots: <reason>",
names the reason.

Patch 0003 makes this safe: the page-flip handler releases the scanned-out fb
(`drm_fb_clear(&plane->current_fb)` after the existing `drm_fb_move`, only when `conn->copy_type`), `drm_atomic_connector_prepare`
(FB_DAMAGE_CLIPS) and `pick_max_bpc` guard against `primary_fb == NULL`, and the
copy-type drop is atomic only.

Swapchain replacement rule (`copy_swapchain_sync`): create the new swapchain
first. Destroy the old one only when the output was disabled in between, or
after one frame has been committed from the new swapchain. If that commit fails,
disable the output, destroy the old swapchain and re-enable. This matters
because `drm_fb_destroy` falls back to `drmModeRmFB` when `CloseFB` returns
EINVAL, and RmFB disables the plane on kernels without CLOSEFB. For the same
reason `output_new` enables `copy_type` automatically only when a probe
(`DRM_IOCTL_MODE_CLOSEFB` on fb 0: ENOENT = present) succeeds; a forced
`copytype = yes` only logs an error. wlroots' own swapchain, allocated by the
buffer-less enabling commits, is destroyed once `copy_swapchain` exists
(`copy_drop_core_swapchain`), so a copy-type output holds one framebuffer.

## Hardware rotation

For a plane with the `rotation` property (MediaQ), the display scans a
landscape 320x240 framebuffer for a 240x320 portrait mode with 90/270, so the
rotated image is produced by the display controller, not by the CPU.

- Patch 0003 adds `wlr_drm_connector_supports_hw_rotation` (iterates the
  possible CRTCs, no CRTC allocation), `wlr_drm_connector_set_hw_rotation`, and
  `set_copy_type`. The transform to DRM rotation bits table has a comment about
  possibly swapping the 90 and 270 rows (kernel convention to be confirmed on
  hardware).
- `set_hw_rotation` is legal only while the output is disabled. It swaps the
  mode dimensions, sets `output->current_mode = NULL` and zeroes
  `output->width`, `height` and `refresh`; the following enabling commit with the mode sets the real size. The NULL `current_mode` is essential:
  `output_compare_state` compares the mode pointer, and `wlr_output_commit_state`
  strips unchanged fields, so re-committing the same mode would be a no-op and
  the swap would never reach the kernel.
- The property is set in `atomic_connector_add`; CRTC width/height and the
  primary plane destination box are swapped to match; a portrait fb with
  ROTATE_90 would be rejected with -ERANGE.
- Hardware cursors are refused while a rotation is set (the cursor plane has no
  rotation), so picowl's hold animation uses the software cursor path.
- picowl: `pw_output_rotate` disables the output, sets the rotation, enables it
  with the mode and a NORMAL transform. If that commit fails it logs "hw
  rotation commit failed" and re-enables the output with software (renderer)
  rotation. A driver that accepts the rotation property and ignores it is not
  detected: the output size comes from the swapped mode, not from the
  kernel. `[rotation]` selects `auto | hardware | software` per output;
  `hardware` logs an error when unsupported and falls back.
- Touch calibration: `pw_input_apply_rotation` (`src/input.c`, needs libinput)
  sets the libinput calibration matrix from `pw_rot_touch_matrix` when hardware
  rotation is in effect, composed with the device's default calibration (`libinput_device_config_
  calibration_get_default_matrix`, e.g. the udev `LIBINPUT_CALIBRATION_MATRIX`
  of resistive panels, read once when the device is added). Without hardware
  rotation the default matrix is restored exactly. Touch devices are mapped to
  the output with `wlr_cursor_map_input_to_output`, so for software rotation
  wlr_cursor applies the output transform to the touch coordinates.

## The three patches and their wiring

Kept in `subprojects/packagefiles/wlroots/` and `oe/recipes-graphics/wlroots/files/`
(byte identical, checked with `diff -r`).

1. `0001-pixman-read-dmabuf-client-buffers-via-mmap.patch`
2. `0002-pixman-pass-memcpy-and-fill-fast-paths.patch`
3. `0003-drm-hardware-rotation-and-copy-type.patch`

Meson: `subprojects/wlroots.wrap` has `diff_files = wlroots/0001..., 0002..., 0003...`.
OpenEmbedded: `wlroots_0.19.0.bb` has `FILESEXTRAPATHS:prepend := "${THISDIR}/files:"`
and three `file://` entries in `SRC_URI`. They apply in order on a pristine
0.19.0 (13a62a23) export with both `patch -p1` and `git apply`. A host prefix
must be rebuilt with them for picowl to link (`wlr_drm_connector_set_hw_rotation`,
`wlr_drm_connector_set_copy_type`, `wlr_drm_connector_supports_hw_rotation`).

## Memory tuning

`src/mem.c`: `pw_mem_init` runs first in `main` (defaults) and again after the
config is loaded; it sets `M_ARENA_MAX`, `M_TRIM_THRESHOLD`, `M_MMAP_THRESHOLD`
and `M_TOP_PAD` with `mallopt`. `pw_mem_trim` calls `malloc_trim(0)` once after
startup (idle callback, `trim_after_start`). `pw_mem_log_status` logs VmRSS and
VmHWM from `/proc/self/status` at startup and at exit (stack buffer, no stdio).
See `[memory]` in the README.

Measured headless (WLR_BACKENDS=headless, pixman, 1280x720, test client mapped),
VmHWM:

| State | VmHWM |
|---|---|
| Baseline, before this work | 9440 kB |
| After (three `meson test rss` runs) | 9528, 9532, 9544 (and 9576 in a full-suite run) kB |

Ceiling: the `rss_ceiling_kb` meson option defaults to
`ceil(9576 * 1.25 / 512) * 512 = 12288` kB (was 16384). Headless has no DRM
buffers, so the single-buffer swapchain and CMA/VRAM effects are not measured
here; they are in the hardware checklist.

## Per-frame allocation audit

Rule: no `malloc`/`calloc`/`strdup` reachable from `output_frame`, the present
handler, surface commit handlers or timer callbacks. Result per file
(`grep` of the allocation calls; remaining ones are in setup/teardown paths):

| File | Result |
|---|---|
| `src/zerocopy.c` | one `calloc` in the `create_buffer` request handler (client-driven, bounded by the buffer count and pool limits, 3 buffers and 2 MiB by default; the rule lookup walks the xdg-shell clients and allocates nothing); commit, timer, present: none |
| `src/copyrel.c`, `src/copytype.c`, `src/rotate.c` | none (pure logic) |
| `src/mem.c` | none |
| `src/output.c` | one `calloc` in `output_new` (hotplug); `output_frame`, present: none. A swapchain is created only on enable, rotate and unblank, not per frame |
| `src/input.c` | `calloc` only in device add (keyboard, pointer, touch); event handlers none |
| `src/layer.c`, `src/view.c` | `calloc` only at surface/popup creation; focus, arrange, panel update: none |
| `src/server.c` | one `calloc` per decoration object creation |

The pixman render pass (patches 0001/0002) was checked in `tests/test-pixman-pass.c`
by interposing `malloc`/`calloc`/`realloc`: the opaque rect loop and the texture
fast path loop make 0 allocations.

### KNOWN CORE COST

The pixman texture has no `update_from_buffer`, and
`wlr_client_buffer_apply_damage` (`types/buffer/client.c` L136) returns false
whenever `n_locks - n_ignore_locks > 1`, which is always the case while the scene
holds a lock. So `surface_apply_damage` in `types/wlr_compositor.c` creates a new
`wlr_client_buffer` (calloc) plus a pixman image for every client commit, shm and
dmabuf alike. This is a wlroots core cost and is not fixed here. A possible
follow-up patch: implement `update_from_buffer` for the pixman texture (keep the
image, re-point it at the new data pointer) and let the scene release its lock
before the apply, so commits recycle the client buffer.

## Hardware-only checklist

None of the DRM paths can run in the build container (no /dev/dri, no vkms); they
are compiled and unit tested only. On a device verify:

- the kernel has CLOSEFB (Linux 6.9 or later or a backport; the log says "copy_type disabled" otherwise). Without it
  `drm_fb_destroy` falls back to `drmModeRmFB`, which disables the plane, so
  swapchain replacement and rotation changes flicker or leave the screen dead;
- mq11xx: `rotation` property present on the primary plane, whether 90 and 270 are
  swapped relative to the table in patch 0003, landscape fb accepted, no -ERANGE;
- FB_DAMAGE_CLIPS is honoured (only the clip rectangles reach VRAM; check with a
  small damage and the bus time);
- the hardware rotation commit succeeds (no "hw rotation commit failed" log);
- touch calibration after each rotation (corners map correctly);
- single-buffer swapchain: no tearing, "degraded to 2 slots" not logged (INFO, once per output; also check there is no "single-buffer off" line);
- direct scanout on pxa-lcdc from the 1 MiB CMA pool (a full RGB565 240x320 frame
  is 150 KiB; picowl-buffer budget 2 MiB must fit CMA together with the
  framebuffer); the software cursor is the only thing that blocks it;
- VmHWM on device against the headless numbers above;
- buffer limits (`rule_for_client` and `exe` matching need real clients and a
  DRM allocator, so no CI): without a rule `pw-test-client --zerocopy-count 7`
  is granted 3 and the debug log says "over count"; with
  `[app.picowl-test-client] zerocopy_buffers = 7` it gets 7 and, on the hx4700,
  `Shmem` in `/proc/meminfo` grows by about 4200 KiB and returns to the
  baseline on exit; a second client with `--app-id picowl-test-client` while
  the first holds 7 gets none (shared pool); with `exe =` set, the same
  app_id from another binary gets the default limits; on the h3970 a rule of 7
  is granted what CMA allows, then `no_memory`, and picowl keeps rendering
  (check dmesg for CMA warnings; unblank and rotate still work).

## C8 / 8-bpp palettised output on MediaQ

DRM_FORMAT_C8 is 8 bpp with a palette. A 240x320 frame is 76.8 KB instead of
153.6 KB in RGB565. Over the ~8 MB/s VRAM bus a full-frame copy drops from about
19 ms to about 9.6 ms, and the CPU stall time halves for every damaged pixel. The
MediaQ graphics controller has 8-bpp palette modes; whether the mq11xx driver
exposes a C8 plane format and a palette (GAMMA_LUT) must be checked first.
The price is 256 colours, so only UI-only or kiosk configurations should use it.
Direct scanout of C8 client buffers is not useful, clients render RGB565.

Options:

a. In-kernel conversion in the mq11xx FB_DAMAGE_CLIPS copy path (preferred).
   The driver already copies the damage rectangles from the shmem shadow plane to
   VRAM during the commit. It keeps advertising RGB565 to userspace, programs the
   controller to 8 bpp with a fixed 3-3-2 palette (or a 6x6x6 cube plus greys),
   and converts each pixel in the same copy loop with a 64 KiB lookup table or
   shifts, optionally with a 4x4 ordered dither. This is the same single pass, so
   there is no extra RAM pass and the compositor does not change. Make it a
   module option.
b. A userspace stage in picowl. Pixman still renders RGB565 into RAM; picowl
   converts the damaged boxes into a C8 dumb buffer that is the primary-plane
   framebuffer, committed with FB_DAMAGE_CLIPS. Needs DRM_FORMAT_C8 in
   `plane->formats`, the CRTC GAMMA_LUT as the palette
   (`wlr_output_state_set_gamma_lut`, 256 entries) and a picowl-owned
   swapchain, because `wlr_output_pick_format` cannot pick C8 (the pixman format
   table in `render/pixman/pixel_format.c` has no C8 entry). Costs one extra RAM
   read and write pass over the damage, small compared to VRAM.
c. Teach the pixman renderer C8 (`PIXMAN_c8` with an indexed palette). Most
   invasive and slowest (pixman's c8 store searches the palette). Not
   recommended.

None of these is implemented.
