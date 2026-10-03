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

### [power.battery] Section (Battery Power Profile)

Active when no AC and battery capacity > `low_capacity`.

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `dim_after_s` | 20 | 0..86400 (s) | Idle seconds before dimming |
| `blank_after_s` | 60 | 0..86400 (s) | Idle seconds before blanking |

### [power.low] Section (Low Battery Profile)

Active when no AC and battery capacity <= `low_capacity`.

| Key | Default | Range | Meaning |
|-----|---------|-------|---------|
| `dim_after_s` | 10 | 0..86400 (s) | Idle seconds before dimming |
| `blank_after_s` | 30 | 0..86400 (s) | Idle seconds before blanking |
| `max_brightness_pct` | 40 | 1..100 (%) | Cap user brightness to this % of `max_brightness` while LOW. E.g., 40 with max 255 = capped to 102. Integer math: `(max * pct) / 100`, minimum 1 |

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

- Dimming is skipped (state machine stays ACTIVE or jumps to BLANKED).
- Backlight writes only control on/off, not brightness levels.
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

Facts below are from the board references (`h2200.md`, `h3800.md`, `h39xx.md`, `h5xxx.md`, `hx4700.md`). Backlight device names depend on the DTS; `backlight = auto` picks the device by type, so you rarely need a name. None of the boards has been tested with this code yet.

| Board | SoC / display | Power-supply data | Backlight | Dimming |
|---|---|---|---|---|
| h2210 | PXA255, MediaQ MQ1188 | `gpio-charger` (AC presence, MAX1898 charge enable) + DS2760 gauge on HAMCOP's 1-Wire | `pwm-backlight` on PWM0 | yes |
| h3870 | SA-1110, `sa1100-lcdc` | AC-adapter/charge sense + DS2760 over ASIC2's 1-Wire (OWM) | `pwm-backlight` on ASIC2 PWM0, levels 0 21 27 33 39 45 51 58 64 | yes |
| h3970 | PXA250, `pxa-lcdc` | `AC_IN_N` (ASIC3) + `adc-battery` on the ASIC2 ADC | ASIC2 PWM | yes |
| h5550 | PXA255, MediaQ MQ1132 | SC801 charger + DS2760 on SAMCOP's 1-Wire | `gpio-backlight`, on/off only (`max_brightness` = 1); the MediaQ PWM is exposed as a `pwm_chip` but not yet wired to `pwm-backlight` | skipped (one info log); blanking still works |
| hx4700 | PXA270, W3220 | none usable: DS1WM never completes a bus reset, so the DS2760 never attaches; the BQ24022 charger is not implemented | PXA PWM, but the DTS points the `backlight` node at the wrong PWM (candidate C3 in `hx4700.md`) | uses the BATTERY profile (no supply data); dimming depends on the C3 fix |

Things to verify on each board:
- **Supply events.** Some battery drivers update sysfs without emitting a `change` uevent. If profile switches lag, set `poll_s` (for example 60) for that board.
- **Backlight access.** `brightness` must be writable by the user picowl runs as. The log reports `not writable, dimming disabled` once otherwise; check that the udev rule ran and that the `video` group is set.
- **Restore after unblank.** Some drivers restore their own level when the output is re-enabled. picowl writes the user level after re-enabling; check that the final level is the user's.

## Testing

`meson test -C build` covers this feature with:
- **`backlight`, `powersupply` and `dim` unit tests**, using fake sysfs trees and an injected clock;
- **the config parser test**, including the `[power*]` keys and the legacy `[idle] timeout_ms` mapping;
- **`power-e2e`**: headless picowl against a fake sysfs tree (`PICOWL_SYSFS_ROOT`). On the AC profile it checks that the real event loop dims the backlight from 40 to 12 after 1 s. On the LOW profile it checks that the startup brightness cap is applied (40 to 25).

The headless backend has no input devices, so restore on input isn't covered end to end; the `dim` unit test covers it. Uevent delivery isn't covered either; the parser is unit-tested on canned messages.

On hardware, run `picowl -d 3` and check:
1. The startup line `power: profile N, backlight NAME, dim … ms, blank … ms`.
2. Plugging and unplugging AC logs `power profile A -> B`.
3. The backlight dims after `dim_after_s`, a tap restores it and is delivered to the app, and the screen blanks after `blank_after_s`.

## Implementation Details

**Modules:**

- `src/power.c`: Main idle loop, timer management, profile switches.
- `src/dim.c`: Pure state machine (ACTIVE/DIMMED/BLANKED), no wlroots/wayland.
- `src/dim.h`: State and action definitions, rules documented as comments.
- `src/powersupply.c`: sysfs power supply reading and uevent parsing.
- `src/powersupply.h`: Profile selection rule (AC/BATTERY/LOW).
- `src/backlight.c`: sysfs backlight device selection and writes.
- `src/backlight.h`: auto-selection algorithm (firmware > platform > raw).

**Constraints:**

- No per-frame or per-event heap allocations in steady state (only uevent and timer callbacks).
- Integer math throughout (no floats, no FPU required).
- Polling fallback when uevent socket unavailable (fallback to `poll_s` timer).

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
