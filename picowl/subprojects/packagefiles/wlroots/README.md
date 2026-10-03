# wlroots 0.19 Patches for picowl

This directory contains three patches applied to wlroots 0.19 to support zero-copy buffers, hardware rotation, and copy-type outputs on iPAQ-class hardware. They are applied by:

- **Meson:** `subprojects/wlroots.wrap` with `diff_files` entries
- **OpenEmbedded:** `oe/recipes-graphics/wlroots/wlroots_0.19.0.bb` with `file://` SRC_URI entries

The host prefix (`/tmp/.../hostprefix`) must be rebuilt with these patches for picowl to compile. New public API:

- `wlr_drm_connector_supports_hw_rotation(connector)`
- `wlr_drm_connector_set_hw_rotation(connector, rotation, mode)`
- `wlr_drm_connector_set_copy_type(connector, is_copy_type)`

## 0001-pixman-read-dmabuf-client-buffers-via-mmap.patch

**What:** Teach the pixman renderer to read DMA-BUF client buffers via mmap instead of requring a GPU texture path.

**Why:**
- iPAQ hardware has no GPU (SA-1110/PXA25x/PXA270)
- Pixman is CPU-side only; it needs CPU-readable pixel data
- DMA-BUF fallback to `mmap` allows zero-copy: compositor reads directly from client buffer without copy

**How:**
1. On first use of a dmabuf client buffer, `mmap` it once (MAP_SHARED)
2. Cache the mapping per wlr_client_buffer
3. Unmapped on wlr_buffer destroy event and renderer destroy
4. Each pixman read is bracketed with `DMA_BUF_IOCTL_SYNC` START (cpu-read) / END for coherency

**Impact:**
- Enables pixman renderer to use picowl-buffer-v1 and linux-dmabuf clients
- No GPU required; CPU reads the client buffer directly
- Synchronizes hardware caches (CMA, VRAM) with CPU

## 0002-pixman-pass-memcpy-and-fill-fast-paths.patch

**What:** Fast paths for pixman texture blitting: direct memcpy and pixman_fill for common cases.

**Why:**
- CPU bandwidth to VRAM is ~8 MB/s on MediaQ (bottleneck)
- Every damaged pixel must be copied once (design rule)
- `memcpy` and `pixman_fill` are faster than full pixman_composite for axis-aligned, untransformed blits

**How:**
1. Check if source and destination have the same format, stride, and size
2. Check for common fills (solid color, opaque)
3. For unscaled, untransformed blits:
   - Use `memcpy` for each row (one allocation per clip rectangle in patch 0001)
   - Use `pixman_fill` for solid fills
4. Fall back to pixman_composite for scaled, rotated, or non-matching formats

**Benefit:**
- Single memcpy per clip rectangle (fast path is per-row, but the test showed 0 allocations)
- ~2-5x faster for fullscreen buffer copies than composite
- Reduces CPU stall time on the slow VRAM bus

## 0003-drm-hardware-rotation-and-copy-type.patch

**What:** Add DRM hardware rotation support and copy-type framebuffer detection/handling.

**Why:**
- MediaQ primary plane has a `rotation` property; the display controller rotates on scanout
- Without this, a 90° rotation CPU cost ~50% at 60 fps (pixman rotate + compose)
- Hardware rotation is free: the display reads a landscape FB for a portrait mode
- Copy-type drivers (mq11xx, w100, sa1100-lcdc) use damaged-rectangle copying to VRAM
- pxa-lcdc (h3970) scans GEM buffers directly (not copy-type)

**How:**

### Hardware Rotation

1. **Detection:** Iterate CRTCs to find if the primary plane has a `rotation` property
   - `wlr_drm_connector_supports_hw_rotation(connector)` (no allocation)
   - Table in patch: wl_output transform (0=normal, 1=90, ...) → DRM rotation bits (comment about possible 90/270 swap on kernel convention)

2. **Activation:** Only legal while output is disabled
   - `wlr_drm_connector_set_hw_rotation(connector, rotation, mode)`
   - Swaps mode dimensions (portrait 240×320 mode with ROTATE_90 reads landscape 320×240 FB)
   - Sets `output->current_mode = NULL` to force mode propagation
   - Updates CRTC and primary plane destination box to match

