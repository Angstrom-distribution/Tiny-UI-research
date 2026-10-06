# picowl design plans

**Status:** the five media player plans (idle-inhibit, buffer-budget, caching-event, drm-lease, per-app-hold) are implemented. The OSK plan is design only, except its Part 3 (input-method relay), which is implemented without the keyboard grab and the popup hold rules.

These are the picowl-side work items from [mediaplayer-integration.md](../mediaplayer-integration.md). Line references in the plans are against picowl commit b9eac6d and wlroots 0.19.0, so they drift as the code changes.

| Plan | What it adds | Interfaces | Effort [est] |
|---|---|---|---|
| [roadmap.md](roadmap.md) | The state of the work, what was found, the plans in the proposed order, what only a board can verify, and the decisions that belong to the project owner. Start here | none | n/a |
| [ipaq-displays.md](ipaq-displays.md) | Reference: panel sizes, densities, stripe orders and modes of the iPAQ families, what the kernel drivers report today, and how picowl-panel turns it into a style and bar height at run time | `wl_output` physical size and subpixel, `[output] size_mm`, `--style auto` | n/a |
| [idle-inhibit.md](idle-inhibit.md) | **Implemented.** No dim or blank while a *visible* surface holds an inhibitor. The power key and output-power still blank. Per-profile `inhibit` key | `zwp_idle_inhibit_manager_v1` (wlroots); `pw_dim_set_inhibited`, `pw_power_inhibit` | ~1.5 days |
| [buffer-budget.md](buffer-budget.md) | **Implemented** (`src/zbquota.c`, `src/zerocopy.c`). Configurable `picowl-buffer-v1` limits with per-`app_id` rules, so the player can have 7 buffers. Bytes accounted at the real allocated size | `[zerocopy]` and `[app.*]` config | ~2.5 days |
| [caching-event.md](caching-event.md) | **Implemented** (`src/zbproto.c`, `src/copytype.c`). `picowl-buffer-v1` version 2: a `caching` event that says whether a buffer may be read back or decoded into. Replaces the `copy_type` stand-in | Protocol v2 (`since="2"`) | ~1.5 days + boards |
| [drm-lease.md](drm-lease.md) | **Implemented** (`src/lease.c`, `doc/lease.md`). Lease the output to the player (`--vo drm:lease`), with policy (focused `app_id` on an allow-list), output parking and take-back. Includes a wlroots patch 0004 (overlay planes, grant fix) | `wp_drm_lease_device_v1` (wlroots) | ~6.5 days |
| [osk.md](osk.md) | Design only, except Part 3 (implemented, without keyboard grab and popup hold rules). wvkbd-ipaq (patch series on upstream wvkbd + OE recipe: meson, RGB565, bitmap text, iPAQ layout, no exclusive zone); picowl `[osk]` supervision, `osk show/hide/toggle` action, built-in `[layer.wvkbd] hold_action = none`; input-method-v2/text-input-v3 relay (Part 3, implemented) | `zwp_virtual_keyboard_v1`, layer-shell, signals | ~4.25 days (+1.5 for Part 3, done) |
| [mediaq-c8.md](mediaq-c8.md) | Design only. MediaQ C8 (8 bpp palettised) output: verified chip and driver facts, benefit estimates, media player over the DRM lease first, havoc damage handling, a compositor C8 mode only after a board benchmark; staged plan and board experiments | DRM `GAMMA_LUT` palette, GC0C flips, picowl-buffer-v1 | not estimated until the quantisation benchmark |
| [panel-text.md](panel-text.md) | Research and decision record. What WinCE did for text on these devices (bi-level, bytecode-hinted Tahoma 12; ClearType off by default; only 1-bit text on the engine), a hinted-TrueType comparison through FreeType, the crisp style's font choice and licence notes, and what is worth accelerating | none | n/a |
| [remote-access.md](remote-access.md) | Research and design proposal, not implemented. Remote access (VNC/RFB first) for a small ARMv5 handheld on Bluetooth PAN or USB: server options, encodings and a bandwidth budget, the client matrix, and a remote-only mode that stops the local display upload while a viewer is connected | wayvnc/neatvnc, `[remote]` config, a headless output | not estimated until the stage 0 measurements |
| [per-app-hold.md](per-app-hold.md) | **Implemented** (`src/config.c`, `src/input.c`). Per-`app_id` and per-layer-namespace overrides of the `[touch]` hold keys, chosen at touch-down and fixed for the gesture | `[app.*]` / `[layer.*]` config | ~1 day |

## Suggested order

1. **idle-inhibit:** playback is unusable without it.
2. **per-app-hold** and **buffer-budget:** both are config-only and share the `[app.*]` section. Implement the section once.
3. **caching-event:** needs only the protocol bump; the player falls back to `copy_type` on v1.
4. **drm-lease:** the largest item, and hardware-dependent. The VT-switch path covers Path B until then.

## How these were made

- Sonnet agents gathered the facts from the picowl, wlroots 0.19.0, labwc, cage, dwl and Linux sources.
- An Opus agent wrote each plan.
- A Sonnet checker verified every code citation and API claim against the source, and the corrections it found have been applied.
