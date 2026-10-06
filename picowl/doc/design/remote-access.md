# Remote access to picowl on the h2200: options, recommendation, design

**Status:** research and design proposal, nothing implemented. Written from five independent online and source sweeps, a skeptic's check of the decisive claims, and a read of the picowl source. Everything about CPU cost on the PXA255 is an extrapolation (nobody has published zlib, ZRLE or JPEG speeds for an ARMv5 core), and every Bluetooth figure comes from other hardware of the era. Nothing was run on the h2200. The tags [read], [inf] and [unknown] are explained in the first line of the text below.


The claim-by-claim evidence, with each source URL and the skeptic's review, is in [remote-access-sources.md](remote-access-sources.md).

Tags: [read] means a sweep read it at the cited URL. [inf] is inference. [unknown] could not be established. The skeptic's corrections are applied. Every CPU figure for the PXA255 is an extrapolation with an error of about 3x or more, because nobody has published zlib, ZRLE, Hextile or JPEG speeds for any ARMv5 core.

## 0. Corrections to the sweep findings

- **neatvnc auth and nettle.** In neatvnc's meson.build, DES, RSA-AES and Apple-DH auth all depend on the `nettle` option [read: https://raw.githubusercontent.com/any1/neatvnc/master/meson.build]. WebSocket needs nettle too.
  - A build with `-Dnettle=disabled` can only offer None, or TLS through gnutls.
  - The "pixman + zlib + aml only" build therefore means None auth.
  - Sweep 3 cites wayvnc's README for the point that macOS Screen Sharing refuses a None server. The skeptic did not re-open it, so I mark that [read, unchecked].
  - A macOS-first user therefore needs nettle, hogweed and gmp on the device.
  - The sweeps report a nettle version pin of `>=4.0 <5.0` in neatvnc master, from a summarised fetch. Nettle 4.0 may not exist in the OE release your build machine uses. This is a possible build blocker and must be checked in the file.
