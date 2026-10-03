# DRM lease

picowl offers its DRM output through `wp_drm_lease_device_v1` (wlroots `wlr_drm_lease_v1`). The media player (`--vo drm:lease`) gets a DRM fd for the connector, its CRTC and planes, and drives KMS directly: the hardware paths a Wayland client cannot reach (the hx4700 YUV overlay, MediaQ C8 and pixel doubling, GC0C tear-free flips). There is no VT switch. picowl keeps its session, its input and its power policy, and takes the output back as soon as the lease ends. The design and the reasoning are in `doc/design/drm-lease.md`; the player side is §3.2 of `doc/mediaplayer-integration.md`.

Source: `src/lease.c` (protocol glue, state), `src/leasepolicy.c` (pure grant and key policy), hooks in `output.c`, `input.c`, `view.c`, `power.c`, and patch 0004 of wlroots (`subprojects/packagefiles/wlroots/README.md`).

## Configuration

```ini
[lease]
enable = true          ; create the wp_drm_lease_device_v1 global
allow = mediaplayer    ; comma-separated app_ids allowed to lease; * = any
```

`enable = false` creates no global, so no client gets a DRM fd. `allow` is a comma-separated list; blanks around entries are ignored, `*` accepts any focused client, an empty value rejects every request. The global exists only with the DRM backend (not headless or nested) and when the user can open the card as a non-master (the `video` group, `data/picowl.service`); picowl logs `lease: no DRM backend, disabled` otherwise.

## Who may lease

picowl grants a request only when all of these hold. Otherwise it rejects (the client gets `finished`) and logs `lease: rejected (reason)`.

1. `[lease] enable` is set and no lease is active (one output, one lease).
2. The session is active (the kernel lease needs DRM master).
3. No layer surface holds exclusive keyboard focus (a lease must not bypass a lock surface).
4. The requester owns the focused, mapped toplevel, and that toplevel's `app_id` is in `allow`.
5. The request names exactly one connector, and picowl has that output.

The player therefore maps a fullscreen toplevel with `app_id = "mediaplayer"` first, for example one `wp_single_pixel_buffer` scaled with a viewport, and waits for the `activated` state before it submits the request.

## What the lessee gets

The connector, its CRTC, the CRTC's primary plane, the cursor plane if there is one, and (patch 0004) every overlay plane which can scan out on that CRTC. The CRTC is disabled when the lease starts. The fd is a new `drm_file`, so the player sets `UNIVERSAL_PLANES` and `ATOMIC` itself and sets every property it relies on (plane `rotation`, which picowl may have left set on MediaQ, `COLOR_ENCODING`, `COLOR_RANGE`) in its first `ALLOW_MODESET` commit.

wlroots withdraws the leased connector while granting, so `wp_drm_lease_connector_v1.withdrawn` can arrive before `lease_fd`. A failed grant sends `finished` (possibly twice); picowl keeps the output then.

## While leased

picowl has no outputs (wlroots destroys the `wlr_output` and creates it again when the lease ends), so it draws nothing.

| What | Behaviour |
|---|---|
| Power | Dimming and blanking are held (`PW_INHIBIT_LEASE`, see `doc/power.md`). A blanked screen is unblanked before the grant. Normal timeouts restart from the end of the lease |
| Keyboard | Unchanged: the lessee's toplevel keeps the focus and gets the keys |
| Keybindings | `blank` (the power key), `cycle`, `spawn`, `panel`: end the lease first, then run. `close` and `quit` run as usual (the lease ends with the client or the compositor). `rotate` is ignored: the player owns scanout |
| Touch | Every touch device is mapped to the region `{0,0,W,H}` in panel-native mode pixels and gets its default calibration matrix, so the player receives pointer coordinates in the frame its KMS code renders in, on hardware- and software-rotation boards alike. The lessee's toplevel is the target. Tap-and-hold works (`BTN_RIGHT`), without the animation |
| Focus | New toplevels and activation requests (xdg-activation, foreign-toplevel) do not take the focus. New toplevels stack behind the lessee, so a pop-up does not end playback |
| Views and panel | No configures go out. The panel's layer surface is closed with the output, as after a VT switch; it re-creates its surface on the next `wl_output` |
| Lessee unmaps or is destroyed | picowl revokes |

