# Hardware versus software rotation: h2200 results

Results of the plan in `doc/rotation-measurement.md`, measured on an h2200 (PXA255, mq11xx, 240x320 panel) running kernel 7.2.0+ #1 (30 Sep 2026) with picowl at commit 3f74900, which includes the fix that makes hardware rotation take effect at all (see below). Each cell is 5 paired runs of 60 s, alternating which mode runs first, with the compositor restarted for every run. The client was `picowl-commit-loop` at 30 fps. Ticks are `/proc/<pid>/stat` utime+stime of the picowl processes in USER_HZ units, assumed to be 100 per second, so a tick rate reads as percent of one CPU.

## Result

Hardware rotation stays the default. At a 90 degree transform with a full-surface client it uses about a quarter less compositor CPU than software rotation, and in no cell did it cost more CPU, drop frames or upload more bytes beyond the control's noise.

| Transform | Workload | picowl CPU hw / sw (ticks/s) | Median pair difference | Pairs agreeing |
|---|---|---|---|---|
| 90 | full surface (W3) | 16.6 / 22.0 | -5.25 (range -5.49 to -5.10) | 5 of 5 lower with hardware |
| 90 | 16x16 square (W4) | 12.8 / 13.0 | -0.22 | 5 of 5 lower, negligible |
| normal (control) | full surface (W3) | 16.3 / 16.6 | -0.34 | mixed (4 lower, 1 higher) |
| normal (control) | 16x16 square (W4) | 12.6 / 12.7 | +0.03 | mixed |

- **Frames:** no discarded frames in any run. The number of presented frames per second was within 0.6 fps between modes (hardware +0.56 fps at 90 degrees, full surface, 5 of 5 pairs). The count of late frames was the same in both modes.
- **Bus bytes:** the uploaded bytes per second were equal within noise at 90 degrees, full surface (hardware +2.5 percent, 3 pairs higher and 2 lower). The no-rotation control, where both modes are identical in what they upload, differed by +3 percent in 5 of 5 pairs, so differences of that size are not evidence either way. Small damage uploads nothing: the driver fills the changed rectangle with the 2D engine, 25 to 26 fills per second.
- **Hardware rotation is applied:** the primary plane's `rotation` property read 2 in every hardware run at 90 degrees and 1 in the others.

## What the measurement found on the way

- **picowl never applied hardware rotation before this work.** The output was already enabled when picowl asked for the rotation, and the patched wlroots only accepts a plane rotation change while the output is disabled. It logged an error and fell back to software rotation, so earlier "hardware" runs were software. picowl now disables the output first. The result above is the first measurement of real hardware rotation.
- **One kernel alignment fault per upload with rotation.** With the rotated 642 byte VRAM pitch, `mq11xx_vram_flush` does a 32-bit read at an address that is 2 mod 4, which traps and is fixed up in the kernel (about 20 faults per second here, one per full-frame upload, none without rotation). The CPU numbers above include that cost, so hardware rotation will look better once the driver aligns the address.
- **Page-flip timestamps are wrong on this board.** libdrm's `drmHandleEvent` loads the seconds and microseconds of each flip event as one 64-bit pair from a buffer that is only 4-byte aligned. On this CPU, with the kernel's default user alignment mode "ignored", both registers receive the same word, so the microseconds equal the seconds and `wp_presentation` timestamps advance only once a second. With user alignment set to fixup (`echo 2 > /proc/cpu/alignment`) the timestamps are correct. This affects every program that reads flip events through libdrm on this board, so the frame statistics that depend on presentation timestamps (the p95 and maximum columns of the client) are not meaningful in these results; the commit, presented and discarded counts and the late count based on the sequence are unaffected.

## Not covered

- The media player workload (W5) and its dither kernels, which were the reason for the original doubt about hardware rotation at 320 wide.
- Idle and terminal workloads (W1 and W2), the 270 degree transform and other boards.
- Touch correctness at each transform, and touch with hardware rotation.
- Results with a kernel that has the flush alignment fix.
