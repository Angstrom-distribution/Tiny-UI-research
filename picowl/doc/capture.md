# Screen capture

Reference for the implemented, opt-in screen capture: what picowl offers, what a client gets, the security trade-off, the cost, and how it is tested. Behaviour that comes from wlroots 0.19.0 internals (rather than from `src/server.c`) is marked not verified, because wlroots is not in this repository. The key is listed in the [README configuration section](../README.md#configuration) and the global in the [README protocol list](../README.md#protocols).

picowl offers `zwlr_screencopy_manager_v1` (version 3, the version of `protocols/wlr-screencopy-unstable-v1.xml`; wlroots creates it with `wlr_screencopy_manager_v1_create()`, see `src/server.c`) so a client such as `grim` or `wf-recorder` can read the screen. It is off by default and enabled with `[capture] enabled = true`. `enabled` takes yes/no, true/false, 1/0 or on/off; any other text logs an error and keeps `false`, and an unknown key in `[capture]` logs an error. With the key on, picowl logs `capture: zwlr_screencopy_manager_v1 enabled` at info level. With it off no global exists.

## Security

A client that can connect to the Wayland socket and sees the global can read everything on screen, including other applications and the on-screen keyboard. picowl has no per-client permission step, so the choice is all or nothing for every client of the session. Leave it off on a production image and turn it on for development images, which is what `data/picowl.ini.example` does.

## What a client gets

- One `wl_shm` format, the output's render format. With the default `[render] format = RGB565` the client must accept `RGB565`; clients that only handle 8888 formats fail. Setting `format = XRGB8888` changes what is offered.
- The buffer is the composited output in the orientation it is committed in. Under hardware rotation that is the frame as the plane receives it (the rotated mode), which `src/output.c` relies on in its headless hardware-rotation test mode (`PICOWL_TEST_HW_ROTATION=1`). Check a captured image on a rotated output before relying on it; this has not been measured on a board.
- The DMA-BUF capture path is advertised by wlroots when the allocator can export DMA-BUFs (not verified), but picowl only uses the pixman renderer and the shm path is the one to use. `tests/pw-capture-client.c` also handles only the shm path.

## Cost

A capture request asks wlroots for a new frame and holds off direct scanout until that frame is done, so a zero-copy client ([zero-copy.md](zero-copy.md)) is composited once per screenshot (wlroots behaviour, not verified here). The capture reads the output buffer with the CPU. On drivers that map dumb buffers write-combined this is slow, so expect a screenshot to take noticeably longer than the frame time on the slower handhelds (an expectation from the caching rules in [buffers.md](buffers.md) and [zero-copy.md](zero-copy.md), not a measurement).

## Not offered

`ext_image_copy_capture_v1` (with `ext_output_image_capture_source_manager_v1`) is not offered. Screen recorders which only speak that protocol need it; it would also keep direct scanout off for the whole recording and needs wayland-protocols 1.37 or newer (not verified here). Whether to add it is a decision for the project owner, see the [roadmap](design/roadmap.md).

## Tests

- `config`: the `[capture]` key, its default (false), an unknown key and an invalid value (`maybe` keeps false).
- `smoke` (headless): with the default config `pw-capture-client` reports `no screencopy`. A second run uses `[render] format = XRGB8888`, `[capture] enabled = true` and `[background] color = #204060`: the log has the `capture: zwlr_screencopy_manager_v1 enabled` line, the captured pixels are all identical, and the pixel equals that colour. The format is XRGB8888 so that the expected value is exact; the default RGB565 capture is not checked for its colour value.