- **"80 KB/s Linux-to-Linux" is not a measurement.** It is Marcel Holtmann's remark in the 2005 LKML thread [read: https://lkml.iu.edu/hypermail/linux/kernel/0510.2/1515.html]. The only measurements in that thread are about 10 KB/s (CF card, BCSP at 921,600 baud) and about 25 KB/s (USB dongle with a Nokia 6230).
- **UART framing.** At 8N1 each byte takes 10 bits on the wire. One 1691-byte BNEP frame at 115,200 baud takes about 147 ms. The 117 ms in the UCL paper ignores start and stop bits.
- **wayvnc and a missing output-power global.** This is non-fatal, but the code warns that "Capturing may fail" [read: https://raw.githubusercontent.com/any1/wayvnc/master/src/main.c].

## 1. Options ranked for this device

Link planning numbers:
- **bt-pan:** the h2200 radio is Bluetooth 1.1, with no EDR [read: https://hp.com/ctg/Manual/c00046403.pdf]. The DH5 ceiling is 90.4 KB/s [inf, arithmetic].
  - Measured BNEP goodput on 1.1-class hardware was 10 to 25 KB/s (2005, other hardware).
  - Latency under load reached seconds.
  - The h2200 Bluetooth UART speed is [unknown]. At 115,200 baud the hard ceiling is 11.5 KB/s.
- **USB (usb0):** the PXA25x UDC is full speed (USB 1.1) and PIO-driven, with no DMA [read: https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/gadget/udc/pxa25x_udc.c]. The bulk ceiling is 1.2 MB/s [read: u_ether.h].
  - Real throughput is [unknown]. Plan on 300 to 600 KB/s [inf].
  - The PIO transfers cost CPU.
- **hx4700 Wi-Fi:** 802.11b. Comparable SD-attached PDAs measured 0.3 to 2.7 Mbit/s [read: https://www.smallnetbuilder.com/wireless/wireless-features/wifipdasecretntkpt1/]. Plan on 125 to 500 KB/s [inf]. The hx4700 frame is 4x larger.

| Rank | Option | CPU cost on PXA255 | Bandwidth | Build effort | Maturity | Verdict |
|---|---|---|---|---|---|---|
| 1 | neatvnc embedded in picowl, headless swap (builtin) | Lowest of the neatvnc variants: no process switch, no screencopy copy. Deflate cost [unknown]. | ZRLE on damage only. Est. 5 to 15 KB per full UI frame, under 1 KB per text line [inf]. | Medium: aml loop in wl_event_loop, callbacks, new module | neatvnc is alive (v1.0.3, 2026-10-03). No ARMv5 report. | Best end state. Build after stage 2 proves out. |
| 2 | wayvnc (external) capturing the headless output | Extra process, one extra RAM copy per frame, plus encoding. RSS [unknown]. | Same as 1 | Low to medium: no wlroots patch, output parking in picowl | wayvnc v0.10.2 (2026-09-25), standard on wlroots. | Best first real implementation. |
| 3 | wayvnc mirroring the DRM output (local display stays on) | As 2, plus the display upload keeps running | Same | Almost none; needs virtual-pointer and data-control exposed | Same as 2 | Quick experiment only. It does not meet the "no upload" wish. |
| 4 | Tiny in-tree RFB server (Raw, Hextile, ZRLE) | Potentially lowest. Hextile est. 2 to 6 ms per full frame [inf]. | Hextile is larger than ZRLE on text | About 800 to 1200 lines [inf], and you own the client quirks | Nothing to inherit | Only if measured zlib cost kills ZRLE. macOS Screen Sharing reportedly needs ZRLE or Zlib (weak source), so Hextile-only is risky. |
| 5 | LibVNCServer embedded | Similar to 4. Its ZRLE defaults to zlib level 6, so it needs patching to level 1. | Broad encoding set | Medium | GPLv2, slow releases (0.9.15, 2024-12-22) | Licence check first. neatvnc is the better fit. |
| 6 | noVNC (browser) | None on the device if served from the Mac | Same as the underlying stream | Client-side only | Alive, MPL 2.0 | A client choice, not a server. |
| 7 | RDP (FreeRDP, xrdp) | RemoteFX is heavy | No better than VNC | High | No ARMv5 evidence | Unsuitable. |
| 8 | Waypipe | zstd or lz4 on the device. Build status [unknown]. | n/a | n/a | Rust, GPLv3 | Different job: forwards single apps, not the screen. No iOS or native macOS client. |
| 9 | SPICE, gnome-remote-desktop, krfb | n/a | n/a | n/a | n/a | Unsuitable: VM-oriented or tied to GNOME or KDE. |

Per-link outcome for the recommended stack (VNC with ZRLE):

| Link | Raw full frame (153,600 B) | Usable? |
|---|---|---|
| h2200 on bt-pan | 6 to 15 s at 10 to 25 KB/s [inf] | Only compressed, damage-only updates. A full refresh takes about 1 to 3 s compressed [inf]. |
| h2200 on USB | about 0.3 to 0.5 s at 300 to 600 KB/s [inf] | Yes. CPU is the limit, not the link. |
| hx4700 on Wi-Fi | 614,400 B raw, about 1.2 to 5 s [inf] | Yes with ZRLE. |

## 2. Recommended path in stages

**Stage 0: measurements.** See section 5. Do them before any code.

**Stage 1: prove the stack on a PC.**
- Run picowl on the headless backend with `[capture] enabled = true`. Connect stock wayvnc and a viewer.
- This tests whether wayvnc and neatvnc accept RGB565 shm frames from picowl. The sweeps could not confirm it. neatvnc has RGB565 in its pixel-format table (read), but wayvnc's own pixels.c returns 0 for RGB565, so it is unknown whether that function sits on the shm path.
- Fallback for the test is `[render] format = XRGB8888`.

**Stage 2: remote-only mode with wayvnc as the server.**
- Build wayvnc and neatvnc for armv5e with jpeg, gbm, h264 and tls disabled, wayvnc without screencopy-dmabuf and PAM, and `--max-fps 5`.
- Use wlr-screencopy, which picowl already offers. ext-image-copy-capture is optional: two lines in server.c, shm only.
- Implement the headless-output swap from section 4.
- Why an existing server first: it is maintained, handles client quirks, and has damage, Fence and flow control already. Writing RFB yourself buys nothing until the CPU is measured.
- Auth, in order of preference:
  1. **None**, bound to bnep0 or usb0 only, for first tests with TigerVNC or noVNC.
  2. **DES VNC auth**, which needs nettle, for macOS Screen Sharing.
  3. **TLS or RSA-AES**, cost unknown. Skip unless the link is untrusted.
- Pacing (a wayvnc and neatvnc source reading, not a tested result):
  - neatvnc's Fence-based flow control drops frames when inflight bytes exceed bandwidth times (33 ms + RTT), and the damage stays accumulated. The 33 ms constant is aggressive for a 10 KB/s link.
  - It only works if the viewer supports Fence, which is unchecked per client.
  - Set `--max-fps` to 2 to 5 and the headless refresh to match.

**Stage 3: embed neatvnc in picowl (`backend = builtin`).**
- Do this only if stage 2 measures too much RSS or CPU from the second process and the copy.
- It feeds the committed RAM buffer with `nvnc_frame_from_raw` plus `nvnc_frame_set_damage`.
- Keys and pointer events are injected straight into the seat. No capture or virtual-input globals are exposed to other clients.
- aml must be driven from the wl_event_loop. aml is pthread-based, so prototype this. A VNC bug can then crash the compositor.

**Encodings and pixel formats for bt-pan** (neatvnc source reading [read], not run):
- neatvnc emits only Raw, Tight and ZRLE, taking the first of these in the client's preference order. It never emits Hextile, CopyRect or RRE. H.264 needs a GBM buffer, so it is out.
- Use **ZRLE with a 16 bpp pixel format**.
  - Raw RGB565 matches picowl's buffer.
  - ZRLE's CPIXEL is 2 bytes at 16 bpp.
  - Crisp-font UI content has two to a few colours per tile, so palette and run-length coding shrink it before deflate.
- Tight with JPEG only for the media player, and only if built with libjpeg-turbo, which has no ARMv5 SIMD. Text goes blurry.
- Avoid 8 bpp (ZRLE gains little), ZYWRLE (neatvnc lacks it, and TigerVNC and noVNC cannot decode it) and TightPNG.
- Zlib level 1 is already what neatvnc uses. TurboVNC and TigerVNC measured that higher levels cost far more CPU for almost no gain [read: https://turbovnc.org/pmwiki/uploads/About/turbototiger.pdf].
- Enable the Cursor pseudo-encoding so the viewer draws the pointer, and set TCP_NODELAY.
- The default neatvnc palette search is a per-pixel memcmp [read]. A 16-bit specialisation may be worth a few ms per frame [inf].

**Bandwidth and frame-rate budget on bt-pan** (derived from the 10 to 25 KB/s measured range; the figures below are estimates):

| Quantity | Value | Basis |
|---|---|---|
| Sustained payload | 5 to 8 KB/s | Leaves headroom for ACKs and input |
| Pending-update cap | 2 to 4 KB | Avoids queue delay of seconds |
| Frame-rate cap | 2 to 5 changed frames/s | Damage-driven, so idle costs nothing |
| One typed character | about 0.3 to 0.6 KB compressed | About 30 to 60 ms of serialisation at 10 KB/s |
| Full refresh (unblank, rotate, app switch) | 5 to 15 KB compressed, about 1 to 3 s | Estimate |
| Video | about 1 to 3 fps at best | 240x136 raw is 65,280 B per frame |

The video row works out as follows. Ten fps in 10 KB/s means 1,000 B per frame, which is 0.25 bit per pixel. The recommended policy is to cap the update rate while the player is on screen (1 to 3 fps).

Latency targets: about 100 ms feels instant and 1 s breaks flow [read: https://www.nngroup.com/articles/response-times-3-important-limits/]. A typed character costs roughly 100 to 140 ms (idle RTT of 40 to 85 ms plus serialisation) only if no large update is queued ahead of it [inf].

## 3. Client matrix

| Client | Platforms | Server settings it needs | Slow-link settings and limits |
|---|---|---|---|
| **TigerVNC viewer** (recommended) | macOS, Linux, Windows | None, VncAuth, VeNCrypt or RA2. No Apple ARD. | `LowColorLevel`, `PreferredEncoding=ZRLE`, `CompressLevel`, `NoJPEG` [read: https://tigervnc.org/doc/vncviewer.html]. Best-documented controls and the easiest packet capture. |
| **macOS Screen Sharing** (`vnc://host:port`) | macOS | Reportedly refuses a None server, so VNC password (DES) is needed. Whether it completes the handshake with neatvnc is unverified. | No tuning controls found. Reportedly only Zlib and ZRLE (weak source, unchecked). Use for convenience, not for the slow link. |
| **noVNC** (runner-up) | Any browser, iOS Safari 15+ | WebSocket from the server (needs nettle in neatvnc) or websockify on the Mac | Quality and compression 0 to 9. No colour-depth setting. Serve the page from the Mac, never from the handheld. Supports Apple DH, RSA-AES and VeNCrypt Plain [read: https://github.com/novnc/noVNC]. |
| **iOS and Android apps** (Screens, Jump Desktop, RealVNC Viewer, bVNC) | iOS, Android | VNC password is the safe common denominator | Jump Desktop reportedly has 8 and 16-bit colour modes (old third-party listing, unchecked). Screens has adaptive quality. Encodings, prices and Fence support are unverified. |
| **Remmina** (libvncclient) | Linux | Any | 256-colour mode and a quality setting. The only common client that can use ZYWRLE, which no server in the plan offers. |
| **RealVNC Viewer** | All | ZRLE works against any server (inferred) | Possible cloud sign-in prompt. Direct IP use stays free: unconfirmed. |

Reaching the device from a Mac:
- **USB:** `pxa25x_udc` reports no altsetting support, so g_ether cannot offer CDC ECM and falls back to CDC Subset or RNDIS [read: https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/gadget/legacy/ether.c]. macOS ships neither of those drivers (from memory, unchecked). CDC EEM needs no altsettings, but whether macOS has an EEM host driver is unchecked.
- A Mac may therefore have to reach the h2200 over bt-pan or through a Linux or Windows intermediate. Test this early.

## 4. picowl design

### 4.1 `[remote]` section (all proposed [inf])

```
[remote]
enable       = false
backend      = external      # external (wayvnc) | builtin (neatvnc, stage 3)
local_output = auto          # auto | on | off
fps          = 5             # headless refresh, 1..30
local_input  = ignore        # ignore | pass
lease        = revoke        # revoke | reject
backlight    = off           # off | keep
listen       = bnep0         # builtin only; else bind in the wayvnc unit
```

- `local_output = off` is remote-only mode.
- `on` is mirror mode. It leaves the display path alone and captures the committed buffer.
- `auto` means off on copy-type outputs (h2200, where upload is expensive) and on elsewhere.
- Capture, virtual-pointer, virtual-keyboard and data-control globals exist only when `enable = true`. Filter them with `wl_display_set_global_filter` to the helper's pid or uid, so ordinary clients cannot capture or inject.

### 4.2 Why the swap is needed

- wlr-screencopy reads the buffer from the output's commit event, and that buffer is already in RAM, so capture never touches video memory [read: `subprojects/wlroots/types/wlr_screencopy_v1.c`].
- A disabled output cannot be captured. Screencopy fails the frame when `!output->enabled`, checked at lines 290, 364 and 525 of the local wlroots 0.19.0 tree. picowl's blank path disables every DRM output and stops rendering.
- Stock wayvnc forces the output power on while a client is connected, which would light the local panel [read: wayvnc `src/main.c`].
- So the capture source must be an always-enabled RAM output. Option 1 is a headless output, using public wlroots API with no patch. Option 2 is a DRM patch that skips the atomic commit. Option 2 does not darken the panel by itself and needs the present events faked.

### 4.3 Remote-only behaviour (`local_output = off`)

**Start**
1. If a lease is active, call `pw_lease_revoke(server, "remote session")`.
2. Create the headless output with `wlr_headless_add_output` at the DRM output's logical size, transform NORMAL, refresh `fps * 1000` mHz. Skip `output_config_transform` for it so a `* = 90` wildcard does not rotate it.
3. Park the DRM `pw_output`:
   - unmap touch devices
   - `output_commit_disable`
   - `wlr_output_layout_remove`
   - move it from `server->outputs` to a parked list
4. The headless backend must be created and added to the multi backend before `wlr_backend_start`.
5. Set a new `PW_INHIBIT_REMOTE` so dim and blank timers do not fire.
6. If `backlight = off`, write backlight 0 yourself. Whether the h2200 backlight goes dark on DRM disable is [unknown].

**Running**
- pixman renders only damage into a 2-slot RGB565 swapchain in RAM, at most `fps` frames per second. That adds 153,600 B of RAM per slot.
- The headless refresh value caps the frame callbacks clients receive.
- The headless output always reports power ON, so wayvnc's power request is a no-op.

**End** (disconnect, wayvnc crash, picowl quit)
- Destroy the headless output and re-adopt the parked DRM output through the existing `output_adopt` and unblank path (`output_enable_rotated`, `copy_swapchain_sync`, `pw_input_apply_rotation`).
- The MODE and ENABLED commit makes wlroots damage the whole output. That is one full upload of 153,600 B, about 17.5 ms of bus, and the first frame appears about one modeset later.
- Drop `PW_INHIBIT_REMOTE`, restore the backlight, and call `pw_power_activity` so idle timers restart from the disconnect.

**Triggering**
- Manual first: SIGUSR1 turns remote on and SIGUSR2 turns it off (picowl already handles signals in server.c).
- Automatic for the external backend: a systemd socket unit bound to bnep0 or usb0 passes the fd to wayvnc (supported since wayvnc 0.9.0). `ExecStartPre` sends SIGUSR1 and `ExecStopPost` sends SIGUSR2. That also covers a wayvnc crash.
- With `builtin`, neatvnc's client callbacks give the session boundary directly.

### 4.4 Awkward cases

| Case | Behaviour | Notes |
|---|---|---|
| DRM lease to the media player | The lease is revoked at session start. New lease requests are rejected while parked, because rule 5 in doc/lease.md requires picowl to have that output. | A leased player's scanout never passes through the compositor, so a viewer cannot see it. Video visibly interrupts. Player recovery from "finished" mid-playback is [unknown]. The player's Path A works on a non-copy-type output (buffers answered "retained", double-buffered). |
| Hardware rotation | The headless output is upright (NORMAL), so there is no rotation work and no software-rotation case. | A runtime rotate during a session must either resize the RFB framebuffer (ExtendedDesktopSize) or be ignored. Viewer behaviour is unknown. On restore, `output_enable_rotated` reapplies hardware rotation. |
| Panel strip, tiled layout | `pw_view_arrange_all` reruns on adopt, and the panel surface is recreated, as on lease teardown. | Check that no window keeps the old output size. |
| Direct scanout | While a screencopy is pending, `attach_render` is locked, so direct scanout is off. The headless output is not copy-type, so zero-copy clients are composited. | An idle screen costs nothing because wayvnc uses copy_with_damage. |
| Local touch while remote | `local_input = ignore` is the default. The power key switches the session to mirror (DRM output re-adopted, headless kept). | Touch devices were unmapped at start. |
| Remote input | A `remote` flag on virtual pointer and keyboard devices makes `activity()` reset the idle timer only, never swallow the event and never wake the display. | Today a blanked display swallows input and wakes. |
| Power and blank | The power key blanks as today unless switching to mirror. Idle timers are held by `PW_INHIBIT_REMOTE`. | Lease and VT-away inhibits still apply. |
| Clock domain | Both outputs use CLOCK_MONOTONIC (inferred). | wp_presentation refresh changes at the swap, so a player that syncs on it may see a change. Test with the player. |

### 4.5 What changes and what it saves

**Files and APIs touched** [inf]:
- New `src/remote.c` and `src/remote.h`.
- `server.c`: headless and multi backend before start, signals, global filter, `wlr_data_control_manager_v1_create`, optional ext-image-copy-capture managers.
- `output.c`: parked list and `remote` flag.
- `input.c`: split `activity()`.
- `power.c` and `dim.c`: `PW_INHIBIT_REMOTE` and backlight.
- `lease.c`: revoke or reject.
- `config.c`: `[remote]` keys.
- Docs and tests.
- No wlroots patch for option 1. Option 2 would be a new DRM patch of roughly 80 to 150 lines [inf, untried].

**Saved:**
- The KMS commit and VRAM upload. Display bus traffic goes to 0 B instead of up to 153,600 B per full frame (17 to 19 ms of bus each at 7.9 to 9.1 MB/s).
- A 240x136 video region costs 7 to 8 ms of bus per frame. That is 18 to 21 percent bus time at 25 fps, an inference from the task's numbers.
- Display power, and frame callbacks above the chosen `fps`.

**Stays:**
- pixman composition, capture copy (stage 2), encoding and the network stack.

**Honest limit:** picowl's own CPU accounting shows only 3.7 ticks/s between a full-surface client (16.5 ticks/s) and a 16x16-damage client (12.8) at 30 fps on the h2200 [read: doc/rotation-results.md]. The bus time may not be charged to picowl's utime and stime. The CPU saving is therefore unproven. Measure it with `mq11xx_copy_stats` before promising a number.

## 5. Experiments before building (cheapest first)

1. **zlib level 1 and neatvnc's `bench/zrle-bench`** on real UI frames (havoc, panel, player). It measures the encode cost per frame. Expected from the extrapolation: tens of ms for a UI frame, 0.2 to 0.4 s for noisy video, with a wide error [inf].
2. **Bluetooth facts:** `hciconfig -a`, `stty -F` on the BT tty, kernel `dmesg`, and the bnep MTU. This shows the Zeevo chip's UART baud and features. If 115,200, the ceiling is 11.5 KB/s.
3. **iperf3 over bnep0 and usb0**, both directions, with `ping` under load and the BT master/slave role noted. Expected 10 to 25 KB/s for bt-pan [inf]. Record which USB gadget mode works with the Mac.
4. **Does the backlight go off when the DRM CRTC is disabled?** Blank the screen and check the backlight.
5. **Per-process RSS and CPU** on the device, with and without a session, as a PC-side build first:
   - Cross-build aml and neatvnc (nettle available?) and wayvnc for armv5e. Confirm the nettle version on the build machine.
   - Run the idle, typing and terminal-scroll scenarios for 5 minutes.
6. **PC test of picowl headless plus wayvnc plus TigerVNC, then Screen Sharing and noVNC.** It answers RGB565 acceptance, auth, and which encodings and Fence each client uses. Capture `SetEncodings`.
7. **Display bus saving:** read `mq11xx_copy_stats` and utime/stime during mirror versus remote-only (stage 2).
8. **Output swap with the panel and player:** park and restore several times with the player in Path A.

## 6. Still unknown

- Encode cost on the PXA255 and the RSS of wayvnc or neatvnc. All CPU figures above are extrapolations.
- Real bt-pan and usb0 throughput and latency on the h2200, and its Bluetooth UART baud rate.
- Whether neatvnc and aml build and run on armv5e soft-float, and whether the nettle pin (reported as `>=4.0 <5.0`) is satisfiable.
- Client behaviour:
  - Per client: encodings, Fence and ContinuousUpdates support. Not checked for Screen Sharing, Jump Desktop, Screens, RealVNC and bVNC.
  - Whether macOS Screen Sharing completes the handshake against neatvnc with DES or Apple-DH.
- Whether a Mac can reach the device over USB (EEM or any ECM-class driver) or only via bt-pan or an intermediate.
- Whether the picowl or wlroots 0.19.0 behaviour matches the local tree on other builds. Only the local tree was verified.
- TLS and RSA-AES cost on the PXA255.
- Whether the player survives lease revocation and the output swap.
- Whether the real CPU saving from skipping the upload is visible beyond the bus.
- picowl's licence against LibVNCServer's GPLv2 (LICENSE not read).
