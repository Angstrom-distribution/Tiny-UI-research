# picowl-buffer-v1 v2: caching event

**Status:** implemented.

Implementation: `protocols/picowl-buffer-v1.xml` (version 2), `src/copytype.c/.h` (`pw_caching_*`), `src/zbproto.c/.h` (`pw_zbproto_send_bind`), `src/zerocopy.c` (`pw_zerocopy_init`, `mgr_bind`), `src/config.c` (`[zerocopy] caching`). Details and deviations are in the final section, "Implementation notes".

Work item 7 (second half) of `doc/mediaplayer-integration.md:133`. The per-`app_id` buffer budget (first half of item 7) needs no protocol change and has its own design.

Line numbers refer to picowl commit b9eac6d. libwayland references are to wayland 1.24 sources; the host used for checking had libwayland 1.22.0.

## 1. Problem

- **What the player needs to know.** Whether it may read back from a display buffer, or decode into one (`media-player.md:217-219`: "whether the buffers are write-combined or uncached (the core then never reads them back and never decodes into them)"). The bare-DRM front-end gets this from `drmGetVersion()` and a driver table (`media-player.md:246-249`). A Wayland client has no DRM fd (`mediaplayer-integration.md:60`).
- **Today's stand-in.** The interim rule is that `copy_type = 1` means cacheable and `copy_type = 0` means write-combined (`mediaplayer-integration.md:62`). `copy_type` describes the outputs, not the memory, so the rule fails in these cases:
  - **Old kernel.** On a kernel without `DRM_IOCTL_MODE_CLOSEFB`, mq11xx and w100 get `copy_type = 0` (`src/output.c:381-393`), so the cacheable shmem is reported as write-combined.
  - **Overrides.** `[copytype]` overrides (`src/config.c:425-437`) change `copy_type` without changing the memory.
  - **Runtime changes.** `copy_type` follows enabled outputs (`src/zerocopy.c:51-58`) and is re-sent when it changes (`src/zerocopy.c:94-104`, `501-518`), but the memory type of a buffer never changes.
  - **sa1100-lcdc.** It is copy-type (`src/copytype.c:48-51`) but CMA, and so write-combined, according to the player docs (`mediaplayer-integration.md:62`).
- **What `caching` is not.** It is not a hint that the compositor keeps reading the buffer: `retained` covers that (`protocols/picowl-buffer-v1.xml:115-129`). It is not a buffer-count hint either: buffer count and reuse stay driven by `copy_type`, `copied`, `retained` and `wl_buffer.release`. `caching` describes only the CPU mapping attributes of the dmabuf the client `mmap`s.

## 2. Design

### 2.1 Semantics

| Property | Rule |
|---|---|
| Object | `picowl_buffer_manager_v1`, since version 2 |
| Value | `caching(uint mode)`, enum `caching`: `write_combined = 0`, `cacheable = 1`. `write_combined` covers every non-cacheable mapping (write-combined or uncached). 0 is the safe value, as with `copy_type` |
| When | Exactly once, during bind, after the last `format` and after `copy_type`. Never sent again |
| Scope | Every buffer created through this manager object. It is constant for the object's lifetime |
| Ordering | It comes before any `picowl_buffer_v1` event of buffers created from this manager. That follows from bind order, since events on one client connection are delivered in the order they were posted. A client that does a `wl_display_roundtrip` after binding has it before the roundtrip returns |
| Relation to `copied`/`retained` | Independent. Neither event changes it, and it changes nothing in the commit/answer state machine (`src/copyrel.c:8-38`). On a `write_combined` buffer, `copied` still allows a redraw into the same buffer. It only forbids reading it back |
| Relation to `copy_type` | Independent. A later `copy_type` (`src/zerocopy.c:94-104`) is not followed by `caching` |
| Not covered | Buffers the client allocates itself and imports through `zwp_linux_dmabuf_v1`, and `wl_shm` buffers (always cacheable) |

