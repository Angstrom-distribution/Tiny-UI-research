#!/bin/sh
# Headless RSS check: run picowl + test client to 'mapped', then read the
# compositor's VmRSS/VmHWM before it is terminated.
# usage: rss.sh PICOWL PW_TEST_CLIENT   (env PW_RSS_CEILING_KB, default 12288)
PICOWL=$1
CLIENT=$2
CEIL=${PW_RSS_CEILING_KB:-12288}
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export WAYLAND_DISPLAY=wayland-rss
unset DISPLAY

PID=
cleanup() {
	[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
	rm -rf "$DIR"
}
trap cleanup EXIT INT TERM
fail() { echo "rss: FAIL: $*"; [ -f "$DIR/picowl.log" ] && cat "$DIR/picowl.log"; exit 1; }

"$PICOWL" -d 2 >"$DIR/picowl.log" 2>&1 &
PID=$!

i=0
while ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
	kill -0 "$PID" 2>/dev/null || fail "picowl exited early"
	i=$((i + 1)); [ $i -gt 100 ] && fail "socket never appeared"
	sleep 0.05
done
SOCK=$(ls "$DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)
export WAYLAND_DISPLAY=$(basename "$SOCK")

OUT=$("$CLIENT" 2>&1) || fail "client failed: $OUT"
case "$OUT" in *mapped*) ;; *) fail "client did not report mapped" ;; esac

RSS=$(awk '/^VmRSS:/ {print $2}' "/proc/$PID/status")
HWM=$(awk '/^VmHWM:/ {print $2}' "/proc/$PID/status")
[ -n "$RSS" ] && [ -n "$HWM" ] || fail "cannot read /proc/$PID/status"
echo "rss: VmRSS=$RSS kB VmHWM=$HWM kB ceiling=$CEIL kB"

kill -TERM "$PID" 2>/dev/null
if [ "$HWM" -gt "$CEIL" ]; then
	echo "rss: FAIL VmHWM $HWM > $CEIL"
	exit 1
fi
echo "rss: ok"
exit 0
