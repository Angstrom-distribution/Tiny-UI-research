# picowl-buffer-v1: client reference

This is how a client uses picowl's compositor-allocated buffers. The protocol itself is `protocols/picowl-buffer-v1.xml`. The design (direct scanout, copy-type outputs, hardware rotation, memory tuning, wlroots patches) and the hardware-only checklist are in [zero-copy.md](zero-copy.md).

## Why it exists

- **No render node:** the iPAQ DRM drivers expose `/dev/dri/card0` but no `/dev/dri/renderD*`. Clients therefore can't allocate dumb buffers themselves.
- **The compositor allocates instead:** picowl creates an RGB565 dumb buffer, PRIME-exports it, and hands the client the dmabuf. A full-screen client can then be scanned out directly with no compositor copy.
- **Single buffering on copy-type drivers:** on mq11xx, w100 and sa1100-lcdc, the kernel copies the damage out during the commit. Once picowl tells the client that copy has happened (`copied`), the client may redraw into the same buffer.

## Flow

1. **Bind `picowl_buffer_manager_v1`.** You receive:
   - `format` once per supported format (only `DRM_FORMAT_RGB565`, `0x36314752`);
   - `copy_type` (1 = an output copies damage to device memory, 0 = scanout only). `copy_type` is sent again whenever it changes.
   - `caching` (version 2 only, see step 1c).
1b. **Bind `min(advertised, 2)`.** Binding a version above the advertised one is a protocol error, so a client built for version 2 must use `min(version from the registry, 2)`. A client which binds 1 gets exactly the events above and never `caching`; picowl gates the event by the bound version, so old binaries keep working. Both interfaces are at version 2; `picowl_buffer_v1` has no new messages, its version follows the manager's.
1c. **`caching(mode)` (version 2).** Sent once on bind, after the last `format` and after `copy_type`, and never again. `1` = `cacheable`: reading the buffer back is cheap. `0` = `write_combined` (this also covers uncached): write sequentially and never read back from the buffer or decode into it. It describes the CPU mapping of the dmabuf fd of every buffer the manager creates and does not depend on `copy_type`, `copied` or `retained`: on a `write_combined` buffer `copied` still allows a redraw, only reading is forbidden. A client does a `wl_display_roundtrip` after binding and then has it. Install a `caching` handler whenever you bind 2: libwayland aborts a client whose listener member is NULL. Without the event (a version 1 picowl, or a client which bound 1) keep the old rule: `copy_type` 1 means cacheable, 0 means treat the buffers as write-combined. Buffers you allocate yourself and import through `zwp_linux_dmabuf_v1`, and `wl_shm` buffers (always cacheable), are not covered.
2. **`create_buffer(id, width, height, format)`.** You get either `dmabuf(fd, stride, offset, modifier_hi, modifier_lo)` followed by `done`, or `failed(reason)`:

   | reason | value |
   |---|---|
   | `unsupported_format` | 0 |
   | `too_large` | 1 |
   | `no_memory` | 2 |
   | `no_drm` | 3 (reserved, never sent: with no DRM device the manager global is simply not advertised) |

   **Limits** are compositor policy (`[zerocopy]` and `[app.<app_id>]` in `picowl.ini`, see the README). By default a client may hold 3 buffers and all clients share 2 MiB. An app with a rule can get more, for example 7 for a media player. Over a limit you get `failed(no_memory)`; destroy that object. Don't assume a count: request what you want, stop at the first `failed`, and use what was granted.

   **Set the `app_id` first.** The compositor picks the limits by the `xdg_toplevel` app_id at the time of `create_buffer`, so call `xdg_toplevel.set_app_id` before it (a commit is not needed). Buffers created earlier stay under the default limits.
3. **Wrap the buffer:**
   - `mmap` the fd for drawing.
   - Wrap it as a `wl_buffer` with `zwp_linux_dmabuf_v1`: `create_params`, `add` with the fd, offset, stride and modifier, then `create_immed` with the same size and format.
4. **`attach_surface(wl_surface)`** on the buffer object. Commits of that surface are numbered from 1 (uint32, wrapping, 0 skipped).
5. **Draw, attach and commit as usual.** Exactly one of these events answers each commit, unless a newer commit supersedes it first:

   | Event | Meaning | What the client does |
   |---|---|---|
   | `copied(serial)` | All commits up to `serial` were shown by direct scanout on a copy-type output and have been copied to device memory | Draw the next frame into the **same** buffer; don't wait for `wl_buffer.release` |
   | `retained(serial)` | The commit was composited, occluded, off-output, on a scanout-type output, or the output is blanked; picowl keeps reading it | Don't touch this buffer. Draw the next frame into a second buffer (create it on demand) and wait for `wl_buffer.release` on this one, as with any Wayland buffer |

**Always commit complete frames.** picowl takes no snapshot. If a direct-scanned surface later has to be composited before your next commit, for example because a popup, the software cursor or the OSK appears over it, the last committed buffer is re-read for that region.

## Fallbacks

| Situation | Client behaviour |
|---|---|
| No `picowl_buffer_manager_v1` global (headless backend, no DRM, no dmabuf allocator, or `[zerocopy] enable = false`); `failed(no_drm)` is never sent | Plain wl_shm, preferring `WL_SHM_FORMAT_RGB565` (picowl advertises it) |
| `copy_type` 0 (h3970, pxa-lcdc) | Direct scanout can still happen, but `copied` is never sent: double buffer and use `wl_buffer.release` |
| `failed(no_memory)` | Use the buffers you have, with fewer frames in flight, or fall back to wl_shm |
| Allocation fails mid-session | Keep using the buffers you have, or fall back to wl_shm |

`tests/pw-test-client.c --zerocopy` implements this negotiation, including the wl_shm fallback that the headless smoke test exercises. `--zerocopy-count N` requests N buffers, prints how many were granted, and `--app-id ID` sets the app_id. `--bind-version N` binds the manager at `min(advertised, N)` (N=1 acts as a version 1 client), `--probe` prints the version and the `format`, `copy_type` and `caching` it received (`caching=-1`: not received) and exits, and `--readback` (with `--zerocopy`) times one pass of 32-bit reads over buffer 0 and prints `readback_us`; it is several times slower on write-combined memory.

## Status

- **Built and tested here:** compile and unit tests (`copyrel`, `copytype`, `zbquota`), the bind events and the version gating (`bufproto`: in-process client over a socketpair, and `pw-test-client` at version 2 and forced to version 1 against a version 2 and a version 1 server), plus the wl_shm fallback in the headless smoke test.
- **Hardware-only:** everything involving a real DRM device, i.e. allocation, direct scanout, `copied`/`retained` timing and the kernel copy. This container and CI have no `/dev/dri`. The checks to run on an iPAQ are listed in [zero-copy.md](zero-copy.md), section "Hardware-only checklist".
