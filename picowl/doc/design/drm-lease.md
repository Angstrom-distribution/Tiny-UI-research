# DRM lease support

**Status:** implemented.

The code is in `src/lease.c` and `src/leasepolicy.c` with hooks in `src/output.c`, `src/input.c`, `src/view.c`, `src/power.c` and `src/config.c`, plus wlroots patch 0004; the user documentation is `doc/lease.md`. Deviations from this plan are listed under [Implementation notes](#implementation-notes).

picowl offers its DRM output through `wp_drm_lease_device_v1` (wlroots `wlr_drm_lease_v1`). The media player (`--vo drm:lease`) can then drive KMS directly, without a VT switch, and picowl takes the output back when the lease ends. This is work item 9 of `doc/mediaplayer-integration.md` §4, and Path B step 2 in §3.2 of that document.

How sources are cited:
- `wlroots …` is the 0.19.0 tree used for this design. It already has picowl patches 0001 to 0003 applied (`backend/drm/drm.c:1443` is `wlr_drm_connector_set_copy_type`).
- `linux …` is the kernel git tree (`git show HEAD:<path>`).
- `wp …` is wayland-protocols `staging/drm-lease/drm-lease-v1.xml`.
- Other paths are relative to the picowl repository.

## 1. Problem

Path A (`--vo wayland`) cannot reach these hardware paths:
- the hx4700 w100 YUV overlay plane;
- MediaQ C8 and pixel doubling;
- GC0C tear-free flips.

Only a KMS client can drive them (`doc/mediaplayer-integration.md` §1, §3). Today the only way to give the player KMS is a VT switch (Path B step 1). That costs:
- **No input in picowl:** the libinput backend suspends while the session is inactive (`wlroots backend/libinput/backend.c:172-174`). The player must open evdev itself and handle the power key.
- **Dimming with nobody watching:** picowl's dim timer keeps running. It can dim the backlight (sysfs) under the player, because no input reaches picowl.
- **No recovery from a player crash:** the device stays on the player's VT. An iPAQ has no keyboard to switch back with.
- **Slow handoff:** two session switches, with seatd and VT ioctls on the way.

A lease gives the player the same KMS objects:
- picowl stays in its session and keeps master;
- picowl keeps input and power policy;
- picowl gets the output back as soon as the player's Wayland connection or lease object goes away.

### Facts that shape the design

| Question | Answer (source) |
|---|---|
| Does wlroots only lease non-desktop connectors? | No. `wlr_drm_lease_v1_manager_offer_output` checks only that the output is a DRM output of a backend with a lease device (`wlroots types/wlr_drm_lease_v1.c:525-577`). Restricting is compositor policy: cage offers only `non_desktop` outputs (`cage/output.c:238-246`), and labwc offers every DRM output (`labwc/src/output.c:663-676`). |
| Can picowl lease its only connector? | Yes. When the lease is granted, wlroots destroys the `wlr_output` (`wlroots backend/drm/drm.c:2321-2327` → `disconnect_drm_connector` :2219-2229). It skips the connector on rescans while the lease lives (:1929-1931) and re-creates it through `new_output` when the lease ends (:2344-2366). picowl runs with zero outputs in between. |
| Can only a plane (e.g. the overlay) be leased? | No. The kernel requires at least one CRTC and one connector (`linux drivers/gpu/drm/drm_lease.c:346-375`), and the wlroots API leases whole outputs. The player always takes the whole output. |
| What objects does wlroots lease? | The connector, its CRTC, the CRTC's primary plane and the cursor plane if one exists (`wlroots backend/drm/drm.c:2270-2298`). **Overlay planes are not leased.** `struct wlr_drm_crtc` knows only `primary` and `cursor` (`wlroots include/backend/drm/drm.h:84-85`; assignment at `drm.c:250-266`). With universal planes on, which wlroots sets at `drm.c:73`, the kernel adds no plane implicitly (`linux drm_lease.c:439-455`). An unleased object is invisible to the lessee. So `mediaplayer-integration.md` §3.2 is wrong: the "w100 overlay" is **not** in a stock 0.19 lease. |
| Kernel driver support needed? | None beyond `DRIVER_MODESET`. Leasing lives in the DRM core: it is built unconditionally (`linux drivers/gpu/drm/Makefile:57`) and gated only on `DRIVER_MODESET` (`drm_lease.c:491-492`). The four iPAQ drivers are out of tree, so their object sets (one CRTC? overlay `possible_crtcs`?) are **unverified**. |
| What does the lessee get? | A new `drm_file` cloned from picowl's (`linux drm_lease.c:551-561`, `is_master = 1`). Client caps are per file, so the player must set `UNIVERSAL_PLANES` and `ATOMIC` again. |
| Who may still touch leased objects? | The top-level lessor holds every object (`linux drm_lease.c:88-94`). wlroots uses this right after creating the lease: destroying the output runs `dealloc_crtc`, which commits the CRTC off (`wlroots drm.c:1271`, `:1493-1510`). The player always starts from a disabled CRTC. |
| Does a lease survive a VT switch? | Yes. `DROPMASTER` does not revoke leases (`linux drivers/gpu/drm/drm_auth.c:280-312`); only closing the lessor's file does (`drm_auth.c:337-363`). But the lessee counts as current master only while its owner is `dev->master` (`drm_auth.c:69`), so its commits fail while picowl's session is inactive. The protocol asks the compositor to revoke when it loses master (`wp drm-lease-v1.xml:290-303`), and wlroots does not (`wlroots backend/drm/backend.c:107-127`). |
| How does the player get the fd? | `wp_drm_lease_v1.lease_fd` after `submit` and grant (`wlroots wlr_drm_lease_v1.c:154-209`). |

## 2. Design

### 2.1 Interfaces

- **Protocol:** `wp_drm_lease_device_v1` version 1 from wayland-protocols staging, implemented inside libwlroots. It is compiled whenever the `drm-backend` feature is on (`wlroots types/meson.build:106-110`), which picowl's OE build enables (`-Dbackends=drm,libinput`, `oe/recipes-graphics/wlroots/wlroots_0.19.0.bb:53`). There is no new XML and no protocol code in picowl.
- **wlroots API used** (`wlroots include/wlr/types/wlr_drm_lease_v1.h:114-155`; the header needs `WLR_USE_UNSTABLE`, :5-7, already set in `meson.build:10`):
  - `wlr_drm_lease_v1_manager_create`
  - `wlr_drm_lease_v1_manager_offer_output`
  - `wlr_drm_lease_request_v1_grant` and `wlr_drm_lease_request_v1_reject`
  - `wlr_drm_lease_v1_revoke`. Its doc comment (:151-154) is a copy of reject's, but the implementation terminates the lease (`.c:221-225`).
  - `manager->events.request`, and `wlr_drm_lease_v1.drm_lease->events.destroy`, from the public `struct wlr_drm_lease` (`wlroots include/wlr/backend/drm.h:20-29`).
  - `wlr_cursor_map_input_to_region` (`wlroots include/wlr/types/wlr_cursor.h:215-216`).
  - `wlr_session.events.active` (`wlroots include/wlr/backend/session.h:59-63`).

### 2.2 Configuration

New section, parsed like `[zerocopy]` (`src/config.c:438-446`, `parse_bool` at :115):

```ini
[lease]
# create the wp_drm_lease_device_v1 global
enable = true
# comma-separated app_ids allowed to lease; * = any
allow = mediaplayer
```

| Key | Default | Meaning |
|---|---|---|
| `enable` | `true` | `false` skips `wlr_drm_lease_v1_manager_create`, so no global is advertised and no client gets a DRM fd |
| `allow` | `mediaplayer` | The requester must own the focused, mapped toplevel, and that toplevel's `app_id` must be in this list. `*` accepts any focused client. An empty value rejects all requests |

### 2.3 Grant policy

`events.request` must be granted or rejected in the handler; otherwise wlroots auto-rejects (`wlr_drm_lease_v1.c:356-359`). picowl grants only when all of these hold:

1. `[lease] enable` is set and no lease is active (one output, one lease).
2. The session is active (`server->session->active`). The kernel lease ioctls need `DRM_MASTER` (`linux drivers/gpu/drm/drm_ioctl.c:744-747`).
3. No layer surface has exclusive keyboard focus (`pw_layer_has_exclusive_focus`, `src/layer.c:276-287`). A lease must not bypass a lock surface.
4. `wl_resource_get_client(request->resource)` (header :75-88) is the client of `server->focused_view` (`src/picowl.h:276`), and that view's `xdg_toplevel->app_id` matches `allow`.

The decision is a pure function in `src/leasepolicy.c`, unit-tested like `dim.c` and `touchhold.c`.

### 2.4 State

```c
/* src/lease.c, owned through pw_server.lease (void *, like pw_server.power) */
struct pw_lease {
	struct pw_server *server;
	struct wlr_drm_lease_v1_manager *mgr;   /* NULL: leasing unavailable */
	struct wlr_drm_lease_v1 *active;        /* NULL when not leased */
	struct wl_client *client;               /* lessee */
	struct pw_view *view;                   /* lessee's focused view at grant */
	struct wlr_box touch_box;               /* {0,0,native_w,native_h} at grant */
	struct wl_listener request;
	struct wl_listener drm_lease_destroy;   /* active->drm_lease->events.destroy */
};
```

`pw_output` (`src/picowl.h:164-186`) gets no lease field: wlroots tracks the offer and withdraws it on output destroy (`wlr_drm_lease_v1.c:518-523`).

### 2.5 State machine

```
            offer in output_new              policy ok + grant ok
  NONE ──► OFFERED ─────────────────────────────────► LEASED
 (no mgr)    ▲                                          │
             │ new_output → output_new → offer          │ drm_lease destroy:
             │                                          │  lessee destroyed object or
             └──────────── RETURNING ◄──────────────────┘  disconnected, LEASE uevent,
                (rescan inside drm_lease_destroy,          picowl revoke, backend destroy
                 wlroots drm.c:2365; if the session is
                 inactive: PARKED until resume)
```

**On grant** (in the request handler, before `grant`):
1. Capture `native_w`/`native_h` of the output (`src/picowl.h:177`); it is destroyed during the grant.
2. Call `grant`. wlroots disables the CRTC and destroys the output. picowl's `output_destroy` (`src/output.c:298-328`) runs inside the call: it closes the layer surfaces, drops the copy swapchain and zero-copy state, and the output leaves the layout.
3. Call `pw_power_hold(server, PW_HOLD_LEASE, true)`; see "Power" below.
4. Call `pw_input_lease_touch(server, &touch_box)`.
5. Add the `drm_lease_destroy` listener.

**On lease end** (the `drm_lease_destroy` listener):
1. Remove the listener first: `drm_lease_destroy` asserts the list is empty (`wlroots drm.c:2347-2349`). wlroots' own `lease_handle_destroy` ran before it and freed the `wlr_drm_lease_v1` (`wlr_drm_lease_v1.c:134-152`, registered at :198), so picowl must not touch `active` beyond setting it to NULL.
2. Call `pw_input_lease_touch(server, NULL)`.
3. Call `pw_power_hold(server, PW_HOLD_LEASE, false)`.
4. Return. wlroots then rescans and emits `new_output`. `output_new` re-applies hardware rotation, copy type, the touch matrix and the view layout, and offers the output again.

**Interaction with existing state:**

| picowl state | While LEASED |
|---|---|
| **Power** (`src/power.c`, `src/dim.c`) | New `pw_power_hold(server, src, on)` with a bitmask of sources (`PW_HOLD_LEASE`, `PW_HOLD_SESSION`; the idle-inhibit plan adds its own bit). While any bit is set, `dim_ms_for`/`blank_ms_for` return 0, so `profile_changed` (`power.c:127-138`) cannot re-arm. Setting the first bit runs `pw_dim_activity`, so DIMMED becomes ACTIVE (backlight restored) and BLANKED becomes ACTIVE (`run_actions` → `pw_output_blank(false)` on an empty output list is a no-op that clears `server->blanked`, `output.c:578-585`). Clearing the last bit restores the profile timeouts and restarts them from now. The lessee is not a visible surface, so an idle inhibitor on its surface would not count; the lease itself holds. |
| **Keyboard** | Unchanged. The lessee's view keeps keyboard focus, and keys go out through `wlr_seat_keyboard_notify_key` (`src/input.c:255-284`). |
| **Bindings** (`pw_input_run_action`, `input.c:179-205`) | `TOGGLE_BLANK` (power key): revoke, then blank. `CYCLE_VIEWS`, `SPAWN`, `TOGGLE_PANEL`: revoke, then run. `CLOSE_VIEW`, `QUIT`: run (the lease ends with the client or the backend). `ROTATE`: dropped with a log line (the player owns scanout). Implemented as `pw_lease_key_policy(action)` in `leasepolicy.c`. |
| **Touch** | With zero outputs, absolute events map to a 0x0 box (`wlroots types/wlr_cursor.c:418-431`) and `warp_closest` leaves the cursor where it was (:394-416). So touch would be dead. `pw_input_lease_touch` maps each touch device to the region `{0,0,native_w,native_h}`; a device's `mapped_box` takes precedence over `mapped_output` (`wlr_cursor.c:343-360`). It also restores the device's default calibration matrix, as the software-rotation branch of `apply_touch_matrix` does (`input.c:429-447`). Coordinates then arrive in panel-native mode pixels, the frame the player's KMS code renders in. `touch_handle_down` (`input.c:545-578`) uses the lessee's view surface as the target, with `sx, sy` set to the cursor coordinates (grab offset 0), instead of `surface_at`, because the scene no longer matches the screen. Tap-and-hold stays active (`pw_touchhold_*`); the hold animation draws nothing because there are no outputs. Passing NULL clears the region, so `pw_input_apply_rotation` (`input.c:450-458`) can re-map to the new output. |
| **Focus** | While leased, `view_map` (`src/view.c:233-242`), `handle_request_activate` (`src/server.c:106-122`) and the foreign-toplevel activate request do not move focus. New views stack behind the lessee, so a pop-up does not end playback. If the lessee's view unmaps or is destroyed, picowl revokes. |
| **Views** | `view_arrange` returns early with no output (`view.c:55-56`). No configures go out while leased, and `pw_view_arrange_all` re-arranges on return (`output.c:448`). |
| **Layer shell** | The panel's layer surface is closed when the output is destroyed (`output.c:305-315`). A new layer surface with no output is closed (`layer.c:294-304`). Same as after a VT switch today; the panel must re-create its surface on the next `wl_output`. |
| **Session** (`server->session`, only stored today: `server.c:197`) | New listener on `session->events.active`. **Pause:** wlroots emits the signal before `libseat_disable_seat` (`wlroots backend/session/session.c:30-35`), so picowl is still master. Revoke any lease (the protocol requires it; the player gets `finished`) and set `PW_HOLD_SESSION`. **Resume:** unpark outputs (§3, `output.c`) and clear `PW_HOLD_SESSION`. The VT-switch path benefits too: no backlight dimming while picowl is not visible. |

## 3. Code changes

| File | Change |
|---|---|
| `src/lease.c` (new, ~220 lines [est]) | `pw_lease_init`: if `[lease] enable`, call `wlr_drm_lease_v1_manager_create(server->display, server->backend)`. It returns NULL on headless or nested backends, or when card0 cannot be opened as non-master (`wlr_drm_lease_v1.c:622-633`, `:697-720`); log once at INFO level. Also: the request handler (policy, grant, state); `pw_lease_offer(output)`, called from `output_new` for `wlr_output_is_drm` outputs; `pw_lease_revoke(server, reason)`; `pw_lease_active(server)`; `pw_lease_touch_target(server)`; `pw_lease_finish`, which revokes and removes the `request` listener. The manager's display-destroy handler asserts that no request listener is left (`wlr_drm_lease_v1.c:686-687`). |
| `src/leasepolicy.c`, `.h` (new, ~70 lines [est]) | Pure functions: `pw_lease_decide(facts, allow)` → GRANT or REJECT_{DISABLED, BUSY, SESSION, LOCKED, NOT_FOCUSED, APP_ID}; `pw_lease_app_allowed(list, app_id)`; `pw_lease_key_policy(action)` → PASS, REVOKE_FIRST or DROP. No wlroots dependency. |
| `src/server.c` | `pw_server_init` (:177-279): call `pw_lease_init` after `pw_output_init` (:267). It needs the backend, from :197, and must run before `wlr_backend_start` (:289) so the first `new_output` can offer. `pw_server_finish` (:325-382): call `pw_lease_finish` before `wlr_backend_destroy` (:362). `handle_request_activate` (:106-122): skip focusing while leased. |
| `src/output.c` | `output_new` (:344-449): split into the listener and `pw_output_adopt(server, wlr_output)`. If `server->session && !server->session->active`, the commit would fail (`wlroots drm.c:929-931`). Today `output_new` then frees the `pw_output` and leaves the `wlr_output` orphaned (`output.c:402-407`), and wlroots does not emit it again on resume (`drm.c:1977-1984`). So such outputs are **parked** on a small list, each with a destroy listener, and adopted on session resume. This happens when a lease ends while picowl is switched away. After a successful adopt, call `pw_lease_offer`. `pw_output_init` (:563-576): add the session `active` listener when `server->session` is set. |
| `src/input.c` | `pw_input_lease_touch(server, box)`; a lease branch in `touch_handle_down` and `touch_handle_motion` (:545-594); in `pw_input_run_action` (:179-205), consult `pw_lease_key_policy` first. |
| `src/power.c`, `power.h` | `pw_power_hold`; `dim_ms_for`/`blank_ms_for` return 0 while a hold bit is set. |
| `src/view.c` | `view_map` (:233-242): do not focus while leased. `view_unmap` and destroy: revoke if `view == lease->view`. |
| `src/config.c`, `src/picowl.h` | Parse `[lease]` (`lease_enable`, `lease_allow`); add `void *lease` to `struct pw_server` (:236-287). |
| `meson.build` | Add `src/lease.c` and `src/leasepolicy.c`. No new dependencies: the protocol header is generated inside wlroots. |
| `data/picowl.ini.example` | Add the `[lease]` section. |
| `subprojects/packagefiles/wlroots/0004-drm-lease-overlay-planes.patch` | Copied byte-identical to `oe/recipes-graphics/wlroots/files/`, and added to `diff_files` in `subprojects/wlroots.wrap` and to `SRC_URI` (`wlroots_0.19.0.bb:17-20`). Changes: (a) store `drm_plane->possible_crtcs` in `struct wlr_drm_plane` in `init_plane` (`drm.c:250-266`; the field does not exist, `include/backend/drm/drm.h:23-48`); (b) in `wlr_drm_create_lease`, size `objects[]` as `3 * n_outputs + drm->num_planes + 1` and add every `DRM_PLANE_TYPE_OVERLAY` plane of `drm->planes` (`drm.c:274-312`) whose `possible_crtcs` has the CRTC's index bit; (c) the grant fix below. About 40 lines [est]. Without libliftoff, wlroots never uses overlay planes, so leasing them takes nothing from picowl. |

**Probable use-after-free in wlroots 0.19.0, part (c) of patch 0004.** The sequence:
1. `wlr_drm_lease_request_v1_grant` calls `wlr_drm_create_lease` (`.c:175`).
2. That destroys the output, and the connector's destroy listener (`.c:518-523`) frees the `wlr_drm_lease_connector_v1` (`.c:63-92`).
3. `grant` then writes `request->connectors[i]->active_lease` (`.c:192-195`), and `lease_handle_destroy` writes it again at lease end (`.c:143-145`). Both writes go to freed memory.

The fix is to keep no connector pointers in the lease, since those connectors are always withdrawn by the grant. Before writing the patch, check whether upstream 0.19.x already has a fix. Confirm the bug under ASan with the vkms test (§6).

## 4. Fallbacks and failure modes

| Condition | Behaviour |
|---|---|
| Headless, nested, or no DRM backend | Manager is NULL; one INFO log line; no global. The player's `--vo auto` falls back to Path B step 1 or Path A. |
| card0 not openable by the picowl user | wlroots skips the device: `open()` at `drm.c:2242`, "Skipping" at `wlr_drm_lease_v1.c:630`. The fix is the `video` group (`data/picowl.service`, `doc/power.md`). |
| Request fails policy | `reject` sends `finished`, and the output stays with picowl. |
| `drmModeCreateLease` fails | wlroots fails before disconnecting (`drm.c:2312-2317`), so picowl keeps the output. `grant` sends `finished` (`.c:178`), and submit's auto-reject sends a second one (`.c:356-359`): picowl must not also call `reject`. The player must tolerate two `finished` events. |
| Player crashes or disconnects | Destroying the `wp_drm_lease_v1` resource terminates the lease (`.c:227-232`), and picowl repaints at once. No udev needed. |
| Player closes the fd but keeps the object | The kernel destroys the lessee and sends a LEASE uevent (`linux drm_lease.c:290-294`). wlroots then runs `scan_drm_leases` (`backend.c:142-145`, `drm.c:2019-2046`), which needs a working udev monitor. On a rootfs without one, the screen stays dark until the object is destroyed. The player contract (§7) avoids this case. |
| VT switch while leased | picowl revokes in the pause handler, while it is still master. The rescan's `new_output` is parked and adopted on resume. |
| Lease ends while switched away | `drmModeRevokeLease` fails with EACCES (logged by wlroots, `drm.c:2336-2339`). The kernel lessee goes away anyway when the fd closes. The output is parked, then adopted on resume. |
| picowl exits or crashes while leased | `pw_lease_finish` revokes. If picowl crashes, the kernel revokes all leases when picowl's master file closes (`linux drm_auth.c:353-358`). The player sees EACCES or ENOENT and a Wayland disconnect. |
| Overlay plane not leased (no patch 0004, or the driver's `possible_crtcs` does not match) | The player's `set_overlay` commit fails and it falls back to the software path for the run (`media-player.md` §5.6). C8, doubling and flips are on the primary plane and still work. |
| Lease while blanked | Granted. The hold sets ACTIVE, and the player modesets. |
| Existing blank mismatch across a VT switch | Today `output_new` enables a re-created output while `server->blanked` stays true (`output.c:402`, `:578-585`). `PW_HOLD_SESSION` sets ACTIVE on pause, which removes the mismatch. |

### Compared with the VT-switch fallback (Path B step 1)

| | VT switch | Lease |
|---|---|---|
| picowl code | none (plus the hold fix above) | ~450 lines + patch 0004 [est] |
| Input | the player reads evdev; picowl is suspended | picowl routes it: keys as today, touch remapped to native pixels |
| Power key, dimming | player's job; picowl's dim timer can fire meanwhile | picowl policy: revoke and blank; dimming held |
| Player crash | stuck on the player's VT | picowl restores at once (Wayland disconnect) |
| Overlay plane | visible (full master) | needs patch 0004 |
| Player privileges | VT ioctls, `KD_GRAPHICS`, DRM master | none; the fd is pre-authorised |
| Handoff | two session switches | one round trip + one ioctl + one CRTC-off commit |

The VT switch stays as the fallback when no lease global exists, or when a grant fails.

## 5. Memory and CPU cost on 64 MiB boards

- **picowl:**
  - code: ~6 KB of text [est];
  - `struct pw_lease` plus parked-list nodes: under 128 bytes [est];
  - nothing new in libwlroots (the lease code is already built with the DRM backend).
- **Kernel:**
  - per client that binds the global: one `drm_file` for the non-master fd (`wlr_drm_lease_v1.c:498`), about 1 KB [est]. GTK2 clients do not bind it.
  - per lease: a `drm_master`, its IDRs and a cloned file, about 1 KB [est].
- **Freed while leased:**
  - picowl's copy swapchain: one full-screen RGB565 buffer, 150 KiB at 320x240 and 600 KiB at 480x640;
  - the scene output;
  - the panel's buffers.
- **CPU:**
  - zero while leased: no outputs, so no frames;
  - grant: one `drmModeCreateLease` and one atomic disable commit;
  - return: one modeset plus a full-damage first frame. On copy-type drivers that is one full-screen upload, about 19 ms at QVGA and about 75 ms at VGA over the ~8 MB/s MediaQ/w100 bus [est].

No new memcpy, no floating point beyond the double math wlroots and libinput already do, and no per-frame allocation.

## 6. Tests

**CI (no `/dev/dri`):**
1. **`test-leasepolicy`:**
   - the grant/reject matrix (each REJECT reason, `*`, empty and comma lists, NULL `app_id`);
   - `pw_lease_key_policy` for every `PW_ACTION_*`.
2. **`test-config`:** `[lease]` defaults, `enable = false`, `allow` lists, and an unknown key (`tests/test-config.ini`).
3. **`test-dim` / power:** `pw_power_hold` with an injected clock:
   - holding from DIMMED undims;
   - holding from BLANKED unblanks;
   - a profile switch while held does not re-arm;
   - releasing restarts `dim_after_s` from the release time;
   - the bitmask: releasing one source keeps the other.
4. **`smoke.sh`:** headless picowl logs `lease: no DRM backend, disabled`, and `pw-test-client` (new `--expect-no-global wp_drm_lease_device_v1`) does not see the global. This guards the NULL-manager path and teardown (the `events.request` assert).

**Optional, root plus vkms (`tests/lease-vkms.sh`, not in default CI):**
- **Setup:** `modprobe vkms enable_overlay=1`; seatd builtin; picowl with `WLR_DRM_DEVICE` pointing at the vkms card; an ASan build of picowl and wlroots.
- **Client:** a small `tests/pw-lease-client.c` requests the lease, checks `drmModeGetLease` (connector, CRTC and primary plane, plus the overlay with patch 0004), sets the caps, commits a dumb buffer, then destroys the lease.
- **Checks:**
  - picowl logs the output re-created and re-offered;
  - the client sees a new `connector` event;
  - ASan is clean, which proves the grant fix;
  - repeat with `kill -9` of the client, and with close-fd-only (the udev path).

**Hardware checklist** (h2210, h5550, hx4700, h3870, h3970):
- [ ] `-d 3` log: `lease: offering <output>` at start and after every re-create.
- [ ] `mediaplayer --vo drm:lease` gets the fd. `drmModeGetLease` lists the expected objects; on the hx4700 that includes the overlay (patch 0004).
- [ ] MediaQ C8 and doubling, and the hx4700 overlay, play. Drops match bare `--vo drm`.
- [ ] No dimming or blanking during a lease longer than `blank_after_s`. Normal timeouts resume afterwards.
- [ ] A tap reaches the player in native pixels on a rotated board. Hold gives `BTN_RIGHT` (or the per-app setting).
- [ ] Power key: the lease ends and the screen blanks. App-cycle key: the lease ends and the next view is shown.
- [ ] Player exit and `kill -9`: picowl repaints, with hardware rotation, copy type, touch matrix and panel back. Measure the restore time.
- [ ] `chvt` away while leased: the player gets `finished`. `chvt` back: picowl's output returns (parking path).
- [ ] udev delivers LEASE uevents on the rootfs (`udevadm monitor -p`, close-fd-only case).
- [ ] RSS of picowl before, during and after a lease.

## 7. Mediaplayer side

`--vo drm:lease` init:
1. Connect, then create an `xdg_toplevel` with `app_id = "mediaplayer"`, fullscreen. Map it with one buffer, for example a 1x1 `wp_single_pixel_buffer` scaled with a viewport; picowl creates both globals at `server.c:248-249`. Wait for the `activated` configure state.
2. Bind `wp_drm_lease_device_v1` v1 and close the `drm_fd` it sends. Collect `connector` events until `done`; there is one connector.
3. Send `create_lease_request`, `request_connector`, `submit`. Then wait for `lease_fd`, or `finished` (possibly twice) on failure: fall back to the VT switch or Path A, with a log line.
4. On the fd:
   - set `DRM_CLIENT_CAP_UNIVERSAL_PLANES` and `DRM_CLIENT_CAP_ATOMIC` (a new `drm_file`);
   - enumerate objects with `drmModeGetLease` and the filtered resource lists.
   - The first commit is a full `ALLOW_MODESET` that sets every property the player relies on: plane `rotation` (picowl may have left MediaQ hardware rotation set), `COLOR_ENCODING`, `COLOR_RANGE`. The CRTC arrives disabled.
   - From there the existing `drm/` code runs: `kms_state`, the overlay planner, C8.
   - No VT ioctls and no `drmSetMaster`.
5. Expect `wp_drm_lease_connector_v1.withdrawn` *before* `lease_fd` arrives: wlroots withdraws the leased connectors while granting (`wlr_drm_lease_v1.c:76-77`). Handle it while waiting for `lease_fd`, don't assume it follows; destroy that connector object.

Runtime:
- **Input from Wayland only.** Keys arrive as `wl_keyboard`. Pointer coordinates are surface-local and equal to native mode pixels. `BTN_RIGHT` comes from a hold. Do not open evdev: picowl owns it.
- **`finished`, or EACCES/ENOENT from a commit:** stop committing, close the fd and destroy the lease object. Then either pause and switch to Path A, or re-request when a new `connector` event arrives.
- **Exit or leaving fullscreen:** close the fd, then `wp_drm_lease_v1.destroy`, then flush. Destroying the object restores picowl without depending on udev.

`--vo auto`: pick the lease when the global exists and either the planner accepts the stream (hx4700, overlay leased) or C8/doubling is requested.

## 8. Open questions

1. Is the grant use-after-free fixed upstream after 0.19.0? Backport the upstream fix, or carry part (c) of patch 0004.
2. What objects do the out-of-tree drivers expose? One CRTC each? The w100 overlay's `possible_crtcs`? Do mq11xx and w100 need the primary plane enabled in the same commit as the overlay? Check with `modetest -p` on each board.
3. Power key while leased: revoke and blank (proposed), or turn off only the backlight and let audio-only playback continue?
4. Touch frame: native pixels (proposed), or picowl's last logical frame? Native matches the player's KMS code on both hardware- and software-rotation boards.
5. Does the OE rootfs run a udev daemon whose monitor wlroots receives? It matters only for the close-fd-only case.
6. Should layer surfaces (panel, OSK) survive output loss, instead of being closed (`output.c:305-315`)? This is shared with the VT-switch path; it is out of scope here.
7. Should a lock-screen layer surface created while leased revoke the lease? Today it is closed (`layer.c:300-304`), and the hold stops idle locking anyway.
8. Is a vkms CI runner with root available? Only that test catches wlroots-level regressions.

## 9. Effort estimate

| Item | Effort [est] |
|---|---|
| `lease.c`, `leasepolicy.c`, hooks in server/output/input/power/view/config | 2 days |
| Output parking on an inactive session | 0.5 day |
| Patch 0004 (overlay planes, grant fix), vkms test and test client | 1.5 days |
| CI tests (policy, config, power hold, smoke) | 0.5 day |
| Hardware bring-up on five boards with the player | 2 days |
| **Total** | **about 6.5 days** |

## Implementation notes

Deviations from the plan above. The line references in the plan are against the old commit; everything was re-located in the current code.

- **No `pw_power_hold`.** The idle-inhibit work had already added a source mask: `pw_power_inhibit(server, reason, on)` with `PW_INHIBIT_CLIENT` and `PW_INHIBIT_SESSION` (`src/power.h`), and `power.c` already listens to the session `active` signal. The lease is one more bit, `PW_INHIBIT_LEASE`, and the plan's `PW_HOLD_*` names do not exist. The plan's "holding from BLANKED unblanks" does not hold for that mask (an inhibitor never unblanks), so the grant calls `pw_power_set_blanked(false)` before it grants. `PW_HOLD_SESSION`'s other job, removing the blank mismatch across a VT switch, was not needed: `output_adopt` already disables a re-created output while `server->blanked` is set (the "Hotplugged while blanked" branch), which keeps the two consistent. The power tests are therefore lease sequences in `tests/test-dim.c` (unblank, hold, release; undim and a profile switch while held), not a new `pw_power_hold` test. The bit mask itself lives in `power.c`, which has no unit test; `power-e2e` covers it for the client reason.
- **`pw_lease_init` runs at the end of `pw_server_init`** (after `pw_power_init`), not after `pw_output_init`. It still runs before `wlr_backend_start`, so the first output is offered. The request handler needs the views, the layers and the power module, which exist by then. `pw_output_finish` is new: wlroots asserts that nothing listens to `session->events.active` when the session is destroyed, so `output.c` removes its listener like `power.c` does. `pw_lease_finish` runs before `wl_display_destroy_clients`, after `pw_zerocopy_finish`.
- **`struct pw_lease` is smaller than the plan's.** It has no `client` and `touch_box` (the box is captured in the handler and given to `pw_input_lease_touch`, which keeps it) and has the session listener (revoke on pause) and the lessee `view`.
- **Native size.** `pw_output` no longer has `native_w`/`native_h`. `lease.c` takes the preferred mode, which is the mode in use, and undoes wlroots' 90/270 swap when the output rotates in hardware (`pw_rot_logical_size`). It does not use `wlr_output->width` because wlroots clears it while a hardware-rotated output is disabled.
- **Requests for other than one connector are rejected** (log: "the request must name one active output"), the plan's "one output, one lease". This is not a verdict of `pw_lease_decide`.
- **`pw_lease_decide` takes a facts struct** (`struct pw_lease_facts`), and `leasepolicy.h` includes `picowl.h` for `enum pw_action`, so the unit test builds against the wlroots headers like `test-config` (there is no wlroots call in it). `REJECT_DISABLED` cannot happen at run time (a disabled lease creates no global), it is kept for the policy.
- **Focus guard in one place.** `pw_view_focus` returns early for any view but the lessee's (`pw_lease_blocks_focus`), which covers `view_map`, xdg-activation, the foreign-toplevel activate request and the click path, so `handle_request_activate` in `server.c` is unchanged. `view_map` also inserts a new view behind the lessee and lowers its scene node. `view_unmap` calls `pw_lease_view_gone` (the destroy path goes through unmap).
- **`touch_handle_motion` is unchanged.** It uses no surface lookup, only the grab offsets, which are 0 under a lease (`pw_input_lease_touch` also resets them). A touch device added during a lease is mapped too. Pointer devices (a mouse) are not remapped.
- **A feature macro for patch 0004.** The patch adds `#define WLR_DRM_LEASE_OVERLAY_PLANES 1` to `<wlr/backend/drm.h>`, and `meson.build` fails the configure without it. The grant fix is needed for leasing to be safe, and nothing else in the headers tells that the patch is applied.
- **Patch 0004 (c).** The lease keeps `connectors = NULL`, `n_connectors = 0`. Whether upstream fixed the use-after-free after 0.19.0 was not checked: the build container has no network access. `objects[]` has `3 * n_outputs + drm->num_planes + 1` entries as planned, and an overlay plane that is possible on several leased CRTCs is added once, because the kernel rejects a duplicate object.
- **Tests that could not run here.** There is no `/dev/dri`, no vkms and no kernel module support in the build container, so `tests/lease-vkms.sh` (with `tests/pw-lease-client.c`, suite `vkms`, exit 77 without root, `/dev/dri` and the module) was compiled but never run. The grant path, the parking path, the touch mapping and the key policy at run time were reviewed but not exercised; the ASan confirmation of the grant fix is open. The `meson test` run under `-Db_sanitize=address,undefined` passes (except `rss`, whose ceiling the sanitizer exceeds). The `lease-vkms` script treats a picowl that cannot start (no seat) as a skip, not a failure. Its close-fd-only case runs only when `/run/udev` exists.
- **Open questions** are answered as proposed: power key and cycle key revoke (question 3), touch in native pixels (4), nothing for udev (5), layer surfaces are closed with the output as in the VT-switch path (6, 7). Questions 1, 2 and 8 are open: they need upstream, the boards and a CI runner.
- **No inline comments in the `[lease]` snippet.** The parser (`src/config.c`) takes only whole-line `#`/`;` comments, so a `; ...` after a value would become part of it. §2.2 and `doc/lease.md` put the comments on their own lines.
