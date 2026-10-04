#!/bin/sh
# Headless end-to-end test of the two-window tiled layout: the configure sizes
# picowl sends to two toplevels, in a landscape (1280x720) and in a portrait
# (the same output turned by 90 degrees) run, and with a keyboard stand-in (a
# bottom anchored layer surface, without and with an exclusive zone) which pans
# the pair instead of resizing it, or, with [layout] pan empty or with the pair
# side by side, shrinks it (a keyboard with a zone) or leaves it (one without).
# [layout] focus keeps the wl_keyboard focus on one app of the pair while the
# other maps; the client holds a virtual keyboard to get a wl_keyboard at all.
# usage: tile-e2e.sh PICOWL PW_TILE_CLIENT
PICOWL=$1
CLIENT=$2
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
unset DISPLAY WAYLAND_DISPLAY
PID=
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$DIR"; }
fail() { echo "tile-e2e: FAIL: $*"; cat "$DIR/picowl.log" 2>/dev/null; cleanup; exit 1; }

# run MODE INI: start picowl with INI, run the client in MODE, stop picowl.
run() {
	rm -f "$DIR"/wayland-*
	"$PICOWL" -d 2 -c "$2" >"$DIR/picowl.log" 2>&1 &
	PID=$!
	i=0
	while ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
		kill -0 "$PID" 2>/dev/null || fail "$1: picowl exited early"
		i=$((i + 1)); [ $i -gt 100 ] && fail "$1: socket never appeared"
		sleep 0.05
	done
	SOCK=$(ls "$DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)
	export WAYLAND_DISPLAY=$(basename "$SOCK")
	OUT=$("$CLIENT" "$1" 2>&1) || fail "$1: client failed: $OUT"
	echo "$OUT"
	case "$OUT" in *"pw-tile-client: ok $1"*) ;; *) fail "$1: client did not report ok" ;; esac
	kill -TERM "$PID"
	i=0
	while kill -0 "$PID" 2>/dev/null; do
		i=$((i + 1)); [ $i -gt 40 ] && fail "$1: picowl did not exit within 2 s"
		sleep 0.05
	done
	wait "$PID"; RC=$?
	PID=
	[ "$RC" -eq 0 ] || fail "$1: picowl exit status $RC"
	if grep -q 'Bad aspect' "$DIR/picowl.log"; then
		fail "$1: picowl rejected the test config"
	fi
}

cat >"$DIR/landscape.ini" <<EOF
[layout]
stack = tile-a, tile-b

[app.tile-a]
aspect = 1:2
EOF
run landscape "$DIR/landscape.ini"

cat >"$DIR/portrait.ini" <<EOF
[output]
* = 90

[layout]
stack = tile-a, tile-b
EOF
run portrait "$DIR/portrait.ini"

cat >"$DIR/pan.ini" <<EOF
[output]
* = 90

[layout]
stack = tile-a, tile-b
EOF
run pan "$DIR/pan.ini"
run pan-zone "$DIR/pan.ini"

cat >"$DIR/nopan.ini" <<EOF
[output]
* = 90

[layout]
stack = tile-a, tile-b
pan =
EOF
run nopan "$DIR/nopan.ini"

cat >"$DIR/pan-landscape.ini" <<EOF
[layout]
stack = tile-a, tile-b
EOF
run pan-landscape "$DIR/pan-landscape.ini"

# the keyboard stays with the named app when the other maps or restarts, and
# goes to the newest window as before without the key
cat >"$DIR/focus.ini" <<EOF
[output]
* = 90

[layout]
stack = tile-a, tile-b
focus = tile-a
EOF
run focus-keep "$DIR/focus.ini"
# one INFO line per change of the focused window, none for a re-focus of the
# window which has it (b mapping while a has it is no change)
logged() { grep -c "focus: app_id=$1 ($2)" "$DIR/picowl.log"; }
[ "$(logged tile-a map)" -eq 1 ] || fail "focus-keep: expected one 'tile-a (map)' log line"
[ "$(logged tile-b cycle)" -eq 1 ] || fail "focus-keep: expected one 'tile-b (cycle)' log line"
[ "$(logged tile-a cycle)" -eq 1 ] || fail "focus-keep: expected one 'tile-a (cycle)' log line"
[ "$(logged other map)" -eq 1 ] || fail "focus-keep: expected one 'other (map)' log line"
[ "$(logged tile-a rule)" -eq 1 ] || fail "focus-keep: expected one 'tile-a (rule)' log line"
[ "$(grep -c 'focus: app_id=tile-b (\(map\|rule\|pair\))' "$DIR/picowl.log")" -eq 0 ] ||
	fail "focus-keep: tile-b took the focus on map"
run focus-default "$DIR/pan.ini"
[ "$(logged tile-b pair)" -ge 1 ] || fail "focus-default: expected a 'tile-b (pair)' log line"
[ "$(logged tile-a rule)" -eq 0 ] || fail "focus-default: the rule fired without the key"

rm -rf "$DIR"
echo "tile-e2e: ok"