## How a lease ends

| Cause | What happens |
|---|---|
| Player destroys the `wp_drm_lease_v1` object, exits or crashes | wlroots revokes at once; picowl repaints |
| Player closes the fd only | The kernel sends a LEASE uevent; wlroots rescans, which needs a working udev monitor in picowl. Without one the screen stays dark until the object is destroyed. The player always destroys the object |
| Key (power, cycle, spawn, panel), lessee unmapped | picowl revokes |
| VT switch away | picowl revokes in the session `active` handler, while it is still master, and the player gets `finished` |
| picowl exits | `pw_lease_finish` revokes before the backend is destroyed. If picowl crashes the kernel revokes when its master fd closes |

When the lease ends, wlroots emits `new_output`; `output_new` applies the hardware rotation, copy type, touch matrix and view layout again and offers the output again (log: `lease: ended, taking the output back`, `lease: offering NAME`). A lease which ends while another VT is active parks the new output (`output NAME: session inactive, waiting for it`) and adopts it when the session returns.

## Failure modes

| Condition | Behaviour |
|---|---|
| Headless, nested or no DRM backend | No global; one info line |
| `drmModeCreateLease` fails | picowl keeps the output; the client gets `finished` |
| Overlay plane not leased (a driver whose `possible_crtcs` does not include the CRTC) | The player's overlay commit fails and it uses its software path; C8, doubling and flips on the primary plane still work |
| Wrong wlroots | `meson setup` fails unless wlroots has patch 0004 (`WLR_DRM_LEASE_OVERLAY_PLANES`): without it the grant writes to freed memory |

## Testing

- `leasepolicy` (unit): the grant matrix, the allow list, the key policy.
- `config` (unit): the `[lease]` keys. `dim` (unit): the lease sequences of the state machine.
- `smoke` (headless): the log line `lease: no DRM backend, disabled`, and `pw-test-client --expect-no-global wp_drm_lease_device_v1`. This covers the NULL manager and the teardown.
- `lease-vkms` (suite `vkms`, needs root and the `vkms` module, skips with exit 77 otherwise): `tests/lease-vkms.sh` with `tests/pw-lease-client.c` runs picowl on a vkms card and checks the rejection, a normal lease cycle, `kill -9` of the lessee and the close-fd-only case, and the contents of the lease. Build picowl and wlroots with ASan to also check the grant fix of patch 0004.

Hardware checklist (h2210, h5550, hx4700, h3870, h3970), with `-d 3`:

- [ ] `lease: offering <output>` at start and after every re-create.
- [ ] `mediaplayer --vo drm:lease` gets the fd; `drmModeGetLease` lists the expected objects (on the hx4700 the overlay too).
- [ ] MediaQ C8 and doubling, and the hx4700 overlay, play; drops match bare `--vo drm`.
- [ ] No dimming or blanking during a lease longer than `blank_after_s`; normal timeouts resume afterwards.
- [ ] A tap reaches the player in native pixels on a rotated board; a hold gives `BTN_RIGHT`.
- [ ] Power key: the lease ends and the screen blanks. App-cycle key: the lease ends and the next view is shown.
- [ ] Player exit and `kill -9`: picowl repaints with hardware rotation, copy type, touch matrix and panel back. Measure the restore time.
- [ ] `chvt` away while leased: the player gets `finished`; `chvt` back: picowl's output returns (parking path).
- [ ] udev delivers LEASE uevents on the rootfs (`udevadm monitor -p`, close-fd-only case).
- [ ] RSS of picowl before, during and after a lease.
