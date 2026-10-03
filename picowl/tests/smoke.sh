#!/bin/sh
# Headless smoke test: picowl + wl_shm xdg-shell client.
# usage: smoke.sh PICOWL PW_TEST_CLIENT
PICOWL=$1
CLIENT=$2
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export WAYLAND_DISPLAY=wayland-smoke
unset DISPLAY

"$PICOWL" -d 2 >"$DIR/picowl.log" 2>&1 &
PID=$!
cleanup() { kill -9 "$PID" 2>/dev/null; rm -rf "$DIR"; }
fail() { echo "smoke: FAIL: $*"; cat "$DIR/picowl.log"; cleanup; exit 1; }

# Wait for the socket (up to 5 s).
i=0
while [ ! -S "$DIR/$WAYLAND_DISPLAY" ] && ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
	kill -0 "$PID" 2>/dev/null || fail "picowl exited early"
	i=$((i + 1)); [ $i -gt 100 ] && fail "socket never appeared"
	sleep 0.05
done
SOCK=$(ls "$DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)
export WAYLAND_DISPLAY=$(basename "$SOCK")

OUT=$("$CLIENT" 2>&1) || fail "client failed: $OUT"
echo "$OUT"
case "$OUT" in *mapped*) ;; *) fail "client did not report mapped" ;; esac

OUT=$("$CLIENT" --zerocopy 2>&1) || fail "zerocopy client failed: $OUT"
echo "$OUT"
case "$OUT" in *"zerocopy unavailable, using wl_shm"*) ;; *) fail "no zerocopy degradation message" ;; esac
case "$OUT" in *mapped*) ;; *) fail "zerocopy client did not report mapped" ;; esac
# Headless has no picowl-buffer global: the count request degrades the same way.
OUT=$("$CLIENT" --zerocopy-count 7 --app-id mediaplayer 2>&1) || fail "zerocopy-count client failed: $OUT"
echo "$OUT"
case "$OUT" in *"zerocopy unavailable, using wl_shm"*) ;; *) fail "no zerocopy-count degradation message" ;; esac
case "$OUT" in *mapped*) ;; *) fail "zerocopy-count client did not report mapped" ;; esac
# Likewise no global to probe, and a v2-capable client must not choke on that.
OUT=$("$CLIENT" --probe 2>&1) || fail "probe client failed: $OUT"
echo "$OUT"
case "$OUT" in *"probe bufmgr none"*) ;; *) fail "headless picowl advertises picowl-buffer" ;; esac
if grep -q 'Failed to upload buffer' "$DIR/picowl.log"; then
	fail "picowl log has 'Failed to upload buffer'"
fi

kill -TERM "$PID"
i=0
while kill -0 "$PID" 2>/dev/null; do
	i=$((i + 1)); [ $i -gt 40 ] && fail "picowl did not exit within 2 s"
	sleep 0.05
done
wait "$PID"; RC=$?
[ "$RC" -eq 0 ] || fail "picowl exit status $RC"
rm -rf "$DIR"
echo "smoke: ok"
