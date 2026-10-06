# picowl panel: widget design

**Status:** partly implemented. `picowl-panel` is a plain C layer-shell client without a toolkit with a clock, a battery, a backlight and a volume widget: the bar is the widgets, a tap on a button pops a row out of the bar (a slider for the backlight and the volume, the date for the clock, the time left for the battery), the backlight goes through sysfs and the volume through alsa-lib. Not implemented are the app list, the network, Bluetooth, keyboard, rotation and storage widgets, xdg popups and tap-and-hold menus, `picowl-control-v1`, `panel.ini` and the LVGL plan; the panel has a tap and no tap-and-hold action. The behaviour of the panel as built (options, density, styles, rotated output, rows) is in [README.md](../README.md#panel). The rest of this document is the design of the full panel, and the parts that exist are marked where they differ from it.

The full panel is a layer-shell client that picowl starts at session start, at the top or bottom edge. It replaces the matchbox panel from the X/GPE stack. The design assumes the iPAQ constraints:
- about 50 MiB of usable RAM;
- no FPU;
- a display bus where every changed pixel costs CPU time (see [hardware.md §3](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/hardware.md)).

## 1. Interaction rules

Every widget follows the same rules, built on picowl's tap-and-hold (see `README.md` and `doc/cursors.md`):

| Gesture | Meaning |
|---|---|
| **Tap** | The widget's primary action: a toggle, or a quick popup |
| **Tap-and-hold** | picowl delivers it as a right click (button 3), which opens the widget's context menu. The menu lists the widget's secondary actions, then **Settings…**, **Move** and **Remove from panel** |
| **Hold on empty panel space** | Panel menu: **Add widget…**, **Panel settings…** (edge top/bottom, auto-hide, height) |

Popups and menus are xdg popups on the panel's layer surface. picowl accepts xdg popups on layer surfaces (`handle_new_popup` in src/layer.c), and its hold animation tells the user a context menu is coming. The current panel uses neither: its rows are part of the bar's own surface, and its surface has `hold_action = none`, so a press is a left click at touch-down.

## 2. Widgets

Implemented in `picowl-panel` today: clock, battery, backlight (called Brightness below) and volume, each with the tap action only and with the interface differences noted after the table. The other rows are design.

| Widget | Kernel / userspace interface | Tap | Tap-and-hold menu |
|---|---|---|---|
| **App list** | foreign-toplevel protocols from picowl (`zwlr_foreign_toplevel_manager_v1`, `ext_foreign_toplevel_list_v1`) | Popup list of running apps; tap one to switch to it | Close app, close all |
| **Battery** | `power_supply` sysfs (`capacity`, `status`, `voltage_now`, `time_to_empty_now`, AC/USB `online`) plus uevent netlink for changes | Popup: %, charging state, AC/USB, time remaining, voltage | Screen off, reboot, power off (via `systemctl`, with a polkit rule for the panel user). Suspend is greyed out until resume works on the board. Power settings… |
| **Brightness** | backlight via picowl (`picowl-control-v1`, §4), not sysfs directly, so dimming and the user's level stay consistent | Brightness slider popup | Power profile settings: dim and blank times, and the brightness cap, per AC / battery / low-battery profile |
| **Clock** | `timerfd` on `CLOCK_REALTIME`, aligned to the minute, with `TFD_TIMER_CANCEL_ON_SET` so clock changes are noticed immediately; one wake-up per minute | Month popup, or launch gpe-calendar | Set date, time and timezone (systemd-timedated); NTP on/off (systemd-timesyncd) |
| **Network** | **iwd** over D-Bus (`net.connman.iwd` is iwd's own bus name, not ConnMan): station state, `Scan`, `GetOrderedNetworks`, `Connect`, passphrase agent, known networks. **systemd-networkd** over D-Bus (`org.freedesktop.network1`): per-link state and addresses for `wlan0`, `bnep0` (Bluetooth PAN) and `usb0` (EEM gadget) | Current link, signal, IP; scan list; tap a network to connect (passphrase prompt through the agent) | Wi-Fi on/off (iwd `Device.Powered`), flight mode (rfkill all), forget network, Network settings… |
| **Bluetooth** | **BlueZ** over D-Bus plus `/dev/rfkill` events. PAN via `org.bluez.Network1.Connect("nap")`, with networkd doing DHCP on `bnep0` | Radio on/off | Devices, pair new (PIN agent; BT 1.1 hardware needs legacy PIN pairing), visibility, connect PAN |
| **Volume** | alsa-lib control events (UDA1380 / UDA1341 / AK4535); no PulseAudio or PipeWire at 64 MiB | Volume slider popup | Mute, mixer, output selection |
| **Keyboard** | wvkbd, controlled by signals (SIGUSR1 hide, SIGUSR2 show, SIGRTMIN toggle); picowl owns the process, see `doc/design/osk.md` §3.5 | Show/hide the on-screen keyboard | Layout choice, keyboard size |
| **Rotation** | `picowl-control-v1` (§4) | Rotate 90° | Choose orientation, rotation lock |
| **Storage** | udev `block` events and `/proc/self/mountinfo`; mounting via `systemd-mount` | List SD/CF cards, with eject | Card details, format… (optional) |

The four implemented widgets differ from the table. The brightness slider writes sysfs directly and picowl adopts the level it left ([power.md](power.md)), since `picowl-control-v1` does not exist. The clock tap opens a row with the weekday and the date, not a month popup. The battery tap opens a row with the time left, an estimate the panel makes from the sysfs attributes, not a popup with voltage and AC state, and the battery is polled every 30 s. The volume tap opens a slider row. All rows are part of the bar's layer surface and close 3 s after the last touch.

### Network stack notes
- **iwd associates; networkd configures.** Set `EnableNetworkConfiguration=false` in iwd so networkd owns DHCP and routes on every link. The network widget then has one source of truth for addresses.
- **systemd-resolved is optional.** Without it, use a static `resolv.conf`, or have networkd write one through `resolvconf`. That saves a daemon.
- **Kernel requirements for iwd:**
  - `CONFIG_KEYS`, `CONFIG_CRYPTO_USER_API_HASH` and `CONFIG_CRYPTO_USER_API_SKCIPHER`;
  - the hash and cipher modules iwd probes at start.

  Add them to the board config fragments.
- **[check] orinoco/Hermes:** the revived driver is full-MAC, so iwd talks to it through nl80211 `CMD_CONNECT`. Verify that the cfg80211 glue implements `connect` and handles the CCMP keys the way iwd expects ([kernel-general.md §7](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/kernel-general.md)). If it doesn't, association fails no matter what the panel does.

## 3. Cost on 64 MiB

The budget the design aims at. The current panel redraws only changed rectangles, has no seconds display, wakes up for a minute-aligned timerfd (`TFD_TIMER_CANCEL_ON_SET`), the mixer's descriptors and a 30 s battery timer, and reads the battery attributes by polling; it has no uevent, D-Bus or rfkill handling.

- **Redraws:** the panel redraws only the widget that changed. There is no seconds display and there are no animated icons. A clock update is about 1.3 KB of damage once a minute, which is negligible even on the MediaQ bus (~8 MB/s).
- **Wake-ups:** everything is event-driven (uevent netlink, D-Bus signals, `/dev/rfkill`, ALSA control events, a minute-aligned timerfd). Polling is only a fallback for drivers that never emit change events, at 60 s.
- **Daemon budget [est]:**

  | Daemon | RSS |
  |---|---|
  | dbus-daemon | ≈ 1 MB |
  | iwd | ≈ 1.5–2 MB |
  | systemd-networkd | ≈ 2–3 MB |
  | bluetoothd | ≈ 2 MB |

  Every service is optional. A widget whose service isn't running hides itself, or shows a "not installed" state; it never fails or retries in a loop.

## 4. Configuration through the UI

None of this exists. The panel is configured by command-line options (README.md#panel) and picowl by `picowl.ini` (README.md#configuration).

### Panel settings
- `~/.config/picowl/panel.ini`, in the same INI style as `picowl.ini`, holds the edge, auto-hide, height, widget order and per-widget options.
- It is edited through each widget's **Settings…** dialog and **Panel settings…**.
- It is reloaded live via inotify, so hand edits take effect too.

### Compositor settings
These are the power profiles, tap-and-hold timing, rotation and cursor. They change at runtime through a private protocol, **`picowl-control-v1`**:
- **Operations:** get and set config keys, with change events; rotate; blank and unblank; brightness.
- **Trust model:** the protocol is offered only to clients that picowl starts itself over a `WAYLAND_SOCKET` socketpair, i.e. the panel and a settings app. This is the same model Weston uses for its desktop shell. Ordinary clients can't change brightness or blank timers.
- **Persistence:** changes are written to `$XDG_CONFIG_HOME/picowl/picowl.ini`, layered over `/etc/picowl.ini`.

## 5. Implementation choice

| Option | For | Against |
|---|---|---|
| GTK+ 2 layer-shell client | Matches the GPE look | Blocked on the GDK2 Wayland backend ([toolkits.md §1](https://github.com/Angstrom-distribution/Tiny-UI-research/blob/docs/ipaq-ui-research/docs/ipaq-ui/toolkits.md)) |
| **LVGL Wayland client** | Works today, small and fast, RGB565 with per-rectangle damage | Needs a small patch to add layer-shell to LVGL's Wayland driver; popups become overlay layer surfaces |

**Proposal: start with LVGL.** The plan was made before the panel was written; the panel that exists is plain C with wl_shm drawing and no toolkit, so LVGL is not used. Keep each widget's model (D-Bus and sysfs handling, state) separate from its drawing, so a GTK2 front end can reuse the models later.

The panel is built with meson inside the picowl tree, as `picowl-panel` is. Remaining widgets and the control protocol: [roadmap](design/roadmap.md).
