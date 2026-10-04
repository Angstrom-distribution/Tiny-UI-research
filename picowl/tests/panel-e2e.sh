#!/bin/sh
# Headless end-to-end test of picowl-panel against picowl, with a 240x320
# output, a fake sysfs tree (PICOWL_SYSFS_ROOT: a backlight with max 1023 at
# 600, a battery at 73 percent) and a fake sound card (pw-fake-ctl, an alsa-lib
# plugin whose state is a file).
#  A: the first frame: widget values and geometry (--dump-state).
#  B: touch (--inject, the same handlers as wl_pointer events): the backlight
#     and volume values reach sysfs and the mixer, the floor, a drag that
#     starts outside a slider, a slider without a mixer.
#  C: the layer surface shrinks the usable area for a second client, top and
#     bottom, with --height, and gives it back when the panel exits.
#  D: no redraw and no wake-up while nothing changes; the battery and an
#     external volume change are followed.
#  E: rotation: the panel is laid out again for the new width.
#  F: errors and exit codes: no compositor, no layer shell, no output, SIGTERM,
#     the compositor going away.
# Opt-in with PW_PANEL_TEST_SETTIME=1 (needs root, sets the system clock and
# puts it back): a change of the system time updates the clock at once, and
# the minute timer fires.
# usage: panel-e2e.sh PICOWL PANEL PW_TEST_CLIENT PW_KEY_CLIENT PW_FAKE_CTL PW_BARE_SERVER
PICOWL=$1
PANEL=$2
CLIENT=$3
KEYS=$4
# alsa-lib opens the plugin by the path it is given, so it must not depend on
# the directory the test runs in.
FAKECTL=$(cd "$(dirname "$5")" && pwd)/$(basename "$5")
BARE=$6
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export PICOWL_HEADLESS_SIZE=240x320
unset DISPLAY WAYLAND_DISPLAY
PID=
PANELPID=
CLOCK_T0=
CLOCK_U0=
uptime_s() { cut -d. -f1 /proc/uptime; }
# Puts the system clock back after the opt-in clock test, by the time that
# passed on the monotonic clock.
restore_clock() {
	[ -n "$CLOCK_T0" ] && date -s "@$((CLOCK_T0 + $(uptime_s) - CLOCK_U0))" >/dev/null 2>&1
	CLOCK_T0=
}
cleanup() {
	restore_clock
	[ -n "$PANELPID" ] && kill -9 "$PANELPID" 2>/dev/null
	[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
	rm -rf "$DIR"
}
fail() {
	echo "panel-e2e: FAIL: $*"
	cat "$DIR/picowl.log" 2>/dev/null
	for f in "$DIR"/*.out "$DIR"/*.err; do [ -f "$f" ] && { echo "--- $f"; cat "$f"; }; done
	cleanup
	exit 1
}

# ---- the fake system ----
SYS=$DIR/sys
export PICOWL_SYSFS_ROOT=$SYS
BL=$SYS/class/backlight/bl0
BAT=$SYS/class/power_supply/bat0
mkdir -p "$BL" "$BAT" "$DIR/ctl"
echo 1023 >"$BL/max_brightness"
echo 600 >"$BL/brightness"
echo raw >"$BL/type"
echo Battery >"$BAT/type"
echo 73 >"$BAT/capacity"
echo Discharging >"$BAT/status"
CTL=$DIR/ctl/state
printf 'volume 8\nswitch 0\n' >"$CTL"
cat >"$DIR/asound.conf" <<EOF
ctl.default {
	type pwfake
}
ctl_type.pwfake {
	lib "$FAKECTL"
}
EOF
export ALSA_CONFIG_PATH=$DIR/asound.conf
export PW_FAKECTL_STATE=$CTL

cat >"$DIR/picowl.ini" <<EOF
[zerocopy]
panel_autohide = false

[keybindings]
code:397 = rotate
EOF

start_picowl() {
	rm -f "$DIR"/wayland-*
	"$PICOWL" -d 2 -c "$DIR/picowl.ini" >"$DIR/picowl.log" 2>&1 &
	PID=$!
	i=0
	while ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
		kill -0 "$PID" 2>/dev/null || fail "picowl exited early"
		i=$((i + 1)); [ $i -gt 100 ] && fail "socket never appeared"
		sleep 0.05
	done
	SOCK=$(ls "$DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)
	export WAYLAND_DISPLAY=$(basename "$SOCK")
}

stop_picowl() {
	kill -TERM "$PID"
	i=0
	while kill -0 "$PID" 2>/dev/null; do
		i=$((i + 1)); [ $i -gt 60 ] && fail "picowl did not exit within 3 s"
		sleep 0.05
	done
	wait "$PID" || fail "picowl exit status $?"
	PID=
}

# wait_for FILE PATTERN SECONDS WHAT: wait until a line of FILE matches.
wait_for() {
	i=0
	while ! grep -q "$2" "$1" 2>/dev/null; do
		i=$((i + 1)); [ $i -gt $(($3 * 20)) ] && fail "$4: no '$2' in $1"
		sleep 0.05
	done
}

# has FILE PATTERN WHAT: the pattern must match a line.
has() { grep -q "$2" "$1" || fail "$3: no line matches '$2' in $1"; }

# start_panel NAME ARGS...: --watch panel in the background, ready after its
# first frame. Output in $DIR/NAME.out.
start_panel() {
	n=$1; shift
	: >"$DIR/$n.out"
	"$PANEL" --watch "$@" >"$DIR/$n.out" 2>"$DIR/$n.err" &
	PANELPID=$!
	wait_for "$DIR/$n.out" '^redraw first' 5 "$n"
}

# stop_panel NAME: SIGTERM, it must exit 0 within 2 s.
stop_panel() {
	kill -TERM "$PANELPID"
	i=0
	while kill -0 "$PANELPID" 2>/dev/null; do
		i=$((i + 1)); [ $i -gt 40 ] && fail "$1: the panel did not exit within 2 s"
		sleep 0.05
	done
	wait "$PANELPID"; RC=$?
	PANELPID=
	[ "$RC" -eq 0 ] || fail "$1: the panel exited with status $RC"
}

mapped() { "$CLIENT" 2>&1 | sed -n 's/.*mapped \([0-9]*x[0-9]*\) .*/\1/p'; }

start_picowl

# ---- A: the first frame ----
T0=$(date +%H:%M)
"$PANEL" --dump-state >"$DIR/a.out" 2>"$DIR/a.err" || fail "A: --dump-state failed"
T1=$(date +%H:%M)
cat "$DIR/a.out"
has "$DIR/a.out" '^panel width=240 height=56 row=28 format=RGB565 anchor=top ' "A geometry"
CLOCK=$(sed -n 's/^clock text=\([0-9:]*\) .*/\1/p' "$DIR/a.out")
[ "$CLOCK" = "$T0" ] || [ "$CLOCK" = "$T1" ] || fail "A: the clock shows '$CLOCK', the time is $T0"
has "$DIR/a.out" '^clock text=[0-2][0-9]:[0-5][0-9] rect=0,0,120,28$' "A clock rect"
has "$DIR/a.out" '^battery text=73% status=discharging percent=73 rect=120,0,120,28$' "A battery"
has "$DIR/a.out" '^backlight available=1 value=59 raw=600 max=1023 cell=0,28,120,28 ' "A backlight"
has "$DIR/a.out" '^volume available=1 value=20 cell=120,28,120,28 ' "A volume (8 of 0..40)"
THUMB_W=$(sed -n 's/^backlight .* thumb=[0-9]*,[0-9]*,\([0-9]*\),[0-9]*$/\1/p' "$DIR/a.out")
[ "${THUMB_W:-0}" -ge 20 ] || fail "A: the thumb is $THUMB_W px wide"
[ "$(cat "$BL/brightness")" = 600 ] || fail "A: the panel changed the brightness by looking"
[ "$(cat "$CTL")" = "$(printf 'volume 8\nswitch 0')" ] || fail "A: the panel changed the volume by looking"
echo "panel-e2e: A ok"

# ---- B: touch ----
# Backlight cell: track x 28..116, thumb 20 wide. x=100 is 91 percent.
"$PANEL" --dump-state --inject "p10,40;m60,40;m100,40;r" >"$DIR/b.out" 2>&1 || fail "B: inject failed"
has "$DIR/b.out" '^backlight available=1 value=91 raw=931 ' "B drag to x=100"
[ "$(cat "$BL/brightness")" = 931 ] || fail "B: brightness is $(cat "$BL/brightness"), wanted 931"
# Left of the track: the floor, 5 percent (51), never black.
"$PANEL" --dump-state --inject "p2,40;r" >"$DIR/b.out" 2>&1 || fail "B: inject failed"
has "$DIR/b.out" '^backlight available=1 value=5 raw=51 ' "B floor"
[ "$(cat "$BL/brightness")" = 51 ] || fail "B: the floor wrote $(cat "$BL/brightness"), wanted 51"
# The whole row cell is the target: top and bottom edge of row 2, on the icon.
echo 600 >"$BL/brightness"
for y in 28 55; do
	"$PANEL" --dump-state --inject "p100,$y;r" >"$DIR/b.out" 2>&1 || fail "B: inject failed"
	has "$DIR/b.out" '^backlight available=1 value=91 raw=931 ' "B press at y=$y"
	echo 600 >"$BL/brightness"
done
# A drag that starts outside a slider, or a press elsewhere, does nothing.
"$PANEL" --dump-state --inject "p60,10;m100,40;m100,45;r" >"$DIR/b.out" 2>&1 || fail "B: inject failed"
has "$DIR/b.out" '^backlight available=1 value=59 raw=600 ' "B drag from outside"
"$PANEL" --dump-state --inject "p300,40;r;p100,56;r;p230,10;r" >"$DIR/b.out" 2>&1 || fail "B: inject failed"
has "$DIR/b.out" '^backlight available=1 value=59 raw=600 ' "B press elsewhere"
[ "$(cat "$BL/brightness")" = 600 ] || fail "B: a press elsewhere wrote the brightness"
# Volume cell: track x 148..236. x=200 is 62 percent of 0..40: 25. Raising it
# above 0 switches the playback on.
"$PANEL" --dump-state --inject "p200,40;r" >"$DIR/b.out" 2>&1 || fail "B: inject failed"
has "$DIR/b.out" '^volume available=1 value=62 ' "B volume"
[ "$(cat "$CTL")" = "$(printf 'volume 25\nswitch 1')" ] || fail "B: the mixer holds '$(cat "$CTL" | tr '\n' ' ')', wanted volume 25 switch 1"
"$PANEL" --dump-state --inject "p125,40;r" >"$DIR/b.out" 2>&1 || fail "B: inject failed"
has "$DIR/b.out" '^volume available=1 value=0 ' "B volume to 0"
grep -q '^volume 0$' "$CTL" || fail "B: the mixer holds '$(cat "$CTL" | tr '\n' ' ')', wanted volume 0"
# A drag across both cells moves only the slider it started on.
printf 'volume 8\nswitch 0\n' >"$CTL"
echo 600 >"$BL/brightness"
"$PANEL" --dump-state --inject "p100,40;m200,40;m230,50;r" >"$DIR/b.out" 2>&1 || fail "B: inject failed"
has "$DIR/b.out" '^backlight available=1 value=100 raw=1023 ' "B drag across the cells"
has "$DIR/b.out" '^volume available=1 value=20 ' "B the other slider stays"
grep -q '^volume 8$' "$CTL" || fail "B: the volume changed under a backlight drag"
# Without a mixer the slider is greyed out and ignores touches.
echo 600 >"$BL/brightness"
ALSA_CONFIG_PATH=/nonexistent "$PANEL" --dump-state --inject "p200,40;r;p100,40;m110,40;r" >"$DIR/b.out" 2>"$DIR/b.err" || fail "B: no mixer: the panel failed"
has "$DIR/b.out" '^volume available=0 value=-1 ' "B no mixer"
has "$DIR/b.out" '^backlight available=1 value=' "B no mixer: the backlight works"
grep -q 'volume slider is disabled' "$DIR/b.err" || fail "B: no message about the missing mixer"
grep -q '^volume 8$' "$CTL" || fail "B: a touch on the disabled slider reached the mixer"
# No backlight device: that slider is greyed out too.
PICOWL_SYSFS_ROOT=$DIR/nosys "$PANEL" --dump-state >"$DIR/b.out" 2>"$DIR/b.err" || fail "B: no sysfs: the panel failed"
has "$DIR/b.out" '^backlight available=0 value=-1 ' "B no backlight"
has "$DIR/b.out" '^battery text=-- status=none percent=-1 ' "B no battery"
echo "panel-e2e: B ok"

# ---- C: the usable area ----
[ "$(mapped)" = 240x320 ] || fail "C: without the panel a toplevel is not 240x320: $(mapped)"
start_panel c
M=$(mapped)
[ "$M" = 240x264 ] || fail "C: with the panel a toplevel is $M, wanted 240x264"
stop_panel C
[ "$(mapped)" = 240x320 ] || fail "C: the area did not come back after the panel exited"
start_panel c --bottom
has "$DIR/c.out" 'anchor=bottom' "C bottom"
M=$(mapped)
[ "$M" = 240x264 ] || fail "C: with the panel at the bottom a toplevel is $M, wanted 240x264"
stop_panel C
start_panel c --height 80
has "$DIR/c.out" '^panel width=240 height=80 row=40 ' "C height 80"
M=$(mapped)
[ "$M" = 240x240 ] || fail "C: with a panel of 80 a toplevel is $M, wanted 240x240"
stop_panel C
start_panel c --height 5
has "$DIR/c.out" 'height=40 ' "C height clamped up"
stop_panel C
start_panel c --height 500
has "$DIR/c.out" 'height=120 ' "C height clamped down"
stop_panel C
start_panel c --scale 2
has "$DIR/c.out" 'scale=2 ' "C scale"
stop_panel C
echo "panel-e2e: C ok"

# ---- D: nothing happens while nothing changes ----
printf 'volume 8\nswitch 0\n' >"$CTL"
echo 73 >"$BAT/capacity"
echo 600 >"$BL/brightness"
export PICOWL_PANEL_BATTERY_POLL_S=1
start_panel d
sleep 3
# The battery was read three times and nothing changed: no redraw but the
# clock's (a minute may turn).
[ "$(grep '^redraw ' "$DIR/d.out" | grep -vc -e 'redraw first' -e 'redraw clock$')" = 0 ] ||
	fail "D: the panel redrew without a change: $(grep '^redraw ' "$DIR/d.out" | tr '\n' ' ')"
# The battery is followed.
echo 20 >"$BAT/capacity"
echo Charging >"$BAT/status"
wait_for "$DIR/d.out" '^redraw battery' 4 "D battery"
tail -n 6 "$DIR/d.out" | grep -q '^battery text=20% status=charging percent=20 ' || fail "D: the battery is not shown as 20% charging"
echo Full >"$BAT/status"
echo 100 >"$BAT/capacity"
wait_for "$DIR/d.out" '^battery text=100% status=full percent=100 ' 4 "D battery full"
rm "$BAT/capacity"
wait_for "$DIR/d.out" '^battery text=-- status=none ' 4 "D battery gone"
mkdir -p "$SYS/class/power_supply/ac"
echo Mains >"$SYS/class/power_supply/ac/type"
echo 1 >"$SYS/class/power_supply/ac/online"
wait_for "$DIR/d.out" '^battery text=AC status=ac ' 4 "D AC"
unset PICOWL_PANEL_BATTERY_POLL_S
# An external change of the volume is followed, event driven.
N=$(grep -c '^redraw volume' "$DIR/d.out")
printf 'volume 32\nswitch 1\n' >"$CTL"
wait_for "$DIR/d.out" '^volume available=1 value=80 ' 3 "D external volume"
[ "$(grep -c '^redraw volume' "$DIR/d.out")" -gt "$N" ] || fail "D: the external change was not redrawn"
printf 'volume 0\nswitch 1\n' >"$CTL"
wait_for "$DIR/d.out" '^volume available=1 value=0 ' 3 "D external volume 0"
stop_panel D
echo "panel-e2e: D ok"

# No busy loop: an idle panel with the 30 s battery timer sleeps.
start_panel idle
sleep 1
V0=$(awk '/^(non)?voluntary_ctxt_switches/ {s += $2} END {print s}' "/proc/$PANELPID/status")
T0=$(awk '{print $14 + $15}' "/proc/$PANELPID/stat")
sleep 4
V1=$(awk '/^(non)?voluntary_ctxt_switches/ {s += $2} END {print s}' "/proc/$PANELPID/status")
T1=$(awk '{print $14 + $15}' "/proc/$PANELPID/stat")
[ $((V1 - V0)) -le 3 ] || fail "D: $((V1 - V0)) context switches in 4 idle seconds"
[ $((T1 - T0)) -le 1 ] || fail "D: $((T1 - T0)) ticks of CPU in 4 idle seconds"
stop_panel idle
echo "panel-e2e: D idle ok ($((V1 - V0)) switches)"

# ---- E: rotation ----
start_panel e
has "$DIR/e.out" '^panel width=240 height=56 ' "E start"
"$KEYS" 397 || fail "E: key client failed"
wait_for "$DIR/e.out" '^panel width=320 height=56 row=28 ' 5 "E rotated"
has "$DIR/e.out" '^redraw resize' "E resize"
tail -n 6 "$DIR/e.out" | grep -q '^backlight available=1 value=59 .* cell=0,28,160,28 ' || fail "E: the sliders are not laid out for 320"
M=$(mapped)
[ "$M" = 320x184 ] || fail "E: a toplevel after the rotation is $M, wanted 320x184"
"$KEYS" 397 || fail "E: key client failed"
i=0
while [ "$(grep -c '^panel width=240' "$DIR/e.out")" -lt 2 ]; do
	i=$((i + 1)); [ $i -gt 100 ] && fail "E: no 240 wide layout after the second rotation"
	sleep 0.05
done
stop_panel E
echo "panel-e2e: E ok"

# ---- F: errors and exit codes ----
WAYLAND_DISPLAY=wayland-nonexistent "$PANEL" --dump-state >"$DIR/f.out" 2>"$DIR/f.err"
[ $? -eq 1 ] || fail "F: no compositor: exit status is not 1"
[ "$(wc -l <"$DIR/f.err" | tr -d ' ')" = 1 ] || fail "F: no compositor: not exactly one message: $(cat "$DIR/f.err")"
grep -q 'cannot connect' "$DIR/f.err" || fail "F: no compositor: unclear message: $(cat "$DIR/f.err")"
"$PANEL" --bogus >"$DIR/f.out" 2>"$DIR/f.err"
[ $? -eq 2 ] || fail "F: a bad option does not exit 2"
"$PANEL" --height abc >"$DIR/f.out" 2>"$DIR/f.err"
[ $? -eq 2 ] || fail "F: a bad height does not exit 2"
"$PANEL" --help >"$DIR/f.out" 2>"$DIR/f.err" || fail "F: --help failed"
grep -q -e '--bottom' -e '--dump-state' "$DIR/f.out" || fail "F: --help does not list the options"

# The compositor going away: exit 0.
start_panel g
kill -TERM "$PID"
wait "$PID"; PID=
i=0
while kill -0 "$PANELPID" 2>/dev/null; do
	i=$((i + 1)); [ $i -gt 60 ] && fail "F: the panel is still running 3 s after the compositor exited"
	sleep 0.05
done
wait "$PANELPID"; RC=$?
PANELPID=
[ "$RC" -eq 0 ] || fail "F: the compositor went away: the panel exited with status $RC"
echo "panel-e2e: F disconnect ok"

# A compositor without the layer shell: one clear message, exit 1.
"$BARE" wayland-bare >"$DIR/bare.out" 2>&1 &
BAREPID=$!
i=0
while [ ! -S "$DIR/wayland-bare" ]; do
	i=$((i + 1)); [ $i -gt 100 ] && { kill "$BAREPID" 2>/dev/null; fail "F: the bare server has no socket"; }
	sleep 0.05
done
WAYLAND_DISPLAY=wayland-bare "$PANEL" --dump-state >"$DIR/f.out" 2>"$DIR/f.err"
RC=$?
kill "$BAREPID"; wait "$BAREPID" 2>/dev/null
[ "$RC" -eq 1 ] || fail "F: no layer shell: exit status is $RC, not 1"
[ "$(wc -l <"$DIR/f.err" | tr -d ' ')" = 1 ] || fail "F: no layer shell: not exactly one message: $(cat "$DIR/f.err")"
grep -q 'zwlr_layer_shell_v1' "$DIR/f.err" || fail "F: no layer shell: unclear message: $(cat "$DIR/f.err")"
[ ! -s "$DIR/f.out" ] || fail "F: no layer shell: something on stdout"

# A compositor without outputs: one clear message, exit 1.
rm -f "$DIR"/wayland-*
WLR_HEADLESS_OUTPUTS=0 "$PICOWL" -d 2 -c "$DIR/picowl.ini" >"$DIR/picowl.log" 2>&1 &
PID=$!
i=0
while ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
	kill -0 "$PID" 2>/dev/null || fail "F: picowl without outputs exited early"
	i=$((i + 1)); [ $i -gt 100 ] && fail "F: picowl without outputs: no socket"
	sleep 0.05
done
SOCK=$(ls "$DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)
export WAYLAND_DISPLAY=$(basename "$SOCK")
"$PANEL" --dump-state >"$DIR/f.out" 2>"$DIR/f.err"
[ $? -eq 1 ] || fail "F: no output: exit status is not 1"
grep -q 'no output' "$DIR/f.err" || fail "F: no output: unclear message: $(cat "$DIR/f.err")"
stop_picowl
echo "panel-e2e: F errors ok"

# ---- opt-in: the system clock ----
if [ "$PW_PANEL_TEST_SETTIME" = 1 ]; then
	start_picowl
	start_panel t
	T0=$(date +%s)
	CLOCK_T0=$T0
	CLOCK_U0=$(uptime_s)
	date -s "@$((T0 + 7200 + 180))" >/dev/null || { CLOCK_T0=; fail "T: cannot set the clock"; }
	NEW=$(date +%H:%M)
	i=0
	while ! grep -q "^clock text=$NEW " "$DIR/t.out"; do
		i=$((i + 1))
		[ $i -gt 40 ] && fail "T: the clock did not follow a change of the system time to $NEW"
		sleep 0.05
	done
	N=$(grep -c "^clock text=$(date -d "@$((T0 + 6))" +%H:%M) " "$DIR/t.out")
	date -s "@$((T0 + 6))" >/dev/null
	BACK=$(date +%H:%M)
	i=0
	while [ "$(grep -c "^clock text=$BACK " "$DIR/t.out")" -le "$N" ]; do
		i=$((i + 1)); [ $i -gt 40 ] && fail "T: the clock did not follow the time back to $BACK"
		sleep 0.05
	done
	# The minute timer: wait for the next full minute (up to 61 s).
	S=$(date +%S)
	S=${S#0}
	[ -z "$S" ] && S=0
	wait_for "$DIR/t.out" "^clock text=$(date -d "@$(( $(date +%s) + 61 - S ))" +%H:%M) " 70 "T the minute timer"
	stop_panel T
	stop_picowl
	restore_clock
	echo "panel-e2e: clock ok"
fi

cleanup
echo "panel-e2e: ok"
