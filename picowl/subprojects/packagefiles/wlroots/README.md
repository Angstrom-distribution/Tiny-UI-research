# wlroots 0.19 patches for picowl

Five patches against wlroots 0.19.0 (commit 13a62a23). They add what picowl
needs on GPU-less iPAQ-class devices: the pixman renderer reads client
DMA-BUFs, the pixman render pass copies and fills without per-frame
allocations, the DRM backend can rotate in hardware and knows about
copy-type drivers, a DRM lease includes the overlay planes and no longer
writes to freed memory, and the scene keeps the windows under a translucent
layer surface visible. The patches only add mechanisms; policy (which output
is copy-type, when to rotate in hardware, the single-buffer swapchain, who
may lease) is picowl code in `src/output.c`, `src/copytype.c`,
`src/zerocopy.c` and `src/lease.c`.

They are applied by:

- **Meson:** `subprojects/wlroots.wrap`, `diff_files` entries (picowl builds
  the wrapped wlroots when pkg-config finds no `wlroots-0.19`)
- **OpenEmbedded:** `oe/recipes-graphics/wlroots/wlroots_0.19.0.bb`, `file://`
  entries in `SRC_URI`

A host build against a wlroots prefix needs that wlroots built from the patched
tree: picowl does not compile against an unpatched one. When meson finds an
installed `wlroots-0.19` instead of building the wrap, `meson.build` probes it
at configure time and stops with an error in two cases: the header
`wlr/backend/drm.h` lacks `wlr_drm_connector_set_copy_type` (patch 0003, the
message names 0001 to 0003) or lacks `WLR_DRM_LEASE_OVERLAY_PLANES` (patch
0004). Patches 0001, 0002 and 0005 add nothing to a header, so a wlroots that
has 0003 and 0004 but not those three passes the check. The wrapped build
applies all five and is not probed.

Upstream status, from the `Upstream-Status` line in each patch header: 0001 to
0004 are `Inappropriate [picowl specific]`, and 0005 is `Pending`. None has been
submitted upstream as far as the repository shows. The headers still say
`[PATCH n/3]` (0001 to 0003) and `[PATCH 4/4]`, `[PATCH 5/5]`; the numbering
was not updated when patches were added, and the order is the one in the
filenames.

New public API (`include/wlr/backend/drm.h`, patch 0003):

```c
bool wlr_drm_connector_supports_hw_rotation(struct wlr_output *output,
	enum wl_output_transform transform);
bool wlr_drm_connector_set_hw_rotation(struct wlr_output *output,
	enum wl_output_transform transform);
void wlr_drm_connector_set_copy_type(struct wlr_output *output, bool copy_type);
```

Patch 0004 adds no function, only a feature macro (`include/wlr/backend/drm.h`),
which picowl's `meson.build` checks at configure time:

```c
#define WLR_DRM_LEASE_OVERLAY_PLANES 1
```

Patch 0001 also reads the environment variable
`WLR_PIXMAN_DMABUF_ALLOW_ANY_FD`, which is for tests only (see below).

## 0001-pixman-read-dmabuf-client-buffers-via-mmap.patch

**What:** the pixman renderer reads DMA-BUF client buffers through a CPU
mapping, for buffers without data pointer access.

**Why:** there is no GPU, so linux-dmabuf and picowl-buffer-v1 buffers are
plain CPU-readable memory (CMA, shadow buffers) which the compositor can read
directly instead of copying it first.

**How** (`render/pixman/renderer.c`, `dmabuf_map_get`):

1. On first use of a buffer whose `wlr_buffer_get_dmabuf` has a single plane,
   a LINEAR (or INVALID) modifier and a pixman-supported format, the fd is
   mapped with `mmap(MAP_SHARED)`: read-only first, remapped read-write if a
   write access is requested later.
2. The mapping is cached per source `wlr_buffer` on the renderer's
   `dmabuf_maps` list, and unmapped on the buffer's destroy event and when the
   renderer is destroyed.
3. Every access is bracketed with `DMA_BUF_IOCTL_SYNC` START/END (read or
   read-write). ENOTTY, EINVAL and ENOSYS from the ioctl count as success.
4. DMA-BUF formats are still not advertised as texture formats by the
   renderer; picowl advertises them itself through its own linux-dmabuf
   feedback (`src/zerocopy.c`).

