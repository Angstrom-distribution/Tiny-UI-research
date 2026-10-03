# Integrating the media player with picowl

This is a handoff plan for the media player work stream. It assumes the player as described in `media-player.md` (two binaries over `libmpcore.a`, the `mp_frontend_ops` contract, `drm/kms_state.c`, the hx4700 overlay planner) and picowl as on branch `picowl` (`README.md`, `doc/buffers.md`, `doc/zero-copy.md`, `doc/power.md`, `doc/cursors.md`).

Nothing here has been run on hardware. Items marked **[picowl]** are work for the picowl side; everything else is player work.

## 1. Goal and the two paths

Under picowl the player must keep the bare-DRM copy budget: one driver copy of the changed rectangle per frame on the copy-type boards (MediaQ, w100, sa1100-lcdc), and none on the h3900's direct scanout. It must also keep the hardware paths that only a KMS client can drive. That needs two paths:

| Path | Used for | Mechanism |
|---|---|---|
| **A. Wayland front-end** (`--vo wayland`) | Default playback on every board, windowed or full screen, coexisting with the panel, the OSK and notifications | Compositor-allocated RGB565 dmabufs via `picowl-buffer-v1`, attached full screen, so picowl scans the buffer out directly with no compositor copy |
| **B. KMS handoff** (`--vo drm` under picowl) | hx4700 YUV overlay plane; MediaQ C8 / pixel doubling / GC0C tear-free flips; anything `mediaplayer-drm` does that a Wayland client cannot | Step 1: VT switch (exists today as `fullscreen_mode = vt-switch`). Step 2: DRM lease from picowl (`wp_drm_lease_device_v1`) **[picowl]** |

`mediaplayer-x11` stays for the GPE/X11 session and is not used under picowl. Adding a third front-end goes against decision 1.2 ("no third binary"). Recommendation: a new front-end directory `wayland/` producing `mediaplayer-wayland`. Expect it to replace `mediaplayer-x11` once picowl plus the GDK2 backend replace the X session. That decision is the player owner's.

## 2. Path A: the Wayland front-end

### 2.1 Mapping `mp_frontend_ops` onto picowl

| Op | Implementation |
|---|---|
| `init` | Connect and bind: `wl_compositor`, `xdg_wm_base`, `zwp_linux_dmabuf_v1`, `picowl_buffer_manager_v1`, `wp_presentation`, `wl_seat`, plus `zwp_idle_inhibit_manager_v1` **[picowl]**. Create a toplevel with `app_id = "mediaplayer"`, request fullscreen, and take the geometry from the first `configure`. Caps: format RGB565; geometry as configured; `nbufs` = buffers obtained (§2.3); write-combined flag from §2.4; no overlay plane (Path A never drives one); DPI from `wl_output` physical size; output name. |
| `acquire` | Return a buffer in FREE state. With `MP_ACQ_NOWAIT`, return 1 at once if none; otherwise dispatch Wayland events for a bounded wait, as the KMS front-end waits for flip events. |
| `present` | `wl_surface_attach`, then one `wl_surface_damage_buffer` per damage rectangle (never full damage unless the whole frame changed), `wp_presentation_feedback`, `wl_surface_commit`. The buffer moves to PENDING. The buffer goes back on every return path, as today. |
| `release` | A HELD buffer becomes FREE without being shown. |
| `set_overlay` | Not provided. Path A has no overlay plane, so the planner never engages and the hx4700 uses the software path or Path B. |
| `fds` | The `wl_display` fd goes into `mp_loop`; read and dispatch on readiness, flush before poll. |
| `fini` | Destroy the inhibitor, buffers and toplevel; disconnect. |

### 2.2 Buffer state machine

Reuse `drm/kms_state.c`. Its FREE/HELD/PENDING/FRONT model maps directly; only the events that move a buffer change:

