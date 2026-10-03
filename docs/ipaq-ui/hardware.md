# Hardware, kernel constraints and design rules

[← index](README.md)

## 1. Targets and display paths

| Board | SoC | RAM | Display driver | Panel | Scanout model | Full-frame update |
|---|---|---|---|---|---|---|
| h3870 | SA-1110 206 MHz, ARMv4 | 64 MiB | `sa1100-lcdc` + `panel-ipaq-h3800` | 320x240, landscape-native | one persistent uncached DMA buffer; damage memcpy'd in; no tear protection | 150 KiB memcpy on a 206 MHz core |
| h3970 | PXA250 400 MHz | 64 MiB | `pxa-lcdc` + `panel-ipaq-h3900` | 320x240, landscape-native | scans GEM DMA buffers directly from a 1 MiB CMA pool (six frames) | zero copy possible |
| h2210 | PXA255 400 MHz | 64 MiB | `mq11xx` (MQ1188) | 240x320 portrait | shmem shadow plane; damage → 256 KiB VRAM over 16-bit VLIO | 16.9–19.4 ms at 7.9–9.1 MB/s with the core stalled; DMA doesn't help |
| h5550 | PXA255 400 MHz | 128 MiB with `mem=128M@0xa0000000` | `mq11xx` (MQ1132) | 240x320 portrait | as h2210 | as h2210 |
| hx4700 | PXA270 624 MHz, iWMMXt | 64 MiB | `w100` (W3220) | 480x640 portrait | shmem shadow plane; damage → 2 MiB VRAM over 32-bit static bus | ~600 KiB, 22–25 ms |

- **Pixel format.** Everything is RGB565. `mq11xx` offers only RGB565 and C8; XRGB8888 is not implemented. Any 32-bit stage costs a conversion and doubles the bytes.
- **Rotation.**
  - The h3870 and h3970 controllers cannot rotate, and their panels are landscape-native, so portrait means software rotation.
  - The MediaQ chips rotate in hardware.
- **MediaQ VRAM.** It holds one RGB565 frame but not two, so there is no tear-free 16 bpp flip. The GC0C flip works only for C8 or pixel-doubled frames.
- **Vblank.**
  - h2210: from the MediaQ frame IRQ (GPIO14).
  - h5550 and hx4700: from the DRM vblank timer at about 56.8 Hz.
- **CPU and toolchain.**
  - There is no GPU and no FPU, so hot paths must be integer.
  - iWMMXt codegen is gone in GCC 16; pin GCC 15.3 for any iWMMXt paths.
  - The hx4700 caps at 416 MHz unless you pass `pxa2xx_cpufreq.pxa27x_maxfreq=624`.
- **Usable RAM.** About 50 MiB on the 64 MiB boards (`Memory: 51004K/65536K`). Toolkit heaps and per-process libraries dominate, not framebuffers.

## 2. Kernel-side constraints

These come from `kernel-general.md` and the board docs, and they shape the UI as much as the display drivers do.

- **A cpufreq transition is a bus-wide event, not a core-clock change** (`kernel-general.md` §3 item 14, §8).
  - Every board boots with the ondemand governor.
  - On PXA25x, every step rewrites `MDREFR` DRI/DB2 underneath the VLIO companion chips, MediaQ included.
  - PXA255 erratum 26 covers peripherals moving data during the switch (UARTs, UDC, SSP).
  - On h39xx, ondemand wedges the bus; the board reaches userspace only with `cpufreq.off=1` or `cpufreq.default_governor=performance`.
  - A compositor's load is bursty (idle, then a frame burst), which is exactly what makes ondemand step. Benchmark with the governor pinned, and decide whether the product pins it [check].
- **VLIO has no bus-error timeout** (`kernel-general.md` §3 item 10). A companion chip that never asserts RDY freezes the core inside one load or store.
  - On the h3970 (SoC LCDC) the panel whites out first. That is the lockup's signature, not a display bug. Left-right wobble is a partial FIFO underrun, which makes it a live bus-contention gauge.
  - pxa-lcdc keeps fetching through the clock switch with underrun interrupts masked. Use `pxa_lcdc.report_underrun=1` to see them.
- **Writing VRAM saturates the bus.** On the MediaQ boards the core makes almost no progress while anything writes the VRAM window, whether CPU or DMA (`mediaq.md` §5.2). Every uploaded byte is CPU time lost.
- **Blank is real power-down** (`kernel-general.md` §8).
  - `mq11xx` cuts chip power on every CRTC disable and turns the backlight off first. Unblank re-runs the full bring-up, and its latency is unmeasured.
  - Idle blanking from the compositor is therefore the largest power lever, but unblank cost bounds how eagerly to use it.
  - No board has a confirmed suspend/resume round trip yet.
- **Memory.**
  - zram is configured on every board with lzo-rle, the least CPU-hungry option in the QEMU sweeps. Sizing (≥100% of RAM) isn't wired into images yet.
  - The h5550 gets 128 MiB only with `mem=128M@0xa0000000`; that hasn't been tested under pressure.
  - fbcon holds a frame. On the h3970 that frame comes out of the 1 MiB LCDC pool, so disable fbcon in production.
- **ABI choices**, from the qemuarmv5 stand-in work (`h2200.md` §7):
  - **Thumb-1** cuts `.text` by 18–27%, but ALU-heavy code such as pixman's inner loops runs about 2x slower. Keep the compositor, pixman and toolkit render paths in ARM mode.
  - **musl** is about 60% leaner at process start but 80–94% slower on malloc churn. wlroots and pixman allocate regions per frame, so measure before switching libc.

