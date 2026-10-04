#!/bin/sh
# End-to-end dimming test: headless picowl against a fake sysfs tree
# (PICOWL_SYSFS_ROOT). Checks that the real event loop dims the backlight
# on the AC profile and applies the LOW-profile brightness cap at startup.
# With a test client it also checks idle inhibitors (cases 4-6).
# usage: power-e2e.sh PICOWL [PW_TEST_CLIENT]
PICOWL=$1
CLIENT=$2
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
unset DISPLAY WAYLAND_DISPLAY
PID=
CPIDS=
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; [ -n "$CPIDS" ] && kill -9 $CPIDS 2>/dev/null; rm -rf "$DIR"; }
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

# 3b. A level set by another process (the panel slider) while ACTIVE becomes
# the user level: the dim is 30 % of it and the clean exit restores it.
mktree "$DIR/sys-ext" ac
bl=$DIR/sys-ext/class/backlight/test-bl
rm "$bl/actual_brightness"; ln -s brightness "$bl/actual_brightness"
PICOWL_SYSFS_ROOT=$DIR/sys-ext "$PICOWL" -c "$DIR/picowl.ini" -d 2 >"$DIR/picowl.log" 2>&1 &
PID=$!
sleep 0.3
echo 60 >"$bl/brightness"
wait_level "$bl/brightness" 18 30
kill -TERM "$PID"; wait "$PID"; PID=
[ "$(cat "$bl/brightness")" = 60 ] || fail "external level lost: $(cat "$bl/brightness"), expected 60"
echo "power-e2e: external level 60 dimmed to 18 and restored"

# Idle inhibitor cases need the test client.
if [ -z "$CLIENT" ]; then
	echo "power-e2e: no test client, inhibitor cases skipped"
	cleanup
	exit 0
fi

cat >"$DIR/inhibit.ini" <<INI
[power]
backlight = test-bl
dim_level = 30
low_capacity = 15
poll_s = 0
[power.ac]
dim_after_s = 2
blank_after_s = 0
[power.battery]
dim_after_s = 0
blank_after_s = 0
[power.low]
dim_after_s = 2
blank_after_s = 0
max_brightness_pct = 100
inhibit = no
INI

# start_picowl TREE: picowl on the inhibit.ini config, waits for the socket.
start_picowl() {
	rm -f "$DIR"/wayland-0 "$DIR"/wayland-0.lock
	PICOWL_SYSFS_ROOT=$1 "$PICOWL" -c "$DIR/inhibit.ini" -d 2 >"$DIR/picowl.log" 2>&1 &
	PID=$!
	i=0
	while [ ! -S "$DIR/wayland-0" ]; do
		kill -0 "$PID" 2>/dev/null || fail "picowl exited early"
		i=$((i + 1)); [ $i -gt 100 ] && fail "socket never appeared"
		sleep 0.05
	done
	export WAYLAND_DISPLAY=wayland-0
}

# start_client NAME ARGS...: background test client, waits until it is mapped.
# The pid is left in $CPID.
start_client() {
	n=$1; shift
	"$CLIENT" "$@" >"$DIR/$n.out" 2>&1 &
	CPID=$!; CPIDS="$CPIDS $CPID"
	i=0
	while ! grep -q mapped "$DIR/$n.out"; do
		kill -0 "$CPID" 2>/dev/null || fail "client $n failed: $(cat "$DIR/$n.out")"
		i=$((i + 1)); [ $i -gt 100 ] && fail "client $n never mapped"
		sleep 0.05
	done
}

# Clean exit with inhibitors still alive (wlroots asserts on leftovers).
stop_picowl() {
	kill -TERM "$PID"
	wait "$PID" || fail "picowl exit status $?"
	PID=; unset WAYLAND_DISPLAY
}

# 4. A visible inhibitor holds the dim timer (2 s); on release the timers
# restart from the release, not from the last input.
mktree "$DIR/sys-inh" ac
bl=$DIR/sys-inh/class/backlight/test-bl/brightness
start_picowl "$DIR/sys-inh"
start_client inh --inhibit --linger 4
sleep 3
[ "$(cat "$bl")" = 40 ] || fail "dimmed despite a visible inhibitor"
wait "$CPID"
sleep 1
[ "$(cat "$bl")" = 40 ] || fail "dimmed right after the inhibitor went away"
wait_level "$bl" 12 50
grep -q "idle inhibit on (app_id picowl-test-client)" "$DIR/picowl.log" || fail "no inhibit-on log"
grep -q "idle inhibit off" "$DIR/picowl.log" || fail "no inhibit-off log"
stop_picowl
echo "power-e2e: inhibitor held dimming, timers restarted on release"

# 5. An inhibitor that is not visible does not count: client B takes focus.
mktree "$DIR/sys-inh2" ac
bl=$DIR/sys-inh2/class/backlight/test-bl/brightness
start_picowl "$DIR/sys-inh2"
start_client a --inhibit --linger 10
APID=$CPID
sleep 3
[ "$(cat "$bl")" = 40 ] || fail "dimmed while A was visible"
start_client b --linger 8
wait_level "$bl" 12 50
kill -0 "$APID" 2>/dev/null || fail "client A exited early"
stop_picowl
echo "power-e2e: inhibitor behind the focused window dimmed"

# 6. [power.low] inhibit = no: the inhibitor is ignored.
mktree "$DIR/sys-inh3" low
bl=$DIR/sys-inh3/class/backlight/test-bl/brightness
start_picowl "$DIR/sys-inh3"
start_client c --inhibit --linger 6
wait_level "$bl" 12 50
stop_picowl
echo "power-e2e: LOW profile with inhibit = no dimmed despite an inhibitor"
cleanup
