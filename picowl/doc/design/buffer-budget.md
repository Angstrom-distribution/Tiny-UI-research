# Per-app buffer budget

**Status:** implemented.

Implementation: `src/zbquota.h/.c` (pure limits), `src/zerocopy.c` (`rule_for_client`, `mgr_create_buffer`), `src/config.c` and `src/picowl.h` (keys, `[app.*]` pool fields, `pw_config_app`). Details and deviations are in the final section, "Implementation notes".

Scope: make the `picowl-buffer-v1` allocation limits configurable, with a per-`app_id` override, so the media player can get up to 7 buffers (work item 7, `doc/mediaplayer-integration.md:133`). The `caching` event (the other half of item 7) is a separate plan. This plan only makes sure the two don't collide.

## 1. Problem

| Fact | Where |
|---|---|
| Limits are compile-time constants: `PW_ZB_MAX_PER_CLIENT 3`, `PW_ZB_BUDGET (2u*1024*1024)` | `src/zerocopy.c:22-23` |
| The per-client count is a loop over every `pw_zbuf` comparing `wl_resource_get_client()`. There is no per-client struct and no per-client byte total | `src/zerocopy.c:284-288`, struct `:25-37` |
| One global byte counter `st.total_bytes`, checked as `st.total_bytes + bytes > PW_ZB_BUDGET` | `src/zerocopy.c:46`, `:289` |
| Bytes are accounted as `w*h*2`, which ignores stride padding and page rounding | `src/zerocopy.c:283` |
| The player asks for 7 buffers (`--decode-ahead 5`): 1.05 MiB at QVGA, 4.1 MiB on the hx4700 (VGA) | `doc/mediaplayer-integration.md:50-55` |
| The requested keys are `[zerocopy] max_buffers_per_client`, `budget_kb`, plus an app_id override | `doc/mediaplayer-integration.md:55` |

So today the player gets 3 buffers on every board. On copy-type outputs that costs little (`copied` frees FRONT, `doc/mediaplayer-integration.md:56`). On the h3900 scanout path, and for decode-ahead in general, it caps queue depth.

The budget also matters because **on shmem drivers it is the only guard**:
- The allocator is wlroots' DRM dumb allocator. It is chosen because the pixman renderer has DATA_PTR caps and the DRM backend has DMABUF caps (`render/allocator/allocator.c:139-152`).
- `drm_gem_shmem_dumb_create` only creates the object (`linux/drivers/gpu/drm/drm_gem_shmem_helper.c:578-588`). Pages come on fault.
- wlroots then `mmap`s the buffer and `memset`s all of it (`render/allocator/drm_dumb.c:78`, `:85`). On mq11xx/w100 that memset is where the RAM is actually taken. Under pressure it ends in the OOM killer, not in `-ENOMEM` from `DUMB_CREATE`.
- On CMA drivers (pxa-lcdc, and sa1100-lcdc per `doc/mediaplayer-integration.md:62`) the kernel fails the allocation cleanly, and that failure already maps to `no_memory` (`src/zerocopy.c:294-305`).

## 2. Design

### 2.1 Model: pools plus an optional ceiling

| Limit | Applies to | Default | Check |
|---|---|---|---|
| **count** | one `wl_client` | `[zerocopy] max_buffers_per_client = 3`, or the app rule's `zerocopy_buffers` | buffers this client holds < count |
| **default pool** | all clients without a matching `[app.*]` rule, shared | `[zerocopy] budget_kb = 2048` | pool used + new ≤ pool cap |
| **app pool** | all clients matching one `[app.<id>]` rule, shared | `zerocopy_budget_kb`, else `zerocopy_buffers × frame(largest output)` | as above |
| **ceiling** | everything | `[zerocopy] total_kb = 0` (0 = no extra limit; worst case = default pool + Σ app pools) | total used + new ≤ ceiling |