The resolved values per driver are below. sa1100-lcdc is still open: see §9 Q1.

| Driver | `copy_type` (`src/copytype.c:44-52`) | `caching` | Source |
|---|---|---|---|
| mq11xx, mediaq | 1 | cacheable | shmem (`doc/zero-copy.md:12`, `media-player.md:247-248`) |
| w100, imageon | 1 | cacheable | shmem (`doc/zero-copy.md:13`) |
| sa1100-lcdc, sa1100_lcdc, sa11x0-lcdc, sa1100 | 1 | write_combined | CMA per `mediaplayer-integration.md:62`; conflicts with `doc/zero-copy.md:14` and the comment at `src/copytype.c:41-43` |
| pxa-lcdc | 0 | write_combined | CMA pool (`doc/zero-copy.md:15`) |
| anything else, or `drmGetVersion` failed | 0 | write_combined | safe default |

### 2.2 Why one manager event and not a per-buffer event

- **There is only one allocator.** picowl creates a single allocator (`src/server.c:210`, `wlr_allocator_autocreate`, `include/wlr/render/allocator.h:52`). With the DRM backend and the pixman renderer this is the dumb allocator on a reopened node of the backend's DRM fd (`render/allocator/allocator.c:139-147` in wlroots 0.19). So every buffer has the same driver, and that driver is the one behind `wlr_backend_get_drm_fd` (`include/wlr/backend.h:82`), which `pw_zerocopy_init` already calls (`src/zerocopy.c:403`).
- **The player needs the value before it allocates.** It fills in its caps in `init` (`media-player.md:207`), before any buffer exists.
- **A per-buffer event can come later.** If allocators ever differ (multi-GPU, udmabuf), `picowl_buffer_v1.caching` can be added at v3 (§9 Q4).

### 2.3 Version negotiation and compatibility

| Server global | Client binds | Result |
|---|---|---|
| v1 (old picowl) | 1 | As today; the client uses the interim heuristic |
| v1 | wants 2 | The client must bind `min(advertised, 2)` = 1. Binding 2 is a protocol error ("invalid version for global", `wayland-server.c:1071-1075`) |
| v2 | 1 (old binaries hard-code 1, e.g. `tests/pw-test-client.c:74`) | `caching` is never sent; behaviour is byte-identical to v1 |
| v2 | 2 | `format`, `copy_type`, `caching` |

- **The server has to gate the send itself.** libwayland does not check event versions on the server side: `handle_array` marshals without a version test (`wayland-server.c:214-244`). A v1 client would choke on the event:
  - a client built from the v1 XML rejects opcode 2 ("interface ... has no event", `wayland-client.c:1619-1622`), which kills the connection;
  - a client built from the v2 XML that bound 1, with no handler set, aborts (`connection.c:1237-1239`, NULL listener).

  So the send is gated with `wl_resource_get_version(r) >= PICOWL_BUFFER_MANAGER_V1_CACHING_SINCE_VERSION`. The scanner emits `_SINCE_VERSION` macros for every message; see the v1 ones in a generated `picowl-buffer-v1-protocol.h`, e.g. `PICOWL_BUFFER_MANAGER_V1_COPY_TYPE_SINCE_VERSION 1`.
- **Both interfaces go to version 2.** `picowl_buffer_v1` has no new messages in v2, but `mgr_create_buffer` creates it with the manager's version (`src/zerocopy.c:262-263`). `wl_resource_create` does not check the version (`wayland-server.c:2033-2061`), but a resource at version 2 of an interface declared as version 1 is incoherent. Bumping both is the convention.
- **Append only.**
  - Event opcodes are positional, so `caching` goes after `copy_type`.
  - The scanner warns when `since` decreases (`scanner.c:810-812`).
  - The `since` attribute on `<enum>` is in the DTD (`wayland.dtd:19`) but the scanner ignores it (`scanner.c:877-895`). It is documentation only, as in `xdg-shell.xml:1176`.
