#!/bin/sh
# Headless end-to-end test of picowl-panel against picowl, with a 240x320
# output, a fake sysfs tree (PICOWL_SYSFS_ROOT: a backlight with max 1023 at
# 600, a battery at 73 percent) and a fake sound card (pw-fake-ctl, an alsa-lib
# plugin whose state is a file).
#  A: the first frame: widget values and geometry (--dump-state).
#  B: touch (--inject, the same handlers as wl_pointer events): the buttons
#     open and close the slider row and set nothing, the row sets the backlight
#     and volume values that reach sysfs and the mixer, the floor, a drag that
#     starts outside the row, a slider without a device, the 3 s auto-close.
#  C: the layer surface: the exclusive zone is the bar and stays so while the
#     row is open (a second client's usable area does not change), the input
#     region is the bar and the row, the protocol sequence on the wire.
#  D: no redraw and no wake-up while nothing changes; the battery and an
#     external volume change are followed.
#  E: rotation with --edge top: the panel is laid out again for the new width.
#  F: errors and exit codes: no compositor, no layer shell, no output, SIGTERM,
#     the compositor going away; a font that is not there.
#  G: a translucent row lets the window behind it show through (capture).
#  H: subpixel text: the output advertises horizontal RGB stripes and the clock
#     has colour fringes (capture), without the advertisement, with
#     --subpixel none and on a rotated output it is grayscale.
#  I: the same over a video window that was playing before the panel started.
#  J: the clock is a button that opens a row with the weekday and the date: in
#     the system's time zone (TZ is set per run), highlighted while open, the
#     usable area of a second client unchanged, closed by the same tap, by the
#     other buttons and after 3 s, a press in the row keeps it open and sets
#     nothing, and the text fits the row.
#  K: the battery is a button that opens a row with the time left: a fake
#     DS2760 (exactly the attributes that driver exports, a charger supply and
#     the current's sign as the driver has it) discharging, charging, full, not
#     charging, idle, without a current and without a battery, a generic battery
#     with current_avg and one with power_now and energy_now; the wording, the
#     rounding and the hint for the first two minutes; the same shared open,
#     switch, close and 3 s auto-close; nothing is ever set by a tap.
#  U: --style crisp: over a bare desktop, a window of one colour and a patterned
#     window the pixels of the bar and of every kind of row are exactly the
#     colours of the theme (no anti-aliasing, no subpixel text, no colour of any
#     other kind), in both pixel fonts (--crisp-font fixed and dejavu: the same
#     colours, the proportional DejaVu bitmaps add none), the smooth style has
#     hundreds, the 1 px lines of the battery are whole rows and columns,
#     --font, --font-size and --subpixel are ignored.
#  V: the bar on the short side of an output rotated by 90 or 270 degrees (the
#     strip): the raw scanout of the bar and of the rows is pixel-identical to
#     the portrait bar, placement, exclusive zone, buffer transform and usable
#     area, --edge top, a rotation while the panel runs, and taps, a slider drag
#     and the auto-close through a virtual pointer at the logical position.
# Opt-in with PW_PANEL_TEST_SETTIME=1 (needs root, sets the system clock and
# puts it back): a change of the system time updates the clock at once, and
# the minute timer fires.
# usage: panel-e2e.sh PICOWL PANEL PW_TEST_CLIENT PW_KEY_CLIENT PW_FAKE_CTL PW_BARE_SERVER PW_CAPTURE_CLIENT PW_POINTER_CLIENT
PICOWL=$1
PANEL=$2
CLIENT=$3
KEYS=$4
# alsa-lib opens the plugin by the path it is given, so it must not depend on
# the directory the test runs in.
FAKECTL=$(cd "$(dirname "$5")" && pwd)/$(basename "$5")
BARE=$6
CAPTURE=$7
POINTER=$8
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export PICOWL_HEADLESS_SIZE=240x320
unset DISPLAY WAYLAND_DISPLAY
PID=
PANELPID=
CLIENTPID=
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
	[ -n "$CLIENTPID" ] && kill -9 "$CLIENTPID" 2>/dev/null
	[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
	rm -rf "$DIR"
}
fail() {
	echo "panel-e2e: FAIL: $*"
	cat "$DIR/picowl.log" 2>/dev/null
	for f in "$DIR"/*.out "$DIR"/*.err "$DIR"/*.cap; do [ -f "$f" ] && { echo "--- $f"; cat "$f"; }; done
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

[background]
color = #ff00ff

# the captures of the pixel checks
[capture]
enabled = true

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
hasnt() { grep -q "$2" "$1" && fail "$3: a line matches '$2' in $1"; return 0; }

# val FILE LINE KEY: the value of KEY=... on the (last) line that starts with LINE.
val() {
	awk -v pre="$2" -v key="$3" '$1 == pre { for (i = 2; i <= NF; i++) { split($i, a, "="); if (a[1] == key) v = a[2] } } END { print v }' "$1"
}
# comp STRING N: the Nth comma separated number.
comp() { echo "$1" | cut -d, -f"$2"; }

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

# dump OUT ARGS...: run the panel with --dump-state and its output in $DIR/OUT.
dump() {
	o=$1; shift
	"$PANEL" --dump-state "$@" >"$DIR/$o" 2>"$DIR/$o.err" || fail "$o: the panel failed: $(cat "$DIR/$o.err")"
}

mapped() { "$CLIENT" 2>&1 | sed -n 's/.*mapped \([0-9]*x[0-9]*\) .*/\1/p'; }

start_picowl

# ---- A: the first frame ----
T0=$(date +%H:%M)
"$PANEL" --dump-state >"$DIR/a.out" 2>"$DIR/a.err" || fail "A: --dump-state failed"
T1=$(date +%H:%M)
cat "$DIR/a.out"
has "$DIR/a.out" '^panel width=240 height=18 bar=18 row=0 format=RGB565 anchor=top popup=none$' "A geometry"
has "$DIR/a.out" '^surface exclusive=18 input=0,0,240,18$' "A exclusive zone and input region are the bar"
CLOCK=$(sed -n 's/^clock text=\([0-9:]*\) .*/\1/p' "$DIR/a.out")
[ "$CLOCK" = "$T0" ] || [ "$CLOCK" = "$T1" ] || fail "A: the clock shows '$CLOCK', the time is $T0"
has "$DIR/a.out" '^clock text=[0-2][0-9]:[0-5][0-9] rect=0,0,[0-9]*,17$' "A clock rect"
has "$DIR/a.out" '^battery text=73% status=discharging percent=73 rect=[0-9]*,0,[0-9]*,17$' "A battery"
BATR=$(val "$DIR/a.out" battery rect)
[ $(($(comp "$BATR" 1) + $(comp "$BATR" 3))) -eq 240 ] || fail "A: the battery rectangle does not reach the edge: $BATR"
has "$DIR/a.out" '^backlight available=1 value=59 raw=600 max=1023 button=[0-9]*,0,[0-9]*,18$' "A backlight button"
has "$DIR/a.out" '^volume available=1 value=20 button=[0-9]*,0,[0-9]*,18$' "A volume button (8 of 0..40)"
for k in backlight volume; do
	B=$(val "$DIR/a.out" $k button)
	[ "$(comp "$B" 3)" -ge 36 ] || fail "A: the $k button is only $(comp "$B" 3) px wide"
	[ "$(comp "$B" 4)" -eq 18 ] || fail "A: the $k button is not the whole bar high"
	[ $(($(comp "$B" 1) + $(comp "$B" 3))) -le "$(comp "$BATR" 1)" ] || fail "A: the $k button overlaps the battery"
done
has "$DIR/a.out" '^style font=' "A style line"
# 11 px for the clock and the battery, 10 px for the percentage in the row (the
# bitmap font draws 5x7 at twice its size for the bar and at its own size in
# the row).
has "$DIR/a.out" '^style font=ttf:.* size=11 small=10 \|^style font=bitmap size=14 small=7 ' "A text sizes of the default bar"
hasnt "$DIR/a.out" '^row ' "A no row while closed"
[ "$(cat "$BL/brightness")" = 600 ] || fail "A: the panel changed the brightness by looking"
[ "$(cat "$CTL")" = "$(printf 'volume 8\nswitch 0')" ] || fail "A: the panel changed the volume by looking"
BLB=$(val "$DIR/a.out" backlight button)
VOLB=$(val "$DIR/a.out" volume button)
echo "panel-e2e: A ok"

# ---- B: touch ----
# The row's geometry, read from a panel that has the backlight row open.
dump geo.out --inject "ibl"
has "$DIR/geo.out" '^panel width=240 height=54 bar=18 row=36 format=ARGB8888 anchor=top popup=backlight$' "B open: surface of bar and row, ARGB8888 for the translucent row"
has "$DIR/geo.out" '^surface exclusive=18 input=0,0,240,54$' "B open: the exclusive zone stays the bar, the input region is bar and row"
has "$DIR/geo.out" '^row slider=backlight rect=0,18,240,36 ' "B row rect"
TRACK=$(val "$DIR/geo.out" row track)
THUMB=$(val "$DIR/geo.out" row thumb)
TX=$(comp "$TRACK" 1); TW=$(comp "$TRACK" 3); TD=$(comp "$THUMB" 3)
[ "$TD" -ge 22 ] || fail "B: the thumb is $TD px"
CELL=$(val "$DIR/geo.out" row cell)
CX=$(comp "$CELL" 1)
CXR=$((CX + $(comp "$CELL" 3) - 1))
# x for a thumb centred at PCT percent of the travel
xat() { echo $((TX + TD / 2 + (TW - TD) * $1 / 100)); }
Y=38
echo 600 >"$BL/brightness"

# A tap on a button opens the row and sets nothing; the same button closes it.
dump b.out --inject "ibl"
has "$DIR/b.out" 'popup=backlight$' "B tap opens the backlight row"
has "$DIR/b.out" '^backlight available=1 value=59 raw=600 ' "B tap on the sun sets nothing"
dump b.out --inject "ibl;ibl"
has "$DIR/b.out" '^panel width=240 height=18 .*popup=none$' "B the same button closes the row and the surface shrinks"
dump b.out --inject "ibl;ivol"
has "$DIR/b.out" 'popup=volume$' "B the other button switches the row"
has "$DIR/b.out" '^row slider=volume ' "B the row shows the volume"
has "$DIR/b.out" '^volume available=1 value=20 ' "B tap on the speaker sets nothing"
dump b.out --inject "ivol;ivol;ivol"
has "$DIR/b.out" 'popup=volume$' "B open, close, open"
[ "$(cat "$BL/brightness")" = 600 ] || fail "B: a tap wrote the brightness"
grep -q '^volume 8$' "$CTL" || fail "B: a tap reached the mixer"
grep -q '^switch 0$' "$CTL" || fail "B: a tap switched the playback on"

# Drag in the backlight row: from 30 to 91 percent.
X0=$(xat 30); X1=$(xat 60); X2=$(xat 91)
dump b.out --inject "ibl;p$X0,$Y;m$X1,$Y;m$X2,$Y;r"
V=$(val "$DIR/b.out" backlight value)
[ "$V" -ge 90 ] && [ "$V" -le 92 ] || fail "B: the drag ended at $V percent, wanted 91"
RAW=$(( (V * 1023 + 50) / 100 ))
[ "$(cat "$BL/brightness")" = "$RAW" ] || fail "B: brightness is $(cat "$BL/brightness"), wanted $RAW for $V percent"
has "$DIR/b.out" 'popup=backlight$' "B a drag keeps the row open"
# Left end of the track: the floor, 5 percent (51), never black.
echo 600 >"$BL/brightness"
dump b.out --inject "ibl;p$CX,$Y;r"
has "$DIR/b.out" '^backlight available=1 value=5 raw=51 ' "B floor"
[ "$(cat "$BL/brightness")" = 51 ] || fail "B: the floor wrote $(cat "$BL/brightness"), wanted 51"
# The whole height of the row is the target, and the area past the track.
for yy in 18 53; do
	echo 600 >"$BL/brightness"
	dump b.out --inject "ibl;p$X2,$yy;r"
	V=$(val "$DIR/b.out" backlight value)
	[ "$V" -ge 90 ] && [ "$V" -le 92 ] || fail "B: a press at y=$yy gave $V"
done
echo 600 >"$BL/brightness"
dump b.out --inject "ibl;p$CXR,$Y;r"
has "$DIR/b.out" '^backlight available=1 value=100 raw=1023 ' "B a press right of the track is 100"
# The icon and the value text of the row are inert.
echo 600 >"$BL/brightness"
dump b.out --inject "ibl;p10,$Y;m$X2,$Y;r"
has "$DIR/b.out" '^backlight available=1 value=59 raw=600 ' "B the row's icon is inert"
[ "$(cat "$BL/brightness")" = 600 ] || fail "B: a press on the row's icon wrote the brightness"
has "$DIR/b.out" 'popup=backlight$' "B and does not close the row"
# Without the row, the area where it would be does nothing.
dump b.out --inject "p$X2,$Y;r;p$X2,$Y;m$X0,$Y;r"
has "$DIR/b.out" '^backlight available=1 value=59 raw=600 ' "B a press where the closed row would be"
[ "$(cat "$BL/brightness")" = 600 ] || fail "B: a press on the closed bar wrote the brightness"
# The panel read the level at start; a change made since by another process
# (picowl dimming, a script) is shown when the pointer enters, and a press
# must not take the stale level for the shown one.
dump b.out --inject "b300;e100,5"
has "$DIR/b.out" '^backlight available=1 value=29 raw=300 ' "B pointer enter re-reads the backlight"
echo 600 >"$BL/brightness"
XS=$(xat 59)
dump b.out --inject "b300;ibl;p$XS,$Y;r"
V=$(val "$DIR/b.out" backlight value)
[ "$V" -ge 58 ] && [ "$V" -le 60 ] || fail "B: a press after an external change gave $V"
[ "$(cat "$BL/brightness")" != 300 ] || fail "B: a press equal to the stale value was dropped"
echo 600 >"$BL/brightness"
# A drag that starts on the clock, or a press elsewhere, does nothing.
dump b.out --inject "ibl;p20,5;m$X2,$Y;m$X2,45;r"
has "$DIR/b.out" '^backlight available=1 value=59 raw=600 ' "B drag from outside"
dump b.out --inject "ibl;p300,$Y;r;p230,5;r"
has "$DIR/b.out" '^backlight available=1 value=59 raw=600 ' "B press elsewhere"
[ "$(cat "$BL/brightness")" = 600 ] || fail "B: a press elsewhere wrote the brightness"
# Volume row: a drag to about 62 percent of 0..40; raising it above 0
# switches the playback on.
XV=$(xat 62)
dump b.out --inject "ivol;p$XV,$Y;r"
V=$(val "$DIR/b.out" volume value)
[ "$V" -ge 61 ] && [ "$V" -le 63 ] || fail "B: the volume row gave $V"
RAW=$(( (40 * V + 50) / 100 ))
[ "$(cat "$CTL")" = "$(printf 'volume %s\nswitch 1' $RAW)" ] || fail "B: the mixer holds '$(cat "$CTL" | tr '\n' ' ')', wanted volume $RAW switch 1"
has "$DIR/b.out" '^row slider=volume ' "B the volume row"
dump b.out --inject "ivol;p$CX,$Y;r"
has "$DIR/b.out" '^volume available=1 value=0 ' "B volume to 0"
grep -q '^volume 0$' "$CTL" || fail "B: the mixer holds '$(cat "$CTL" | tr '\n' ' ')', wanted volume 0"
# A drag in the volume row moves the volume only, the backlight stays.
printf 'volume 8\nswitch 0\n' >"$CTL"
echo 600 >"$BL/brightness"
dump b.out --inject "ivol;p$X0,$Y;m239,$Y;m239,55;r"
has "$DIR/b.out" '^volume available=1 value=100 ' "B drag across to the end"
has "$DIR/b.out" '^backlight available=1 value=59 raw=600 ' "B the other slider stays"
[ "$(cat "$BL/brightness")" = 600 ] || fail "B: the backlight changed under a volume drag"
# Without a mixer the button is greyed out and ignores taps.
printf 'volume 8\nswitch 0\n' >"$CTL"
ALSA_CONFIG_PATH=/nonexistent "$PANEL" --dump-state --inject "ivol;p$XV,$Y;r;ibl;p$X0,$Y;m$X1,$Y;r" >"$DIR/b.out" 2>"$DIR/b.err" || fail "B: no mixer: the panel failed"
has "$DIR/b.out" '^volume available=0 value=-1 ' "B no mixer"
has "$DIR/b.out" 'popup=backlight$' "B no mixer: the speaker opened nothing, the sun still does"
has "$DIR/b.out" '^backlight available=1 value=' "B no mixer: the backlight works"
grep -q 'volume slider is disabled' "$DIR/b.err" || fail "B: no message about the missing mixer"
grep -q '^volume 8$' "$CTL" || fail "B: a touch on the disabled slider reached the mixer"
# No backlight device: that button is greyed out too.
PICOWL_SYSFS_ROOT=$DIR/nosys "$PANEL" --dump-state --inject "ibl" >"$DIR/b.out" 2>"$DIR/b.err" || fail "B: no sysfs: the panel failed"
has "$DIR/b.out" '^backlight available=0 value=-1 ' "B no backlight"
has "$DIR/b.out" 'popup=none$' "B no backlight: the sun opens nothing"
has "$DIR/b.out" '^battery text=-- status=none percent=-1 ' "B no battery"

# The auto-close: 3 s after the last touch, and a touch moves it. The waits
# run the panel's own event loop.
echo 600 >"$BL/brightness"
dump b.out --inject "ibl;w2500"
has "$DIR/b.out" 'popup=backlight$' "B still open after 2.5 s"
dump b.out --inject "ibl;w2500;w800"
has "$DIR/b.out" '^panel width=240 height=18 .*popup=none$' "B closed after 3.3 s, the surface is the bar again"
dump b.out --inject "ibl;w2000;p$X0,$Y;r;w2000"
has "$DIR/b.out" 'popup=backlight$' "B a touch 2 s in keeps it open at 4 s"
dump b.out --inject "ibl;w2000;p$X0,$Y;r;w2000;w1300"
has "$DIR/b.out" 'popup=none$' "B and it closes 3 s after that touch"
dump b.out --inject "ibl;p$X0,$Y;w3500"
has "$DIR/b.out" 'popup=backlight$' "B a stylus held down keeps it open"
dump b.out --inject "ibl;p$X0,$Y;w3500;r;w3300"
has "$DIR/b.out" 'popup=none$' "B and it closes 3 s after the release"
dump b.out --inject "ibl;w1500;ivol;w2000"
has "$DIR/b.out" 'popup=volume$' "B a tap on the other button is a touch too"
echo "panel-e2e: B ok"

# ---- C: the layer surface ----
[ "$(mapped)" = 240x320 ] || fail "C: without the panel a toplevel is not 240x320: $(mapped)"
start_panel c
M=$(mapped)
[ "$M" = 240x302 ] || fail "C: with the panel a toplevel is $M, wanted 240x302"
stop_panel C
[ "$(mapped)" = 240x320 ] || fail "C: the area did not come back after the panel exited"
# With the row open the usable area is the same: the row covers windows. The
# stylus stays down, so the row stays open while the client is asked.
start_panel c --inject "ibl;p$X0,$Y"
has "$DIR/c.out" 'popup=backlight$' "C the row is open"
M=$(mapped)
[ "$M" = 240x302 ] || fail "C: with the row open a toplevel is $M, wanted 240x302 as without"
has "$DIR/c.out" '^surface exclusive=18 input=0,0,240,54$' "C open: exclusive zone and input region"
stop_panel C
start_panel c --bottom
has "$DIR/c.out" 'anchor=bottom' "C bottom"
M=$(mapped)
[ "$M" = 240x302 ] || fail "C: with the panel at the bottom a toplevel is $M, wanted 240x302"
stop_panel C
# At the bottom the row opens above the bar: the bar stays at the edge.
start_panel c --bottom --inject "ivol;p$X0,15"
has "$DIR/c.out" '^panel width=240 height=54 bar=18 row=36 .*anchor=bottom popup=volume$' "C bottom: row open"
has "$DIR/c.out" '^row slider=volume rect=0,0,240,36 ' "C bottom: the row is the top of the surface"
has "$DIR/c.out" '^backlight available=1 value=[0-9]* raw=.* button=[0-9]*,36,[0-9]*,18$' "C bottom: the bar is at the bottom of the surface"
M=$(mapped)
[ "$M" = 240x302 ] || fail "C: with the panel at the bottom and the row open a toplevel is $M, wanted 240x302"
stop_panel C
start_panel c --height 80
has "$DIR/c.out" '^panel width=240 height=80 bar=80 row=0 ' "C height 80"
M=$(mapped)
[ "$M" = 240x240 ] || fail "C: with a bar of 80 a toplevel is $M, wanted 240x240"
stop_panel C
start_panel c --height 80 --inject "ibl;p$X0,100"
has "$DIR/c.out" 'popup=backlight$' "C row under a tall bar"
M=$(mapped)
[ "$M" = 240x240 ] || fail "C: with a bar of 80 and the row open a toplevel is $M, wanted 240x240"
stop_panel C
start_panel c --height 5
has "$DIR/c.out" 'height=18 bar=18 ' "C height clamped up"
stop_panel C
start_panel c --height 500
has "$DIR/c.out" 'height=80 bar=80 ' "C height clamped down"
stop_panel C
start_panel c --font-size 18
has "$DIR/c.out" 'size=18 ' "C font size"
stop_panel C
# --scale is gone.
"$PANEL" --scale 2 >"$DIR/c.out" 2>"$DIR/c.err"
[ $? -eq 2 ] || fail "C: the removed --scale does not exit 2"

# What goes over the wire: the layer surface is configured once with the bar's
# exclusive zone and only its size changes; the region requests; the buffers.
WAYLAND_DEBUG=1 "$PANEL" --dump-state --inject "ibl;p$X0,$Y;m$X1,$Y;m$X2,$Y;r;ibl" >"$DIR/w.out" 2>"$DIR/w.err" || fail "C: the panel failed with WAYLAND_DEBUG"
grep -q 'protocol error' "$DIR/w.err" && fail "C: protocol error"
EZ=$(grep -c 'set_exclusive_zone' "$DIR/w.err")
[ "$EZ" = 1 ] || fail "C: the exclusive zone was set $EZ times, wanted once"
grep -q 'set_exclusive_zone(18)' "$DIR/w.err" || fail "C: the exclusive zone is not the bar"
grep 'set_size' "$DIR/w.err" | sed 's/.*set_size/set_size/' | tr '\n' ' ' >"$DIR/w.sizes"
[ "$(cat "$DIR/w.sizes")" = "set_size(0, 18) set_size(0, 54) set_size(0, 18) " ] || fail "C: the sizes asked for: $(cat "$DIR/w.sizes")"
grep -q 'set_anchor(13)' "$DIR/w.err" || fail "C: the anchor is not top|left|right"
# Buffers: RGB565 for the bar, ARGB8888 for the surface with the translucent row.
grep 'create_buffer' "$DIR/w.err" | sed 's/.*create_buffer/create_buffer/' | tr '\n' ' ' >"$DIR/w.bufs"
echo "$(sed 's/new id wl_buffer[#@][0-9]*, //g' "$DIR/w.bufs")" | grep -q '^create_buffer(0, 240, 18, 480, 909199186) create_buffer(0, 240, 54, 960, 0) create_buffer(0, 240, 18, 480, 909199186) $' ||
	fail "C: the buffers are not RGB565 bar, ARGB8888 bar and row, RGB565 bar: $(cat "$DIR/w.bufs")"
# The input region: the bar, then the bar and the row; never more. Each
# set_input_region names a region that was filled by add calls just before.
awk '
	/wl_region[#@][0-9]+\.add\(/ { s = $0; sub(/.*wl_region[#@]/, "", s); id = s; sub(/\..*/, "", id); r = s; sub(/^[0-9]+\.add\(/, "", r); sub(/\).*/, "", r); gsub(/ /, "", r); reg[id] = reg[id] " " r; next }
	/wl_region[#@][0-9]+\.destroy\(/ { s = $0; sub(/.*wl_region[#@]/, "", s); sub(/\..*/, "", s); reg[s] = ""; next }
	/set_input_region\(/ { s = $0; sub(/.*set_input_region\(wl_region[#@]/, "", s); sub(/\).*/, "", s); print "input" reg[s]; next }
	/set_opaque_region\(/ { s = $0; sub(/.*set_opaque_region\(wl_region[#@]/, "", s); sub(/\).*/, "", s); print "opaque" reg[s]; next }
' "$DIR/w.err" >"$DIR/w.regions"
cat "$DIR/w.regions"
[ "$(grep -c '^input' "$DIR/w.regions")" -ge 3 ] || fail "C: no input regions on the wire"
grep '^input' "$DIR/w.regions" | sort -u | tr '\n' ';' >"$DIR/w.in"
[ "$(cat "$DIR/w.in")" = "input 0,0,240,18;input 0,0,240,54;" ] || fail "C: input regions on the wire: $(cat "$DIR/w.in")"
# The opaque region of the translucent row's surface is the bar only.
grep '^opaque' "$DIR/w.regions" | sort -u | tr '\n' ';' >"$DIR/w.op"
[ "$(cat "$DIR/w.op")" = "opaque 0,0,240,18;" ] || fail "C: opaque regions on the wire: $(cat "$DIR/w.op"), wanted the bar only, closed and open"
# Damage: after the surface grew, a drag damages the value rectangle of the row
# and not the whole surface; opening damages everything once.
awk '/set_size\(0, 54\)/ { open = 1 } open && /damage_buffer/ { sub(/.*damage_buffer\(/, ""); sub(/\).*/, ""); gsub(/ /, ""); print }' "$DIR/w.err" >"$DIR/w.damage"
[ "$(grep -c '^0,0,240,54$' "$DIR/w.damage")" = 1 ] || fail "C: the full surface was damaged $(grep -c '^0,0,240,54$' "$DIR/w.damage") times after opening: $(tr '\n' ' ' <"$DIR/w.damage")"
grep -q '^[0-9]*,\(1[89]\|[2-9][0-9]\),' "$DIR/w.damage" || fail "C: a drag damaged nothing in the row: $(tr '\n' ' ' <"$DIR/w.damage")"
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
tail -n 16 "$DIR/d.out" | grep -q '^battery text=20% status=charging percent=20 ' || fail "D: the battery is not shown as 20% charging"
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

# With the row open, an external change moves the thumb (the whole row is
# redrawn only for the value, not the icon).
printf 'volume 8\nswitch 1\n' >"$CTL"
start_panel d --inject "ivol"
has "$DIR/d.out" 'popup=volume$' "D volume row open"
printf 'volume 32\nswitch 1\n' >"$CTL"
wait_for "$DIR/d.out" '^redraw volume,row$' 2 "D external volume redraws the open row"
tail -n 16 "$DIR/d.out" | grep -q '^volume available=1 value=80 ' || fail "D: the row does not show the new volume"
# And the row closes by itself, event driven: the surface shrinks again.
N=$(grep -c '^redraw resize' "$DIR/d.out")
i=0
while [ "$(grep -c '^redraw resize' "$DIR/d.out")" -le "$N" ]; do
	i=$((i + 1)); [ $i -gt 100 ] && fail "D: the row did not close by itself"
	sleep 0.05
done
tail -n 16 "$DIR/d.out" | grep -q '^panel width=240 height=18 .*popup=none$' || fail "D: the surface did not shrink to the bar"
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
# After a row has closed again, too: its timer is disarmed and nothing wakes it.
start_panel idle --inject "ibl;ibl"
sleep 1
V0=$(awk '/^(non)?voluntary_ctxt_switches/ {s += $2} END {print s}' "/proc/$PANELPID/status")
sleep 4
V1=$(awk '/^(non)?voluntary_ctxt_switches/ {s += $2} END {print s}' "/proc/$PANELPID/status")
[ $((V1 - V0)) -le 3 ] || fail "D: $((V1 - V0)) context switches in 4 seconds after a row was closed"
stop_panel idle
echo "panel-e2e: D idle after a row ok ($((V1 - V0)) switches)"

# ---- E: rotation ----
# --edge top: the bar stays on the top of the rotated view, as it did before the
# strip on the short side (section V) existed.
start_panel e --edge top
has "$DIR/e.out" '^panel width=240 height=18 ' "E start"
"$KEYS" 397 || fail "E: key client failed"
wait_for "$DIR/e.out" '^panel width=320 height=18 bar=18 ' 5 "E rotated"
has "$DIR/e.out" '^redraw resize' "E resize"
BLX=$(comp "$(tail -n 16 "$DIR/e.out" | awk '$1 == "backlight" { for (i = 2; i <= NF; i++) { split($i, a, "="); if (a[1] == "button") print a[2] } }')" 1)
[ "$BLX" -gt "$(comp "$BLB" 1)" ] || fail "E: the buttons are not laid out for 320 (x=$BLX)"
tail -n 16 "$DIR/e.out" | grep -q '^surface exclusive=18 input=0,0,320,18$' || fail "E: the input region is not the new bar"
M=$(mapped)
[ "$M" = 320x222 ] || fail "E: a toplevel after the rotation is $M, wanted 320x222"
"$KEYS" 397 || fail "E: key client failed"
i=0
while [ "$(grep -c '^panel width=240' "$DIR/e.out")" -lt 2 ]; do
	i=$((i + 1)); [ $i -gt 100 ] && fail "E: no 240 wide layout after the second rotation"
	sleep 0.05
done
stop_panel E
# With the row open while the output turns: the row follows the new width.
start_panel e --edge top --inject "ibl;p$X0,$Y"
"$KEYS" 397 || fail "E: key client failed"
wait_for "$DIR/e.out" '^panel width=320 height=54 bar=18 row=36 ' 5 "E rotated with the row open"
tail -n 16 "$DIR/e.out" | grep -q '^row slider=backlight rect=0,18,320,36 ' || fail "E: the row is not 320 wide"
"$KEYS" 397 || fail "E: key client failed"
i=0
while [ "$(grep -c '^panel width=240 height=54' "$DIR/e.out")" -lt 2 ]; do
	i=$((i + 1)); [ $i -gt 100 ] && fail "E: no 240 wide layout with the row after the second rotation"
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
"$PANEL" --popup-alpha x >"$DIR/f.out" 2>"$DIR/f.err"
[ $? -eq 2 ] || fail "F: a bad alpha does not exit 2"
"$PANEL" --font >"$DIR/f.out" 2>"$DIR/f.err"
[ $? -eq 2 ] || fail "F: --font without a path does not exit 2"
"$PANEL" --help >"$DIR/f.out" 2>"$DIR/f.err" || fail "F: --help failed"
for o in --bottom --dump-state --font --font-size --height --popup-alpha --bar-alpha; do
	grep -q -e "$o" "$DIR/f.out" || fail "F: --help does not list $o"
done

# A font that is not there: the built-in bitmap font, one message, and the
# panel still draws (the clock rectangle of the capture has text pixels).
dump f.out --font /nonexistent/font.ttf
has "$DIR/f.out" '^style font=bitmap ' "F a missing font falls back"
grep -q "cannot use the font '/nonexistent/font.ttf'" "$DIR/f.out.err" || fail "F: no message about the missing font: $(cat "$DIR/f.out.err")"
echo "not a font" >"$DIR/notafont.ttf"
dump f.out --font "$DIR/notafont.ttf"
has "$DIR/f.out" '^style font=bitmap ' "F a file that is not a font falls back"
dump f.out --font "$DIR"
has "$DIR/f.out" '^style font=bitmap ' "F a directory falls back"
start_panel fb --subpixel none --font /nonexistent/font.ttf
sleep 0.3
"$CAPTURE" --distinct 8,2,60,16 >"$DIR/f.cap" 2>&1 || fail "F: capture failed: $(cat "$DIR/f.cap")"
D=$(sed -n 's/.*distinct \([0-9]*\)/\1/p' "$DIR/f.cap")
[ "${D:-0}" -ge 2 ] || fail "F: the fallback font drew nothing ($D colours where the clock is)"
stop_panel fb
# A real font gives an anti-aliased clock: more than two colours.
if [ -n "$(sed -n 's/^style font=ttf.*/ttf/p' "$DIR/a.out")" ]; then
	start_panel tt --subpixel none
	sleep 0.3
	"$CAPTURE" --distinct 8,2,40,16 >"$DIR/f.cap" 2>&1 || fail "F: capture failed"
	D=$(sed -n 's/.*distinct \([0-9]*\)/\1/p' "$DIR/f.cap")
	[ "${D:-0}" -ge 5 ] || fail "F: the TrueType clock has only $D colours, not anti-aliased"
	stop_panel tt
	dump f.out --font-size 20
	has "$DIR/f.out" '^style font=ttf:.* size=20 ' "F font size"
else
	echo "panel-e2e: no default font installed, the TrueType capture checks are skipped"
fi

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

# ---- G: a translucent row lets the window behind it show through ----
# The desktop background is magenta, the window green. The window is mapped
# after the panel, so that it is laid out for the bar's strip as a window of a
# running session is.
start_picowl
# g_case ARGS...: the panel with ARGS, then the window
g_case() {
	start_panel g "$@"
	"$CLIENT" --linger 30 >"$DIR/client.out" 2>&1 &
	CLIENTPID=$!
	wait_for "$DIR/client.out" 'mapped' 5 "G client"
	sleep 0.5
}
g_pixel() {
	"$CAPTURE" --at "$1" >"$DIR/g.cap" 2>&1 || fail "G: capture failed: $(cat "$DIR/g.cap")"
	RGB=$(sed -n 's/.*rgb=\(.*\)/\1/p' "$DIR/g.cap" | head -n1)
	[ -n "$RGB" ] || fail "G: no pixel in the capture"
	R=$((0x$(echo "$RGB" | cut -c1-2))); G=$((0x$(echo "$RGB" | cut -c3-4))); B=$((0x$(echo "$RGB" | cut -c5-6)))
}
g_end() {
	kill "$CLIENTPID" 2>/dev/null; wait "$CLIENTPID" 2>/dev/null; CLIENTPID=
	stop_panel g
}
RY=$((18 + 3))
# Row ground #252930 (37,41,48) at 224/255 over green (0,255,0): about (32,67,42).
g_case --inject "ibl;p$X0,$Y"
has "$DIR/g.out" '^panel width=240 height=54 .*format=ARGB8888 ' "G the surface is ARGB8888 for a translucent row"
g_pixel 3,$RY
[ "$G" -gt 55 ] && [ "$G" -lt 85 ] || fail "G: the pixel under the row is #$RGB: not a blend of the row ground and the window (green is $G)"
[ "$R" -lt 50 ] && [ "$B" -lt 60 ] || fail "G: the pixel under the row is #$RGB"
echo "panel-e2e: G row pixel #$RGB is a blend of the window and the row ground"
g_pixel 80,3
[ "$G" -lt 50 ] || fail "G: the opaque bar shows the window through it (#$RGB)"
g_end
# Opaque: the same pixel is the row ground, the window does not show.
g_case --popup-alpha 255 --inject "ibl;p$X0,$Y"
has "$DIR/g.out" '^panel width=240 height=54 .*format=RGB565 ' "G an opaque row needs no alpha: RGB565 again"
g_pixel 3,$RY
[ "$G" -lt 50 ] || fail "G: an opaque row shows the window through it (#$RGB)"
g_end
# A translucent bar shows the background behind it, closed as well.
g_case --bar-alpha 128
has "$DIR/g.out" '^panel width=240 height=18 .*format=ARGB8888 ' "G a translucent bar is ARGB8888 even when closed"
g_pixel 80,3
[ "$R" -gt 100 ] || fail "G: the translucent bar shows no background through it (#$RGB)"
g_end
# Closing the row leaves nothing of it on screen: the window is there again.
start_panel g --inject "ibl;w3500"
"$CLIENT" --linger 30 >"$DIR/client.out" 2>&1 &
CLIENTPID=$!
wait_for "$DIR/client.out" 'mapped' 5 "G client"
sleep 0.5
g_pixel 3,$RY
[ "$G" -gt 200 ] || fail "G: after the row closed the window does not show where it was (#$RGB)"
g_end
stop_picowl
echo "panel-e2e: G ok"

# ---- H: subpixel text ----
# The clock is white-ish on a dark ground. Grayscale text only moves along the
# line between the two, so the channels of every pixel have the same coverage;
# subpixel text has a different coverage under each stripe. pw-capture-client
# --spread reports the largest difference between the coverages of one pixel.
cp "$DIR/picowl.ini" "$DIR/picowl.base"
# h_picowl SUBPIXEL: picowl whose output advertises that layout ("": none given)
h_picowl() {
	cp "$DIR/picowl.base" "$DIR/picowl.ini"
	[ -n "$1" ] && printf '\n[output]\nsubpixel = %s\n' "$1" >>"$DIR/picowl.ini"
	start_picowl
}
# h_spread X,Y,W,H: the spread and the ink of the rectangle in the capture
h_spread() {
	"$CAPTURE" --spread "$1" --bg 1c1f24 --fg e8eaed >"$DIR/h.cap" 2>&1 || fail "H: capture failed: $(cat "$DIR/h.cap")"
	HSPREAD=$(sed -n 's/.*spread max=\([0-9.]*\) ink=.*/\1/p' "$DIR/h.cap")
	HINK=$(sed -n 's/.* ink=\([0-9]*\)/\1/p' "$DIR/h.cap")
	[ -n "$HSPREAD" ] || fail "H: no spread in the capture"
}
# h_clock NAME MODE LOW|HIGH: the panel's last text mode, and the spread of its clock
h_clock() {
	has "$DIR/h.out" "^text subpixel=$2\$" "$1 text mode"
	CR=$(val "$DIR/h.out" clock rect)
	h_spread "$(comp "$CR" 1),$(comp "$CR" 2),$(comp "$CR" 3),$(comp "$CR" 4)"
	[ "$HINK" -gt 30 ] || fail "H $1: the clock has $HINK ink pixels in the capture (#$CR)"
	if [ "$3" = HIGH ]; then
		awk "BEGIN { exit !($HSPREAD > 0.35) }" || fail "H $1: spread $HSPREAD: no colour fringes on the clock"
	else
		awk "BEGIN { exit !($HSPREAD < 0.15) }" || fail "H $1: spread $HSPREAD: the clock is not grayscale"
	fi
	echo "panel-e2e: H $1: mode $2, spread $HSPREAD over $HINK pixels"
}

# The h2200: horizontal RGB advertised, the clock has fringes.
h_picowl horizontal_rgb
"$PANEL" --dump-state >"$DIR/h.out" 2>"$DIR/h.err" || fail "H: --dump-state failed"
has "$DIR/h.out" '^text subpixel=rgb$' "H rgb advertised"
start_panel h --subpixel auto
sleep 0.3
h_clock rgb rgb HIGH
stop_panel H
# Forced off, the same output is grayscale.
start_panel h --subpixel none
sleep 0.3
h_clock forced-none none LOW
stop_panel H
# Forced BGR on the same output draws fringes too.
start_panel h --subpixel bgr
sleep 0.3
h_clock forced-bgr bgr HIGH
stop_panel H
# Opaque row: the value in it has fringes too; the translucent one does not.
start_panel h --subpixel auto --popup-alpha 255 --inject "ibl;w600"
wait_for "$DIR/h.out" '^row slider=' 5 "H row open"
sleep 0.3
RV=$(val "$DIR/h.out" row rect)
h_spread "$((240 - 60)),$(comp "$RV" 2),60,$(comp "$RV" 4)"
[ "$HINK" -gt 20 ] || fail "H: the value in the opaque row has $HINK ink pixels"
awk "BEGIN { exit !($HSPREAD > 0.35) }" || fail "H: the value in an opaque row has no fringes (spread $HSPREAD)"
stop_panel H
start_panel h --subpixel auto --inject "ibl;w600"
wait_for "$DIR/h.out" '^row slider=' 5 "H row open"
sleep 0.3
h_spread "$((240 - 60)),$(comp "$RV" 2),60,$(comp "$RV" 4)"
awk "BEGIN { exit !($HSPREAD < 0.2) }" || fail "H: the value in the translucent row has fringes (spread $HSPREAD)"
stop_panel H
# Rotated with the bar on the top of the rotated view: the output tells the
# stripes are across the other axis, grayscale. (In the strip they are not, see V.)
start_panel h --subpixel auto --edge top
"$KEYS" 397 || fail "H: key client failed"
i=0
while [ "$(grep '^text subpixel=' "$DIR/h.out" | tail -n1)" != "text subpixel=none" ]; do
	i=$((i + 1)); [ $i -gt 100 ] && fail "H: the panel did not go grayscale on a rotated output"
	sleep 0.05
done
sleep 0.3
# The capture is in the panel's own orientation, so the bar is an 18 px strip on
# the left or the right side; the other strip is the desktop background.
h_spread "0,0,18,320"
if [ "$HINK" -gt 30 ]; then
	STRIP=left
else
	h_spread "222,0,18,320"
	STRIP=right
fi
[ "$HINK" -gt 30 ] || fail "H: no text in either strip of the rotated capture"
LS=$HSPREAD
awk "BEGIN { exit !($LS < 0.15) }" || fail "H: the rotated output shows fringes (spread $LS, $STRIP strip)"
echo "panel-e2e: H rotated: grayscale, spread $LS in the $STRIP strip"
stop_panel H
stop_picowl
# Nothing advertised (unknown), or none: grayscale.
for sp in "" none; do
	h_picowl "$sp"
	start_panel h
	sleep 0.3
	h_clock "advertised-${sp:-nothing}" none LOW
	stop_panel H
	stop_picowl
done
cp "$DIR/picowl.base" "$DIR/picowl.ini"
echo "panel-e2e: H ok"

# ---- I: the same over a window which was there before the panel ----
# A video is playing in an RGB565 window when the panel starts, and the row
# opens long after the window last changed size. The row grows the layer
# surface from the opaque bar buffer to the ARGB8888 one: the window must not be
# culled under it (wlroots patch 0005). The window is redrawn at 30 frames a
# second, as a player does, and follows the size it is configured for.
start_picowl
"$CLIENT" --color ffba5a --video 30 --linger 20 >"$DIR/client.out" 2>&1 &
CLIENTPID=$!
wait_for "$DIR/client.out" 'mapped' 5 "I client"
# The injected script holds the event loop while it waits, so the panel is
# not waited for with start_panel.
"$PANEL" --watch --popup-alpha 150 --inject 'w1500;ibl;w9000' >"$DIR/h.out" 2>"$DIR/h.err" &
PANELPID=$!
sleep 2.5
g_pixel 3,21
BLEND=$RGB
# Row ground (28,31,36) at 150/255 over (255,186,90): about (121,95,58).
[ "$R" -gt 90 ] && [ "$R" -lt 160 ] && [ "$G" -gt 70 ] && [ "$G" -lt 130 ] && [ "$B" -gt 40 ] && [ "$B" -lt 90 ] ||
	fail "I: the pixel under the row is #$RGB: not a blend of the row ground and the playing window (premultiplied ground alone is #101818, the window #ffba5a)"
g_pixel 200,40
[ "$R" -gt 90 ] && [ "$R" -lt 160 ] && [ "$G" -gt 70 ] && [ "$G" -lt 130 ] || fail "I: the row at 200,40 is #$RGB, not a blend"
g_pixel 3,5
[ "$R" -lt 50 ] && [ "$G" -lt 50 ] || fail "I: the opaque bar is #$RGB"
echo "panel-e2e: I row pixel #$BLEND is a blend of the video and the row ground"
# Once the row has closed by itself the window is there again.
sleep 3
g_pixel 3,21
[ "$G" -gt 150 ] || fail "I: after the row closed the video does not show where it was (#$RGB)"
kill -9 "$PANELPID" 2>/dev/null
wait "$PANELPID" 2>/dev/null
PANELPID=
kill "$CLIENTPID" 2>/dev/null; wait "$CLIENTPID" 2>/dev/null; CLIENTPID=
stop_picowl
echo "panel-e2e: I ok"

# ---- J: the clock opens the date row ----
start_picowl
echo 600 >"$BL/brightness"
printf 'volume 8\nswitch 0\n' >"$CTL"
# The date as the system says it for a zone; a midnight in between makes the
# panel's answer either of the two.
date_in() { TZ=$1 LC_ALL=C date '+%A %-d %B %Y'; }
clock_in() { TZ=$1 LC_ALL=C date '+%H:%M'; }
# row_text FILE: the text of the open text row in a dump
row_text() { sed -n 's/^row kind=[a-z]* .* text="\(.*\)"$/\1/p' "$1" | tail -n1; }
for ZONE in UTC0 'NZST-12NZDT,M9.5.0,M4.1.0/3' 'EST5EDT,M3.2.0,M11.1.0'; do
	D0=$(date_in "$ZONE")
	TZ=$ZONE dump j.out --inject "icl"
	D1=$(date_in "$ZONE")
	has "$DIR/j.out" '^panel width=240 height=54 bar=18 row=36 format=ARGB8888 anchor=top popup=clock$' "J open: the clock opens the surface of bar and row"
	has "$DIR/j.out" '^row kind=date rect=0,18,240,36 ' "J the date row is the row"
	T=$(row_text "$DIR/j.out")
	[ "$T" = "$D0" ] || [ "$T" = "$D1" ] || fail "J: in $ZONE the date row says '$T', the system says '$D0'"
	C=$(sed -n 's/^clock text=\([0-9:]*\) .*/\1/p' "$DIR/j.out")
	[ "$C" = "$(clock_in "$ZONE")" ] || [ "$C" = "$(TZ=$ZONE LC_ALL=C date -d '1 minute ago' +%H:%M)" ] ||
		fail "J: the clock shows $C in $ZONE, not $(clock_in "$ZONE")"
	echo "panel-e2e: J $ZONE: '$T'"
done
# The names do not come from the locale.
LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 TZ=UTC0 dump j.out --inject "icl"
T=$(row_text "$DIR/j.out")
[ "$T" = "$(date_in UTC0)" ] || [ "$T" = "$(TZ=UTC0 LC_ALL=C date -d '1 minute ago' '+%A %-d %B %Y')" ] || fail "J: another locale changed the date row: '$T'"
TZ=UTC0 dump j.out --inject "icl"
has "$DIR/j.out" '^surface exclusive=18 input=0,0,240,54$' "J open: the exclusive zone is the bar, the input region bar and row"
# The text fits in the row, in the larger font.
W=$(sed -n 's/^row kind=date .* width=\([0-9]*\) avail=\([0-9]*\) .*/\1 \2/p' "$DIR/j.out")
[ "${W% *}" -gt 0 ] && [ "${W% *}" -le "${W#* }" ] || fail "J: the date row's text is $W px wide: it does not fit"
SZ=$(sed -n 's/^row kind=date .* size=\([0-9]*\) .*/\1/p' "$DIR/j.out")
BAR_SZ=$(sed -n 's/^style .* size=\([0-9]*\) small=.*/\1/p' "$DIR/j.out")
[ "$SZ" -ge "$BAR_SZ" ] || fail "J: the date is in a $SZ px font, smaller than the bar's $BAR_SZ px"
# The button: 36 px wide, the whole bar high, and open when it is.
has "$DIR/j.out" '^target clock rect=0,0,[0-9]*,18 hl=[0-9]*,[0-9]*,[0-9]*,[0-9]* open=1$' "J the clock target is the whole bar high and open"
CB=$(sed -n 's/^target clock rect=\([0-9,]*\) .*/\1/p' "$DIR/j.out")
[ "$(comp "$CB" 3)" -ge 36 ] || fail "J: the clock button is only $(comp "$CB" 3) px wide"
has "$DIR/j.out" '^target backlight .* open=0$' "J the other buttons are not open"
# Nothing was set by looking or tapping.
[ "$(cat "$BL/brightness")" = 600 ] || fail "J: a tap on the clock wrote the brightness"
grep -q '^volume 8$' "$CTL" && grep -q '^switch 0$' "$CTL" || fail "J: a tap on the clock reached the mixer"
# Shared logic: the same button closes, the others switch.
dump j.out --inject "icl;icl"
has "$DIR/j.out" '^panel width=240 height=18 .*popup=none$' "J the same tap closes the row and the surface shrinks"
hasnt "$DIR/j.out" '^row ' "J closed: no row"
dump j.out --inject "icl;ibl"
has "$DIR/j.out" 'popup=backlight$' "J the sun switches from the date"
has "$DIR/j.out" '^row slider=backlight ' "J to the slider row"
dump j.out --inject "ibl;icl"
has "$DIR/j.out" 'popup=clock$' "J the clock switches from the sun"
has "$DIR/j.out" '^row kind=date ' "J to the date"
dump j.out --inject "ivol;icl;ivol"
has "$DIR/j.out" 'popup=volume$' "J and the speaker switches from the date"
# A press anywhere in the date row sets nothing and keeps it open; it also
# resets the 3 s.
X=$(( TX + TD / 2 ))
dump j.out --inject "icl;p$X,$Y;m$((X + 30)),$Y;r"
has "$DIR/j.out" 'popup=clock$' "J a press and a drag in the date row keep it open"
has "$DIR/j.out" '^backlight available=1 value=59 raw=600 ' "J a drag in the date row sets no backlight"
has "$DIR/j.out" '^volume available=1 value=20 ' "J nor a volume"
[ "$(cat "$BL/brightness")" = 600 ] || fail "J: a press in the date row wrote the brightness"
grep -q '^volume 8$' "$CTL" || fail "J: a press in the date row reached the mixer"
dump j.out --inject "icl;w2500"
has "$DIR/j.out" 'popup=clock$' "J still open after 2.5 s"
dump j.out --inject "icl;w2500;w800"
has "$DIR/j.out" '^panel width=240 height=18 .*popup=none$' "J closed after 3.3 s"
dump j.out --inject "icl;w2000;p$X,$Y;r;w2000"
has "$DIR/j.out" 'popup=clock$' "J a press in the row 2 s in keeps it open at 4 s"
dump j.out --inject "icl;w2000;p$X,$Y;r;w2000;w1300"
has "$DIR/j.out" 'popup=none$' "J and it closes 3 s after that press"
# The usable area of a second client does not change with the row open (the
# stylus stays down, so the row stays open while the client is asked).
start_panel j --inject "icl;p$X,$Y"
has "$DIR/j.out" 'popup=clock$' "J the row is open"
M=$(mapped)
[ "$M" = 240x302 ] || fail "J: with the date row open a toplevel is $M, wanted 240x302 as without"
# The pixels: the open clock is highlighted, the row has text in it.
sleep 0.3
HL=$(sed -n 's/^target clock .* hl=\([0-9,]*\) .*/\1/p' "$DIR/j.out" | tail -n1)
RW=$(sed -n 's/^row kind=date rect=\([0-9,]*\) .*/\1/p' "$DIR/j.out" | tail -n1)
HLX=$(comp "$HL" 1); HLY=$(comp "$HL" 2); HLH=$(comp "$HL" 4)
"$CAPTURE" --at "$((HLX + 1)),$((HLY + HLH / 2))" >"$DIR/j.cap" 2>&1 || fail "J: capture failed"
RGB=$(sed -n 's/.*rgb=\(.*\)/\1/p' "$DIR/j.cap" | head -n1)
R=$((0x$(echo "$RGB" | cut -c1-2))); G=$((0x$(echo "$RGB" | cut -c3-4))); B=$((0x$(echo "$RGB" | cut -c5-6)))
# the ground of the highlight is #2b3a57 (43,58,87); RGB565 rounds each channel a little
[ "$R" -ge 36 ] && [ "$R" -le 50 ] && [ "$G" -ge 52 ] && [ "$G" -le 64 ] && [ "$B" -ge 80 ] && [ "$B" -le 94 ] ||
	fail "J: the open clock's highlight pixel is #$RGB, wanted about #2b3a57"
"$CAPTURE" --distinct "0,$(comp "$RW" 2),240,36" >"$DIR/j.cap" 2>&1 || fail "J: capture failed"
D=$(sed -n 's/.*distinct \([0-9]*\)/\1/p' "$DIR/j.cap")
[ "${D:-0}" -ge 5 ] || fail "J: the date row has $D colours: no anti-aliased text"
stop_panel J
# Closed, the same pixel is the bar's ground.
start_panel j
sleep 0.3
"$CAPTURE" --at "$((HLX + 1)),$((HLY + HLH / 2))" >"$DIR/j.cap" 2>&1 || fail "J: capture failed"
RGB=$(sed -n 's/.*rgb=\(.*\)/\1/p' "$DIR/j.cap" | head -n1)
R=$((0x$(echo "$RGB" | cut -c1-2)))
[ "$R" -lt 36 ] || fail "J: the closed clock has a highlight (#$RGB)"
stop_panel J
stop_picowl
echo "panel-e2e: J ok"

# ---- K: the battery opens the estimate row ----
start_picowl
echo 600 >"$BL/brightness"
printf 'volume 8\nswitch 0\n' >"$CTL"
K=$DIR/sys-k
mkdir -p "$K/class/backlight/bl0"
echo 1023 >"$K/class/backlight/bl0/max_brightness"
echo 600 >"$K/class/backlight/bl0/brightness"
echo raw >"$K/class/backlight/bl0/type"
KB=$K/class/power_supply/ds2760-battery.0
KAC=$K/class/power_supply/ac
# mk_ds2760 STATUS CAPACITY CURRENT CHARGE_NOW [TTE]: the attributes of the
# DS2760 driver and no others (no current_avg, no time_to_full, no energy_*).
# CURRENT is in uA and signed, negative while discharging, or "none".
mk_ds2760() {
	rm -rf "$KB"
	mkdir -p "$KB"
	echo Battery >"$KB/type"
	echo "$1" >"$KB/status"
	echo "$2" >"$KB/capacity"
	echo 3900000 >"$KB/voltage_now"
	echo 250 >"$KB/temp"
	echo 1100000 >"$KB/charge_full_design"
	echo 1000000 >"$KB/charge_full"
	echo 50000 >"$KB/charge_empty"
	echo "$4" >"$KB/charge_now"
	[ "$3" = none ] || echo "$3" >"$KB/current_now"
	[ -n "$5" ] && echo "$5" >"$KB/time_to_empty_now"
	return 0
}
# mk_ac ONLINE: a mains supply, or none
mk_ac() {
	rm -rf "$KAC"
	[ "$1" = none ] && return 0
	mkdir -p "$KAC"
	echo Mains >"$KAC/type"
	echo "$1" >"$KAC/online"
}
# row_line FILE: the last text of the open battery row in a dump
est_text() { sed -n 's/^row kind=estimate .* text="\(.*\)"$/\1/p' "$1" | tail -n1; }
# k_dump NAME WANT: an immediate dump with the row open has this text
k_dump() {
	export PICOWL_SYSFS_ROOT=$K
	dump k.out --inject "ibat"
	export PICOWL_SYSFS_ROOT=$SYS
	T=$(est_text "$DIR/k.out")
	[ "$T" = "$2" ] || fail "K $1: the row says '$T', wanted '$2'"
	echo "panel-e2e: K $1: '$T'"
}
# k_warm NAME WANT: after the filter has warmed up (a sample per second, four
# of them), with the stylus held in the row so that it stays open
k_warm() {
	export PICOWL_SYSFS_ROOT=$K PICOWL_PANEL_BATTERY_POLL_S=1
	start_panel k --inject "ibat;p$X,$Y"
	i=0
	while [ "$(est_text "$DIR/k.out")" != "$2" ]; do
		i=$((i + 1)); [ $i -gt 200 ] && fail "K $1: the row says '$(est_text "$DIR/k.out")', wanted '$2' after 10 s"
		sleep 0.05
	done
	echo "panel-e2e: K $1: '$2' after warm-up"
	KW=$(grep -c . "$DIR/k.out")
	stop_panel K
	unset PICOWL_SYSFS_ROOT PICOWL_PANEL_BATTERY_POLL_S
	export PICOWL_SYSFS_ROOT=$SYS
}

# Discharging at 600 mA with 950000 uAh in the counter, of which 50000 uAh are
# the reserve below charge_empty: (950000 - 50000) uAh / 600000 uA = 1.5 h =
# 90 min, rounded to 5 min (it is between 10 min and 5 h) is 90: 1 h 30 min.
# The kernel's time_to_empty_now (5400 s, in seconds) is the hint until four
# samples are in.
mk_ac 0
mk_ds2760 Discharging 94 -600000 950000 5400
k_dump "cold start: the kernel's time as a hint" "94%  ~1 h 30 min left"
has "$DIR/k.out" '^panel width=240 height=54 bar=18 row=36 format=ARGB8888 anchor=top popup=battery$' "K open: the battery opens the surface of bar and row"
has "$DIR/k.out" '^surface exclusive=18 input=0,0,240,54$' "K open: the exclusive zone is the bar"
has "$DIR/k.out" '^estimate kind=left minutes=90 hint=1 ' "K the hint is the kernel's 5400 s as 90 min"
k_warm "discharging, warm" "94%  1 h 30 min left"
# A different current: 900000 uAh at 1.2 A is 45 min (rounded to 5: 45).
mk_ds2760 Discharging 94 -1200000 950000 5400
k_warm "discharging at 1.2 A" "94%  45 min left"
# Over 5 h the rounding is 15 min: 950000 - 50000 = 900000 uAh at 150 mA is 6 h.
mk_ds2760 Discharging 94 -150000 950000
k_warm "discharging at 150 mA, 6 h" "94%  6 h 00 min left"
# Without time_to_empty_now and before warm-up there is nothing to show.
mk_ds2760 Discharging 94 -600000 950000
k_dump "no hint without time_to_empty_now" "94%  Estimating..."
# No current attribute at all, or an idle one: no fake numbers, ever.
mk_ds2760 Discharging 94 none 950000
k_warm "no current_now" "94%  Estimating..."
mk_ds2760 Discharging 94 -2000 950000
k_warm "idle, 2 mA" "94%  Estimating..."
mk_ds2760 Discharging 94 none 950000 5400
k_warm "no current_now, the kernel's time as the only source" "94%  ~1 h 30 min left"
# Charging: the charger is online and the current is positive. 1000000 -
# 500000 = 500000 uAh to go at 400 mA is 75 min: 1 h 15 min.
mk_ac 1
mk_ds2760 Charging 47 400000 500000
k_warm "charging" "47%  1 h 15 min to full"
mk_ds2760 Charging 95 400000 950000
k_dump "charging above the taper" "95%  Charging"
mk_ds2760 Full 100 -1000 1000000
k_dump "full" "100%  Fully charged"
mk_ds2760 "Not charging" 80 -100000 800000
k_dump "not charging, by the status" "80%  Not charging"
# The status lags the current by up to a minute in the driver: a charger that is
# online and a battery that gives current is not charging, whatever it says.
mk_ds2760 Charging 80 -100000 800000
k_warm "not charging, by the current" "80%  Not charging"
# On the charger the sign of the current is what decides, not the status.
mk_ds2760 Discharging 47 400000 500000
k_warm "charging, by the current" "47%  1 h 15 min to full"
# No battery: mains alone, or nothing.
rm -rf "$KB"
k_dump "mains and no battery" "On AC power"
mk_ac none
k_dump "no battery and no mains" "--"
# A generic battery: current_avg is the rate (300 mA, not the 900 mA that
# current_now says) and there is no charge_empty to subtract: 900000 uAh is 3 h.
mk_ac 0
rm -rf "$KB"
mkdir -p "$KB"
echo Battery >"$KB/type"; echo Discharging >"$KB/status"; echo 90 >"$KB/capacity"
echo -900000 >"$KB/current_now"; echo -300000 >"$KB/current_avg"; echo 900000 >"$KB/charge_now"
echo 1000000 >"$KB/charge_full"
k_warm "generic battery, current_avg" "90%  3 h 00 min left"
# power_now and energy_now: 6 Wh at 2 W is 3 h.
rm -rf "$KB"
mkdir -p "$KB"
echo Battery >"$KB/type"; echo Discharging >"$KB/status"; echo 60 >"$KB/capacity"
echo 2000000 >"$KB/power_now"; echo 6000000 >"$KB/energy_now"; echo 10000000 >"$KB/energy_full"
k_warm "generic battery, power_now and energy_now" "60%  3 h 00 min left"
# Only a capacity and the user's --battery-mah: 80 percent of 1000 mAh at 400 mA is 2 h.
rm -rf "$KB"
mkdir -p "$KB"
echo Battery >"$KB/type"; echo Discharging >"$KB/status"; echo 80 >"$KB/capacity"; echo -400000 >"$KB/current_now"
k_warm "capacity only, no battery size" "80%  Estimating..."
export PICOWL_SYSFS_ROOT=$K PICOWL_PANEL_BATTERY_POLL_S=1
start_panel k --battery-mah 1000 --inject "ibat;p$X,$Y"
wait_for "$DIR/k.out" '^row kind=estimate .* text="80%  2 h 00 min left"$' 10 "K --battery-mah"
stop_panel K
unset PICOWL_SYSFS_ROOT PICOWL_PANEL_BATTERY_POLL_S
export PICOWL_SYSFS_ROOT=$SYS
echo "panel-e2e: K --battery-mah 1000: 2 h 00 min"
"$PANEL" --battery-mah x >"$DIR/k.out" 2>"$DIR/k.err"
[ $? -eq 2 ] || fail "K: a bad --battery-mah does not exit 2"
"$PANEL" --help | grep -q -e --battery-mah || fail "K: --help does not list --battery-mah"

# The row is a text row: a tap sets nothing and the layout is the shared one.
mk_ac 0
mk_ds2760 Discharging 94 -600000 950000 5400
export PICOWL_SYSFS_ROOT=$K
dump k.out --inject "ibat"
has "$DIR/k.out" '^row kind=estimate rect=0,18,240,36 ' "K the estimate row is the row"
W=$(sed -n 's/^row kind=estimate .* width=\([0-9]*\) avail=\([0-9]*\) .*/\1 \2/p' "$DIR/k.out")
[ "${W% *}" -gt 0 ] && [ "${W% *}" -le "${W#* }" ] || fail "K: the estimate's text is $W px wide: it does not fit"
has "$DIR/k.out" '^target battery rect=[0-9]*,0,[0-9]*,18 hl=[0-9]*,[0-9]*,[0-9]*,[0-9]* open=1$' "K the battery target is the whole bar high and open"
BB=$(sed -n 's/^target battery rect=\([0-9,]*\) .*/\1/p' "$DIR/k.out")
[ "$(comp "$BB" 3)" -ge 36 ] || fail "K: the battery button is only $(comp "$BB" 3) px wide"
[ $(( $(comp "$BB" 1) + $(comp "$BB" 3) )) -eq 240 ] || fail "K: the battery button does not reach the edge: $BB"
[ "$(cat "$K/class/backlight/bl0/brightness")" = 600 ] || fail "K: a tap on the battery wrote the brightness"
grep -q '^volume 8$' "$CTL" && grep -q '^switch 0$' "$CTL" || fail "K: a tap on the battery reached the mixer"
dump k.out --inject "ibat;ibat"
has "$DIR/k.out" '^panel width=240 height=18 .*popup=none$' "K the same tap closes the row"
dump k.out --inject "ibat;icl"
has "$DIR/k.out" 'popup=clock$' "K the clock switches from the battery"
dump k.out --inject "icl;ibat"
has "$DIR/k.out" 'popup=battery$' "K the battery switches from the clock"
has "$DIR/k.out" '^row kind=estimate ' "K to the estimate"
dump k.out --inject "ibl;ibat;ivol"
has "$DIR/k.out" 'popup=volume$' "K the speaker switches from the battery"
dump k.out --inject "ibat;p$X,$Y;m$((X + 30)),$Y;r"
has "$DIR/k.out" 'popup=battery$' "K a press and a drag in the row keep it open"
[ "$(cat "$K/class/backlight/bl0/brightness")" = 600 ] || fail "K: a press in the row wrote the brightness"
dump k.out --inject "ibat;w2500"
has "$DIR/k.out" 'popup=battery$' "K still open after 2.5 s"
dump k.out --inject "ibat;w2500;w800"
has "$DIR/k.out" '^panel width=240 height=18 .*popup=none$' "K closed after 3.3 s"
dump k.out --inject "ibat;w2000;p$X,$Y;r;w2000"
has "$DIR/k.out" 'popup=battery$' "K a press in the row 2 s in keeps it open at 4 s"
dump k.out --inject "ibat;w2000;p$X,$Y;r;w2000;w1300"
has "$DIR/k.out" 'popup=none$' "K and it closes 3 s after that press"
# The usable area of a second client does not change with the row open.
start_panel k --inject "ibat;p$X,$Y"
has "$DIR/k.out" 'popup=battery$' "K the row is open"
M=$(mapped)
[ "$M" = 240x302 ] || fail "K: with the estimate row open a toplevel is $M, wanted 240x302 as without"
sleep 0.3
HL=$(sed -n 's/^target battery .* hl=\([0-9,]*\) .*/\1/p' "$DIR/k.out" | tail -n1)
HLX=$(comp "$HL" 1); HLY=$(comp "$HL" 2); HLH=$(comp "$HL" 4)
"$CAPTURE" --at "$((HLX + 1)),$((HLY + HLH / 2))" >"$DIR/k.cap" 2>&1 || fail "K: capture failed"
RGB=$(sed -n 's/.*rgb=\(.*\)/\1/p' "$DIR/k.cap" | head -n1)
R=$((0x$(echo "$RGB" | cut -c1-2))); G=$((0x$(echo "$RGB" | cut -c3-4))); B=$((0x$(echo "$RGB" | cut -c5-6)))
[ "$R" -ge 36 ] && [ "$R" -le 50 ] && [ "$G" -ge 52 ] && [ "$G" -le 64 ] && [ "$B" -ge 80 ] && [ "$B" -le 94 ] ||
	fail "K: the open battery's highlight pixel is #$RGB, wanted about #2b3a57"
RW=$(sed -n 's/^row kind=estimate rect=\([0-9,]*\) .*/\1/p' "$DIR/k.out" | tail -n1)
"$CAPTURE" --distinct "0,$(comp "$RW" 2),240,36" >"$DIR/k.cap" 2>&1 || fail "K: capture failed"
D=$(sed -n 's/.*distinct \([0-9]*\)/\1/p' "$DIR/k.cap")
[ "${D:-0}" -ge 5 ] || fail "K: the estimate row has $D colours: no anti-aliased text"
stop_panel K
# A line that is too long for the largest size shrinks and still fits: at a bar
# text size of 20 px the rows start at 26 px.
dump k.out --font-size 20 --inject "ibat"
W=$(sed -n 's/^row kind=estimate .* width=\([0-9]*\) avail=\([0-9]*\) .*/\1 \2/p' "$DIR/k.out")
[ "${W% *}" -le "${W#* }" ] || fail "K: at font size 20 the estimate is $W px wide: it is clipped"
export PICOWL_SYSFS_ROOT=$SYS
stop_picowl
echo "panel-e2e: K ok"

# ---- U: the crisp style ----
# --style crisp draws nothing anti-aliased: over a bare desktop, over a window of
# one colour and over a patterned one, the pixels of the panel are exactly the
# flat colours of its theme and no others, and the 1 px lines of the icons are
# whole rows and columns. The smooth style over the same scene has hundreds of
# colours. The ground is opaque (--popup-alpha 255), so that the window cannot
# show through and the set of colours is exact.
start_picowl
echo 600 >"$BL/brightness"
printf 'volume 8\nswitch 0\n' >"$CTL"
# The colour of a theme colour once it has been through an RGB565 buffer, which
# is what an opaque panel draws in.
q565() {
	v=$((0x$1))
	r=$(( ((v >> 16) & 255) * 31 + 127 )); r=$((r / 255))
	g=$(( ((v >> 8) & 255) * 63 + 127 )); g=$((g / 255))
	b=$(( (v & 255) * 31 + 127 )); b=$((b / 255))
	printf '%02x%02x%02x\n' $(( (r << 3) | (r >> 2) )) $(( (g << 2) | (g >> 4) )) $(( (b << 3) | (b >> 2) ))
}
C_BG=1c1f24 C_LINE=363b44 C_ROW=252930 C_ROWLINE=3d434d C_FG=e8eaed C_ACCENT=4c8dff
C_HL=2b3a57 C_TRACK=454b55 C_THUMB=ffffff C_RING=aeb4be C_FILL=c9cdd3 C_EMPTY=30353c
C_LOW=e5484d C_CHARGING=3fb950
u_want() { for c in "$@"; do q565 "$c"; done | sort -u; }
u_got() { sed -n 's/^pw-capture-client: colour \([0-9a-f]*\) .*/\1/p' "$1" | sort -u; }
# u_bat PCT STATUS
u_bat() { echo "$1" >"$BAT/capacity"; echo "$2" >"$BAT/status"; }
# u_scene NAME HEIGHT PCT STATUS INJECT WINARGS COLOURS...: the panel's pixels
# (the top HEIGHT rows of the capture) are exactly these colours.
u_scene() {
	name=$1; uh=$2; pct=$3; st=$4; inj=$5; win=$6; shift 6
	u_bat "$pct" "$st"
	CLIENTPID=
	if [ -n "$win" ]; then
		"$CLIENT" $win --linger 30 >"$DIR/client.out" 2>&1 &
		CLIENTPID=$!
		wait_for "$DIR/client.out" 'mapped' 5 "U client"
	fi
	start_panel u --style crisp --crisp-font "$UFONT" --popup-alpha 255 --inject "$inj"
	[ "$uh" -gt 18 ] && wait_for "$DIR/u.out" '^row ' 5 "U $name row open"
	sleep 0.3
	"$CAPTURE" --palette "0,0,240,$uh" --dump "0,0,240,$uh" >"$DIR/u.cap" 2>&1 || fail "U $name: capture failed: $(head -c 300 "$DIR/u.cap")"
	stop_panel U
	[ -n "$CLIENTPID" ] && { kill "$CLIENTPID" 2>/dev/null; wait "$CLIENTPID" 2>/dev/null; CLIENTPID=; }
	u_want "$@" >"$DIR/u.want"
	u_got "$DIR/u.cap" >"$DIR/u.got"
	if ! cmp -s "$DIR/u.want" "$DIR/u.got"; then
		echo "U $name: wanted"; cat "$DIR/u.want"; echo "got"; cat "$DIR/u.got"
		fail "U $name: the colours of the panel are not exactly its palette (font: $UFONT, window: ${win:-none})"
	fi
	echo "panel-e2e: U $name (font: $UFONT, window: ${win:-none}): $(wc -l <"$DIR/u.got") colours, exactly the palette"
}
# One scene per kind of row, each over the three grounds.
for UFONT in fixed dejavu; do
for WIN in "" "--color 00ff00" "--pattern"; do
	u_scene "closed bar" 18 73 Discharging "w1" "$WIN" $C_BG $C_LINE $C_FG $C_EMPTY $C_FILL
	u_scene "backlight row" 54 73 Charging "ibl" "$WIN" $C_BG $C_LINE $C_FG $C_EMPTY $C_CHARGING \
		$C_HL $C_ACCENT $C_ROW $C_ROWLINE $C_TRACK $C_THUMB $C_RING
	u_scene "volume row" 54 10 Discharging "ivol" "$WIN" $C_BG $C_LINE $C_FG $C_EMPTY $C_LOW \
		$C_HL $C_ACCENT $C_ROW $C_ROWLINE $C_TRACK $C_THUMB $C_RING
	u_scene "date row" 54 73 Discharging "icl" "$WIN" $C_BG $C_LINE $C_FG $C_EMPTY $C_FILL \
		$C_HL $C_ROW $C_ROWLINE
	u_scene "battery row" 54 10 Charging "ibat" "$WIN" $C_BG $C_LINE $C_FG $C_EMPTY $C_LOW \
		$C_THUMB $C_HL $C_ROW $C_ROWLINE
done
done

# The same scene in the smooth style has intermediate colours all over.
u_bat 73 Charging
start_panel u --style smooth --subpixel none --popup-alpha 255 --inject "ibl"
wait_for "$DIR/u.out" '^row ' 5 "U smooth row open"
sleep 0.3
"$CAPTURE" --palette "0,0,240,54" >"$DIR/u.cap" 2>&1 || fail "U: capture failed"
SMOOTH_N=$(sed -n 's/.*palette \([0-9]*\)$/\1/p' "$DIR/u.cap")
stop_panel U
[ "${SMOOTH_N:-0}" -gt 30 ] || fail "U: the smooth style has only ${SMOOTH_N:-0} colours, the contrast with crisp is lost"
echo "panel-e2e: U the smooth style has $SMOOTH_N colours where the crisp one has 12"

# The 1 px lines of the battery are whole rows and columns: its outline is a
# 16x8 box of which the first and last rows and the first and last columns are
# set but for the corner pixels, with a nub of 2 columns and 4 rows.
u_bat 50 Discharging
start_panel u --style crisp --popup-alpha 255
sleep 0.3
BR=$(val "$DIR/u.out" battery rect)
BX=$(comp "$BR" 1); BW=$(comp "$BR" 3); BY=$(comp "$BR" 2)
IX=$((BX + BW - 8 - 21 - 4 - 16)); IY=$((BY + 4))
"$CAPTURE" --dump "$IX,$IY,16,8" >"$DIR/u.cap" 2>&1 || fail "U: capture failed"
stop_panel U
# bp X Y: the colour at x, y of the icon
bp() { awk -v y=$((IY + $2)) -v x=$(($1 + 1)) '$3 == y { print $(3 + x) }' "$DIR/u.cap"; }
FGQ=$(q565 $C_FG); BGQ=$(q565 $C_BG)
for x in 1 2 3 4 5 6 7 8 9 10 11 12; do
	[ "$(bp $x 0)" = "$FGQ" ] || fail "U: the battery's top row is not set at x=$x ($(bp $x 0), wanted $FGQ)"
	[ "$(bp $x 7)" = "$FGQ" ] || fail "U: the battery's bottom row is not set at x=$x"
done
for y in 1 2 3 4 5 6; do
	[ "$(bp 0 $y)" = "$FGQ" ] || fail "U: the battery's left column is not set at y=$y"
	[ "$(bp 13 $y)" = "$FGQ" ] || fail "U: the battery's right column is not set at y=$y"
done
for corner in "0 0" "13 0" "0 7" "13 7"; do
	[ "$(bp $corner)" = "$BGQ" ] || fail "U: the battery's corner $corner is not cut"
done
for y in 2 3 4 5; do
	[ "$(bp 14 $y)" = "$FGQ" ] && [ "$(bp 15 $y)" = "$FGQ" ] || fail "U: the battery's nub is not 2x4"
done
[ "$(bp 14 1)" = "$BGQ" ] && [ "$(bp 15 6)" = "$BGQ" ] || fail "U: the battery's nub is longer than 4 rows"
# 50 percent of 10 columns: 5 whole columns of fill, each as high as the interior.
FILLQ=$(q565 $C_FILL); EMPTYQ=$(q565 $C_EMPTY)
for x in 2 3 4 5 6; do
	for y in 2 3 4 5; do
		[ "$(bp $x $y)" = "$FILLQ" ] || fail "U: the fill is not whole at $x,$y"
	done
done
for x in 7 8 9 10 11; do
	for y in 2 3 4 5; do
		[ "$(bp $x $y)" = "$EMPTYQ" ] || fail "U: the empty part is not whole at $x,$y"
	done
done
echo "panel-e2e: U the battery's outline, nub and fill are whole pixels"

# What the style asks for: the pixel fonts, no subpixel text whatever the output
# advertises, --font and --font-size ignored without a word, and the odd thumb.
stop_picowl
h_picowl horizontal_rgb
dump u.out --style crisp --subpixel rgb --font /nonexistent/font.ttf --font-size 30 --inject "ibl"
has "$DIR/u.out" '^style crisp font=pixel size=13 small=13 ' "U the style line"
has "$DIR/u.out" '^text subpixel=none$' "U crisp text is never subpixel text"
[ ! -s "$DIR/u.out.err" ] || fail "U: --font and --font-size are ignored with a message: $(cat "$DIR/u.out.err")"
TH=$(val "$DIR/u.out" row thumb)
[ "$(comp "$TH" 3)" -eq 21 ] && [ "$(comp "$TH" 4)" -eq 21 ] || fail "U: the thumb is $TH, wanted 21 px"
has "$DIR/u.out" ' crisp_font=fixed$' "U the style line names the default pixel font"
dump u.out --style crisp --inject "icl"
has "$DIR/u.out" '^row kind=date .* size=20 ' "U the date is in the 20 px font"
# The DejaVu bitmaps: Bold 11 in the bar (a 12 px cell), the date in Bold 12 (13 px),
# and the font is not the one --font names.
dump u.out --style crisp --crisp-font dejavu --subpixel rgb --font /nonexistent/font.ttf --inject "icl"
has "$DIR/u.out" '^style crisp font=pixel size=12 small=12 .* crisp_font=dejavu$' "U the style line of the DejaVu font"
has "$DIR/u.out" '^text subpixel=none$' "U crisp DejaVu text is never subpixel text"
has "$DIR/u.out" '^row kind=date .* size=13 ' "U the date is in the 13 px cell of Bold 12"
[ ! -s "$DIR/u.out.err" ] || fail "U: --crisp-font dejavu printed a message: $(cat "$DIR/u.out.err")"
"$PANEL" --crisp-font comic --dump-state >"$DIR/u.out" 2>"$DIR/u.err"
[ $? -eq 2 ] || fail "U: --crisp-font comic is not an error"
has "$DIR/u.err" "is not fixed or dejavu" "U the message for a bad crisp font"
dump u.out --style smooth --crisp-font dejavu
has "$DIR/u.out" '^style font=' "U --crisp-font does nothing in the smooth style"
dump u.out --style smooth
has "$DIR/u.out" '^style font=' "U --style smooth is the default style"
"$PANEL" --style fancy --dump-state >"$DIR/u.out" 2>"$DIR/u.err"
[ $? -eq 2 ] || fail "U: --style fancy is not an error"
has "$DIR/u.err" "is not smooth or crisp" "U the message for a bad style"
stop_picowl
echo "panel-e2e: U ok"

# ---- V: the bar on the short side of a rotated output ----
# [output] * = 90 or 270 turns the 240x320 output into a 320x240 view; the bar
# is then a vertical strip on the edge that is the physical top of the device,
# drawn as in portrait and turned onto the strip by the buffer transform. The
# raw scanout (the capture is in the panel's own orientation) must show the
# same bar as at normal: the headless backend rotates in software, hardware
# rotation is not testable here.
export PICOWL_TEST_VIRTUAL_POINTER=1
# v_picowl ROT [SUBPIXEL]: picowl whose output is turned by ROT with software
# rotation, advertising SUBPIXEL if given
v_picowl() {
	cp "$DIR/picowl.base" "$DIR/picowl.ini"
	printf '\n[output]\n* = %s\n' "$1" >>"$DIR/picowl.ini"
	[ -n "$2" ] && printf 'subpixel = %s\n' "$2" >>"$DIR/picowl.ini"
	printf '\n[rotation]\n* = software\n' >>"$DIR/picowl.ini"
	start_picowl
}
# v_rows NAME HEIGHT: HEIGHT rows of the scanout from row $VY (the top), in
# $DIR/NAME.rows
VY=0
v_rows() {
	"$CAPTURE" --dump "0,$VY,240,$2" >"$DIR/$1.cap" 2>&1 || fail "V: capture failed: $(cat "$DIR/$1.cap")"
	grep '^pw-capture-client: row ' "$DIR/$1.cap" >"$DIR/$1.rows"
	[ "$(wc -l <"$DIR/$1.rows")" -eq "$2" ] || fail "V: the capture of $1 has $(wc -l <"$DIR/$1.rows") rows, wanted $2"
}
# v_shot NAME ROT HEIGHT SUBPIXEL PANEL-ARGS...: one run of picowl and the panel
v_shot() {
	vn=$1; vrot=$2; vh=$3; vsp=$4; shift 4
	v_picowl "$vrot" "$vsp"
	start_panel "$vn" "$@"
	sleep 0.4
	v_rows "$vn" "$vh"
	stop_panel "$vn"
	stop_picowl
}
# v_same WHAT HEIGHT SUBPIXEL PANEL-ARGS...: the top HEIGHT rows of the scanout
# at 90 and at 270 are the ones of the portrait bar, pixel for pixel. A minute
# that turns during the three runs makes the clocks differ: the runs are
# repeated.
v_same() {
	vwhat=$1; vh=$2; vsp=$3; shift 3
	vtry=0
	while :; do
		vtry=$((vtry + 1))
		v_shot vref normal "$vh" "$vsp" "$@"
		v_shot v90 90 "$vh" "$vsp" "$@"
		v_shot v270 270 "$vh" "$vsp" "$@"
		C0=$(val "$DIR/vref.out" clock text)
		[ "$(val "$DIR/v90.out" clock text)" = "$C0" ] && [ "$(val "$DIR/v270.out" clock text)" = "$C0" ] && break
		[ $vtry -ge 3 ] && fail "V $vwhat: the clock changed in each of three tries"
	done
	VCOLOURS=$(sed 's/^[^ ]* [^ ]* [^ ]* //' "$DIR/vref.rows" | tr ' ' '\n' | sort -u | wc -l)
	[ "$VCOLOURS" -ge 3 ] || fail "V $vwhat: the portrait bar has only $VCOLOURS colours, nothing to compare"
	for vt in 90 270; do
		VDIFF=$(diff "$DIR/vref.rows" "$DIR/v$vt.rows" | grep -c '^<')
		[ "$VDIFF" -eq 0 ] || fail "V $vwhat: at $vt $VDIFF of the $vh rows of the scanout differ from the portrait bar: $(diff "$DIR/vref.rows" "$DIR/v$vt.rows" | head -n 4 | cut -c1-150)"
	done
	echo "panel-e2e: V $vwhat: pixel-identical to portrait at 90 and 270 ($vh rows, $VCOLOURS colours, clock $C0)"
}
# The bar, the slider row (translucent, over the background) and the date row,
# grayscale text; then with the stripes the output advertises.
v_same "bar" 18 none --subpixel none
v_same "slider row" 54 none --subpixel none --inject "ibl"
v_same "date row" 54 none --subpixel none --inject "icl"
v_same "battery row" 54 none --subpixel none --inject "ibat"
v_same "bar with subpixel text" 18 horizontal_rgb --subpixel auto
v_same "date row with subpixel text" 54 horizontal_rgb --subpixel auto --inject "icl"
v_same "crisp bar" 18 none --style crisp
v_same "crisp slider row" 54 none --style crisp --inject "ibl"
# --bottom is the physical bottom: the last rows of the scanout.
VY=302
v_same "bottom bar" 18 none --subpixel none --bottom
VY=0

# Subpixel text in the strip: the panel's own stripes count, so the portrait
# rule holds and the clock has colour fringes in the scanout.
v_picowl 90 horizontal_rgb
start_panel v --subpixel auto
has "$DIR/v.out" '^text subpixel=rgb$' "V the strip draws subpixel text with the native stripes"
sleep 0.3
CR=$(val "$DIR/v.out" clock rect)
h_spread "$(comp "$CR" 1),$(comp "$CR" 2),$(comp "$CR" 3),$(comp "$CR" 4)"
[ "$HINK" -gt 30 ] || fail "V: the clock has $HINK ink pixels in the scanout"
awk "BEGIN { exit !($HSPREAD > 0.35) }" || fail "V: spread $HSPREAD: no colour fringes on the clock in the strip"
echo "panel-e2e: V subpixel text in the strip: spread $HSPREAD over $HINK pixels"
stop_panel V
stop_picowl

# Where the surface is: the edge, the transform, the sizes, the input region in
# surface coordinates, the exclusive zone and the usable area.
for rot in 90 270; do
	if [ $rot = 90 ]; then
		EDGE=right; TR=1; ANCH=11; SIDE=right
	else
		EDGE=left; TR=3; ANCH=7; SIDE=left
	fi
	v_picowl $rot
	start_panel v --subpixel none
	has "$DIR/v.out" "^placement edge=$EDGE transform=$TR surface=18x240 input=0,0,18,240\$" "V $rot placement"
	has "$DIR/v.out" '^panel width=240 height=18 bar=18 row=0 ' "V $rot the layout is the portrait one"
	has "$DIR/v.out" '^surface exclusive=18 input=0,0,240,18$' "V $rot the bar's own frame"
	M=$(mapped)
	[ "$M" = 302x240 ] || fail "V $rot: a toplevel beside the strip is $M, wanted 302x240 (the strip's width, not height, is taken)"
	stop_panel V
	# with the row open the surface grows by the row's height, the zone does not
	start_panel v --subpixel none --inject "ibl"
	has "$DIR/v.out" "^placement edge=$EDGE transform=$TR surface=54x240 input=0,0,54,240\$" "V $rot placement with the row open"
	has "$DIR/v.out" '^panel width=240 height=54 bar=18 row=36 ' "V $rot the row is in the buffer"
	M=$(mapped)
	[ "$M" = 302x240 ] || fail "V $rot: with the row open a toplevel is $M, wanted 302x240"
	stop_panel V
	stop_picowl
	# on the wire
	v_picowl $rot
	WAYLAND_DEBUG=1 "$PANEL" --dump-state --subpixel none --inject "ibl;ibl" >"$DIR/w.out" 2>"$DIR/w.err" || fail "V $rot: the panel failed with WAYLAND_DEBUG"
	grep -q 'protocol error' "$DIR/w.err" && fail "V $rot: protocol error"
	grep -q "set_anchor($ANCH)" "$DIR/w.err" || fail "V $rot: the anchor is not $EDGE|top|bottom ($ANCH)"
	[ "$(grep -c 'set_anchor' "$DIR/w.err")" = 1 ] || fail "V $rot: the anchor was sent more than once"
	[ "$(grep -c 'set_exclusive_zone(18)' "$DIR/w.err")" = 1 ] || fail "V $rot: the exclusive zone is not the bar's width, once"
	grep 'set_size' "$DIR/w.err" | sed 's/.*set_size/set_size/' | tr '\n' ' ' >"$DIR/w.sizes"
	[ "$(cat "$DIR/w.sizes")" = "set_size(18, 0) set_size(54, 0) set_size(18, 0) " ] || fail "V $rot: the sizes asked for: $(cat "$DIR/w.sizes")"
	grep -q "set_buffer_transform($TR)" "$DIR/w.err" || fail "V $rot: the buffer transform is not $TR"
	grep -q 'set_buffer_transform(0)' "$DIR/w.err" && fail "V $rot: a buffer was attached with the transform 0"
	grep 'create_buffer' "$DIR/w.err" | sed 's/.*create_buffer/create_buffer/; s/new id wl_buffer[#@][0-9]*, //' | tr '\n' ' ' >"$DIR/w.bufs"
	grep -q '^create_buffer(0, 240, 18, 480, 909199186) create_buffer(0, 240, 54, 960, 0) create_buffer(0, 240, 18, 480, 909199186) $' "$DIR/w.bufs" ||
		fail "V $rot: the buffers are in the panel's own orientation, 240 wide: $(cat "$DIR/w.bufs")"
	stop_picowl
done
echo "panel-e2e: V placement, exclusive zone, transform and usable area ok"

# --edge top: the old place, at any rotation: along the long edge of the
# rotated view, which is a column of the scanout.
v_picowl 90
start_panel v --edge top --subpixel none
has "$DIR/v.out" '^panel width=320 height=18 bar=18 ' "V --edge top: laid out for the long edge"
has "$DIR/v.out" '^placement edge=top transform=0 surface=320x18 ' "V --edge top: on the top of the view"
M=$(mapped)
[ "$M" = 320x222 ] || fail "V --edge top: a toplevel is $M, wanted 320x222"
sleep 0.3
"$CAPTURE" --palette 18,0,222,18 >"$DIR/v.cap" 2>&1 || fail "V: capture failed"
has "$DIR/v.cap" 'palette 1$' "V --edge top: nothing of the bar in the physical top rows beyond the first 18 columns"
"$CAPTURE" --palette 0,0,18,320 >"$DIR/v.cap" 2>&1 || fail "V: capture failed"
grep -q 'palette 1$' "$DIR/v.cap" && fail "V --edge top: the bar is not on the first 18 columns"
stop_panel V
stop_picowl
"$PANEL" --edge sideways --dump-state >"$DIR/v.out" 2>"$DIR/v.err"
[ $? -eq 2 ] || fail "V: --edge sideways is not an error"
has "$DIR/v.err" "is not auto or top" "V the message for a bad edge"
"$PANEL" --help | grep -q -- '--edge MODE' || fail "V: --edge is not in the usage text"
# the other transforms keep the bar on the top: 180, and flipped ones
for rot in 180 flipped flipped-90; do
	v_picowl $rot
	start_panel v --subpixel none
	has "$DIR/v.out" '^placement edge=top transform=0 ' "V $rot: the bar stays on the top edge"
	stop_panel V
	stop_picowl
done
echo "panel-e2e: V --edge and the other transforms ok"

# A rotation while the panel runs: normal -> 90 -> 180 -> 270 -> normal, each
# time the edge, the buffer and the scanout follow.
v_picowl normal
start_panel v --subpixel none
sleep 0.3
v_rows vref 18
"$KEYS" 397 || fail "V: key client failed"
wait_for "$DIR/v.out" '^placement edge=right transform=1 surface=18x240 ' 5 "V rotating to 90"
sleep 0.4
v_rows vlive 18
C0=$(val "$DIR/v.out" clock text)
if [ "$(diff "$DIR/vref.rows" "$DIR/vlive.rows" | grep -c '^<')" -ne 0 ]; then
	# a minute that turned is the only excuse
	[ "$C0" != "$(date +%H:%M)" ] || fail "V: after the rotation to 90 the scanout differs from the portrait bar"
fi
M=$(mapped)
[ "$M" = 302x240 ] || fail "V: after the rotation to 90 a toplevel is $M, wanted 302x240"
"$KEYS" 397 || fail "V: key client failed"
wait_for "$DIR/v.out" '^placement edge=top transform=0 surface=240x18 ' 5 "V rotating to 180"
M=$(mapped)
[ "$M" = 240x302 ] || fail "V: at 180 a toplevel is $M, wanted 240x302"
"$KEYS" 397 || fail "V: key client failed"
wait_for "$DIR/v.out" '^placement edge=left transform=3 surface=18x240 ' 5 "V rotating to 270"
sleep 0.4
v_rows vlive 18
if [ "$(diff "$DIR/vref.rows" "$DIR/vlive.rows" | grep -c '^<')" -ne 0 ]; then
	[ "$(val "$DIR/v.out" clock text)" != "$(date +%H:%M)" ] || fail "V: after the rotation to 270 the scanout differs from the portrait bar"
fi
M=$(mapped)
[ "$M" = 302x240 ] || fail "V: after the rotation to 270 a toplevel is $M, wanted 302x240"
"$KEYS" 397 || fail "V: key client failed"
wait_for "$DIR/v.out" '^placement edge=top transform=0 surface=240x18 .*' 5 "V rotating back to normal"
[ "$(grep -c '^placement edge=top transform=0 ' "$DIR/v.out")" -ge 3 ] || fail "V: no return to the top edge"
stop_panel V
stop_picowl
echo "panel-e2e: V rotation while the panel runs ok"

# Touch: the pointer is put where the physical bar is, in the logical view
# (the scanout turned back: at 90 the physical top is the right edge, at 270
# the left one), and everything works as in portrait.
# v_at ROT PX PY: the position in the 320x240 view of the point PX,PY of the
# 240x320 scanout
v_at() {
	if [ "$1" = 90 ]; then echo "$((319 - $3)),$2"; else echo "$3,$((239 - $2))"; fi
}
# v_popup FILE: the row the panel last said is open
v_popup() { grep '^panel ' "$1" | tail -n 1 | sed 's/.* popup=//'; }
# v_wait_popup FILE POPUP SECONDS WHAT: wait until the panel's last state has
# POPUP open (an earlier line does not count)
v_wait_popup() {
	i=0
	while [ "$(v_popup "$1")" != "$2" ]; do
		i=$((i + 1)); [ $i -gt $(($3 * 20)) ] && fail "$4: the row is '$(v_popup "$1")', wanted '$2'"
		sleep 0.05
	done
}
[ -x "$POINTER" ] || fail "V: no pointer client ($POINTER)"
for rot in 90 270; do
	v_picowl $rot
	echo 600 >"$BL/brightness"
	start_panel v --subpixel none
	CR=$(val "$DIR/v.out" clock rect)
	PX=$(( $(comp "$CR" 1) + $(comp "$CR" 3) / 2 ))
	"$POINTER" --size 320x240 tap "$(v_at $rot $PX 8)" || fail "V $rot: the pointer client failed"
	wait_for "$DIR/v.out" '^panel .* popup=clock$' 3 "V $rot: a tap on the physical clock opens the date row"
	has "$DIR/v.out" '^placement edge=.* surface=54x240 ' "V $rot: the surface grew by the row"
	# 3 s after the touch: open at 2 s, closed by 3.6 s
	sleep 2
	[ "$(v_popup "$DIR/v.out")" = clock ] || fail "V $rot: the date row closed before its 3 s"
	v_wait_popup "$DIR/v.out" none 3 "V $rot: the date row closes by itself"
	# The surface shrinks after that, and at 90 the bar's place in it moves with
	# its thickness (the row grows inward from the edge): let it settle.
	sleep 0.5
	# a tap on the sun opens the slider row, a press on the track and a drag set the value
	BB=$(val "$DIR/v.out" backlight button)
	"$POINTER" --size 320x240 tap "$(v_at $rot $(( $(comp "$BB" 1) + 10 )) 8)" || fail "V $rot: the pointer client failed"
	wait_for "$DIR/v.out" '^row slider=backlight ' 3 "V $rot: a tap on the physical backlight button opens its row"
	TRACK=$(val "$DIR/v.out" row track)
	TX=$(comp "$TRACK" 1); TW=$(comp "$TRACK" 3)
	[ "$(cat "$BL/brightness")" = 600 ] || fail "V $rot: opening the row set the brightness"
	# One process for the whole drag: a virtual pointer that goes away with
	# its button down leaves the press behind.
	"$POINTER" --size 320x240 move "$(v_at $rot $((TX + TW / 4)) 36)" down wait 300 \
		move "$(v_at $rot $((TX + TW * 3 / 4)) 36)" wait 300 up || fail "V $rot: the pointer client failed"
	has "$DIR/v.out" '^backlight available=1 value=2[0-9] ' "V $rot: a press at a quarter of the track sets about 25 percent"
	has "$DIR/v.out" '^backlight available=1 value=7[0-9] ' "V $rot: dragging to three quarters sets about 75 percent"
	RAW=$(cat "$BL/brightness")
	[ "$RAW" -gt 700 ] && [ "$RAW" -lt 900 ] || fail "V $rot: the drag left the brightness at $RAW, wanted about 75 percent of 1023"
	# a press near the start of the track, then out of the panel's surface into
	# the window area: the touch ends there and does not stay down, which would
	# keep the row open for good
	"$POINTER" --size 320x240 move "$(v_at $rot $((TX + TW / 10)) 36)" down wait 200 \
		move "$(v_at $rot $((TX + TW / 10)) 150)" wait 200 up || fail "V $rot: the pointer client failed"
	RAW2=$(cat "$BL/brightness")
	[ "$RAW2" -lt 300 ] || fail "V $rot: a press near the start of the track left $RAW2, wanted about 10 percent"
	# the row closes 3 s after the last touch
	v_wait_popup "$DIR/v.out" none 5 "V $rot: the slider row closes by itself"
	# a tap in the window area, away from the strip, does nothing to the panel
	N=$(grep -c '^redraw ' "$DIR/v.out")
	"$POINTER" --size 320x240 tap 150,120 || fail "V $rot: the pointer client failed"
	sleep 0.3
	[ "$(grep -c '^redraw ' "$DIR/v.out")" = "$N" ] || fail "V $rot: a tap away from the strip redrew the panel"
	stop_panel V
	stop_picowl
	echo "panel-e2e: V $rot: taps, the slider drag and the 3 s auto-close work on the strip"
done
unset PICOWL_TEST_VIRTUAL_POINTER
cp "$DIR/picowl.base" "$DIR/picowl.ini"
echo "panel-e2e: V ok"

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
	# Midnight with the date row open (the stylus is held down, so it stays
	# open): 8 s before the day changes, the row must show the next day after
	# the minute tick, with no other timer. A virtual machine whose clock is
	# set from its host in the meantime makes the run void, not failed: it is
	# tried again.
	export TZ=UTC0
	start_picowl
	TRY=0
	DONE=
	while [ -z "$DONE" ]; do
		TRY=$((TRY + 1))
		[ $TRY -gt 3 ] && fail "T: the system clock was reset under the midnight test three times"
		CLOCK_T0=$(date +%s)
		CLOCK_U0=$(uptime_s)
		MID=$(( (CLOCK_T0 / 86400 + 1) * 86400 ))
		BEFORE=$(date -d "@$((MID - 8))" '+%A %-d %B %Y')
		AFTER=$(date -d "@$MID" '+%A %-d %B %Y')
		date -s "@$((MID - 8))" >/dev/null || { CLOCK_T0=; fail "T: cannot set the clock"; }
		start_panel t --inject "icl;p$X,$Y"
		has "$DIR/t.out" "^row kind=date .* text=\"$BEFORE\"\$" "T the date row before midnight"
		i=0
		while ! grep -q "^row kind=date .* text=\"$AFTER\"\$" "$DIR/t.out"; do
			i=$((i + 1))
			if [ $i -gt 400 ] || [ "$(date +%s)" -lt $((MID - 30)) ]; then
				[ "$(date +%s)" -lt $((MID - 30)) ] && break
				fail "T the date row after midnight: no '$AFTER' in $DIR/t.out"
			fi
			sleep 0.05
		done
		[ $i -le 400 ] && [ "$(date +%s)" -ge $((MID - 30)) ] && DONE=1
		stop_panel T
		restore_clock
	done
	stop_picowl
	unset TZ
	echo "panel-e2e: midnight ok"
fi

cleanup
echo "panel-e2e: ok"