Properties:
- **Defaults reproduce today's behaviour exactly.** No `[app.*]` sections means one pool of 2 MiB and 3 buffers per client. That is `src/zerocopy.c:289` with page rounding added, which changes nothing at QVGA or VGA (§5).
- **An app pool is keyed by the rule, not by the client.** Ten clients that all claim `app_id=mediaplayer` share one 7-buffer pool, so spoofing can't multiply memory (§2.6).
- **Pools are isolated.** Unlisted clients can't starve the player, unless `total_kb` is set below the sum (deliberate overcommit, logged at init).
- **The per-app cap is board-independent by default.** `zerocopy_buffers = 7` alone gives 7 × 152 KiB on QVGA and 7 × 600 KiB on the hx4700. One `picowl.ini` works on every board.

### 2.2 Config syntax

`[app.<app_id>]` is a new per-app section, parsed by prefix like `[power.*]` (`src/config.c:516`). Everything after `app.` is the app_id, dots included (`[app.org.example.Player]` → `org.example.Player`). The section name is taken verbatim between the brackets (`src/config.c:254-262`), so matching is case-sensitive and the name is not trimmed, the same as every other section.

The keys use a `zerocopy_` prefix so the section can also carry the per-app `hold_action` from the tap-and-hold plan without clashes.

```ini
[zerocopy]
# existing: enable, single_buffer, panel_autohide
max_buffers_per_client = 3     # 1..32, default 3
budget_kb = 2048               # 0..65536, default 2048; pool of unlisted clients (0 = none may allocate)
total_kb = 0                   # 0 or 1..65536, default 0 = no extra ceiling

[app.mediaplayer]
zerocopy_buffers = 7           # 1..32; unset = [zerocopy] max_buffers_per_client
# zerocopy_budget_kb = 4200    # 0..65536; unset = zerocopy_buffers x full frame of largest output
exe = /usr/bin/mediaplayer-wayland   # optional; rule matches only this binary (§2.6)
```

Parse rules, following the existing style:
- Integers go through `atoi` with a range check. Out of range → `pw_log(WLR_ERROR, ...)` and keep the default, as in `[memory]` (`src/config.c:447-479`).
- A repeated `[app.X]` merges into the same entry (find-or-create), not a second list node.
- An `[app.X]` with no zerocopy keys leaves the zerocopy fields at `-1` (inherit) and gets no pool.
- `exe` is resolved once at load with `realpath(val, buf)` into a stack `char[PATH_MAX]`, then `strdup`'d. That way a symlinked install path still matches `/proc/<pid>/exe`. If `realpath` fails, log at INFO and keep the literal string.
- Unknown keys in `[app.*]` are ignored silently, like unknown keys in `[zerocopy]` (`src/config.c:438-446`).

### 2.3 Data structures

`src/picowl.h`, in `struct pw_config` (`:99-142`, next to the zerocopy fields at `:124-126`):

```c
struct pw_app_config {
	struct wl_list link;       /* pw_config.apps, file order */
	char *app_id;              /* exact match, case-sensitive */
	char *exe;                 /* realpath, or NULL = trust the app_id claim */
	int zb_buffers;            /* -1 = inherit [zerocopy] max_buffers_per_client */
	int zb_budget_kb;          /* -1 = zb_buffers x frame(largest output) */
	unsigned zb_pool;          /* 0 = no pool (no zerocopy keys); else 1..n, set by the parser */
	/* the per-app hold plan adds its fields here */
};

	/* in struct pw_config: */
	int zb_max_buffers;        /* [zerocopy] max_buffers_per_client (default 3) */
	int zb_budget_kb;          /* [zerocopy] budget_kb (default 2048) */
	int zb_total_kb;           /* [zerocopy] total_kb (default 0 = none) */
	unsigned zb_n_pools;       /* 1 + number of rules with zb_pool != 0 */
	struct wl_list apps;       /* struct pw_app_config */

/* Rule for app_id (exact match), or NULL. NULL-safe. config.c */
const struct pw_app_config *pw_config_app(const struct pw_config *c, const char *app_id);
```

There is no `"*"` wildcard, unlike `pw_config_rot_mode` (`src/config.c:692-715`). The `[zerocopy]` keys are the wildcard.