- **A version mismatch fails loudly.** `wl_global_create` returns NULL if the advertised version is above the XML's (`wayland-server.c:1371-1376`). picowl then logs "zero-copy disabled: global or timer creation failed" (`src/zerocopy.c:453-454`), so an XML/code mismatch shows at start-up, not as a client crash.

### 2.4 XML diff (`protocols/picowl-buffer-v1.xml`)

```diff
-  <interface name="picowl_buffer_manager_v1" version="1">
+  <interface name="picowl_buffer_manager_v1" version="2">
     <description summary="allocate compositor-owned RGB565 buffers">
@@
       again after a copied event; when a retained event answers a commit
       instead, they switch to a second buffer and use wl_buffer.release.
+      From version 2, the caching event tells whether the CPU mapping of
+      the buffers is cacheable.
     </description>
@@
     <event name="copy_type">
       ...
     </event>
+
+    <enum name="caching" since="2">
+      <entry name="write_combined" value="0"
+             summary="not cacheable: write sequentially, never read back"/>
+      <entry name="cacheable" value="1"
+             summary="ordinary cacheable memory: reading back is cheap"/>
+    </enum>
+
+    <event name="caching" since="2">
+      <description summary="CPU caching of the buffers' memory">
+        Sent once, when the manager is bound, after copy_type. Describes
+        how the CPU mapping of the dmabuf fd of every buffer created by
+        this manager behaves. write_combined also covers uncached
+        mappings: reads from them are very slow, so clients should only
+        write, sequentially, and never decode into or read back from the
+        buffer. The value never changes for the lifetime of the manager
+        object, and does not depend on copy_type, copied or retained.
+      </description>
+      <arg name="caching" type="uint" enum="caching"/>
+    </event>
   </interface>

-  <interface name="picowl_buffer_v1" version="1">
+  <interface name="picowl_buffer_v1" version="2">
     <description summary="a compositor-allocated buffer">
       Describes a dmabuf allocated by the compositor. Wrap the fd with
       zwp_linux_dmabuf_v1 (params.add + create_immed).
+      Version 2 has no changes; the version follows the manager's.
     </description>
```

### 2.5 Configuration

| Section | Key | Default | Values | Meaning |
|---|---|---|---|---|
| `[zerocopy]` | `caching` | `auto` | `auto`, `cacheable`, `write_combined` (case-insensitive) | `auto` uses the driver table in §2.1. An invalid value logs an error and keeps `auto` |

The key is global, not per output like `[copytype]`, because the allocator is global (§2.2).

### 2.6 Data structures

- `src/copytype.h`, next to `enum pw_copy_override` (`src/copytype.h:7`):
  ```c
  enum pw_caching { PW_CACHING_WRITE_COMBINED = 0, PW_CACHING_CACHEABLE = 1 }; /* wire values */
  enum pw_caching_override { PW_CACHING_OV_AUTO, PW_CACHING_OV_CACHEABLE, PW_CACHING_OV_WC };
  bool pw_caching_parse(const char *s, enum pw_caching_override *out);
  enum pw_caching pw_caching_driver(const char *drm_driver_name);
  enum pw_caching pw_caching_resolve(const char *driver, enum pw_caching_override ov);
  const char *pw_caching_name(enum pw_caching c);  /* "cacheable" | "write_combined" */
  ```
- `struct pw_config` (`src/picowl.h:121-125`): add `enum pw_caching_override caching_override;`, defaulting to AUTO next to `c->zerocopy = true` (`src/config.c:195-197`).
- `st` in `src/zerocopy.c:39-49`: add `uint32_t caching;`, the resolved wire value.

No new state machine. `struct pw_copyrel` (`src/copyrel.h:8-10`) and its transitions are unchanged.

## 3. Code changes