| KMS event (today) | Wayland equivalent |
|---|---|
| Page-flip event for buffer *b* (replaces FRONT) | `picowl_buffer_v1.copied(serial)` on copy-type outputs: every buffer committed at or before `serial` is FREE, **including the one on screen**. That is the single-buffer win: the next frame can go into the buffer just shown. |
| — | `retained(serial)`: that commit was composited or is otherwise still read. It stays FRONT until `wl_buffer.release`. |
| Buffer no longer on screen | `wl_buffer.release`: FREE. |
| Stale event for a retired flip | Serials are monotonic per surface. Ignore a `copied`/`retained` older than the newest serial already settled, as with the 24-bit commit sequence. |

- **On a copy-type output** (h2200, h5550, hx4700 RGB path, h3800), `copied` arrives for each full-screen frame, so two buffers are enough for real double buffering, and one is enough at `--decode-ahead 0`.
- **On the h3900** (scanout type), `copied` never comes and `wl_buffer.release` drives everything, exactly like real flips.

Extend `tests/kms_state_selftest.c` with a scripted Wayland event model covering: `copied` freeing FRONT, `retained` keeping it, release after retained, stale serials, and disconnect.

### 2.3 Buffer count

- **picowl's limits today:** 3 buffers per client and 2 MiB in total across all clients (`PW_ZB_MAX_PER_CLIENT`, `PW_ZB_BUDGET` in `src/zerocopy.c`).
- **The player's default:** `--decode-ahead 5` asks for 7 buffers. That is 1.05 MiB at QVGA, but 4.1 MiB on the hx4700, which exceeds the total budget.

Plan:
- Request `mp_core_want_bufs()` and accept what is granted: `failed(no_memory)` or `failed(too_large)` ends allocation, and `nbufs` reports the count. The core already degrades to rendering fewer frames ahead, with decoded pictures still queuing N deep.
- **[picowl]** Make the limits configurable (`[zerocopy] max_buffers_per_client`, `budget_kb`), with an `app_id`-specific override so the player can get its 7 buffers on QVGA boards.
- On copy-type outputs fewer buffers cost little, because `copied` frees FRONT immediately.

### 2.4 Write-combined versus cacheable

The KMS front-end keys caching off `drmGetVersion()`. A Wayland client has no DRM fd.

- **Interim heuristic:** `copy_type = 1` means a shmem (cacheable) driver, and `copy_type = 0` means treat the buffers as write-combined. That matches all four drivers today: the copy-type ones are mq11xx and w100, which are shmem, plus sa1100-lcdc, which is CMA and write-combined but whose memory is only written. The core's rule (never decode into, never read back from display buffers) holds either way.
- **[picowl]** Replace the heuristic with an explicit `caching` event in `picowl_buffer_manager_v1` v2: `cacheable | write_combined`, taken from the driver name on picowl's side.

### 2.5 Keeping direct scanout (and the copy budget)

picowl scans a client buffer out only when that buffer is the single visible node, has no transform mismatch and is a dmabuf. Rules for the player:

1. **Full screen, one surface.**
   - Draw the OSD, subtitles and statistics into the video buffer, as today. No subsurfaces and no separate OSD window.
   - Any second visible surface (subsurface, popup, the picowl panel or OSK) forces composition, which costs a pixman copy of the damage.
   - picowl auto-hides its panel while an app is focused.
2. **Match the output orientation through `wl_surface.set_buffer_transform`, never through compositor rotation.**
   - If picowl reports an output transform (software portrait on the h3800/h3900), render pre-rotated in the player's fused transform pass and set the matching buffer transform. The scene then sees buffer transform == output transform and keeps direct scanout.
   - If picowl rotates in hardware (MediaQ `[rotation] mode = hardware`), the output is presented to clients unrotated at the logical size. Render at that logical size with rotation 0; the plane rotates.
   - This replaces `--rotate auto`/`--hw-rotation` under Path A: the compositor decides orientation, and the player follows `wl_output.transform` and the configure size.
3. **Exact damage.** Pass the core's damage rectangles through `damage_buffer`. picowl forwards them as `FB_DAMAGE_CLIPS` in direct scanout, so letterbox bars stay out of the upload as on bare DRM.
4. **Commit complete frames.** picowl takes no snapshot. If composition starts mid-frame (a notification pops up), the last committed buffer is re-read.

