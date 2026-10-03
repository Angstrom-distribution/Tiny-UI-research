# Power Management and Backlight Dimming

Power-aware idle timeout and screen blanking for embedded handhelds. Behaviour is controlled by power profile (AC, BATTERY, LOW) selected from sysfs power supply, with per-profile dim and blank timeouts. Dimming writes the backlight via sysfs `/sys/class/backlight/` without heap allocations.

## State Machine: ACTIVE, DIMMED, BLANKED

Display goes through three idle states, driven by user input (keyboard, touch, pointer) and power profile timeouts:

1. **ACTIVE**: User activity in the last `dim_after_s` seconds. Backlight at user-set level.
2. **DIMMED**: No input for `dim_after_s` seconds. Backlight dimmed to `dim_level` % of user level. Dim timing only applies if backlight is usable (has `max_brightness > 1`); if `max_brightness == 1` (on/off device), dimming is skipped.
3. **BLANKED**: No input for `blank_after_s` seconds. Outputs disabled; backlight not touched (for on/off devices) or remains dimmed.

Transitions:

- **Activity in DIMMED**: return to ACTIVE, undim immediately.
- **Activity in BLANKED**: return to ACTIVE (input swallowed if from keyboard/touch, not passed to clients).
- **Inhibited** (see [Idle inhibit](#idle-inhibit)): the timers stop; the state only changes through input, the power key or the output power protocol.
- **No timers**: remains in current state (e.g., if `dim_after_s = 0` and `blank_after_s = 0`, stays ACTIVE forever).
- **Timers both set**: blank takes precedence if `blank_after_s <= dim_after_s` (screen blanks directly without dimming).

## Power Profile Selection

Profiles: AC, BATTERY, LOW. Read from sysfs `/sys/class/power_supply/`. Rule:

- **AC**: any supply of type Mains or USB has `online = 1`
- **LOW**: no AC, and any Battery has `capacity <= low_capacity` (%)
- **BATTERY**: neither AC nor LOW
- **No readable supplies**: defaults to BATTERY

Profiles switch on kernel uevent (immediate) or fallback sysfs poll (every `poll_s` seconds). Each profile has its own `[power.ac]`, `[power.battery]`, `[power.low]` timings.

## Configuration Keys

All keys are in `data/picowl.ini.example`. Out-of-range values log a warning and fall back to the default.

### [power] Section (Main Settings)

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `backlight` | `auto` | `auto` or a device name | `auto` prefers type firmware > platform > raw, then name order. A name selects that device; a name that doesn't exist (for example `none`) leaves dimming inactive |
| `low_capacity` | 15 | 0..100 (%) | LOW is selected when a battery's capacity is at or below this. 0 means LOW only at 0% |
| `dim_level` | 30 | 1..100 (%) | Dimmed brightness as % of user level. E.g., 30 with user level 100 = dimmed to 30. Integer math: `(level * dim_level) / 100`, minimum 1 |
| `poll_s` | 300 | 0 or 30..86400 (s) | Fallback sysfs re-read period, for supply drivers that don't emit `change` uevents. 0 disables polling |

### [power.ac] Section (AC Power Profile)

Active when mains or USB is online.

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `dim_after_s` | 120 | 0..86400 (s) | Idle seconds before backlight dims. 0 disables dimming |
| `blank_after_s` | 600 | 0..86400 (s) | Idle seconds before screen blanks. 0 disables blanking. If <= `dim_after_s`, blanks directly without dimming |
| `inhibit` | yes | yes/no | Honour client idle inhibitors on this profile (see [Idle inhibit](#idle-inhibit)). Anything other than yes/no/true/false/1/0/on/off logs an error and keeps `yes` |

### [power.battery] Section (Battery Power Profile)

Active when no AC and battery capacity > `low_capacity`.

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `dim_after_s` | 20 | 0..86400 (s) | Idle seconds before dimming |
| `blank_after_s` | 60 | 0..86400 (s) | Idle seconds before blanking |
| `inhibit` | yes | yes/no | Honour client idle inhibitors |

### [power.low] Section (Low Battery Profile)

Active when no AC and battery capacity <= `low_capacity`.

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `dim_after_s` | 10 | 0..86400 (s) | Idle seconds before dimming |
| `blank_after_s` | 30 | 0..86400 (s) | Idle seconds before blanking |
| `inhibit` | yes | yes/no | Honour client idle inhibitors. Set `no` to let the screen dim and blank during playback when the battery is low. The brightness cap below applies either way |
| `max_brightness_pct` | 40 | 1..100 (%) | Cap user brightness to this % of `max_brightness` while LOW. E.g., 40 with max 255 = capped to 102. Integer math: `(max * pct) / 100`, minimum 1 |

## Idle inhibit

picowl offers `zwp_idle_inhibit_manager_v1` (version 1, from wlroots). A video player creates an inhibitor on its toplevel surface while it plays. While at least one inhibitor counts, picowl stops its dim and blank timers; nothing wakes up during playback. The same result goes to `ext_idle_notifier_v1` clients, which are told the compositor is inhibited (version 2 `get_input_idle_notification` objects ignore inhibitors by protocol).

An inhibitor counts only while its surface is visible. picowl does not compute occlusion; it uses its stacking rules. Popups and subsurfaces count with the surface they belong to (at most 8 levels).

| Root surface | Counts when |
|---|---|
| xdg toplevel | mapped and it is the focused toplevel (all toplevels are maximized, so the others are covered) |
| xdg popup | its parent chain ends in a counting surface |
| layer surface, TOP or OVERLAY | mapped and its scene node is enabled. While autohide hides the panel, its surfaces don't count |
| layer surface, BACKGROUND or BOTTOM | mapped and no toplevel is focused |
| anything else | never |

The state is re-evaluated on every focus, stacking or panel change and on inhibitor create, destroy, map and unmap (`pw_idle_inhibit_update()` in `src/idle.c`), and logged at info level as `idle inhibit on (app_id X)` and `idle inhibit off`.

| Situation | Behaviour |
|---|---|
| Inhibitor appears while DIMMED | Back to ACTIVE with the backlight restored. A client can undim but never unblank |
| BLANKED (idle timeout) | An inhibitor never unblanks. The screen stays blank until input or the power key |
| Power key, output power protocol | Always blank, whatever the inhibitors. On unblank the inhibitor is still in force, so no timers start |
| Input while BLANKED and inhibited | Unblanks and the first event is swallowed, as without an inhibitor. The screen then stays on |
| Inhibit released | Timers restart from the release, so the screen doesn't blank right after a 2 h film |
| Profile change while inhibited | The new profile's `inhibit` key applies. `inhibit = no` releases the hold, with the timers restarted from that moment |
| LOW profile | Honoured by default. The LOW brightness cap still applies |
| No usable backlight | Only blanking is held; there is no dimming to hold |
| Client crashes or disconnects | The inhibitor is destroyed and the timers restart |
| Panel applet inhibitors | Don't count while autohide hides the panel (known limitation) |

Any client can keep the screen on while its surface is visible, as on other Wayland compositors. A hung client that stays connected and visible keeps the screen on; use the power key or switch to another app.

**Session inactive.** While another program owns the display on another VT (for example `mediaplayer-drm`), picowl's session is inactive: it gets no input, and its dim timer would otherwise write the sysfs backlight under the other program's video. `power.c` listens to the session `active` signal and holds the timers (reason `PW_INHIBIT_SESSION`) while the session is inactive; they restart from the moment it becomes active again. This reason ignores the `inhibit` key. There is no session on the headless backend.

### Legacy [idle] Section (Deprecated)

For backwards compatibility:

| Key | Meaning |
|-----|---------|
| `timeout_ms` | Milliseconds before blank. If no `[power.*] blank_after_s` is set, converted to seconds and applied to all profiles |

## Backlight Selection and Behaviour

Backlight device is selected once at startup from `/sys/class/backlight/<dev>/`.

### Auto Selection (Default)

`backlight = auto` or `backlight` not set: scan `/sys/class/backlight/`, prefer device by type order:

1. `type = firmware` (e.g., UEFI backlight)
2. `type = platform` (e.g., ACPI, driver-specific)
3. `type = raw` (direct hardware registers)
4. Ties broken alphabetically by device name

### Explicit Selection

`backlight = <device-name>`: use that device directly (e.g., `backlight = intel_backlight`). If not found, backlight control is disabled.

### On/Off Case (max_brightness == 1)

Devices with `max_brightness = 1` are on/off only (e.g., GPIO backlight, ASIC2 PWM configured as binary). When detected at open:

- picowl does not write the backlight at all for these devices: dimming and the `[power.low]` `max_brightness_pct` cap are skipped, and blanking only disables the outputs (the state machine goes ACTIVE to BLANKED).
- Logged as "backlight <name> is on/off only, dimming skipped".

Example: h5550 with GPIO backlight. h3870 and h3970 with PWM can have > 1, allowing dimming.

### Write Failures

If a backlight write fails with EACCES, EPERM or ENOENT (permission denied, device gone), dimming is disabled and blanking continues. Logged as "backlight <name> not writable (...), dimming disabled". Any other error (EIO, EBUSY) is logged as "write failed, will retry" and the level is written again on the next input or transition, so the panel is not left dimmed; the user level is still restored at exit.

While the panel is held at a level other than the user level (dimmed or LOW-capped), the user level is also kept in `$XDG_RUNTIME_DIR/picowl-backlight-<dev>` (`/run` if unset). After a crash the restarted picowl takes the user level from that file instead of the reduced live brightness. The file is removed when the user level is written back.

## sysfs Paths and Access Control

Backlight brightness file: `/sys/class/backlight/<device>/brightness`

### Permission requirements

picowl runs as a normal user on seatd and writes `brightness` directly.

- **udev rule:** meson installs `data/90-picowl-backlight.rules` (option `udev`, into the udev rules directory), and so does the OE recipe. It gives the `video` group write access when a backlight device appears:
  ```udev
  ACTION=="add", SUBSYSTEM=="backlight", RUN+="/bin/chgrp video /sys%p/brightness", RUN+="/bin/chmod g+w /sys%p/brightness"
  ```
- **Group membership:** `data/picowl.service` sets `SupplementaryGroups=video`. If you start picowl another way, put its user in `video`.

Don't run picowl as root or give it `CAP_DAC_OVERRIDE` just for the backlight.

## Per-board notes

Facts below are from the kernel port's per-board hardware notes (`h2200.md`, `h3800.md`, `h39xx.md`, `h5xxx.md`, `hx4700.md`), which are not in this repo and are not verified here. Backlight device names depend on the DTS; `backlight = auto` picks the device by type, so you rarely need a name. None of the boards has been tested with this code yet.

| Board | SoC / display | Power-supply data | Backlight | Dimming |
|---|---|---|---|---|
| h2210 | PXA255, MediaQ MQ1188 | `gpio-charger` (AC presence, MAX1898 charge enable) + DS2760 gauge on HAMCOP's 1-Wire | `pwm-backlight` on PWM0 | yes |
| h3870 | SA-1110, `sa1100-lcdc` | AC-adapter/charge sense + DS2760 over ASIC2's 1-Wire (OWM) | `pwm-backlight` on ASIC2 PWM0, levels 0 21 27 33 39 45 51 58 64 | yes |
| h3970 | PXA250, `pxa-lcdc` | `AC_IN_N` (ASIC3) + `adc-battery` on the ASIC2 ADC | ASIC2 PWM | yes |
| h5550 | PXA255, MediaQ MQ1132 | SC801 charger + DS2760 on SAMCOP's 1-Wire | `gpio-backlight`, on/off only (`max_brightness` = 1); the MediaQ PWM is exposed as a `pwm_chip` but not yet wired to `pwm-backlight` | skipped (one info log); blanking still works |
| hx4700 | PXA270, W3220 | none usable: DS1WM never completes a bus reset, so the DS2760 never attaches; the BQ24022 charger is not implemented | PXA PWM, but the DTS points the `backlight` node at the wrong PWM (candidate C3 in the kernel port's `hx4700.md` notes) | uses the BATTERY profile (no supply data); dimming depends on the C3 fix |

Things to verify on each board:
- **Supply events.** Some battery drivers update sysfs without emitting a `change` uevent. If profile switches lag, set `poll_s` (for example 60) for that board.
- **Backlight access.** `brightness` must be writable by the user picowl runs as. The log reports `not writable, dimming disabled` once otherwise; check that the udev rule ran and that the `video` group is set.
- **Restore after unblank.** Some drivers restore their own level when the output is re-enabled. picowl writes the user level after re-enabling; check that the final level is the user's.

## Testing

`meson test -C build` covers this feature with:
- **`backlight`, `powersupply` and `dim` unit tests**, using fake sysfs trees and an injected clock;
- **the config parser test**, including the `[power*]` keys, the legacy `[idle] timeout_ms` to `blank_after_s` mapping (rounded up, ignored when any `[power.*] blank_after_s` is set) and the `[core]` alias;
- **`power-e2e`**: headless picowl against a fake sysfs tree (`PICOWL_SYSFS_ROOT`). On the AC profile it checks that the real event loop dims the backlight from 40 to 12 after 1 s. On the LOW profile it checks that the startup brightness cap is applied (40 to 25). With the test client (`--inhibit`, `--linger S`) it also checks that a visible inhibitor holds dimming and the timers restart on release, that an inhibitor behind the focused window does not count, and that `[power.low] inhibit = no` is honoured. `smoke` checks the inhibit log lines.

The `dim` unit test covers `pw_dim_set_inhibited()` in every state.

The headless backend has no input devices, so restore on input isn't covered end to end; the `dim` unit test covers it. Uevent delivery isn't covered either; the parser is unit-tested on canned messages.

On hardware, run `picowl -d 3` and check:
1. The startup line `power: profile N, backlight NAME, dim … ms, blank … ms`.
2. Plugging and unplugging AC logs `power profile A -> B`.
3. The backlight dims after `dim_after_s`, a tap restores it and is delivered to the app, and the screen blanks after `blank_after_s`.
4. Idle inhibit, with the player: `idle inhibit on (app_id mediaplayer)` when playback starts and no dim or blank for longer than `blank_after_s`; `idle inhibit off` on pause, and the screen dims `dim_after_s` after the pause, not at once. The power key blanks during playback and unblanks again, and the screen then stays on. A tap while blanked is swallowed. Alt-tab to another app dims after `dim_after_s`. `kill -9` of the player releases the hold. With `[power.low] inhibit = no`, crossing `low_capacity` lets the screen dim.
5. On the h5550 (on/off backlight) playback longer than `blank_after_s` does not blank.
6. Switching to another VT for longer than `dim_after_s` leaves the backlight alone, and the timers restart on return.

## Implementation Details

**Modules:**

- `src/power.c`: Main idle loop, timer management, profile switches, inhibit reasons (`pw_power_inhibit()`, `PW_INHIBIT_CLIENT` and `PW_INHIBIT_SESSION`).
- `src/idle.c`: ext-idle-notify, and the idle-inhibit protocol: which inhibitors count (`pw_idle_inhibit_update()`).
- `src/dim.c`: Pure state machine (ACTIVE/DIMMED/BLANKED, plus the `inhibited` flag), no wlroots/wayland.
- `src/dim.h`: State and action definitions, rules documented as comments.
- `src/powersupply.c`: sysfs power supply reading and uevent parsing.
- `src/powersupply.h`: Profile selection rule (AC/BATTERY/LOW).
- `src/backlight.c`: sysfs backlight device selection and writes.
- `src/backlight.h`: auto-selection algorithm (firmware > platform > raw).

**Constraints:**

- No per-frame or per-event heap allocations in steady state (only uevent and timer callbacks).
- Integer math throughout (no floats, no FPU required).
- The `poll_s` timer re-reads sysfs periodically (when `poll_s > 0`) in addition to uevents. If the uevent socket cannot be opened (DEBUG log only), polling is the only update path, and there is none with `poll_s = 0`.

## Configuration Examples

### Conservative (Long Timeouts, Slow Dimming)

```ini
[power]
backlight = auto
low_capacity = 10
dim_level = 50
poll_s = 300

[power.ac]
dim_after_s = 300
blank_after_s = 900

[power.battery]
dim_after_s = 60
blank_after_s = 180

[power.low]
dim_after_s = 30
blank_after_s = 90
max_brightness_pct = 50
```

### Aggressive (Fast Blanking, Low Power)

```ini
[power]
low_capacity = 20
dim_level = 20

[power.ac]
dim_after_s = 60
blank_after_s = 180

[power.battery]
dim_after_s = 10
blank_after_s = 30

[power.low]
dim_after_s = 5
blank_after_s = 15
max_brightness_pct = 30
```

### Dimming Only (No Blanking)

```ini
[power.ac]
dim_after_s = 120
blank_after_s = 0

[power.battery]
dim_after_s = 20
blank_after_s = 0

[power.low]
dim_after_s = 10
blank_after_s = 0
```

### No Idle (Always On)

```ini
[power.ac]
dim_after_s = 0
blank_after_s = 0

[power.battery]
dim_after_s = 0
blank_after_s = 0

[power.low]
dim_after_s = 0
blank_after_s = 0
```