**Hardening:** the fd and the layout come from a client, so before mapping:

- the fd must be a real DMA-BUF (`DMA_BUF_IOCTL_SYNC` without flags fails with
  EINVAL on a DMA-BUF and with ENOTTY on anything else). A memfd passed as a
  DMA-BUF could be truncated by the client afterwards, and the next read of the
  cached mapping would raise SIGBUS in the compositor.
- `stride >= width * bytes per pixel`, stride and offset multiples of 4, and
  `offset + stride * height` inside the fd (`lseek(SEEK_END)`), computed in 64
  bits. Otherwise the last rows would be read past the end of the mapping.

picowl's `check_dmabuf` (`src/zerocopy.c`) applies the same stride and offset
rules at import time, so a bad buffer fails with the protocol's `failed` event.

**Tests:** `tests/test-pixman-dmabuf.c` stands memfds in for DMA-BUFs. The
renderer reads `WLR_PIXMAN_DMABUF_ALLOW_ANY_FD` once, at creation. Unset, the
test checks that a memfd is rejected; set, it runs the mapping, caching,
padded-stride and bad-layout cases. It also maps a buffer from
`/dev/dma_heap/system` when that exists. Do not set the variable on a device.

## 0002-pixman-pass-memcpy-and-fill-fast-paths.patch

**What:** fast paths in the pixman render pass (`render/pixman/pass.c`), and no
per-frame allocation.

**Why:** the bus to video memory is slow (about 8 MB/s on MediaQ) and every
damaged pixel is written once, so the work per pixel should be as small as
possible.

**How:**

1. `try_copy_texture`: for the same pixel format on source and destination
   (the strides may differ), operator SRC (or OVER with a format without
   alpha), alpha 1, `WL_OUTPUT_TRANSFORM_NORMAL` and source size equal to
   destination size, each clip rectangle is copied with one `memcpy` per row.
   Anything else falls through to `pixman_image_composite32`.
2. Rectangles drawn with blend mode none (operator SRC) on r5g6b5, x8r8g8b8
   and a8r8g8b8 use `pixman_fill`.
3. Translucent rectangles reuse a solid fill image cached in the
   `wlr_pixman_buffer`, which also embeds its render pass, so a frame does not
   allocate.

The output is pixel-identical to the composite path (`tests/test-pixman-pass.c`).
There is no benchmark in the test suite.

## 0003-drm-hardware-rotation-and-copy-type.patch

**What:** hardware rotation through the primary plane's `rotation` property,
and copy-type connectors. Atomic modesetting only (`backend/drm/atomic.c`,
`backend/drm/drm.c` and two headers).

### Hardware rotation

MediaQ's primary plane rotates while scanning out, so a portrait 240x320 panel
can be fed a landscape 320x240 framebuffer and pixman does not rotate.
picowl falls back to software rotation whenever this is unavailable.

1. `wlr_drm_connector_supports_hw_rotation(output, transform)`: true if the
   primary plane of the connector's CRTC (or, while no CRTC is allocated, of
   any CRTC it could use) has a `rotation` property whose values include the
   bits for `transform`. No CRTC is allocated. The table maps
   `wl_output_transform` to `DRM_MODE_ROTATE_n`, flipped transforms to
   `DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_n`; its comment notes that the 90/270
   rows may need swapping for a given driver.
2. `wlr_drm_connector_set_hw_rotation(output, transform)`: only legal while
   the output is disabled. It records the rotation bits, swaps the size of
   every `wlr_output_mode` for 90/270 (and the flipped variants), and clears
   `current_mode`, `width`, `height` and `refresh`, so that the caller's next
   enabling commit with `wlr_output_state_set_mode()` is not stripped as
   unchanged. The caller commits enabled + mode + a NORMAL output transform
   afterwards. `WL_OUTPUT_TRANSFORM_NORMAL` is the reset: it skips the
   support check, records no rotation bits and unswaps the modes, so it
   succeeds even when the enabling commit failed on a CRTC that cannot rotate
   (that CRTC stays allocated). Other transforms fail when unsupported.