| File | Change |
|---|---|
| `protocols/picowl-buffer-v1.xml` | As §2.4. The generators in `meson.build:43`, `47-61` and `tests/meson.build:25-31` need no change |
| `src/copytype.c` | Add `pw_caching_driver`: the table from §2.1, reusing `strcaseeq` (`src/copytype.c:7-18`). Also add `pw_caching_parse`, `pw_caching_resolve` (the override wins, as in `pw_copytype_resolve`, `src/copytype.c:56-61`) and `pw_caching_name`. Fix the comment at `src/copytype.c:41-43`, which calls sa1100-lcdc a shmem driver. About 40 lines [est] |
| `src/copytype.h` | Declarations from §2.6 |
| `src/picowl.h` | One field in `struct pw_config` |
| `src/config.c` | Default (`src/config.c:195-197`). A `caching` key in the `[zerocopy]` branch (`src/config.c:438-446`), using `pw_caching_parse`, with an error log on bad values as the `[copytype]` branch does (`src/config.c:436`) |
| `src/zbproto.c` / `.h` (new) | `void pw_zbproto_send_bind(struct wl_resource *mgr, bool copy_type, uint32_t caching)` sends `format(RGB565)`, `copy_type`, then `caching` behind the version gate. It depends only on wayland-server, the generated header and `drm_fourcc.h`, so a CI test can link it without wlroots (§6). About 20 lines [est] |
| `src/zerocopy.c` | Detailed in the list below the table |
| `meson.build` | Add `src/zbproto.c` to `sources` (`meson.build:63-80`) |
| `data/picowl.ini.example:91-98`, `README.md:105-109` | Document `caching` |
| `doc/buffers.md:12-16` | Add flow step 1c, "`caching` (v2): bind `min(version, 2)`" |
| `doc/zero-copy.md:10-15`, `125-131` | Add a caching column; update the log line |
| `tests/pw-test-client.c` | Bind `ver < 2 ? ver : 2` (line 74). Add a third, positional listener member `pbm_caching` (`tests/pw-test-client.c:61-63`); it is mandatory once binding 2, or `connection.c:1237-1239` aborts. Print `caching=%d` in the summary line (`tests/pw-test-client.c:339`, `-1` when not received). Optional `--readback`: time one pass of 32-bit loads over buffer 0 with `clock_gettime(CLOCK_MONOTONIC)`, integer µs, no FPU |

Changes in `src/zerocopy.c`:
- **Detect the caching mode.** In `pw_zerocopy_init`, after `memset(&st, 0, …)` (`src/zerocopy.c:417`), call `drmGetVersion(drm_fd)` (the fd from line 403). Then `st.caching = pw_caching_resolve(name, s->config->caching_override)` and `drmFreeVersion`, as in `src/output.c:370-376`. Add `#include <xf86drm.h>`; libdrm is already a dependency (`meson.build:21`, `95`).
- **Log it.** Extend the start-up log line (`src/zerocopy.c:467`) to `zero-copy enabled (copy_type=%d caching=%s driver '%s')`.
- **Advertise version 2.** Change `wl_global_create(..., 1, ...)` (`src/zerocopy.c:450-451`) to `PW_ZB_MGR_VERSION`, with `#define PW_ZB_MGR_VERSION 2` next to the limits (`src/zerocopy.c:22-23`). The explicit constant stops a future XML bump from advertising a version picowl does not implement.
- **Send the bind events.** In `mgr_bind`, replace lines 348-349 (`send_format` and `send_copy_type`) with `pw_zbproto_send_bind(r, st.copy_type, st.caching)`.
- **Unchanged:** `send_copy_type` and the re-send loops (`src/zerocopy.c:89-104`, `512-517`), and `mgr_create_buffer`, which already propagates the version (`src/zerocopy.c:262-263`).

`src/copyrel.c` and `src/copyrel.h` need no change: `caching` is fixed at bind and never touches the commit serials.

## 4. Fallbacks and failure modes

