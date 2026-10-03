#!/bin/sh
# End-to-end dimming test: headless picowl against a fake sysfs tree
# (PICOWL_SYSFS_ROOT). Checks that the real event loop dims the backlight
# on the AC profile and applies the LOW-profile brightness cap at startup.
# usage: power-e2e.sh PICOWL
PICOWL=$1
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
unset DISPLAY WAYLAND_DISPLAY
PID=
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$DIR"; }
fail() { echo "power-e2e: FAIL: $*"; cat "$DIR/picowl.log" 2>/dev/null; cleanup; exit 1; }

mktree() { # $1 = root, $2 = supplies: ac | low
	bl=$1/class/backlight/test-bl
	mkdir -p "$bl"
	echo 64 >"$bl/max_brightness"; echo 40 >"$bl/actual_brightness"
	echo 40 >"$bl/brightness"; echo raw >"$bl/type"
	if [ "$2" = ac ]; then
		mkdir -p "$1/class/power_supply/ac"
		echo Mains >"$1/class/power_supply/ac/type"; echo 1 >"$1/class/power_supply/ac/online"
	else
		mkdir -p "$1/class/power_supply/battery"
		echo Battery >"$1/class/power_supply/battery/type"
		echo 5 >"$1/class/power_supply/battery/capacity"
		echo Discharging >"$1/class/power_supply/battery/status"
	fi
}

# wait_level FILE VALUE TIMEOUT_TENTHS
wait_level() {
	i=0
	while [ "$(cat "$1")" != "$2" ]; do
		kill -0 "$PID" 2>/dev/null || fail "picowl exited early"
		i=$((i + 1)); [ $i -gt "$3" ] && fail "brightness is $(cat "$1"), expected $2"
		sleep 0.1
	done
}

cat >"$DIR/picowl.ini" <<INI
[power]
backlight = test-bl
dim_level = 30
low_capacity = 15
poll_s = 0
[power.ac]
dim_after_s = 1
blank_after_s = 0
[power.battery]
dim_after_s = 0
blank_after_s = 0
[power.low]
dim_after_s = 0
blank_after_s = 0
max_brightness_pct = 40
INI

# 1. AC profile: 40 -> 30 % of the user level (12) after ~1 s.
mktree "$DIR/sys-ac" ac
PICOWL_SYSFS_ROOT=$DIR/sys-ac "$PICOWL" -c "$DIR/picowl.ini" -d 2 >"$DIR/picowl.log" 2>&1 &
PID=$!
sleep 0.5
[ "$(cat "$DIR/sys-ac/class/backlight/test-bl/brightness")" = 40 ] || fail "dimmed too early"
wait_level "$DIR/sys-ac/class/backlight/test-bl/brightness" 12 30
kill -TERM "$PID"; wait "$PID"; PID=
echo "power-e2e: AC profile dimmed 40 -> 12"

# 2. LOW profile: brightness capped to 40 % of max (25) at startup.
mktree "$DIR/sys-low" low
PICOWL_SYSFS_ROOT=$DIR/sys-low "$PICOWL" -c "$DIR/picowl.ini" -d 2 >"$DIR/picowl.log" 2>&1 &
PID=$!
wait_level "$DIR/sys-low/class/backlight/test-bl/brightness" 25 20
kill -TERM "$PID"; wait "$PID"; PID=
echo "power-e2e: LOW profile capped 40 -> 25"

# 3. Crash while dimmed: the restarted picowl must still know the user level
# (40), not take the dimmed 12 for it, and restore 40 on a clean exit.
mktree "$DIR/sys-crash" ac
bl=$DIR/sys-crash/class/backlight/test-bl
rm "$bl/actual_brightness"; ln -s brightness "$bl/actual_brightness"
PICOWL_SYSFS_ROOT=$DIR/sys-crash "$PICOWL" -c "$DIR/picowl.ini" -d 2 >"$DIR/picowl.log" 2>&1 &
PID=$!
wait_level "$bl/brightness" 12 30
kill -9 "$PID"; wait "$PID" 2>/dev/null; PID=
PICOWL_SYSFS_ROOT=$DIR/sys-crash "$PICOWL" -c "$DIR/picowl.ini" -d 2 >"$DIR/picowl.log" 2>&1 &
PID=$!
sleep 0.5
kill -TERM "$PID"; wait "$PID"; PID=
[ "$(cat "$bl/brightness")" = 40 ] || fail "user level lost after crash: $(cat "$bl/brightness"), expected 40"
echo "power-e2e: user level 40 survives a crash while dimmed"
cleanup