Expected cost: identical bus bytes to `--vo drm`, plus one compositor wakeup and a few protocol messages per frame. Verify with the driver counters (§5).

### 2.6 Timing and A/V sync

- Use `wp_presentation` feedback (`presented`: timestamp, refresh, sequence) as the display clock input, as page-flip timestamps are used now. picowl creates `wp_presentation` version 2.
- None of the drivers has a real vblank, so presentation times come from the kernel's fake-vblank helper, as for the KMS path.
- Don't block on frame callbacks; pace from the audio clock as today, and treat a missing `presented` as discarded (`discarded` event).

### 2.7 Input

- **Touch** arrives as pointer events. picowl converts touch to pointer and applies the libinput calibration matrix, so coordinates are calibrated and rotated. Path A needs no tslib; the §4.7 tslib work remains for bare DRM only.
- **Tap-and-hold:**
  - picowl turns a hold into `BTN_RIGHT` (picowl `doc/cursors.md`). A right click is the natural "open menu / OSD" gesture. A tap is a `BTN_LEFT` press and release delivered together at lift; a drag starts after 8 px.
  - **[picowl]** Add a per-`app_id` `hold_action` override, so the player can turn hold off if it wants raw long-press timing, for example for seeking.
- **Keys** arrive as `wl_keyboard` events with an xkb keymap. Map keysyms to the command vocabulary with a keysym column in the existing keymap files (§8.5). picowl consumes its own bindings first (power key = blank, app-cycle key), so don't bind those in the player.
- **Bluetooth keyboards** appear as ordinary keyboards through picowl; no inotify needed in this front-end.

### 2.8 Idle, dimming and blanking

- picowl dims and blanks on idle (`doc/power.md`). During playback the player must hold `zwp_idle_inhibit_manager_v1` on its surface, and drop it when paused or stopped.
- **[picowl]** Idle-inhibit is not implemented yet. picowl must add `idle-inhibit-unstable-v1` and honour inhibitors from a visible surface in `power.c` (no dim, no blank). Until then, playback longer than `dim_after_s` dims the screen.