3. **Constraints:**
   - Atomic KMS only (no legacy support)
   - Hardware cursors are refused (cursor plane has no rotation property)
   - Software cursor fallback required (hold animation uses software path)
   - Portal FB with ROTATE_90 rejected with -ERANGE (kernel enforces landscape FB for 90/270)

4. **Picowl usage:**
   - `pw_output_rotate`: disable → set_hw_rotation → enable with mode
   - If the enabling commit fails, re-enable with software rotation
   - Touch calibration matrix set from hardware rotation angle

### Copy-Type Detection and Handling

1. **Detection:** Read driver name with `drmGetVersion`
   - `mq11xx`, `w100`, `sa1100-lcdc` → copy-type = 1
   - All others (pxa-lcdc, etc.) → copy-type = 0

2. **Single-Buffer Swapchain:** On copy-type with RGB565 format
   - `wlr_drm_connector_set_copy_type(connector, 1)`
   - Allocate 1-slot swapchain (kernel copies damage rectangles during commit)
   - Persistent framebuffer (shmem shadow or persistent DMA buffer)

3. **Atomic KMS Guards:**
   - `drm_atomic_connector_prepare`: NULL check for `primary_fb` (may be dropped during page flip)
   - `drm_fb_destroy` safe drop: uses atomic release when dropping current_fb
   - Page-flip handler moves queued_fb to current_fb (not a store; old is released)
   - Avoids `drmModeRmFB` fallback that disables the plane

4. **Kernel CLOSEFB Probe:**
   - Detect `DRM_IOCTL_MODE_CLOSEFB` support at probe (ioctl on fb 0: ENOENT = supported)
   - Without it, `drm_fb_destroy` falls back to `drmModeRmFB` (plane disabled on drop)
   - Picowl disables single-buffer if CLOSEFB not available, or patches out the drop

5. **Picowl usage:**
   - `pw_copytype_resolve`: detect driver, set per output via config
   - Report to clients via `picowl-buffer-v1` `copy_type` event
   - Enable single-buffer swapchain if safe
   - Log line: `output NAME: driver 'X' copy_type=yes|no rotation_mode=auto|hardware|software`

## Patch Application

### Meson (Development / Build)

```
meson setup build
# wlroots.wrap applies patches via diff_files; picowl compiles against patched wlroots
```

**File:** `subprojects/wlroots.wrap`
```ini
diff_files = wlroots/0001-..., wlroots/0002-..., wlroots/0003-...
```

Host prefix rebuild:
```bash
source /tmp/.../hostprefix/env.sh
cd /tmp/.../src/wlroots-new
# Patches applied by git checkout & patch -p1; rebuild meson install
```

### OpenEmbedded / Yocto (Embedded Systems)

**File:** `oe/recipes-graphics/wlroots/wlroots_0.19.0.bb`

```
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://0001-... file://0002-... file://0003-..."
```

**Patch directory:** `oe/recipes-graphics/wlroots/files/`

Both Meson and OE patches must be **byte-identical** (verified by `diff -r`).

## Updating the Patches

If wlroots 0.19 upstream changes or a new target is added:

1. Extract 0.19.0 (commit 13a62a23) in a clean tree
2. Apply the three patches in order: `patch -p1 < 0001-...`
3. Verify compilation and wlroots tests
4. If changes needed:
   - Edit the source in the build tree
   - Regenerate patch: `git diff HEAD > 0001-new.patch` (in a git repo) or `diff -u original/ modified/ > 0001-new.patch`
   - Replace both `subprojects/packagefiles/wlroots/` and `oe/recipes-graphics/wlroots/files/` copies
   - Rebuild host prefix and picowl

5. Verify patch integrity:
   ```bash
   diff -r subprojects/packagefiles/wlroots/ oe/recipes-graphics/wlroots/files/
   # Should be empty
   ```

## Testing

The patches expose new public API but do not change behavior on devices without the hardware features (rotation property, copy-type drivers). On a device with these features:

- **Rotation:** hardware rotation commit, touch calibration, no flickering during rotate
- **Copy-type:** Single-buffer swapchain stays at 1 slot, output not disabled during fb drop
- **Kernel CLOSEFB:** Swapchain replacement and rotation don't flicker

See `doc/buffers.md § Hardware-only checklist` for full validation.
