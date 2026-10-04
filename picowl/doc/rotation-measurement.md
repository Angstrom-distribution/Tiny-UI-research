# Hardware versus software rotation: measurement plan

picowl defaults to hardware rotation (`[rotation] auto`). The only data so far comes from the media player on the h2200, which is a different workload. The player's paired on/off runs showed, at 320 wide with ordered dither, about +2.5% process CPU and roughly double the dropped frames with hardware rotation, because the physical Bayer phase forced the general dither kernel. At 240 wide it was neutral, and it saved no bus bytes. Those figures predate the player's Bayer-aware kernels. This plan measures the compositor itself. The default stays hardware unless the results below show a regression.

## Question

For a compositor workload on mq11xx boards, does `hardware` cost or save CPU, frame time, bus bytes or latency compared with `software`?

## Setup

- **Boards:** h2200 first (mq11xx, known quirks). Then any other board whose primary plane exposes `rotation`.
- **Kernel:** one build with the damage-clip fix (commit `daf8e6712ae1`). Builds #201 and #202 ignore `FB_DAMAGE_CLIPS` after any rotation or format change and would bias the result against hardware rotation. Record `uname -r` with every run.
- **Fixed conditions:** same picowl binary, same config apart from `[rotation]`, same backlight and CPU frequency governor (pin it), battery or mains held the same, 5 minutes of idle warm-up before each run.
- **Switch:** `* = hardware` versus `* = software`, restarting picowl for each run. Do not use the runtime `rotate` action, which would mix transitions into the numbers.
- **Design:** paired and interleaved (hw, sw, hw, sw, ...), 5 pairs per cell, as the player did.

## Matrix

| Axis | Values |
|---|---|
| Transform | 90 and 270 (the portrait panel in landscape), plus 0 as a control |
| Output width | 240 and 320 where the board allows it |
| Workload | W1 idle with clock panel, W2 terminal scrolling, W3 full-screen client commit loop at 30 fps (a test client that fills the surface with a moving pattern), W4 small-damage updates (cursor blink, 16x16), W5 media player under picowl (Path A) with ordered dither |

## Metrics

| Metric | How |
|---|---|
| picowl CPU | `/proc/<pid>/stat` utime+stime over the run, divided by wall time |
| Frame time | picowl's per-commit timing log, or `wp_presentation` `presented` deltas in the test client |
| Dropped or late frames | count of `presented` deltas above 1.5x the target period; `discarded` events |
| Bus bytes per commit | the mq11xx `mq11xx_vram_flush` counters or trace points; expect 153,600 bytes for a full frame, so anything near that for W4 means damage clips are not honoured |
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

## Open items before running

- A W3 test client does not exist yet. A small program in `tools/` using `wl_shm` plus `wp_presentation` is enough.
- Check whether picowl already logs per-commit timing. If not, add it behind a debug option.
- Confirm where the mq11xx flush counters are exposed on the current kernel.
- Optional hardening: log the kernel release at startup (`uname`), so every picowl log is attributable to a kernel build.