New pure module `src/zbquota.{c,h}`. It has no wlroots dependency, like `dim.c`, `touchhold.c` and `copyrel.c`, and uses integers only:

```c
enum pw_zb_verdict { PW_ZB_OK, PW_ZB_OVER_COUNT, PW_ZB_OVER_POOL, PW_ZB_OVER_TOTAL };

struct pw_zb_req {
	uint32_t bytes;            /* page-rounded size of the new buffer */
	unsigned count, max_count; /* buffers the client holds, its limit */
	uint32_t pool_used, pool_cap;
	uint32_t total_used, total_cap;   /* total_cap 0 = no ceiling */
};

/* w*h*2 rounded up to page; 0 if w/h <= 0 or on overflow. */
uint32_t pw_zb_frame_bytes(int32_t w, int32_t h, uint32_t page);
/* Checks count, then pool, then ceiling; first failure wins. Uses
 * "bytes > cap - used" with used <= cap guarded: no 64-bit math needed. */
enum pw_zb_verdict pw_zb_check(const struct pw_zb_req *r);
/* used -= bytes, clamped at 0 (cf. the guard at zerocopy.c:246). */
void pw_zb_refund(uint32_t *used, uint32_t bytes);
```

`src/zerocopy.c` runtime state:
- `st` (`:39-49`) gains `uint32_t *pool_used` (one `calloc` of `zb_n_pools` entries in `pw_zerocopy_init`, freed in `pw_zerocopy_finish`) and `uint32_t page`, from `sysconf(_SC_PAGESIZE)` at init.
- `st.total_bytes` stays as the ceiling counter.
- `struct pw_zbuf` (`:25-37`) gains `uint16_t pool`. A buffer is refunded to the pool it was charged to, even if the client's app_id or exe changed in between.

### 2.4 Matching a client to a rule

`create_buffer` carries no surface (`protocols/picowl-buffer-v1.xml:21-30`). `attach_surface` comes later (`src/zerocopy.c:218-232`). So the match is per `wl_client`, made at each `create_buffer`, and never cached.

`server->views` is the wrong source. It holds only **mapped** views: `view_map` inserts at `src/view.c:239`. The player and `pw-test-client` both allocate after the first configure and before the first buffer commit, so their toplevel is not mapped yet. `pw-test-client` sets app_id and commits at `tests/pw-test-client.c:373-376`, then allocates at `:383` → `:239`. Walk wlroots' xdg-shell client list instead, which includes unmapped toplevels:

```c
/* zerocopy.c */
static const struct pw_app_config *rule_for_client(struct wl_client *client)
{
	struct wlr_xdg_shell *sh = st.server->xdg_shell;   /* picowl.h:255 */
	if (!sh)
		return NULL;
	struct wlr_xdg_client *xc;
	wl_list_for_each(xc, &sh->clients, link) {         /* wlr_xdg_shell.h:21, :39-46 */
		if (xc->client != client)
			continue;                                  /* a client may bind xdg_wm_base twice */
		struct wlr_xdg_surface *xs;
		wl_list_for_each(xs, &xc->surfaces, link) {    /* wlr_xdg_shell.h:265-286 */
			if (xs->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL || !xs->toplevel ||
			    !xs->toplevel->app_id)                 /* app_id: wlr_xdg_shell.h:207 */
				continue;
			const struct pw_app_config *a =
				pw_config_app(st.server->config, xs->toplevel->app_id);
			if (a && a->zb_pool && exe_ok(client, a))
				return a;
		}
	}
	return NULL;                                       /* default pool */
}
```

- **Order requirement for clients:** call `xdg_toplevel.set_app_id` before `create_buffer`. The XML description and `doc/buffers.md` need one sentence on this; the text change needs no version bump. A buffer created earlier is charged to the default pool and stays there.
- **Clients with no toplevel** (layer-shell panel, OSK) always use the default pool. Layer surfaces have a `namespace` (`wlr_layer_shell_v1.h`) but no app_id, and picowl does not store the namespace (`src/layer.c:301-302` only logs it).
- **Several toplevels with different app_ids:** the first one in `xc->surfaces` with a pool rule wins. wlroots inserts at the head (`types/xdg_shell/wlr_xdg_surface.c:445`), so the newest wins. That is deterministic but unspecified for clients, and documented as unsupported.
- **app_id changes later:** this only affects later requests. Existing buffers are never revoked.