| Case | Behaviour |
|---|---|
| zero-copy inactive (headless, `enable = false`, no dmabuf allocator: `src/zerocopy.c:401-409`) | No global, so the client uses `wl_shm`, which is cacheable |
| `drmGetVersion` returns NULL | The driver is treated as `""`, giving `write_combined` with the reason in the log |
| Unknown driver | `write_combined`. Wrong in the safe direction: the player takes one extra pass instead of slow uncached reads. Users can fix it with `[zerocopy] caching = cacheable` |
| Table wrong in the other direction (WC memory reported as cacheable) | Output stays correct, but every read-back runs at uncached speed. It shows up with `--readback` (§6.2) |
| Old picowl (v1) | The client sees version 1 in the registry and keeps the interim heuristic |
| Old client (v1) | Nothing is sent (gated) |
| XML and `PW_ZB_MGR_VERSION` disagree | `wl_global_create` fails and zero-copy is disabled with a log line (§2.3) |
| Cacheable memory and CPU cache coherency | Unchanged from today. The drm prime exporter has no `begin_cpu_access` (`drivers/gpu/drm/drm_prime.c:827-836`), so `DMA_BUF_IOCTL_SYNC` does not flush. Coherency of the driver's damage copy is the kernel driver's job, as for the KMS front-end, which already treats these buffers as cacheable (`media-player.md:246-248`) |

## 5. Memory and CPU cost

- **RAM:** 4 bytes in `st`, one enum in `pw_config`, and about 0.5 KiB of text [est] for the table, the parser and `zbproto.c`.
- **Start-up:** one `drmGetVersion` call, whose allocation libdrm frees immediately. `src/output.c:370-376` already does the same once per output.
- **Wire:** 12 bytes per v2 bind (8-byte header plus one uint).
- **Hot paths:** no per-frame, per-commit or per-buffer work. The commit, sample, present and idle paths (`src/zerocopy.c:160-203`, `378-393`, `529-541`) are untouched.
- **Constraints:** no memcpy and no FPU. The only new API is wayland-server's `wl_resource_get_version` and libdrm's `drmGetVersion`/`drmFreeVersion`, both already used in-tree. There is no new wlroots API.

## 6. Tests

### 6.1 Headless and unit tests (CI, no /dev/dri)

All are in the copyrel style (`tests/test-copyrel.c`): `assert` plus `printf("ok …")`.

1. **Driver table** (`tests/test-copytype.c`, extended; build entry `tests/meson.build:74-75` unchanged):
   - `pw_caching_driver` returns `cacheable` for every mq11xx and w100 alias, in any case;
   - it returns `write_combined` for the sa1100 aliases, `pxa-lcdc`, `""`, `NULL` and `"vc4"`;
   - `pw_caching_resolve` gives the override precedence over the table;
   - `pw_caching_parse` accepts `auto`, `cacheable` and `write_combined` in any case, and rejects `""` and `"wc"`;
   - `pw_caching_name` round-trips.

   One assert loop checks that every name `pw_copytype_driver` accepts gets an explicit table entry, so a new copy-type driver cannot silently default to WC. The loop uses a test-local list mirroring `src/copytype.c:44-51`.
2. **Config** (`tests/test-config.c`, next to `test_zerocopy_config` at line 249): the default is AUTO; `caching = cacheable` parses; an invalid value keeps AUTO.
3. **Protocol gating** (`tests/test-bufproto.c`, new). It runs in one process with no wlroots, over a `socketpair`:
   - a server-side `wl_display` gets `wl_client_create`, and the client side connects with `wl_display_connect_to_fd`. Including both headers in one translation unit is what libwayland's own `tests/display-test.c:46-47` does;
   - the server registers a global at version 2 whose bind calls `pw_zbproto_send_bind(r, true, 1)`;
   - a pump loop runs: client flush → `wl_event_loop_dispatch(loop, 0)` → `wl_display_flush_clients` → client `wl_display_dispatch_pending` after a read.

   Cases:
   - Bind 2: events arrive as `format, copy_type, caching(1)`, in that order, before a `wl_display.sync` issued right after the bind completes; `caching` arrives exactly once.
   - Bind 1, with a listener whose `caching` member is NULL: there is no abort, and only `format` and `copy_type` arrive.
   - A `create_buffer` child of a v2 manager has `wl_proxy_get_version` 2.

   Build: `executable('test-bufproto', 'test-bufproto.c', '../src/zbproto.c', protocol_headers, picowl_buffer_code, pb_client_h, dependencies: [wayland_server, wayland_client, libdrm])`. Link the private code once; it is the same for client and server.
