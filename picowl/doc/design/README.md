# picowl design plans

**Status:** design only. Nothing here is implemented yet.

These are the picowl-side work items from [mediaplayer-integration.md](../mediaplayer-integration.md). Line references in the plans are against picowl commit b9eac6d and wlroots 0.19.0, so they drift as the code changes.

| Plan | What it adds | Interfaces | Effort [est] |
|---|---|---|---|
| [idle-inhibit.md](idle-inhibit.md) | No dim or blank while a *visible* surface holds an inhibitor. The power key and output-power still blank. Per-profile `inhibit` key | `zwp_idle_inhibit_manager_v1` (wlroots); `pw_dim_set_inhibited`, `pw_power_inhibit` | ~1.5 days |
| [buffer-budget.md](buffer-budget.md) | Configurable `picowl-buffer-v1` limits with per-`app_id` rules, so the player can have 7 buffers. Bytes accounted at the real allocated size | `[zerocopy]` and `[app.*]` config | implemented, ~2.5 days |
| [caching-event.md](caching-event.md) | `picowl-buffer-v1` version 2: a `caching` event that says whether a buffer may be read back or decoded into. Replaces the `copy_type` stand-in | Protocol v2 (`since="2"`) | ~1.5 days + boards |
| [drm-lease.md](drm-lease.md) | Lease the output to the player (`--vo drm:lease`), with policy (focused `app_id` on an allow-list), output parking and take-back. Includes a wlroots patch 0004 (overlay planes, grant fix) | `wp_drm_lease_device_v1` (wlroots) | ~6.5 days |
| [per-app-hold.md](per-app-hold.md) | Per-`app_id` and per-layer-namespace overrides of the `[touch]` hold keys, chosen at touch-down and fixed for the gesture | `[app.*]` / `[layer.*]` config | implemented, ~1 day |

## Suggested order

1. **idle-inhibit:** playback is unusable without it.
2. **per-app-hold** and **buffer-budget:** both are config-only and share the `[app.*]` section. Implement the section once.
3. **caching-event:** needs only the protocol bump; the player falls back to `copy_type` on v1.
4. **drm-lease:** the largest item, and hardware-dependent. The VT-switch path covers Path B until then.

## How these were made

- Sonnet agents gathered the facts from the picowl, wlroots 0.19.0, labwc, cage, dwl and Linux sources.
- An Opus agent wrote each plan.
- A Sonnet checker verified every code citation and API claim against the source, and the corrections it found have been applied.