### 2.5 `create_buffer` flow (replaces `src/zerocopy.c:283-293`)

```
bytes  = pw_zb_frame_bytes(w, h, st.page)        // after the too_large check at :277
rule   = rule_for_client(client)
pool   = rule ? rule->zb_pool : 0
maxc   = rule && rule->zb_buffers > 0 ? rule->zb_buffers : cfg->zb_max_buffers
pcap   = pool_cap(rule)          // kb*1024, or zb_buffers * pw_zb_frame_bytes(largest_output)
tcap   = cfg->zb_total_kb * 1024 // 0 = none
count  = existing loop (:284-288)
v = pw_zb_check({bytes, count, maxc, st.pool_used[pool], pcap, st.total_bytes, tcap})
v != OK          -> failed(no_memory), wlr_log(WLR_DEBUG, reason, pid, app_id, used/cap)
allocate         (:294-305, unchanged)
real = lseek(attr.fd[0], 0, SEEK_END), then lseek(..., 0, SEEK_SET)
       // dma_buf_llseek returns dmabuf->size: linux/drivers/dma-buf/dma-buf.c:251-276
       // on error: real = page_round(attr.stride[0] * h)
real > bytes && re-check fails -> drop, failed(no_memory)   // stride padding edge case
z->bytes = real; z->pool = pool; st.pool_used[pool] += real; st.total_bytes += real
```

- The check runs **before** allocating, so a rejected request costs no `DUMB_CREATE` and no full-buffer `memset`.
- `buffer_resource_destroy` (`:239-251`) refunds `z->bytes` to `st.pool_used[z->pool]` and `st.total_bytes` via `pw_zb_refund`.
- `largest_output()` (`:60-70`) is reused for the auto pool cap. With no outputs the cap is 0, and `too_large` fires first anyway (`:277`).

### 2.6 Security

A client can claim any app_id, and picowl does not track pids today: there is no `wl_client_get_credentials` in `src/`. The threat on a single-user PDA is memory exhaustion, not privilege. Mitigations, from cheapest up:

1. **Bounded by construction.** A spoofer can use at most the pool the admin granted to that app_id, shared with the real app. Worst case total = default pool + Σ app pools, or `total_kb`. Without `exe` this is the whole defence, and it is the documented default.
2. **`exe` binding (opt-in per rule).** `wl_client_get_credentials(client, &pid, &uid, &gid)` (`/usr/include/wayland-server-core.h:323-325`), then `readlink("/proc/<pid>/exe")` into a stack buffer, then `strcmp` against the rule's `exe`. A mismatch, an unreadable link (other uid) or a `" (deleted)"` suffix (binary replaced during an upgrade) all mean the rule doesn't match → default pool. Cost: one `readlink` per `create_buffer` of a client claiming that app_id. No allocation.
3. **Caveat for `WAYLAND_SOCKET`.** The credentials are those of the socket's peer at connect time (SO_PEERCRED). For a `socketpair()` that is the process that **created** the pair (unix(7)), e.g. a launcher, not the app. picowl's own `pw_spawn` runs `sh -c` and clients connect to `WAYLAND_DISPLAY` (`src/server.c:144-163`, `:295`), so `exe` works for autostart and keybinding launches. A `WAYLAND_SOCKET` launch through a third-party launcher fails closed to the default pool. A picowl-owned trusted spawn (`socketpair` + `wl_client_create` + tag) is possible later but not part of this plan (Open questions).

### 2.7 Interaction with other state machines