4. **Smoke** (`tests/smoke.sh:32-35`): unchanged. Headless, zero-copy stays disabled, and the client must still print the `wl_shm` fallback. This proves the v2-capable client still degrades.

### 6.2 Hardware-only checklist

- [ ] **Log line on each board.** `zero-copy enabled (copy_type=… caching=… driver '…')` should show: h2210 and h5550 `1 cacheable`, hx4700 `1 cacheable`, h3870 `1 write_combined`, h3970 `0 write_combined`.
- [ ] **Test client.** `pw-test-client --zerocopy --readback` prints `caching=` matching the log.
- [ ] **Read-back check.** `readback_us` on `write_combined` boards should be several times that on `cacheable` boards at the same size [est]. If sa1100-lcdc or mq11xx read like the other class, the table is wrong (§9 Q1, Q2).
- [ ] **Old client on new picowl.** A test client built from the v1 XML runs 5 frames with no protocol error.
- [ ] **New client on old picowl.** A v2-capable client against a v1 picowl binds 1 and prints `caching=-1`.
- [ ] **Runtime state.** Blank, unblank and output hot-unplug re-send `copy_type` only (see `WAYLAND_DEBUG=1`).
- [ ] **CLOSEFB.** On a kernel without CLOSEFB, mq11xx shows `copy_type=0 caching=cacheable`. This is the case the heuristic gets wrong.

## 7. Mediaplayer side

- **Bind** `min(advertised, 2)`. Always install the `caching` handler; a NULL member aborts on a v2 bind (`connection.c:1237-1239`).
- **Read the mode after the init roundtrip:**
  - `caching` received: caps say write-combined iff the value is 0.
  - No `caching` (v1 picowl): keep the interim rule (`mediaplayer-integration.md:62`).
  - `wl_shm` fallback: cacheable.
- **Log** `vo wayland: caching=cacheable|write_combined (picowl v2|copy_type heuristic|wl_shm)`. This is the acceptance line for work item 7 (`mediaplayer-integration.md:133`).
- **Behaviour.** Only the read-back and decode-into rule depends on caching (`media-player.md:217-219`). Buffer count (`mp_core_want_bufs()`) and the FREE/HELD/PENDING/FRONT transitions (`mediaplayer-integration.md:32-46`) do not change.
- **Keep the tables aligned.** The KMS front-end's driver table and picowl's table should agree, sa1100-lcdc included.

## 8. Effort estimate

| Part | Estimate |
|---|---|
| XML, `zbproto.c`, `zerocopy.c`, `copytype.c`, config | 0.5 day [est] |
| Unit, config and gating tests; test-client changes | 0.5 day [est] |
| Docs (`buffers.md`, `zero-copy.md`, README, ini example) | 0.25 day [est] |
| Hardware checklist | about 30 min per board, 4 boards [est] |
| Player side | 0.25 day [est] |

## 9. Open questions