## 3. Design rules

### 3.1 Damage clipping is mandatory end to end
- The copy-type drivers (`mq11xx`, `w100`, `sa1100-lcdc`) copy only the `FB_DAMAGE_CLIPS` rectangles. A commit without clips counts as full damage.
- On a MediaQ board, a commit without clips costs about 18 ms of stalled core, even when only a cursor blinked.
- Verify every stack with `mq11xx_copy_stats`.

### 3.2 One copy per changed pixel

| Stack | Copies after rendering |
|---|---|
| X today | GDK pixmap → X front buffer, then the driver copy |
| Naive Wayland (shm clients, 2–3-buffer swapchains) | GDK pixmap → shm; client buffer-age repair; compositor composite ×2–3 (its own buffer age); then the driver copy |
| **This design** | **the driver copy only**; none on the h3970 with direct scanout |
| Bare-DRM LVGL with the single-buffer patch | the driver copy only |

The mechanisms are in [compositor.md §5.3](compositor.md#53-buffer-model-the-zero-copy-path):
- compositor-allocated client buffers;
- direct scanout of the full-screen app;
- popups drawn inside the app's buffer;
- single client buffers with early release on the copy-type drivers;
- composition only as a damage-limited fallback.

### 3.3 Cached vs uncached targets
- **Cached (mq11xx, w100):** their shmem buffers are cached, so render into them directly.
- **Uncached (pxa-lcdc, sa1100):** scanout memory is write-combined or uncached, so blending into it (text, OVER) reads uncached memory and is slow.
- **h3970 trade-off:** zero copies (render into the scanned-out buffer) vs rendering in cached memory plus one streaming copy. Decide by measurement on text-heavy screens.
- **h3870:** the current driver already has the one-copy shape (render cached, the driver copies into uncached memory). Keep it.

### 3.4 UI design follows the bus
- **Keep the screen static.** No animations, kinetic scrolling or blinking cursors.
- **Keep it flat.** No gradients, textures or translucency. Flat solid backgrounds become 2D-engine fills, and the MediaQ engine fills about 24x faster than the CPU.
- **Keep damage tight,** so that background regions arrive as their own solid rectangles.
- **Run the h3870 and h3970 in landscape** to avoid software rotation entirely. Portrait there is a cost you choose to pay.
- **Full-screen damage is fine occasionally, not continuously.** App switching (one full-screen damage) is fine; continuous full-screen motion is not.

## 4. Using the MediaQ and W3220 features

| Feature | Board | How it's used | Status / effort |
|---|---|---|---|
| Damage-only uploads | mq11xx, w100, sa1100 | wlroots sends `FB_DAMAGE_CLIPS` | free with wlroots; see [compositor.md](compositor.md) for the other stacks |
| 2D-engine fills and copies | MediaQ, W3220 | Driver-internal: solid-colour damage rectangles found in the shadow become engine fills. No userspace stack can drive the engines, because the shadow plane hides "this rectangle moved" and a private blit ioctl was rejected (`mediaq.md` §5.5). Lever: flat themes plus tight damage | works through any stack that sends tight damage |
| Vblank-paced frames | all | Flip events (IRQ on h2210, timer on the others) | free |
| Hardware cursor (64x64) | MediaQ | Cursor plane, if it accepts ARGB8888. Pointless with a stylus, so hide the cursor | free / unused |
| Hardware rotation (plane `rotation`) | MediaQ | Compositor writes the primary-plane property, reports swapped size, and rotates touch | phase 2, 50–150 lines |
| 2x pixel doubling (exact-2x plane scaling) | MediaQ | Full-screen low-res mode: render 120x160, commit with a 240x320 destination box; a quarter of the bus bytes | phase 2, ~100 lines |
| C8 with gamma-LUT palette, tear-free GC0C flip | MediaQ | Video and games via `mediaplayer-drm`; compositor C8 output only if ever needed | handoff (phase 2); compositor C8 is new code |
| YUV overlay, 1–4x upscale, colour key | W3220 | Full-screen video: lease the output (or hand over DRM master) to `mediaplayer-drm`. Windowed: client dmabufs (udmabuf/dma-heap import works in the shmem helper), YUV formats in linux-dmabuf, libliftoff output layer, colour-key properties | handoff ~150 lines; windowed several hundred |

## 5. Board notes

| Board | Notes |
|---|---|
| h3870 | Weakest CPU, no cpufreq driver. Use landscape, so there is no rotation. If the compositor plus GTK2 clients is too heavy, stay on X or run a single-process LVGL UI on bare DRM. Keep the driver's render-cached, copy-to-uncached design. |
| h3970 | Best Wayland case: direct scanout of compositor-allocated client buffers. Grow `lcdc-pool` to one full-screen buffer per open app plus the compositor's. Choose zero-copy vs cached-plus-copy by measurement. Disable fbcon. ondemand wedges this board, so pin the governor. |
| h2210, h5550 | The VRAM leg dominates, so Wayland's gain over X is the removed pixmap copy plus tighter damage. Flat themes get the full engine-fill benefit. Phase 2 adds hardware rotation and pixel doubling. On the h5550, test 128 MiB under pressure. |
| hx4700 | 4x the pixels per full-screen change, so damage discipline matters most. Pin GCC 15 for pixman's iWMMXt paths, and pass `pxa27x_maxfreq=624`. Overlay video goes through the `mediaplayer-drm` handoff. `w100` has no PM ops yet, so blank/resume behaviour is unproven. |