- **copyrel / `copied` / `retained`:** none. Quota is checked only in `create_buffer`.
- **Output hotplug / rotation:** the auto pool cap tracks `largest_output()` at request time. Shrinking an output never revokes buffers; a pool can sit above its new cap until buffers are freed.
- **Buffer protocol v2 (`caching` plan):** independent. If v2 adds a `budget(max_buffers, bytes)` manager event (Open questions), it would send `maxc`/`pcap` from §2.5.
- **Per-app hold plan:** shares `[app.*]`, `struct pw_app_config` and `pw_config_app`. Whichever lands first adds the section; the second only adds fields.

## 3. Code changes

| File | Change | Touches |
|---|---|---|
| `src/picowl.h` | `struct pw_app_config`; `zb_*` and `apps` fields in `struct pw_config`; `pw_config_app()` prototype | `:99-142`, after `:443` |
| `src/config.c` | Defaults in `pw_config_default` (`wl_list_init(&c->apps)`, 3/2048/0) | `:170-214` |
| | Three keys in the `[zerocopy]` branch | `:438-446` |
| | New `strncmp(section, "app.", 4) == 0 && section[4]` branch before the unknown-section `else` | `:559-561` |
| | Post-parse: assign `zb_pool` numbers and `zb_n_pools`; INFO if `total_kb` < sum of fixed pools (overcommit) | after `:589` |
| | Free `apps` | `pw_config_free` `:649-690` |
| | `pw_config_app()` | next to `pw_config_rot_mode` `:692` |
| `src/zbquota.{c,h}` (new) | §2.3 pure functions, about 60 lines [est] | — |
| `src/zerocopy.c` | Remove `:22-23`; `pool` in `pw_zbuf`; `pool_used`/`page` in `st` | `:25-49` |
| | `rule_for_client`, `exe_ok`, `pool_cap` | new |
| | Replace the check | `mgr_create_buffer` `:283-293` |
| | Real size and charge | `:312-317` |
| | Refund | `buffer_resource_destroy` `:239-251` |
| | `calloc`/`free` of `pool_used` | `pw_zerocopy_init` `:417-420`, `pw_zerocopy_finish` `:471-492` |
| | New includes: `<unistd.h>`, `<stdio.h>` | — |
| `meson.build` | Add `src/zbquota.c` to `sources` | `:63-87` |
| `tests/meson.build` | New `test-zbquota` executable and test, same pattern as `test-touchhold` | — |
| `protocols/picowl-buffer-v1.xml` | `create_buffer` description: limits are compositor policy; set app_id first; failed objects should be destroyed. Text only, no version change | `:21-30` |
| `doc/buffers.md` | Rewrite the limits line | `:25` |
| `doc/zero-copy.md` | Limits and audit row | `:40-41`, `:244` |
| `README.md` | `[zerocopy]` keys plus a new `[app.<app_id>]` subsection | `:105-110` |
| `data/picowl.ini.example` | New keys, plus a commented `[app.mediaplayer]` example | `:91-99` |
| `tests/pw-test-client.c` | Optional `--zerocopy-count N`: request N buffers and print the count granted (for the hardware checklist); `zc_create` stays as is | `:234-256` |

No change to `view.c`, `input.c`, `power.c` or the wlroots patches. Only wlroots 0.19 public headers are used: `wlr_xdg_shell.h` (clients, surfaces, role, toplevel, app_id) and the existing `wlr_buffer_get_dmabuf`/allocator.

## 4. Fallbacks and failure modes

| Situation | Result |
|---|---|
| No `[app.*]` and no new keys | Identical to today: 3 per client, 2 MiB shared |
| Over count, pool or ceiling | `failed(no_memory)` as today (`:291`); DEBUG log naming which limit was hit, with pid, app_id and used/cap. No new reason code, since the player handles `no_memory` and `too_large` alike (`doc/mediaplayer-integration.md:54`) |
| Kernel allocation fails (CMA exhausted on pxa-lcdc) | `failed(no_memory)` via `:299-304`; pool untouched |
| shmem pages can't be faulted during wlroots' `memset` (`drm_dumb.c:85`) | OOM killer, as today. The budget is the only protection, so don't set app pools above free RAM |
| app_id set after some buffers | Those buffers stay in the default pool; later requests use the app pool |
| app_id changes or toplevel destroyed | Buffers are kept and refunded to their original pool on destroy |
| `exe` mismatch or unreadable `/proc/<pid>/exe` | Default pool (fails closed). DEBUG log |
| `budget_kb = 0` | Only clients with an app pool may allocate; others get `no_memory` and fall back to wl_shm (`doc/buffers.md:43-45`) |
| `total_kb` below the sum of pools | Allowed (overcommit); INFO at load; ceiling checked last |
| Bad values | ERROR log, default kept (`[memory]` style) |
| No outputs | `too_large` first (`:277`); auto pool cap is 0 |
| Zero-copy disabled (headless, no DRM, `enable = false`) | No global (`:401-410`); keys parsed but unused |