3. Commits that enable the CRTC and update the primary plane write the
   rotation property: the recorded bits, or `ROTATE_0` if none were set (so
   the plane does not keep a value from before picowl started, which is the one
   behaviour change on planes that have the property). SRC stays in
   framebuffer coordinates, the width and height of the CRTC destination box
   are swapped for 90/270, `FB_DAMAGE_CLIPS` stay in framebuffer coordinates.
4. Guards:
   - With a rotation set (any transform but NORMAL), direct scanout is only
     accepted for a buffer whose destination box is the whole output: a
     rotated CRTC box is not the same box with its sides swapped, so anything
     else is composited.
   - A commit that enables a CRTC whose primary plane cannot do the rotation
     fails. The CRTC allocated can differ from the ones checked while the
     output was disabled, and the swapped modes must never be sent unrotated.
     picowl then resets to NORMAL and enables with software rotation.
   - Hardware cursors and cursor moves are refused while a rotation is set (the
     cursor plane has no rotation), so picowl uses its software cursor.
   - The rotation, swap and copy-type state is reset when the connector's
     `wlr_output` is destroyed (e.g. a VT switch); the compositor opts in again
     for the new output.
5. A driver can reject the rotation in the commit; the commit then fails and
   picowl falls back to software rotation. A driver that silently ignores the
   property is not detected.

picowl usage (`src/output.c`): `output_enable_rotated` (initial setup,
unblank, runtime rotation) disables the output, calls `set_hw_rotation`,
enables with the mode, and falls back to software rotation if the commit
fails. `src/input.c` sets the touch
calibration matrix from the hardware rotation, composed with the device's
default libinput matrix; that needs picowl built with libinput and a device that
has a calibration matrix.

### Copy-type connectors

A copy-type driver (mq11xx, w100, sa1100-lcdc) copies the damaged part of the
framebuffer to video memory during the commit, so the buffer is free again as
soon as the page-flip completes. The patch provides the mechanism:

1. `wlr_drm_connector_set_copy_type(output, true)` sets a flag on the
   connector (atomic only, ignored otherwise).
2. `handle_page_flip` releases `plane->current_fb` with `drm_fb_clear()` after
   the usual queued-to-current move, so the swapchain can reuse the buffer
   (buffer age 1).
3. `drm_connector_prepare`, `drm_atomic_connector_prepare` and `pick_max_bpc`
   tolerate `primary_fb == NULL`. A commit without a new buffer leaves the
   primary plane untouched; enabling the output or changing the mode still
   needs a buffer.

`fb.c` is not changed. Destroying an fb uses `DRM_IOCTL_MODE_CLOSEFB` where
the kernel has it (6.9+), otherwise wlroots falls back to `drmModeRmFB`, which
disables the plane that is scanning the fb out. So picowl probes for CLOSEFB
(`drm_closefb_supported()` in `src/output.c`: closing fb 0 gives ENOENT where
the ioctl exists) and turns copy-type off when it is missing, or only logs an
error if the user forced `copytype = yes`.

Everything else is picowl code: the `drmGetVersion` driver detection and its
aliases (`pw_copytype_driver`) and the config override (`pw_copytype_resolve`)
in `src/copytype.c`, the one-slot swapchain (`single_buffer`, RGB565,
`copy_swapchain_create` in `src/output.c`, which can degrade to two slots), the
`copy_type` event for clients, and the log line `output NAME: driver 'X'
copy_type=yes|no rotation_mode=auto|hardware|software`.

## 0004-drm-lease-overlay-planes.patch

**What:** `wlr_drm_create_lease()` also leases the overlay planes, and
`wlr_drm_lease_request_v1_grant()` no longer writes to freed memory.

**Why:** a lessee (the media player, `--vo drm:lease`) can only use objects
that are in the lease, and stock wlroots 0.19 leases the connector, its CRTC,
the CRTC's primary plane and its cursor plane, never an overlay plane (the
hx4700 w100 overlay). Separately, the grant is a use-after-free (below).

**How:**

1. `struct wlr_drm_plane` gets `possible_crtcs`, filled in `init_plane()` from
   `drmModePlane`.
2. `wlr_drm_create_lease()` sizes its `objects[]` array as
   `3 * n_outputs + drm->num_planes + 1` and adds every
   `DRM_PLANE_TYPE_OVERLAY` plane of `drm->planes` whose `possible_crtcs` has
   the bit of the leased CRTC. A plane which several CRTCs can use is added
   once (the kernel rejects a duplicate object). Without libliftoff wlroots
   never uses overlay planes, so this takes nothing from the compositor.