1. **sa1100-lcdc memory type.** `doc/zero-copy.md:14` ("persistent buffer, as above") and `src/copytype.c:41-43` imply a shmem shadow plane. `mediaplayer-integration.md:62` and `media-player.md:246-248` say CMA and write-combined. The driver is not in the reference kernel tree (`drivers/gpu/drm` there has only helpers and `tiny/`). The design defaults to `write_combined`, which is safe, and the `--readback` check settles it.
2. **Do mq11xx and w100 set `map_wc` on their shmem objects?** `drm_gem_shmem_mmap` maps write-combined only if `map_wc` is set (`drivers/gpu/drm/drm_gem_shmem_helper.c:786-787`). The drivers are out of tree, so this is unverified; `--readback` answers it.
3. **A separate `uncached` value.** Not needed by the player, whose rule is the same for both. It can be added later as an enum entry with `since="3"`.
4. **Per-buffer `caching`.** Needed only if a second allocator ever appears (multi-GPU, udmabuf): `picowl_buffer_v1.caching` at v3, sent before `done`.
5. **Ship v2 together with the per-app budget?** They are independent; the budget changes no wire format. Bundling them only saves one docs pass.

## Implementation notes

- **As planned.** The XML diff of §2.4, the `pw_caching_*` functions in `copytype.c/.h`, `pw_zbproto_send_bind` in `zbproto.c/.h` (the version gate uses `PICOWL_BUFFER_MANAGER_V1_CACHING_SINCE_VERSION`), `PW_ZB_MGR_VERSION 2` (defined in `zbproto.h` instead of `zerocopy.c`, so the test can see it), `[zerocopy] caching`, the start-up log line, and the driver table (sa1100-lcdc is `write_combined`; the §9 Q1 default stands). `mgr_create_buffer` is unchanged and already creates the buffer at the manager's version. `struct pw_copyrel` and the re-send loops are untouched.
- **Driver table.** `pw_caching_driver` lists only the cacheable names (mq11xx, mediaq, w100, imageon); everything else, sa1100 aliases included, falls to `write_combined`. The test keeps the list mirroring `pw_copytype_driver` with an expected value for each entry, so a new copy-type driver is a conscious choice. The comment in `pw_copytype_driver` that called sa1100-lcdc a shmem driver was fixed.
- **Driver name.** `pw_zerocopy_init` copies the `drmGetVersion` name into a local `char driver[32]` (also used for the log line) and logs at INFO when the call fails.
- **Test client.** `--bind-version N` and `--probe` were added beyond the plan, so the real client can be run at version 1 and 2 without a DRM device. `--probe` prints `probe bufmgr version=V format=F copy_type=T caching=C` (`caching=-1`: not received; `probe bufmgr none`: no global) and exits right after the registry roundtrips. `--readback` is as planned, with an extra summary line `readback_us=... bytes=... caching=...`.
- **`tests/test-bufproto.c`.** Takes the client binary as `argv[1]` (meson passes `pw_test_client`). Part one is the planned in-process test over a `socketpair`: bind 2, bind 1 with the v2 listener, bind 1 with a NULL `caching` member, and a version 1 global with a version 2 client; each checks the event order against a `wl_display.sync` queued right behind the bind, the proxy version of the manager and of a `create_buffer` child, and that `caching` is not sent again. Part two (not in the plan) serves a real socket from the same server code and runs `pw-test-client --probe` as a child: default against a v2 server, `--bind-version 1` against a v2 server, and default against a v1 server. This is the "v1 client keeps working" test with the real binary. The server side in the test mimics `mgr_bind`; the real `mgr_bind` needs a DRM backend.
- **Smoke.** `tests/smoke.sh` gained one `--probe` run, which must print `probe bufmgr none` headless; the existing shm-fallback checks are unchanged.
- **Docs.** `README.md`, `data/picowl.ini.example`, `doc/buffers.md` (steps 1b and 1c, the version negotiation), `doc/zero-copy.md` (caching column, "Caching event" section, hardware checklist) and `doc/mediaplayer-integration.md` (§2.4 marks the picowl half done). The player side (§7) is not part of this repository.
- **Not done.** The hardware checklist (§6.2) and the open questions of §9 are unchanged.
