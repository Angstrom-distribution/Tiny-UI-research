# Screen capture

picowl offers `zwlr_screencopy_manager_v1` (version 3, from wlroots `wlr_screencopy_manager_v1`) so a client such as `grim` or `wf-recorder` can read the screen. It is off by default and enabled with `[capture] enabled = true`.

## Security

A client that can connect to the Wayland socket and sees the global can read everything on screen, including other applications and the on-screen keyboard. picowl has no per-client permission step, so the choice is all or nothing for every client of the session. Leave it off on a production image and turn it on for development images, which is what `data/picowl.ini.example` does.

## What a client gets

- One `wl_shm` format, the output's render format. With the default `[render] format = RGB565` the client must accept `RGB565`; clients that only handle 8888 formats fail. Setting `format = XRGB8888` changes what is offered.
- The buffer is the composited output in the orientation it is committed in. Check a captured image on a rotated output before relying on it.
- The DMA-BUF capture path is advertised by wlroots when the allocator can export DMA-BUFs, but picowl only uses the pixman renderer and the shm path is the one to use.

## Cost

A capture request asks wlroots for a new frame and holds off direct scanout until that frame is done, so a zero-copy client (`doc/zero-copy.md`) is composited once per screenshot. The capture reads the output buffer with the CPU. On drivers that map dumb buffers write-combined this is slow, so expect a screenshot to take noticeably longer than the frame time on the slower handhelds.

## Not implemented

`ext_image_copy_capture_v1` (with `ext_output_image_capture_source_manager_v1`) is not offered. Screen recorders which only speak that protocol need it; it would also keep direct scanout off for the whole recording and needs wayland-protocols 1.37 or newer.

## Tests

- `config`: the `[capture]` key and its default.
- `smoke` (headless): with the default config `pw-capture-client` reports `no screencopy`; with `[capture] enabled = true` and a `[background]` colour the captured pixels are all identical and, for the 8888 formats, equal to that colour.
