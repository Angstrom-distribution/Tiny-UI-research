# picowl design plans and research

**Status:** index, checked against the code at the time of the last documentation revision. The roadmap is the one place for plans and open items; the plans below keep their reasoning, and the behaviour that was implemented is documented in the [README](../../README.md) and the documents in `doc/`.

The implemented plans began as the picowl-side work items of [mediaplayer-integration.md](../mediaplayer-integration.md). Each plan says on its Status line which commit its line references into picowl are against (mostly b9eac6d); references into wlroots are against 0.19.0. The effort figures, tagged [est], are the estimates made when each plan was written.

## The plan

| Document | Status | What it covers |
|---|---|---|
| [roadmap.md](roadmap.md) | maintained | Where things stand, what comes next, what only a board can verify, the decisions for the project owner, the ideas not started and the known issues. Start here |

## Implemented plans

| Plan | Status | What it adds | Interfaces | Effort [est] |
|---|---|---|---|---|
| [idle-inhibit.md](idle-inhibit.md) | implemented; documented in [power.md](../power.md) | No dim or blank while a *visible* surface holds an inhibitor; the power key and output-power still blank; per-profile `inhibit` key | `zwp_idle_inhibit_manager_v1` (wlroots); `pw_dim_set_inhibited`, `pw_power_inhibit` | ~1.5 days |
| [per-app-hold.md](per-app-hold.md) | implemented; documented in the README, Touch input | Per-`app_id` and per-layer-namespace overrides of the `[touch]` hold keys, chosen at touch-down and fixed for the gesture | `[app.*]` / `[layer.*]` config | ~1 day |
| [buffer-budget.md](buffer-budget.md) | implemented (`src/zbquota.c`, `src/zerocopy.c`); documented in the README, `[zerocopy]` | Configurable `picowl-buffer-v1` limits with per-`app_id` pools, so the player can have 7 buffers; bytes counted at the real allocated size | `[zerocopy]` and `[app.*]` config | ~2.5 days |
| [caching-event.md](caching-event.md) | implemented (`src/zbproto.c`, `src/copytype.c`); not verified on a board | `picowl-buffer-v1` version 2: a `caching` event that says whether a buffer may be read back or decoded into; replaces the `copy_type` stand-in | protocol version 2 (`since="2"`) | ~1.5 days + boards |
| [drm-lease.md](drm-lease.md) | implemented (`src/lease.c`); documented in [lease.md](../lease.md); not run on a board | Lease the output to the player (`--vo drm:lease`) with a policy (focused `app_id` on an allow-list), output parking and take-back; wlroots patch 0004 (overlay planes, grant fix) | `wp_drm_lease_device_v1` (wlroots) | ~6.5 days |
| [osk.md](osk.md) | partly implemented: the wvkbd-ipaq series (11 patches) and its recipe, `[osk]` supervision with the `osk` key action, the built-in `[layer.wvkbd]` rule and the input method relay exist; the input method keyboard grab, popup hold rules and the panel's keyboard button do not | The on-screen keyboard: wvkbd-ipaq, its supervision by picowl, and the text-input-v3 to input-method-v2 relay | `zwp_virtual_keyboard_v1`, layer-shell, signals, input-method-v2 | ~4.25 days (+1.5 for the relay) |

The order they were built in, and why: idle-inhibit first (playback is unusable without it); per-app-hold and buffer-budget next (both config-only, sharing the `[app.*]` section); caching-event (only the protocol bump; a version 1 client falls back to `copy_type`); drm-lease last (the largest item, hardware-dependent; the VT switch covers the player's Path B until then).

## Reference and partly implemented research

| Document | Status | What it covers |
|---|---|---|
| [ipaq-displays.md](ipaq-displays.md) | reference; its picowl part is implemented (`[output] size_mm`, the panel's density rule) | Panel sizes, densities, stripe orders and modes of the iPAQ families, what the kernel drivers report, and how picowl-panel turns that into a style and a bar height |
| [panel-text.md](panel-text.md) | research and decision record; the crisp style and `--crisp-font dejavu` are implemented | What Windows CE did for text on these devices (bi-level, bytecode-hinted Tahoma 12; ClearType off by default; only 1-bit text on the engine), a hinted-TrueType comparison through FreeType, the crisp style's font choice and licence notes, and what is worth accelerating |
| [mediaq-c8.md](mediaq-c8.md) | design and research; only stage 1 (havoc per-cell damage) is implemented | MediaQ C8 (8 bpp palettised) output and the 2D engine: verified chip and driver facts, benefit estimates, the player over the DRM lease first, a compositor C8 mode only after a board benchmark, a staged plan and board experiments |

## Design only

| Document | Status | What it covers |
|---|---|---|
| [remote-access.md](remote-access.md) | research and design proposal, nothing implemented | Remote access (VNC/RFB first) for an ARMv5 handheld on Bluetooth PAN or USB: server options, encodings and a bandwidth budget, the client matrix, and a remote-only mode that stops the local display upload while a viewer is connected |
| [remote-access-sources.md](remote-access-sources.md) | appendix to remote-access.md | Every research finding with its source and the skeptic's review of the decisive claims |

## How these were made

- The five media player plans: agents gathered the facts from the picowl, wlroots 0.19.0, labwc, cage, dwl and Linux sources, another agent wrote each plan, and a checker verified every code citation and API claim against the source; its corrections were applied.
- The research documents (mediaq-c8, panel-text, ipaq-displays, remote-access) say at their top how they were made and what their evidence tags mean.
- The Status lines were checked against the code when the documentation was last revised; a difference between a plan and the code is noted in the plan, and the code wins.
