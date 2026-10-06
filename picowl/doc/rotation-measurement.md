# Hardware versus software rotation: measurement plan

**Status:** historical record, written on 2026-10-04 (picowl commits 985571e and 55002c6) as the plan for the measurement whose results are in [rotation-results.md](rotation-results.md). It was run on an h2200 (PXA255, mq11xx) only. The setup below describes the kernel builds of that day (builds #201 and #202 are the ones with the damage-clip bug). The current behaviour of the `[rotation]` section is in [README.md](../README.md#configuration). The items still open are listed at the end.

picowl defaults to hardware rotation (`[rotation] <output> = auto`, which uses the DRM primary plane's `rotation` property when the driver supports it and software rotation otherwise). The only data so far comes from the media player on the h2200, which is a different workload. The player's paired on/off runs showed, at 320 wide with ordered dither, about +2.5% process CPU and roughly double the dropped frames with hardware rotation, because the physical Bayer phase forced the general dither kernel. At 240 wide it was neutral, and it saved no bus bytes. Those figures predate the player's Bayer-aware kernels. This plan measures the compositor itself. The default stays hardware unless the results below show a regression.

## Question

For a compositor workload on mq11xx boards, does `hardware` cost or save CPU, frame time, bus bytes or latency compared with `software`?

## Setup

- **Boards:** h2200 first (mq11xx, known quirks). Then any other board whose primary plane exposes `rotation`.
- **Kernel:** one build with the damage-clip fix (commit `daf8e6712ae1`). Builds #201 and #202 ignore `FB_DAMAGE_CLIPS` after any rotation or format change and would bias the result against hardware rotation. Record `uname -r` with every run.
- **Fixed conditions:** same picowl binary, same config apart from `[rotation]`, same backlight and CPU frequency governor (pin it), battery or mains held the same, 5 minutes of idle warm-up before each run.
- **Switch:** `[rotation] * = hardware` versus `* = software`, restarting picowl for each run. Do not use the runtime `rotate` action, which would mix transitions into the numbers.
- **Design:** paired and interleaved (hw, sw, hw, sw, ...), 5 pairs per cell, as the player did.

## Matrix

| Axis | Values |
|---|---|
| Transform | 90 and 270 (the portrait panel in landscape), plus 0 as a control |
| Output width | 240 and 320 where the board allows it |
| Workload | W1 idle with clock panel, W2 terminal scrolling, W3 full-screen client commit loop at 30 fps (`picowl-commit-loop --fps 30 --damage full`), W4 small-damage updates, a blinking 16x16 square like a cursor (`picowl-commit-loop --fps 30 --damage 16x16`), W5 media player under picowl (Path A) with ordered dither |

## Test client

`tools/picowl-commit-loop.c` implements W3 and W4. It is a `wl_shm` plus xdg-shell toplevel (app_id `picowl-commit-loop`) that uses RGB565 buffers when the compositor offers them and XRGB8888 otherwise, and takes its size from the first configure. It builds with the `tools` meson option and needs libwayland-client, not wlroots. The compositor must advertise `wp_presentation`, because the per-frame feedback is the measurement.

| Flag | Meaning |
|---|---|
| `--fps N` | Target commit rate (default 30). Commits wait for the `wl_surface.frame` callback and for a timer, so the rate never exceeds N |
| `--damage full` | W3 (default): a diagonal pattern that changes every pixel on every frame, with `damage_buffer` over the whole surface |
| `--damage WxH` | W4: a WxH square (`square` means 16x16) centred on the surface and blinking each frame, with `damage_buffer` over that rectangle only. A square larger than the surface is clamped |
| `--duration SECONDS` | Run time before a clean exit (default 60) |

The first commit damages the whole surface in both modes, because the compositor has no earlier content to keep. It is not counted. Every counted commit damages exactly what `--damage` says, and the exact rectangle and byte count are printed to stderr at start.

At exit the client writes a header line and one CSV row to stdout, with diagnostics on stderr:

`frames_committed,presented,discarded,late,mean_delta_ms,p95_delta_ms,max_delta_ms,format,width,height`

The deltas are between consecutive `presented` timestamps. `late` counts deltas above 1.5 times the target period. The delta columns are `nan` if fewer than two frames were presented. The client exits non-zero with a message on any Wayland or shared memory failure.

## Metrics

| Metric | How |
|---|---|
| picowl CPU | `/proc/<pid>/stat` utime+stime over the run, divided by wall time |
| Frame time | the `presented` deltas from `picowl-commit-loop`. They are only valid when the kernel's user alignment mode is fixup (`echo 2 > /proc/cpu/alignment`) or libdrm is built without its unaligned 64-bit load, otherwise the timestamps advance once a second (see `doc/rotation-results.md`) |
| Dropped or late frames | the `late` and `discarded` columns of `picowl-commit-loop` |
| Bus bytes per commit | the `bytes:` and `copies:` lines of the debugfs file `dri/0/mq11xx_copy_stats`, enabled by writing `Y` to `mq11xx_copy_stats_enable` (mount debugfs first). The `engine fills` line counts small rectangles that the 2D engine filled instead of uploading. Expect 153,600 bytes for a full frame, so anything near that for W4 means damage clips are not honoured |
| Alignment traps | `/proc/cpu/alignment` before and after (the player saw traps that differ by layout) |
| Touch correctness | tap the four corners and a centre target at each transform and check the reported coordinates against the matrix in `src/rotate.c` |
| Fallback | any `hw rotation commit failed` log line fails the run |

## Procedure per run

1. Set the mode, restart picowl, wait for the warm-up.
2. Snapshot `/proc/<pid>/stat`, `/proc/cpu/alignment` and the vram flush counters.
3. Run the workload for 60 s (W5: a fixed 2 minute clip).
4. Snapshot again, save the picowl log, and write one row to a CSV: board, kernel, mode, transform, width, workload, pair index, then the metrics.

## Analysis

- Per cell, compute the paired difference (hw minus sw) and report the median and range across the 5 pairs. Report a result only if the sign is the same in at least 4 of 5 pairs.
- Decision rule:
  - Keep hardware as the default if no cell shows a regression above 2% CPU or a doubling of drops, and W3 and W4 show fewer or equal bus bytes.
  - If W5 alone regresses, as the player saw, keep hardware for the compositor and document that the player should render unrotated, which is what Path A already does.
  - If compositor cells regress, switch the default to software and update the README, `data/picowl.ini.example` and `tests/test-config.c`.
- Write the results to `doc/rotation-results.md`, with the kernel build and picowl commit.

## Open items

Done when the results were written: the comparison on a kernel with the `mq11xx_vram_flush` alignment fix and with correct flip timestamps (see [rotation-results.md](rotation-results.md)). Still open, and tracked in the [roadmap](design/roadmap.md):

- Workloads W1 (idle with clock panel), W2 (terminal scrolling) and W5 (media player under picowl, Path A, with ordered dither) were not run.
- The 270 degree transform and boards other than the h2200 were not measured.
- Touch correctness at each transform, and with hardware rotation, was not checked.
- Logging the kernel release at picowl start-up, so every picowl log is attributable to a kernel build, is not implemented.
