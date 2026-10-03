# UI stack for iPAQs on Linux 7.2

This picks a display-server and toolkit architecture for the h3870, h3970, h2210, h5550 and hx4700. All five run the mainline-derived 7.2 port (`unified-mainline-port`) with an Angstrom/OE userspace. The current baseline is Xorg `modesetting` + matchbox + GPE on GTK+ 2. GTK+ 2 stays, because GTK3 is too heavy on these devices.

| File | Contents |
|---|---|
| [README.md](README.md) | Summary, decisions, plan, measurements, open checks |
| [hardware.md](hardware.md) | Targets and display drivers, kernel-side constraints, design rules (damage, copy budget, bus-aware UI), MediaQ/W3220 features, board notes |
| [compositor.md](compositor.md) | Display-server evaluation (X, Weston, wlroots, rejected bases), pixman, the compositor design and buffer model, build; source notes on Weston and wlroots |
| [toolkits.md](toolkits.md) | The GTK+ 2 Wayland backend, LVGL, EFL/Evas; source notes on each |

**Evidence.**
- Board and kernel facts come from the project's references: `kernel-general.md`, `hx4700.md`, `h39xx.md`, `h3800.md`, `h2200.md`, `h5xxx.md` and `mediaq.md`.
- Software claims come from reading current source:
  - EFL `e303767c34` (2026-09)
  - Weston `6cce279a` (16.0+, 2026-09)
  - wlroots master `7392b331` (0.20-dev)
  - LVGL 9.6
  - pixman, from a 2013 snapshot (the only tree reachable)
- Nothing here has been built or run on hardware.
- **[check]** marks claims that need verifying on the device or against the tree you ship. **[est]** marks estimates.

## Summary

1. **Every board already has a KMS driver:** sa1100-lcdc, pxa-lcdc, mq11xx and w100.
   - The binding constraint is presentation bandwidth, not CPU. A full-frame update stalls the core for 17–25 ms on the MediaQ and W3220 boards.
   - Any stack that doesn't send `FB_DAMAGE_CLIPS` is disqualified as shipped.
2. **Design rule: one copy per changed pixel after rendering.** That copy is the driver's damage-only copy into device memory; every other copy is a defect ([hardware.md §3](hardware.md#3-design-rules)).
3. **Compositor: a purpose-built wlroots compositor of about 1.5–2.5k lines, grown from tinywl.**
   - pixman renderer, RGB565 output.
   - Compositor-allocated client buffers.
   - Direct scanout of the full-screen app, with no compositor copy.
   - Weston serves only as the feasibility test ([compositor.md](compositor.md)).
4. **Toolkit: GTK+ 2 with a new `gdk/wayland` backend.**
   - RGB565, single buffer.
   - Popups drawn inside the app's full-screen buffer.
   - LVGL is the second toolkit: Wayland clients for small fast apps, or the whole UI on bare DRM for the h3870 ([toolkits.md](toolkits.md)).
