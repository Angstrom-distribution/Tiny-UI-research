# Zero-copy, hardware rotation and memory

Reference for how picowl keeps the number of pixel copies and the resident memory low on iPAQ class hardware (about 50 MiB usable RAM, a VRAM bus of about 8 MB/s that stalls the CPU): the buffer model, `picowl-buffer-v1` on the compositor side, direct scanout, copy-type detection, the caching event, the single-buffer swapchain, hardware rotation, the five wlroots patches, memory tuning and the allocation audit. The client side of the protocol is in [buffers.md](buffers.md). It ends with a hardware-only checklist, which has mostly not been run on a board (see "Hardware-only checklist" for what has). References to wlroots files and lines are against wlroots 0.19.0. The design rule is one copy per changed pixel after rendering.

Targets:

| Driver | Kind | `caching` (client buffers) | Notes |
|---|---|---|---|
| mq11xx (MediaQ, h2200/h2210/h5550) | copy-type | cacheable | shmem shadow plane; the kernel copies FB_DAMAGE_CLIPS rectangles to VRAM during the commit; primary plane has `rotation` |
| w100 (hx4700) | copy-type | cacheable | as above |
| sa1100-lcdc (h3870) | copy-type | write_combined | persistent buffer, as above, but CMA (per `mediaplayer-integration.md` and a comment in `src/copytype.c`; not verified, see the checklist) |
| pxa-lcdc (h3970) | scanout | write_combined | scans GEM DMA buffers directly from a 1 MiB CMA pool (from the kernel port's notes; not verified here) |
| anything else | scanout | write_combined | the safe default |

Every display is RGB565.

## Buffer model

picowl renders with the wlroots pixman renderer. Pixman reads client pixels when the compositor draws, so a client buffer is "in use" until the output commit that consumed it has been rendered (copy-type: and copied to VRAM). For this reason `wl_buffer.release` cannot be sent early with pixman: the renderer holds the buffer through the scene (a wlr_client_buffer lock) until the frame that reads it is done, and the commit of the next frame may read it again while it is still lock-held. A client that wants single buffering needs a signal that is exact, which is what `picowl-buffer-v1` `copied` (direct scanout only) and `retained` provide.

## picowl-buffer-v1 flow

Protocol: `protocols/picowl-buffer-v1.xml` (version 2). Server code: `src/zerocopy.c`, serial logic: `src/copyrel.c` (pure integer state machine, unit tested), bind events: `src/zbproto.c`, limits: `src/zbquota.c`.

1. The client binds `picowl_buffer_manager_v1`. It receives `format` (RGB565), `copy_type` (1 = the output copies damage to device memory, 0 = scanout) and, if it bound version 2, `caching`.
2. `create_buffer(w, h, RGB565)` allocates a compositor-owned dmabuf from the server allocator. The client gets `dmabuf` (fd, stride, offset, modifier) and `done`, or `failed(reason)`. A format other than RGB565 gives `unsupported_format`. A width or height of 0 or below, or above the width or height of the largest output, gives `too_large`. Limits (`src/zbquota.c`, wired in `mgr_create_buffer`): buffers per client, then the memory of the client's pool, then an optional ceiling over all pools, each over its limit answered with `no_memory`. Defaults: 3 buffers, one 2 MiB pool shared by clients without an `[app.*]` rule, no ceiling. A rule selected by the `xdg_toplevel` app_id (see the [README](../README.md#configuration)) has its own pool; a rule with `exe` set only matches a client whose `/proc/<pid>/exe` is that file. The check runs before the allocation, and sizes are page-rounded real sizes (`lseek` on the dmabuf fd). A failed allocation also gives `no_memory`.
3. The client wraps the fd in a wl_buffer with `zwp_linux_dmabuf_v1` (`params.add` + `create_immed`). picowl's `check_dmabuf` accepts a single-plane RGB565 buffer with a LINEAR or INVALID modifier, a stride of at least width times 2, stride and offset multiples of 4, and both sides at most the larger side of the largest output (either orientation).
4. `attach_surface(surface)` makes the compositor number the commits of that surface from 1 (uint32 wrap, 0 skipped).
5. With `copy_type == 1` every surface commit is answered by exactly one of two events (unless a newer commit supersedes it):
   - `copied(serial)`: the commit was consumed by **direct scanout** on a copy-type output (tracked through the surface's scene_buffer `output_sample` event with `direct_scanout=true`), exactly one output is enabled, and the present of that output commit has arrived (the kernel copy has finished). The client may draw into the same buffer again.
   - `retained(serial)`: the commit is composited (pixman samples the buffer now and again whenever a region over the surface is redrawn: software cursor, touch-hold animation, popup, OSK, unblank, rotation), or it is occluded, off-output, idle, on a scanout-type output, shown by more than one enabled output, or the outputs are blanked. The compositor keeps the buffer as the surface's content until a newer commit replaces it, so the client must switch to a second buffer (allocated on demand) and use `wl_buffer.release`.

   `copied` is never sent for a commit shown on a non-copy-type output (pxa). With `copy_type == 0` the client keeps double buffering and uses `wl_buffer.release`.

Known limit of `copied`: the compositor takes no snapshot. When a surface that was direct-scanned and answered `copied` has to be composited before the client's next commit, pixman re-reads the buffer for the redrawn region and may see a partly drawn frame; the next commit replaces it. Clients commit complete frames. `retained` never has this problem.

The idle rule: commits which no output sampled (no damage, surface occluded or off the output, outputs disabled or blanked) are answered `retained` instead of waiting for a present that never comes. `pw_copyrel_on_retain` computes it; a persistent `wl_event_source` timer (`idle_timer` in `zerocopy.c`, created at init, re-armed with `wl_event_source_timer_update`, no per-frame allocation) and the end of every output frame run it, once no output needs a frame. A direct commit awaiting its present is left alone unless all outputs are off. A client therefore never waits forever.

`pw-test-client --zerocopy` (or `PW_TEST_ZEROCOPY=1`) exercises the path and falls back to `wl_shm` when the globals are missing (headless).

## linux-dmabuf custom feedback

The pixman renderer has no DRM fd and advertises no DMABUF texture formats, so `wlr_linux_dmabuf_v1_create_with_renderer` cannot work. picowl builds a `wlr_linux_dmabuf_feedback_v1` by hand (formats collected into a `struct wlr_drm_format_set` with `wlr_drm_format_set_add`/`get`; `wlr_drm_format_add` is not public) and calls `wlr_linux_dmabuf_v1_create(display, 4, &feedback)`. It never calls `wlr_scene_set_linux_dmabuf_v1`. The tranche lists RGB565/LINEAR with the SCANOUT flag.

## Direct-scanout conditions

wlroots tries direct scanout in `scene_entry_try_direct_scanout` (`types/scene/wlr_scene.c`), only when the render list has exactly one entry and there is no color transform. It also requires:

- no MODE/ENABLED/RENDER_FORMAT change in the pending state;
- `wlr_output_is_direct_scanout_allowed`: no visible software cursor and no `attach_render` locks;
- buffer transform equal to the output transform;
- `wlr_output_test_state` accepts the dmabuf: `drm_fb_import` against the primary plane formats (RGB565, LINEAR).

With a hardware rotation set, patch 0003 also refuses a buffer whose destination box is not the whole output, so only a full-output buffer is scanned out (see "Hardware rotation").

picowl helps by disabling the panel scene node while a toplevel is focused (`[zerocopy] panel_autohide`, default true; `pw_panel_update` in `src/layer.c`; every toplevel is maximized), so the list has one entry. The overlay, OSK layer, background layer still being rendered, or a visible software cursor (for example during the hold animation) force the composition path. `WLR_SCENE_DISABLE_DIRECT_SCANOUT=1` forces composition. No test in this repository sets it.

## Composition fallback

When scanout is not possible the pixman renderer composes into the output buffer. Patch 0001 lets it read DMA-BUF client buffers: it `mmap`s a single-plane LINEAR (or INVALID modifier) dmabuf once with MAP_SHARED, caches the mapping per buffer (unmapped on the buffer destroy event and on renderer destroy), and brackets each access with `DMA_BUF_IOCTL_SYNC` START/END (ENOTTY, EINVAL and ENOSYS count as success, EINTR is retried). Patch 0002 adds fast paths: same-format, unscaled, untransformed textures are copied with one row `memcpy` per clip rectangle; opaque rectangles use `pixman_fill`.

## Copy-type detection and override

`src/output.c` (`output_new`) reads the driver name with `drmGetVersion` and calls `pw_copytype_resolve` (`src/copytype.c`). Copy-type driver names, compared case-insensitively: `mq11xx`, `mediaq`, `w100`, `imageon`, `sa1100-lcdc`, `sa1100_lcdc`, `sa11x0-lcdc`, `sa1100`. `pxa-lcdc` and everything else are not. The `[copytype]` config section overrides per output (`<output> = auto | yes | no`; `yes` also reads `true`, `1`, `on`, and `no` also `false`, `0`, `off`). Two conditions switch copy-type off or warn:

- The kernel lacks `DRM_IOCTL_MODE_CLOSEFB` (Linux 6.9 or a backport; `drm_closefb_supported`, probed by closing fb 0, where ENOENT means present). A detected copy-type output then runs with copy-type off and logs `copy_type disabled` at info level. A forced `copytype = yes` only logs an error and stays on, because destroying a scanned-out client buffer can blank the display.
- `WLR_DRM_NO_ATOMIC=1` is set: wlroots ignores copy-type without atomic modesetting, so picowl turns it off (info line `copy_type disabled: legacy KMS`).

The result is passed to wlroots with `wlr_drm_connector_set_copy_type` (patch 0003), and reported to clients through the `copy_type` event. The log line is `output NAME: driver 'X' copy_type=yes|no rotation_mode=auto|hardware|software`.

## Caching event

picowl-buffer-v1 version 2 adds `caching` to the manager: once, on bind, after `copy_type`. It tells whether the CPU mapping of the dmabufs is cacheable (`1`) or write-combined or uncached (`0`), so that a client knows if it may read back from a buffer or decode into one. It is not derived from `copy_type`, which describes the outputs: an old kernel without CLOSEFB, a `[copytype]` override or a runtime change of `copy_type` does not change the memory.

`pw_zerocopy_init` reads the driver name of the backend's DRM fd with `drmGetVersion` (picowl has one allocator, so one value) and `pw_caching_resolve` (`src/copytype.c`) maps it with the table above: `cacheable` for `mq11xx`, `mediaq`, `w100` and `imageon`, `write_combined` for everything else (the `[zerocopy] caching` override is `auto | cacheable | write_combined`). The value is fixed for the life of the process. `pw_zbproto_send_bind` (`src/zbproto.c`) sends `format`, `copy_type` and `caching`, and sends `caching` only to managers bound at version 2 or later: libwayland does not check event versions on the server side, and a version 1 client would be disconnected or abort. The log line is `zero-copy enabled (copy_type=N caching=cacheable|write_combined driver 'D')`. An unknown driver, or a failed `drmGetVersion`, gives `write_combined`, which is wrong only in the safe direction (one extra pass in the player instead of slow reads); `[zerocopy] caching = cacheable` fixes it. If the table says cacheable for memory that is really write-combined, the output stays correct but every read-back is slow, which `pw-test-client --zerocopy --readback` shows. `caching` does not change the commit and answer rules (`copied`, `retained`). The CPU cache coherency of the driver's damage copy is the kernel driver's job, as before.

## Single-buffer swapchain

On a copy-type output the kernel copies out of the framebuffer during the commit, so one persistent buffer is enough. With `[zerocopy] single_buffer` (default true), a copy-type output that is enabled, renders RGB565 and has RGB565/LINEAR in the primary formats gets a picowl-owned `wlr_swapchain` with one slot, passed as `swapchain` in the commit options (info line `output NAME: single-buffer swapchain WxH`). If the allocator or the renderer needs a second slot picowl logs once per output, at INFO, "single-buffer degraded to 2 slots" and carries on with 2 slots. If the swapchain cannot be created at all, one INFO line per output, "single-buffer off, using 2 slots: <reason>", names the reason.

Patch 0003 makes this safe: the page-flip handler releases the scanned-out fb (`drm_fb_clear(&plane->current_fb)` after the existing `drm_fb_move`, only when `conn->copy_type`), `drm_connector_prepare`, `drm_atomic_connector_prepare` (FB_DAMAGE_CLIPS) and `pick_max_bpc` tolerate `primary_fb == NULL`, and the copy-type drop is atomic only.

Swapchain replacement rule (`copy_swapchain_sync`): create the new swapchain first. Destroy the old one only when the output was disabled in between, or after one frame has been committed from the new swapchain. If that commit fails, disable the output, destroy the old swapchain and re-enable. This matters because `drm_fb_destroy` falls back to `drmModeRmFB` when `CloseFB` returns EINVAL, and RmFB disables the plane on kernels without CLOSEFB. For the same reason `output_new` enables `copy_type` automatically only when the CLOSEFB probe succeeds (see "Copy-type detection and override"). wlroots' own swapchain, allocated by the buffer-less enabling commits, is destroyed once `copy_swapchain` exists (`copy_drop_core_swapchain`), so a copy-type output holds one framebuffer.

## Hardware rotation

For a plane with the `rotation` property (MediaQ), the display scans a landscape 320x240 framebuffer for a 240x320 portrait mode with 90/270, so the rotated image is produced by the display controller, not by the CPU. Hardware rotation is the default (`[rotation] <output> = auto|hardware|software`, see the [README](../README.md#configuration)). The measurements against software rotation are in [rotation-results.md](rotation-results.md).

- Patch 0003 adds `wlr_drm_connector_supports_hw_rotation` (iterates the possible CRTCs, no CRTC allocation), `wlr_drm_connector_set_hw_rotation`, and `set_copy_type`. The transform to DRM rotation bits table has a comment about possibly swapping the 90 and 270 rows (kernel convention). The 90 degree row works on an h2200 (the plane `rotation` property read 2 in every hardware run); 270 has not been measured.
- `set_hw_rotation` is legal only while the output is disabled. It swaps the mode dimensions, sets `output->current_mode = NULL` and zeroes `output->width`, `height` and `refresh`; the following enabling commit with the mode sets the real size. The NULL `current_mode` is essential: `output_compare_state` compares the mode pointer, and `wlr_output_commit_state` strips unchanged fields, so re-committing the same mode would be a no-op and the swap would never reach the kernel.
- The rotation property is written in `atomic_connector_add`; CRTC width/height and the primary plane destination box are swapped to match, while SRC and FB_DAMAGE_CLIPS stay in framebuffer coordinates. Without the swap a portrait fb with ROTATE_90 would be rejected (reported as -ERANGE; not verified here).
- Hardware cursors are refused while a rotation is set (the cursor plane has no rotation), so picowl's hold animation uses the software cursor path.
- picowl: `output_enable_rotated` (`src/output.c`; initial setup, unblank and runtime rotation through `pw_output_rotate`) disables the output first, because the plane rotation can only change while it is disabled, then calls `set_hw_rotation` and enables it with the mode and a NORMAL transform. If that commit fails it logs `hw rotation commit failed, using software rotation` and enables the output with software (renderer) rotation. A driver that accepts the rotation property and ignores it is not detected: the output size comes from the swapped mode, not from the kernel. `[rotation]` selects `auto | hardware | software` per output; `hardware` logs an error when unsupported and falls back. Under hardware rotation `wl_output` advertises the transform `normal` and the rotated mode; `picowl-rotation-v1` tells clients the real transform (see the [README](../README.md#protocols)).
- Touch calibration: `pw_input_apply_rotation` (`src/input.c`, needs libinput) sets the libinput calibration matrix from `pw_rot_touch_matrix` when hardware rotation is in effect, composed with the device's base calibration (a `[touch] calibration`, a tslib pointercal or the udev `LIBINPUT_CALIBRATION_MATRIX`, chosen once when the device is added; a touch device with none is disabled, see the [README](../README.md#touch-input)). Without hardware rotation the base matrix is restored exactly. Touch devices are mapped to the output with `wlr_cursor_map_input_to_output`, so for software rotation wlr_cursor applies the output transform to the touch coordinates.

## The five patches and their wiring

Kept in `subprojects/packagefiles/wlroots/` and `oe/recipes-graphics/wlroots/files/` (byte identical, checked with `cmp`; see the patch [README](../subprojects/packagefiles/wlroots/README.md)).

1. `0001-pixman-read-dmabuf-client-buffers-via-mmap.patch`
2. `0002-pixman-pass-memcpy-and-fill-fast-paths.patch`
3. `0003-drm-hardware-rotation-and-copy-type.patch`
4. `0004-drm-lease-overlay-planes.patch` (DRM lease, see [lease.md](lease.md))
5. `0005-scene-update-a-buffer-node-when-its-opacity-changes.patch` (a translucent layer surface over an existing window)

Meson: `subprojects/wlroots.wrap` has `diff_files = wlroots/0001..., 0002..., 0003..., 0004..., 0005...`. OpenEmbedded: `wlroots_0.19.0.bb` has `FILESEXTRAPATHS:prepend := "${THISDIR}/files:"` and five `file://` entries in `SRC_URI`. They apply in order on a pristine 0.19.0 (13a62a23) export with both `patch -p1` and `git apply`. A host prefix must be rebuilt with them for picowl to link (`wlr_drm_connector_set_hw_rotation`, `wlr_drm_connector_set_copy_type`, `wlr_drm_connector_supports_hw_rotation`, and the `WLR_DRM_LEASE_OVERLAY_PLANES` macro that `meson.build` checks).

## Memory tuning

`src/mem.c`: `pw_mem_init` runs first in `main` (defaults) and again after the config is loaded; it sets `M_ARENA_MAX`, `M_TRIM_THRESHOLD`, `M_MMAP_THRESHOLD` and `M_TOP_PAD` with `mallopt`. `pw_mem_trim` calls `malloc_trim(0)` once after startup (idle callback, `trim_after_start`). `pw_mem_log_status` logs VmRSS and VmHWM from `/proc/self/status` at startup and at exit (stack buffer, no stdio). See `[memory]` in the [README](../README.md#configuration).

Measured headless (WLR_BACKENDS=headless, pixman, 1280x720, test client mapped), VmHWM:

| State | VmHWM |
|---|---|
| Baseline, before this work | 9440 kB |
| After (three `meson test rss` runs) | 9528, 9532, 9544 (and 9576 in a full-suite run) kB |

Ceiling: the `rss_ceiling_kb` meson option defaults to `ceil(9576 * 1.25 / 512) * 512 = 12288` kB (was 16384). Headless has no DRM buffers, so the single-buffer swapchain and CMA/VRAM effects are not measured here; they are in the hardware checklist.

## Per-frame allocation audit

Rule: no `malloc`/`calloc`/`strdup` reachable from `output_frame`, the present handler, surface commit handlers or timer callbacks. Result per file (`grep` of the allocation calls; remaining ones are in setup/teardown paths):

| File | Result |
|---|---|
| `src/zerocopy.c` | one `calloc` in the `create_buffer` request handler (client-driven, bounded by the buffer count and pool limits, 3 buffers and 2 MiB by default; the rule lookup walks the xdg-shell clients and allocates nothing); commit, timer, present: none |
| `src/copyrel.c`, `src/copytype.c`, `src/rotate.c` | none (pure logic) |
| `src/mem.c` | none |
| `src/output.c` | one `calloc` in `output_new` (hotplug); `output_frame`, present: none. A swapchain is created only on enable, rotate and unblank, not per frame |
| `src/input.c` | `calloc` only in device add (keyboard, pointer, touch); event handlers none |
| `src/layer.c`, `src/view.c` | `calloc` only at surface/popup creation; focus, arrange, panel update: none |
| `src/server.c` | one `calloc` per decoration object creation |
| `src/idle.c` | one `calloc` per idle inhibitor (client-driven) |

The pixman render pass (patches 0001/0002) was checked in `tests/test-pixman-pass.c` by interposing `malloc`/`calloc`/`realloc`: the opaque rect loop and the texture fast path loop make 0 allocations. The audit table was written for the earlier code; the `src/idle.c` row was added from reading `handle_new_inhibitor`, the other rows were not re-checked.

### Known core cost

The pixman texture has no `update_from_buffer`, and `wlr_client_buffer_apply_damage` (`types/buffer/client.c` L136, wlroots 0.19.0) returns false whenever `n_locks - n_ignore_locks > 1`, which is always the case while the scene holds a lock. So `surface_apply_damage` in `types/wlr_compositor.c` creates a new `wlr_client_buffer` (calloc) plus a pixman image for every client commit, shm and dmabuf alike. This is a wlroots core cost and is not fixed here. The idea for a fix is in the [roadmap](design/roadmap.md).

## Hardware-only checklist

None of the DRM paths can run in the headless tests (no `/dev/dri`): they are compiled and unit tested only, and the one DRM-backend test (`lease-vkms`) is opt-in and has no recorded run. Only a part of this list has been run on a board, on one h2200 (PXA255, mq11xx), in [rotation-results.md](rotation-results.md): hardware rotation at the 90 degree transform with a full-surface and a 16x16 client, compared with software rotation, including the plane's `rotation` property reading 2 and no discarded frames. Everything else below has not been run on any board. Rotation results that are still open (270 degrees, touch at each transform, other boards, the media player workload) are in the [roadmap](design/roadmap.md). On a device verify:

- the kernel has CLOSEFB (Linux 6.9 or later or a backport; the log says "copy_type disabled" otherwise). Without it `drm_fb_destroy` falls back to `drmModeRmFB`, which disables the plane, so swapchain replacement and rotation changes flicker or leave the screen dead;
- mq11xx: `rotation` property present on the primary plane (seen on the h2200, value 2 at 90 degrees), whether 270 is right relative to the table in patch 0003 (not measured), landscape fb accepted, no -ERANGE;
- FB_DAMAGE_CLIPS is honoured (only the clip rectangles reach VRAM; check with a small damage and the bus time; rotation-results.md reports uploaded bytes per second for two workloads, not this check);
- the hardware rotation commit succeeds (no "hw rotation commit failed" log): seen at 90 degrees on the h2200, where hardware rotation was applied in every hardware run;
- touch calibration after each rotation (corners map correctly), not covered by rotation-results.md;
- single-buffer swapchain: no tearing, "degraded to 2 slots" not logged (INFO, once per output; also check there is no "single-buffer off" line);
- direct scanout on pxa-lcdc from the 1 MiB CMA pool (a full RGB565 240x320 frame is 150 KiB; picowl-buffer budget 2 MiB must fit CMA together with the framebuffer); the software cursor is the only thing that blocks it;
- VmHWM on device against the headless numbers above;
- buffer limits (`rule_for_client` and `exe` matching need real clients and a DRM allocator, so no CI): without a rule `pw-test-client --zerocopy-count 7` is granted 3 and the debug log says "over count"; with `[app.picowl-test-client] zerocopy_buffers = 7` it gets 7 and, on the hx4700, `Shmem` in `/proc/meminfo` grows by about 4200 KiB and returns to the baseline on exit; a second client with `--app-id picowl-test-client` while the first holds 7 gets none (shared pool); with `exe =` set, the same app_id from another binary gets the default limits; on the h3970 a rule of 7 is granted what CMA allows, then `no_memory`, and picowl keeps rendering (check dmesg for CMA warnings; unblank and rotate still work).
- caching event: the log line `zero-copy enabled (copy_type=... caching=... driver '...')` shows h2200, h2210, h5550 and hx4700 `caching=cacheable`, h3870 and h3970 `caching=write_combined` (and `copy_type=1` for all but the h3970); `pw-test-client --zerocopy --readback` prints a `caching=` matching the log (as 1 or 0), and `readback_us` on `write_combined` boards should be several times that of `cacheable` boards at the same size. If sa1100-lcdc or mq11xx read like the other class, the table is wrong (does the sa1100-lcdc driver use CMA, and do mq11xx and w100 set `map_wc` on their shmem objects? Both are out of tree and not verified);
- a client built from the v1 XML (or `--bind-version 1`) runs 5 frames with no protocol error, and a version 2 client against an old (version 1) picowl binds 1 and prints `caching=-1`;
- blank, unblank and output hot-unplug re-send `copy_type` only, never `caching` (`WAYLAND_DEBUG=1`);
- on a kernel without CLOSEFB, mq11xx shows `copy_type=0 caching=cacheable`.

## C8 / 8-bpp palettised output on MediaQ

Not implemented. The options (kernel-side conversion in the damage copy, a userspace stage in picowl, a C8 pixman render target), their measured costs and the recommended plan are in [design/mediaq-c8.md](design/mediaq-c8.md), which replaces the short options list that used to be here.