## 5. Memory and CPU cost on 64 MiB boards

Frame sizes, RGB565, 4 KiB pages:

| Board / driver | Memory | Frame | 3 (default) | 7 (player rule) |
|---|---|---|---|---|
| h2200, h5550 / mq11xx | shmem (`doc/zero-copy.md:12`) | 240x320 = 152 KiB (38 pages) | 456 KiB | 1064 KiB |
| hx4700 / w100 | shmem (`doc/zero-copy.md:13`) | 480x640 = 600 KiB | 1800 KiB | 4200 KiB (4.1 MiB) |
| h3800-class / sa1100-lcdc | CMA, write-combined (`doc/mediaplayer-integration.md:62`) | 152 KiB | 456 KiB | 1064 KiB, if the CMA pool allows (UNVERIFIED size) |
| h3970 / pxa-lcdc | 1 MiB CMA pool, shared with picowl's swapchain (`doc/zero-copy.md:15`, `:283-285`) | 152 KiB | kernel-limited | kernel-limited |

- **shmem boards.** The pages are resident from allocation (memset) and unswappable on swapless iPAQs. They are mapped by both picowl (`drm_dumb.c:78`) and the client, so they appear in **picowl's** RssShmem and VmHWM. On the hx4700 the player rule adds about 2.4 MiB over the default (4.1 MiB in total), around 5% of about 50 MiB usable (`doc/zero-copy.md:4`) [est]. The headless `rss_ceiling_kb` test (`doc/zero-copy.md:225-233`) never sees this; check it on the device.
- **pxa-lcdc.** CMA allocations are aligned to `min(get_order(size), CONFIG_CMA_ALIGNMENT)`. That is the rule in `linux/drivers/dma-buf/heaps/cma_heap.c:291-308`; that the GEM DMA path uses the same rule is UNVERIFIED (`kernel/dma` is not in the tree). A 152 KiB buffer then occupies a 256 KiB-aligned slot, so the 1 MiB pool holds about 4 frames including picowl's own scanout buffers (up to `WLR_SWAPCHAIN_CAP 4`, `include/wlr/render/swapchain.h:8`; normally 2) [est]. **Don't configure a player rule on the h3970.** The kernel caps it anyway, and the player needs only 2-3 buffers there (`copy_type = 0`).
- **picowl heap.** `pw_app_config` is about 28 bytes plus strings per rule. `pool_used` is 4 bytes × pools. `pw_zbuf` grows by 2 bytes (padding absorbs it). Total well under 1 KiB [est].
- **CPU.** Only `create_buffer` changes, and it runs a handful of times per client lifetime. Added work: a walk over xdg clients × surfaces (single digits), one `lseek` pair, plus a `readlink` only for `exe` rules. Nothing touches commit, frame, timer or present paths, so the per-frame allocation audit (`doc/zero-copy.md:236-246`) is unchanged apart from the wording of the bound. No FPU and no 64-bit multiply in the hot path: `pw_zb_frame_bytes` is bounded by output size, and the checks use subtraction.

## 6. Tests