3. `wlr_drm_lease_request_v1_grant()` kept the request's
   `wlr_drm_lease_connector_v1` pointers in the lease and wrote
   `active_lease` through them. But `wlr_drm_create_lease()` destroys the
   leased outputs, which frees those connectors through
   `handle_output_destroy()`, so both that write and the one in
   `lease_handle_destroy()` at the end of the lease hit freed memory. The
   connectors are always withdrawn by a grant, so the lease now keeps none
   (`connectors = NULL`, `n_connectors = 0`).

Not verified: whether a later upstream release fixes (3), and the overlay
planes of the out-of-tree iPAQ drivers (their `possible_crtcs` show in
`modetest -p` on a board). See the [roadmap](../../../doc/design/roadmap.md).

**Tests:** none run in the build container (no `/dev/dri`).
`tests/lease-vkms.sh` runs a full lease on vkms (opt-in with `PW_LEASE_VKMS=1`, needs root); with ASan builds
of picowl and wlroots it catches the use-after-free, and with
`PW_LEASE_EXPECT_OVERLAY=1` it checks that the overlay is in the lease.

## 0005-scene-update-a-buffer-node-when-its-opacity-changes.patch

**What:** `wlr_scene_buffer_set_buffer_with_options()` updates the node (and
with it the visible regions of the nodes below) when the new buffer is opaque
and the old one was not, or the other way round.

**Why:** a surface commit sets the opaque region and the size of its scene
node before it swaps the buffer (`surface_reconfigure` in
`types/scene/surface.c`). When a layer surface grows from an opaque RGB565
buffer (picowl-panel's bar) to an ARGB8888 one (the bar with its translucent
slider row), the update that the new size triggers still sees the opaque
buffer, so the whole grown node occludes what is below it. The swap to the
ARGB8888 buffer then changed `buffer_is_opaque` without an update: a window
that was there before stays culled under the row, which is drawn over whatever
the frame buffer held (black on the iPAQ). A window mapped after the surface
grew, or one that is resized afterwards, is not affected, because their own
updates recompute their regions.

**Tests:** `tests/test-scene-opaque.c` replays the commit sequence on a bare
scene and checks the visible region of a node below; `tests/panel-e2e.sh`
section I opens the row over a window that was already playing and checks the
pixel under the row.

## Updating the patches

The patches apply with `patch -p1` to a pristine wlroots 0.19.0 tree, in order.
To change one:

1. Extract 0.19.0 into a git repository, commit it, then apply the patches one
   commit each (`patch -p1 < 000N-...`).
2. Edit and amend the commit of the patch you change (rebase the later ones).
3. Regenerate each patch from its commit with the same `git format-patch` style
   header (keep the `From:`/`Subject:` lines and the description; `git diff
   --abbrev=8` for the body, `--stat=80` for the diffstat).
4. Copy the five `.patch` files into both `subprojects/packagefiles/wlroots/`
   and `oe/recipes-graphics/wlroots/files/`; the two sets must be
   byte-identical:

   ```sh
   for p in subprojects/packagefiles/wlroots/*.patch; do
       cmp "$p" "oe/recipes-graphics/wlroots/files/$(basename "$p")"
   done
   ```

5. Apply them to a pristine tree again, build wlroots (`-Dbackends=drm,libinput
   -Drenderers=[] -Dallocators=[]`, as the OE recipe does), build picowl
   against it and run `meson test`.

## Testing

On a machine without the hardware features the patches change little: no
rotation is requested, but a plane that has a `rotation` property now gets
`ROTATE_0` written on every commit that enables the CRTC and updates the plane,
and the pixman renderer maps DMA-BUFs that it used to refuse. Buffers with data
pointer access behave as before.

On a device with the features, check rotation (the hardware rotation commit, touch
calibration, no flicker while rotating), copy-type (the swapchain stays at one
slot, the output is not disabled when a framebuffer is dropped) and the
kernel's CLOSEFB (swapchain replacement and rotation do not flicker). See
[doc/zero-copy.md](../../../doc/zero-copy.md), section "Hardware-only checklist", for the full list.