### 2.9 What stays the same
The socket protocol and `ctl` (the Unix socket path is unchanged, `$XDG_RUNTIME_DIR` exists under picowl), direct ALSA, telemetry, the transform pass and the file browser (optional under Path A, because picowl's panel launcher or a file manager can open files).

## 3. Path B: KMS handoff for the hardware paths

### 3.1 Step 1: VT switch (no picowl changes expected)
- picowl runs on seatd/libseat. `mediaplayer-drm` on another VT takes DRM master once picowl's session is deactivated, and picowl regains it on return.
- The x11 front-end's `fullscreen_mode = vt-switch` (`x11/vtswitch.c`) already does the switch and state handover, and needs only to be launchable from a Wayland context: a `wayland/vtswitch` call path, or picowl's launcher starting `mediaplayer-drm` with the handed-over arguments.
- **Check on hardware [picowl]:**
  - seatd releases the VT and the DRM master on an external `VT_ACTIVATE`;
  - wlroots restores picowl's outputs on reactivation, including picowl's own state: hardware rotation, the copy-type swapchain, the dim state and the cursor.

  If any of these fails, it is a picowl bug to fix.

### 3.2 Step 2: DRM lease (seamless, preferred long term)
- **[picowl]** Offer the output for lease (`wlr_drm_lease_v1_manager_create`, then `wlr_drm_lease_v1_manager_offer_output` for the internal output on request). While a lease is granted, picowl's `wlr_output` is destroyed. It is re-created when the lease ends, and picowl repaints.
- **Player:** a `--vo drm:lease` init path. Connect to Wayland, request a lease for the connector (the lease includes the CRTC and its planes: primary, cursor and the w100 overlay), and use the leased fd instead of opening `/dev/dri/card0`. Everything after that is the existing KMS code: atomic commits, `kms_state`, the overlay planner, `hw_rotation`, C8 when implemented.
  - There are no VT ioctls and no DRM master handling; the lease fd is already authorised.
  - Revoke the lease (close the fd) on exit or when leaving full screen.
- **When to choose Path B automatically:** the overlay planner (`core/ovplan.c`) accepts the stream on the hx4700, or a MediaQ C8/doubling mode is requested. Otherwise use Path A. Expose it as `--vo auto` with a log line naming the reason.

## 4. Work items

| # | Item | Owner | Depends on | Acceptance |
|---|---|---|---|---|
| 1 | `wayland/` front-end: `init`/`acquire`/`present`/`release`/`fds`/`fini` over `wl_shm` RGB565 first (works under any compositor, including headless picowl) | player | — | Plays a clip under headless picowl; `kms_state` selftest extended with the Wayland event model passes |
| 2 | `picowl-buffer-v1` buffers + linux-dmabuf wrapping, `copied`/`retained` handling, wl_shm fallback on `no_drm` | player | 1 | Headless: falls back to wl_shm with one log line. On a board: driver copy stats show damage-only uploads; picowl logs `Direct scan-out enabled` |
| 3 | Buffer-transform rendering (orientation from `wl_output.transform`), damage pass-through | player | 2 | On the h3800/h3900 in portrait, direct scanout stays enabled (no pixman composite) |
| 4 | `wp_presentation` timing, keysym keymap column, pointer/hold handling | player | 1 | A/V sync within the existing targets; all commands reachable by buttons and stylus |
| 5 | Idle-inhibit protocol in picowl, honoured by `power.c` | picowl | — | No dim or blank during playback with an inhibitor; normal timeouts after it is destroyed |
| 6 | Player holds the idle inhibitor while playing | player | 5 | As above |
| 7 | Configurable buffer budget per `app_id`; `caching` event (buffer protocol v2) | picowl | — | Player gets 7 QVGA buffers; reports the caching mode |
| 8 | VT-switch handoff under picowl (Path B step 1), launched from the Wayland front-end | player (+ picowl fixes found) | 1 | hx4700 overlay path plays from a picowl session and picowl restores cleanly afterwards |
| 9 | DRM lease offer in picowl | picowl | — | `wlr_drm_lease_v1` global; output destroyed while leased and restored after |
| 10 | `--vo drm:lease` and `--vo auto` in the player | player | 8, 9 | Overlay and C8 paths run without a VT switch; automatic choice logged |
| 11 | Per-`app_id` `hold_action` override | picowl | — | Optional, only if the player wants raw long-press |

**Order:** 1 → 2 → 3 → 4 on the player side, and 5/7 in parallel on picowl. Then 6. Then 8 (it gives hx4700 overlay playback under picowl early). 9 → 10 last.

## 5. Measurements to take

Use the same clip, kernel and rootfs for `--vo drm` (bare console), `--vo wayland` (Path A) and Path B, using the player telemetry plus picowl's driver counters (`mq11xx_copy_stats`, `w100_2d`):
- **Per-frame VRAM bytes and present time.** Path A full screen must match `--vo drm` within noise on the h2200/h5550/hx4700/h3800.
- **Composition.** Count frames where picowl composited instead of direct scanout (picowl debug log or `retained` counts). Expect zero in steady full-screen playback.
- **Drops and idle CPU per frame,** against the acceptance numbers in §10.4.
- **RSS of the player plus picowl,** compared with the player plus X11.
- **Path B:** the handoff time and picowl's restore time, plus overlay-path drops identical to bare DRM.

## 6. Open questions for the two owners

- Does Path A replace `mediaplayer-x11` once GPE runs on picowl, or does the player keep three front-ends for a while? (player owner)
- Should picowl prefer hardware rotation or software rotation with client buffer transforms on the MediaQ boards for video? Hardware rotation removes the player's rotate pass but its dither regression in §9.10 still applies. (both; measure with item 3)
- Should `--vo auto` pick Path B on the hx4700 whenever the planner accepts the stream, given that §12.1 has not decided rotation 0 versus 90 for landscape clips? (player owner)
- Exact `copied` timing relative to the kernel copy on each copy-type driver (picowl `doc/zero-copy.md` hardware checklist). Both Path A's single-buffer win and its correctness depend on it. (picowl, on hardware)
