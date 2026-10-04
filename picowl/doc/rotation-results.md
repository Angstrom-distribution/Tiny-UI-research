# Hardware versus software rotation: h2200 results

Results of the plan in `doc/rotation-measurement.md`, measured on an h2200 (PXA255, mq11xx, 240x320 panel) with picowl at commit 3f74900 or later (it includes the fix that makes hardware rotation take effect at all, see below), a kernel build with the mq11xx flush alignment fix, and a libdrm 2.4.131 package with an 8-byte aligned event read buffer. Each cell is 5 paired runs of 60 s, alternating which mode runs first, with the compositor restarted for every run. The client was `picowl-commit-loop` at 30 fps. Ticks are `/proc/<pid>/stat` utime+stime of the picowl processes in USER_HZ units, assumed to be 100 per second, so a tick rate reads as percent of one CPU. User alignment handling was left at the kernel default.

## Result

Hardware rotation stays the default. At a 90 degree transform with a full-surface client it uses about a quarter less compositor CPU than software rotation and presents slightly more frames, and in no cell did it cost more CPU, drop frames or upload more bytes beyond the control's noise.

| Transform | Workload | picowl CPU hw / sw (ticks/s) | Median pair difference | Pairs agreeing |
|---|---|---|---|---|
| 90 | full surface (W3) | 16.5 / 21.9 | -5.20 (range -5.74 to -5.07) | 5 of 5 lower with hardware |
| 90 | 16x16 square (W4) | 12.8 / 13.1 | -0.33 | 5 of 5 lower, negligible |
| normal (control) | full surface (W3) | 16.5 / 16.3 | -0.03 | mixed (3 lower, 2 higher) |
| normal (control) | 16x16 square (W4) | 12.6 / 12.8 | -0.15 | mixed |

- **Frames:** no discarded frames in any run, and every run presented about 1,500 frames or more. At 90 degrees with the full surface, hardware presented 0.56 fps more (25.4 against 24.9, 5 of 5 pairs) and the mean presentation interval was 0.5 ms shorter (33.9 against 34.5 ms, 5 of 5 pairs). The 95th percentile interval was the same in both modes (70.4 ms, four vblank periods). The maximum interval was one vblank period (17.5 ms) higher with hardware in 4 of 5 pairs, and by a similar amount in the no-rotation control, so it is not evidence either way. The late-frame count differed by less than the pair-to-pair spread.
- **Bus bytes:** the uploaded bytes per second were equal within noise at 90 degrees, full surface (hardware +3 percent, 3 pairs higher and 2 lower). In the no-rotation control, where both modes upload exactly the same, the per-pair difference ranged from -110 kB/s to +84 kB/s, so differences of that size are not evidence either way. Small damage uploads nothing: the driver fills the changed rectangle with the 2D engine, 26 fills per second.
- **Alignment faults:** zero user and zero kernel alignment faults in every run.
- **Hardware rotation is applied:** the primary plane's `rotation` property read 2 in every hardware run at 90 degrees and 1 in the others.
- **Stability:** an earlier run with the same hardware before the kernel and libdrm fixes (with the alignment traps present) gave the same CPU result, 16.6 against 22.0 ticks/s for hardware against software at 90 degrees, so the CPU saving does not depend on those fixes.
- **Kernel warnings:** the kernel logged `vblank wait timed out` 18 times in the 100 minutes of testing since the board booted, none of which stopped a run. It looks like an occasional lost frame interrupt in the driver and is not related to rotation.

## What the measurement found on the way

- **picowl never applied hardware rotation before this work.** The output was already enabled when picowl asked for the rotation, and the patched wlroots only accepts a plane rotation change while the output is disabled. It logged an error and fell back to software rotation, so earlier "hardware" runs were software. picowl now disables the output first.
- **One kernel alignment fault per upload with rotation, now fixed.** With the rotated 642 byte VRAM pitch, `mq11xx_vram_flush` did a 32-bit read at an address that was 2 mod 4, which trapped and was fixed up in the kernel (about 20 faults per second, one per full-frame upload, none without rotation). The kernel now aligns that address down.
- **Page-flip timestamps were wrong on this board, now fixed.** libdrm's `drmHandleEvent` loaded the seconds and microseconds of each flip event as one 64-bit pair from a buffer that was only 4-byte aligned. On this CPU, with the kernel's default user alignment mode "ignored", both registers received the same word, so the microseconds equalled the seconds and `wp_presentation` timestamps advanced only once a second. The patched libdrm aligns the buffer, and the timestamps now advance in real steps. Any program that reads flip events through an unpatched libdrm on this CPU has the problem, and setting user alignment to fixup (`echo 2 > /proc/cpu/alignment`) is a workaround.

## Not covered

- The media player workload (W5) and its dither kernels, which were the reason for the original doubt about hardware rotation at 320 wide.
- Idle and terminal workloads (W1 and W2), the 270 degree transform and other boards.
- Touch correctness at each transform, and touch with hardware rotation.
