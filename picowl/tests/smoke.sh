#!/bin/sh
# Headless smoke test: picowl + wl_shm xdg-shell client.
# usage: smoke.sh PICOWL PW_TEST_CLIENT PW_CAPTURE_CLIENT PW_IM_CLIENT PW_KEY_CLIENT
PICOWL=$1
CLIENT=$2
CAPTURE=$3
IMCLIENT=$4
KEYCLIENT=$5
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

OUT=$("$CLIENT" --inhibit 2>&1) || fail "inhibit client failed: $OUT"
case "$OUT" in *mapped*) ;; *) fail "inhibit client did not report mapped" ;; esac
sleep 0.2
grep -q 'idle inhibit on' "$DIR/picowl.log" || fail "no 'idle inhibit on' in picowl log"
grep -q 'idle inhibit off' "$DIR/picowl.log" || fail "no 'idle inhibit off' in picowl log"

# Headless has no DRM backend: no lease global, and a clean teardown of the
# NULL manager (wlroots asserts on a leftover request listener).
grep -q 'lease: no DRM backend, disabled' "$DIR/picowl.log" || fail "no 'lease: no DRM backend' in picowl log"
OUT=$("$CLIENT" --expect-no-global wp_drm_lease_device_v1 2>&1) || fail "lease global advertised: $OUT"

# Protocols that need no configuration are always advertised.
OUT=$("$CLIENT" --expect-global wp_content_type_manager_v1 2>&1) || fail "content-type global missing: $OUT"
OUT=$("$CLIENT" --expect-global zwp_primary_selection_device_manager_v1 2>&1) || fail "primary selection global missing: $OUT"
OUT=$("$CLIENT" --expect-global wp_cursor_shape_manager_v1 2>&1) || fail "cursor-shape global missing: $OUT"
OUT=$("$CLIENT" --expect-global zwp_tablet_manager_v2 2>&1) || fail "tablet global missing: $OUT"
OUT=$("$CLIENT" --expect-global zwp_text_input_manager_v3 2>&1) || fail "text-input global missing: $OUT"
OUT=$("$CLIENT" --expect-global zxdg_output_manager_v1 2>&1) || fail "xdg-output global missing: $OUT"
OUT=$("$CLIENT" --expect-global zwp_input_method_manager_v2 2>&1) || fail "input-method global missing: $OUT"
# The older path stays for GTK+2 clients and OSK function keys.
OUT=$("$CLIENT" --expect-global zwp_virtual_keyboard_manager_v1 2>&1) || fail "virtual-keyboard global missing: $OUT"

# Without any input device (WLR_LIBINPUT_NO_DEVICES) a client which binds
# wl_keyboard on the capability, as the media player does at startup, still
# gets a keymap; with none it ignores every key.
OUT=$("$CLIENT" --expect-keymap 2>&1) || fail "no keymap without a keyboard device: $OUT"
echo "$OUT"
# The seat's active keyboard goes away with the on-screen keyboard's virtual
# keyboard: a client created afterwards must still get a keymap.
OUT=$("$KEYCLIENT" 2>&1) || fail "virtual keyboard client failed: $OUT"
OUT=$("$CLIENT" --expect-keymap 2>&1) || fail "no keymap after the virtual keyboard was destroyed: $OUT"
echo "$OUT"

# text-input to input-method relay: activate, commit_string and deactivate.
OUT=$("$IMCLIENT" 2>&1) || fail "input method client failed: $OUT"
echo "$OUT"
case "$OUT" in *"pw-im-client: ok"*) ;; *) fail "input method client did not report ok" ;; esac

# [capture] is off by default: no screencopy global.
OUT=$("$CAPTURE" 2>&1) || fail "capture client failed: $OUT"
echo "$OUT"
case "$OUT" in *"no screencopy"*) ;; *) fail "default config advertises screencopy: $OUT" ;; esac

kill -TERM "$PID"
i=0
while kill -0 "$PID" 2>/dev/null; do
	i=$((i + 1)); [ $i -gt 40 ] && fail "picowl did not exit within 2 s"
	sleep 0.05
done
wait "$PID"; RC=$?
[ "$RC" -eq 0 ] || fail "picowl exit status $RC"

# Second run with [capture] enabled: the capture must be the background colour.
# XRGB8888 keeps the expected pixel value exact.
cat >"$DIR/capture.ini" <<EOF
[render]
format = XRGB8888

[capture]
enabled = true

[background]
color = #204060
EOF
rm -f "$DIR"/wayland-*
export WAYLAND_DISPLAY=wayland-smoke
"$PICOWL" -d 2 -c "$DIR/capture.ini" >"$DIR/picowl.log" 2>&1 &
PID=$!
i=0
while ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
	kill -0 "$PID" 2>/dev/null || fail "capture picowl exited early"
	i=$((i + 1)); [ $i -gt 100 ] && fail "capture socket never appeared"
	sleep 0.05
done
SOCK=$(ls "$DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)
export WAYLAND_DISPLAY=$(basename "$SOCK")
grep -q 'capture: zwlr_screencopy_manager_v1 enabled' "$DIR/picowl.log" || fail "no capture INFO line in picowl log"
OUT=$("$CAPTURE" 2>&1) || fail "capture client failed: $OUT"
echo "$OUT"
case "$OUT" in *captured*) ;; *) fail "capture client did not report captured" ;; esac
case "$OUT" in *"identical=yes"*) ;; *) fail "captured pixels are not uniform: $OUT" ;; esac
# wl_shm format 0 is ARGB8888, 1 is XRGB8888: the low 24 bits are the colour.
case "$OUT" in
*"format=0 "*|*"format=1 "*)
	case "$OUT" in *"pixel="??204060) ;; *) fail "captured pixel is not the background colour: $OUT" ;; esac ;;
*) fail "unexpected capture format, the colour check would be skipped: $OUT" ;;
esac

kill -TERM "$PID"
i=0
while kill -0 "$PID" 2>/dev/null; do
	i=$((i + 1)); [ $i -gt 40 ] && fail "capture picowl did not exit within 2 s"
	sleep 0.05
done
wait "$PID"; RC=$?
[ "$RC" -eq 0 ] || fail "capture picowl exit status $RC"
# PICOWL_HEADLESS_SIZE sets the size of the headless output: a maximized
# toplevel gets all of it.
rm -f "$DIR"/wayland-*
PICOWL_HEADLESS_SIZE=240x320 "$PICOWL" -d 2 >"$DIR/picowl.log" 2>&1 &
PID=$!
i=0
while ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
	kill -0 "$PID" 2>/dev/null || fail "sized picowl exited early"
	i=$((i + 1)); [ $i -gt 100 ] && fail "sized socket never appeared"
	sleep 0.05
done
SOCK=$(ls "$DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)
export WAYLAND_DISPLAY=$(basename "$SOCK")
OUT=$("$CLIENT" 2>&1) || fail "sized client failed: $OUT"
case "$OUT" in *"mapped 240x320 "*) ;; *) fail "output is not 240x320: $OUT" ;; esac
kill -TERM "$PID"
wait "$PID" || fail "sized picowl exit status $?"
PID=

rm -rf "$DIR"
echo "smoke: ok"