Headless and unit tests, runnable in CI without `/dev/dri`:
- **`tests/test-zbquota.c` (new).**
  - `pw_zb_frame_bytes`: 240x320 → 155648, 480x640 → 614400, 1x1 → 4096, 0/negative → 0, 65535x65535 → 0 (overflow).
  - `pw_zb_check`: verdict order (count before pool before ceiling); exact-fit OK; one byte over → OVER_POOL; `total_cap = 0` means unlimited; `used > cap` (after an output shrink) rejects without underflow.
  - `pw_zb_refund`: clamps at 0.
  - A script of 7 player allocations on VGA plus 3 from another client against the default pool, then refunds after a simulated app_id change: totals return to 0.
- **`tests/test-config.c` + `tests/test-config.ini`.**
  - Defaults: 3, 2048, 0, empty `apps`.
  - `[zerocopy] max_buffers_per_client = 5`, `budget_kb = 1024`, `total_kb = 3072` are parsed.
  - Out-of-range `max_buffers_per_client = 0` keeps 3.
  - `[app.mediaplayer] zerocopy_buffers = 7`.
  - `[app.org.example.Player]` (dotted id).
  - Two `[app.mediaplayer]` sections merge into one entry.
  - `[app.holdonly]` without zerocopy keys gets `zb_pool == 0`.
  - `pw_config_app` exact match, miss, and NULL config/app_id.
  - `exe` pointing at a nonexistent path keeps the literal.
  - Source list unchanged unless config.c calls into zbquota.c (it shouldn't). `tests/meson.build:1-14`.
- **`smoke` and `rss`** must pass unchanged. Headless has no buffer global (`src/zerocopy.c:403`), so the wl_shm fallback path is what still gets checked (`tests/smoke.sh`).
- **What CI can't reach:** `rule_for_client` and `exe_ok` need real `wl_client`s and a DRM allocator, so they go on the hardware list.

Hardware-only checklist (hx4700 and h2200 at least, plus the h3970 for the CMA path):
- Without a rule: `pw-test-client --zerocopy-count 7` gets 3; the 4th gets `no_memory`; DEBUG log says "count".
- With `[app.picowl-test-client] zerocopy_buffers = 7`: gets 7. On the hx4700, `Shmem` in `/proc/meminfo` grows by about 4200 KiB; after exit it returns to the baseline and the pools read 0 (add a DEBUG line at refund).
- A second client claiming the same app_id while the first holds 7 gets `no_memory` (shared pool).
- With `exe =` set, a client claiming the app_id from another binary gets the default limits.
- On the h3970, a rule of 7 grants what CMA allows, `no_memory` follows, and picowl keeps rendering. Watch dmesg for CMA warnings. Unblank and rotate still work (swapchain re-allocation must not starve; `doc/zero-copy.md:283-285`).
- The player itself: `nbufs` reports 7 on QVGA copy-type boards, with no change in `copied`/`retained` behaviour.
- Compare VmHWM and `oom_score` of picowl with the player at 7 buffers against the default.

## 7. Mediaplayer side

- Keep the init order already planned (`doc/mediaplayer-integration.md:24`): toplevel with `app_id = "mediaplayer"` → first configure → then `create_buffer`. The new XML text makes `set_app_id` before `create_buffer` a requirement.
- Request `mp_core_want_bufs()`, stop at the first `failed`, and destroy the failed `picowl_buffer_v1` object. That is already the plan in §2.3 of that doc; nothing new.
- Ship a commented `[app.mediaplayer]` snippet with `zerocopy_buffers = 7` and `exe = <install path>` in the player's packaging notes, not in picowl's defaults.
- Never assume a count. On the h3970 expect 2-3 buffers.

## 8. Open questions

1. **OOM attribution.** picowl maps every client buffer, so a larger budget raises picowl's OOM badness as much as the player's. Should picowl set `oom_score_adj` negative? Lowering it needs CAP_SYS_RESOURCE, which picowl may not have under seatd.
2. **Tell clients their quota.** A v2 manager event `budget(max_buffers, bytes)` would save the probe-until-failed loop. Fold it into the `caching` v2 plan, or skip it, since a rejection costs one event and no allocation?
3. **Trusted spawn.** Should picowl launch configured apps over a `socketpair` and tag the `wl_client` (no pid or exe guessing, and it also fixes the `WAYLAND_SOCKET` caveat)? It needs `pw_spawn` to pass an fd through the double fork (`src/server.c:144-163`).
4. **The sa1100-lcdc CMA pool size** on the h3800-class boards, and whether 7 QVGA buffers fit next to the persistent copy-type buffer.
5. **Default pool when GTK2/GDK uses picowl-buffer.** If the future GDK2 backend allocates with picowl-buffer for every app, is 2 MiB shared enough, or should the default be per-client (`client_kb`)?

## 9. Effort estimate

| Part | Effort |
|---|---|
| config.c and picowl.h: keys, `[app.*]`, lookup, free | 0.5 day [est] |
| zbquota.c/.h and test-zbquota.c | 0.5 day [est] |
| zerocopy.c wiring: rule walk, exe, real size, pools | 0.5 day [est] |
| Config tests, ini example, README, buffers.md, zero-copy.md, XML text | 0.25 day [est] |
| `pw-test-client --zerocopy-count` and the hardware checklist on 2-3 boards | 0.5-1 day [est] |
| **Total** | **about 2-2.75 days [est]**, about 250 lines of C including tests [est] |

## Implementation notes

- **Shared rule type.** The per-app-hold plan landed first with `struct pw_app_rule` (kind `PW_RULE_APP` or `PW_RULE_LAYER`, list `pw_config.app_rules`), so the plan's `struct pw_app_config` and `apps` list do not exist. The buffer fields (`zb_buffers`, `zb_budget_kb`, `zb_pool`, `exe`) were added to `pw_app_rule`. `pw_config_app()` returns the `PW_RULE_APP` rule for an app_id. `zerocopy_*` keys and `exe` are accepted only in `[app.*]`; in `[layer.*]` they fall into the "unknown key" log.
- **Parsing.** One helper, `parse_int_log()`, does the range checks with `strtol` and rejects trailing junk (the plan said `atoi`). `total_kb` accepts 0..65536. A rule gets a pool if `zerocopy_buffers` or `zerocopy_budget_kb` was accepted; pool numbers are assigned after parsing, so merged sections and bad values are settled first. `exe` needs `realpath()`, so `config.c` defines `_XOPEN_SOURCE 700` (the project's `_POSIX_C_SOURCE` alone hides it in glibc).
- **`zbquota` additions.** The plan's three functions plus `pw_zb_round()` (page rounding with overflow check, also used for the real size), `pw_zb_pool_cap()` (kb or buffers x frame, saturating) and `pw_zb_exe_match()`. The last one reads `/proc/<pid>/exe`, so unlike the rest of the module it is not integer-only, but it needs no wlroots and can be unit-tested with `getpid()`. `exe_ok` from the plan is that function.
- **Real size.** The charge is the larger of the page-rounded request and the rounded `lseek(fd, 0, SEEK_END)` size (a smaller `lseek` result is not trusted). A buffer which only exceeds a limit by its real size is dropped and answered `no_memory`. Without a usable `lseek` the size is `stride * h`, rounded.
- **`rule_for_client`** follows the plan. It does not walk `server->views`. With an `exe` rule that does not match, the loop goes on to the client's other toplevels instead of returning.
- **DEBUG log.** One line per rejection with pid, app_id, the limit hit and used/cap. The plan's extra DEBUG line at refund was not added.
- **Not done.** The open questions (OOM attribution, a `budget` event, trusted spawn) are unchanged.
- **Tests.** `tests/test-zbquota.c` (the plan's cases, plus `pw_zb_pool_cap`, `pw_zb_round` and `pw_zb_exe_match`); `tests/test-config.c` `test_zerocopy_limits` with `tests/test-config-zb.ini`; `tests/pw-test-client.c` gets `--zerocopy-count N` and `--app-id ID`, and `tests/smoke.sh` runs `--zerocopy-count 7 --app-id mediaplayer` headless (wl_shm fallback). `rule_for_client`, the real-size path and the pools need a DRM allocator; they are on the hardware checklist in `doc/zero-copy.md`.