5. **Rejected:** Enlightenment and a revival of the Evas 16-bit engine; swc, Smithay, phoc and labwc as compositor bases; GTK3 and GTK4.
6. **Hardware features:**
   - The MediaQ and W3220 2D engines are reached through tight damage and flat themes. The drivers already turn solid damaged rectangles into engine fills.
   - MediaQ hardware rotation and pixel doubling become compositor features.
   - Full-screen video goes to the existing `mediaplayer-drm` through a lease or master handover, so the W3220 YUV overlay and the MediaQ C8/doubling paths get used ([hardware.md §4](hardware.md#4-using-the-mediaq-and-w3220-features)).
7. **Build: everything we write uses meson.** One decision is open: how to build the GTK+ 2.24 backend ([toolkits.md §1.4](toolkits.md#14-build-system-decision)).

## Plan

1. **Measure the X baseline** on today's drivers (below). Apply the cheap X levers now: `AccelMethod none`, no animations or cursor blink, a flat theme. Run with `cpufreq.default_governor=performance` so ondemand transitions don't distort the numbers ([hardware.md §2](hardware.md#2-kernel-side-constraints)).
2. **Weston feasibility run** ([compositor.md §2](compositor.md#2-weston)). It confirms software compositing and damage behaviour on each board before any compositor code exists.
3. **Compositor milestones:**
   1. tinywl with RGB565 shows a client on all boards, with per-rectangle uploads in the copy stats. Start in the QEMU hx4700 machine, which models W3220 scanout.
   2. Window policy and app switching.
   3. Touch and keys.
   4. Layer-shell panel and wvkbd.
   5. Idle/blank and rotation.
   6. The zero-copy buffer model ([compositor.md §5.3](compositor.md#53-buffer-model-the-zero-copy-path)).
4. **GDK2 backend milestones:**
   1. gtk-demo draws.
   2. In-buffer menus and combos.
   3. Clipboard.
   4. gpe-calendar.
   5. The GPE panel as a layer-shell client.
5. **Phase 2,** gated on measurements: MediaQ hardware rotation, pixel doubling, video handoff, windowed W3220 overlay ([compositor.md §5.4](compositor.md#54-phase-2)).

## Measurements

Use the same kernel and rootfs for each stack, with the driver counters: `mq11xx_copy_stats`, `mq11xx_timing`, and `w100_2d` in debugfs.

- **Bytes and time uploaded per workload** for X, Weston and our compositor. Workloads: idle, typing, opening a menu, switching apps, scrolling a list. This decides X vs Wayland on the copy-type boards.
- **Damage sanity checks:**
  - One keystroke should upload hundreds of bytes.
  - A no-change frame should upload zero.
  - A cursor-only frame should upload zero.
- **Userspace copies per frame:** pixman composite calls and boxes, and memcpy bytes, using an LD_PRELOAD shim or uprobes.
- **Engine-fill hit rate,** flat theme vs textured theme.
- **RSS/PSS of the whole UI,** idle and with 3 apps open:
  - X + matchbox + GPE
  - Weston
  - our compositor + GTK2 apps
  - LVGL single-process
- **h3970:** rendering into write-combined memory vs cached rendering plus one copy, with text-heavy content.
- **h3870/h3970:** software rotation cost with RGB565 clients.
- **Cold start to first frame** of one GPE app, X vs Wayland.
- **Touch latency,** tap to pixel. Also unblank latency on mq11xx, which re-inits the chip on every unblank.

Methodology follows `kernel-general.md` §11:
- **T1:** code inspection.
- **T2:** QEMU, which is meaningful for DRM/modeset shape only. It has no companion chips and no bus timing.
- **T3:** real hardware. Every number above is T3.

## Open checks

- **PRIME and dmabuf support on all five drivers:**
  - PRIME export, which the wlroots dumb-buffer allocator needs.
  - dmabuf mmap, which the zero-copy buffer model needs.
- **Copy completion before the flip event** on the copy-type drivers. This is what makes early release of client buffers safe.
- **Lock-free C11 atomics on the SA-1110** with GCC 15.3.
- **mq11xx cursor plane** accepts ARGB8888.
- **h3970 `lcdc-pool` sizing** with several apps open.
- **GDK2 out-of-tree build:** whether the backend can be built out of tree with meson.
- **pixman fast path** for identity 8888→565 SRC in the pixman tree you ship.
- **wlroots DRM lease** of the only output, in practice.
- **Compositor frame bursts under ondemand:** whether they provoke the PXA25x bus/erratum-26 class of failures. If yes, pin the governor in the product image.

## Sources

**Source trees** (GitHub mirrors, because gitlab.freedesktop.org was unreachable):
- [Enlightenment/efl](https://github.com/Enlightenment/efl)
- [Enlightenment/enlightenment](https://github.com/Enlightenment/enlightenment)
- [wayland-mirror/weston](https://github.com/wayland-mirror/weston)
- [myaiexp/wlroots-ayasa0520](https://github.com/myaiexp/wlroots-ayasa0520) (wlroots 0.19 / 0.20-dev)
- [lvgl/lvgl](https://github.com/lvgl/lvgl)
- [servo/pixman](https://github.com/servo/pixman) (2013)
- [michaelforney/swc](https://github.com/michaelforney/swc)
- [Smithay/smithay](https://github.com/Smithay/smithay)
- [cage-kiosk/cage](https://github.com/cage-kiosk/cage)
- [labwc/labwc](https://github.com/labwc/labwc)
- [jjsullivan5196/wvkbd](https://github.com/jjsullivan5196/wvkbd)
- [torvalds/linux](https://github.com/torvalds/linux)

**Pages and commits:**
- [weston-drm(7)](https://www.mankier.com/7/weston-drm)
- [Weston RGB565 pixman commit](https://code.tokarch.uk/mainnika/weston/commit/f8da0c2552d89a9519a31b669b4334807e539232)
- [wlroots pixman renderer](https://git.nixnet.services/blankie/wlroots/commit/0d90dddfab48a12a7519c11c6062b717ca3b7581)
- [wlroots FB_DAMAGE_CLIPS](https://git.nixnet.services/blankie/wlroots/commit/46c42e55c6e0b08d9e7db989d6a10f073525e999)
- [pixman iWMMXt (debian-x)](https://lists.debian.org/debian-x/2011/10/msg00247.html)
- [GNOME: no GTK+ 2 Wayland backend](https://wiki.gnome.org/Initiatives(2f)Wayland(2f)GTK(2b).html)
- [LVGL N9H30 review](https://blog.lvgl.io/2022-02-18/numaker-hmi-n9h30-review)
