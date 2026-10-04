#!/bin/sh
# Headless pixel test of the keyboard pan: two tiled windows drawn red (top)
# and blue (bottom) on the 1280x720 output turned to portrait (720x1280), and a
# green keyboard stand-in of 100 rows. Screenshots taken with grim before the
# keyboard shows, while it is shown and after it hides must show the stack
# moved up by 100 rows and back, the lower window entirely above the keyboard.
# The headless backend has one fixed 1280x720 mode, so this is not 240x320, but
# the layout is the same: an even split, the zone is 100 rows.
# Skips (exit 77) without grim or python3 with PIL.
# MODE is pixels (the keyboard asks for no exclusive zone, as the shipped one
# does) or pixels-zone (it asks for a zone of its height): the picture is the
# same.
# usage: pan-e2e.sh PICOWL PW_TILE_CLIENT MODE
PICOWL=$1
CLIENT=$2
MODE=${3:-pixels}
command -v grim >/dev/null 2>&1 || { echo "pan-e2e: no grim, skipped"; exit 77; }
python3 -c 'import PIL.Image' >/dev/null 2>&1 || { echo "pan-e2e: no python3 PIL, skipped"; exit 77; }
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
unset DISPLAY WAYLAND_DISPLAY
PID=
CPID=
cleanup() {
	[ -n "$CPID" ] && kill -9 "$CPID" 2>/dev/null
	[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
	rm -rf "$DIR"
}
fail() { echo "pan-e2e: FAIL: $*"; cat "$DIR/picowl.log" "$DIR/client.log" 2>/dev/null; cleanup; exit 1; }

cat >"$DIR/pan.ini" <<EOF
[output]
* = 90

[layout]
stack = tile-a, tile-b

[capture]
enabled = true
EOF
mkdir "$DIR/sync"
"$PICOWL" -d 2 -c "$DIR/pan.ini" >"$DIR/picowl.log" 2>&1 &
PID=$!
i=0
while ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
	kill -0 "$PID" 2>/dev/null || fail "picowl exited early"
	i=$((i + 1)); [ $i -gt 100 ] && fail "socket never appeared"
	sleep 0.05
done
SOCK=$(ls "$DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)
export WAYLAND_DISPLAY=$(basename "$SOCK")

"$CLIENT" "$MODE" "$DIR/sync" >"$DIR/client.log" 2>&1 &
CPID=$!
for step in before shown hidden; do
	i=0
	while [ ! -e "$DIR/sync/$step.ready" ]; do
		kill -0 "$CPID" 2>/dev/null || fail "$step: client exited early"
		i=$((i + 1)); [ $i -gt 300 ] && fail "$step: client never got there"
		sleep 0.1
	done
	grim -g "0,0 720x1280" "$DIR/$step.png" || fail "$step: grim failed"
	touch "$DIR/sync/$step.go"
done
wait "$CPID" || fail "client failed"
CPID=
grep -q "pw-tile-client: ok $MODE" "$DIR/client.log" || fail "client did not report ok"

python3 - "$DIR" <<'PYEOF' || fail "pixel check"
import sys
from PIL import Image

RED, BLUE, GREEN = (255, 0, 0), (0, 0, 255), (0, 255, 0)
d = sys.argv[1]

def runs(name, x):
    """Colour runs down column x: [(first_y, colour), ...]."""
    im = Image.open("%s/%s.png" % (d, name)).convert("RGB")
    assert im.size == (720, 1280), (name, im.size)
    out, last = [], None
    for y in range(im.size[1]):
        p = im.getpixel((x, y))
        if p != last:
            out.append((y, p))
            last = p
    return im, out

def check(name, want):
    # every column must look the same: a pan moves rows, not part of them
    for x in (0, 1, 360, 718, 719):
        im, got = runs(name, x)
        if got != want:
            print("%s column %d: %r, expected %r" % (name, x, got, want))
            sys.exit(1)
    print("pan-e2e: %s: %r" % (name, want))

# before: red above the tile boundary at 640, blue below
check("before", [(0, RED), (640, BLUE)])
# shown: the stack is 100 rows higher; the lower window ends where the green
# keyboard starts (1280 - 100), the upper one has 100 rows off the top
check("shown", [(0, RED), (540, BLUE), (1180, GREEN)])
# hidden: back where it was
check("hidden", [(0, RED), (640, BLUE)])
PYEOF

kill -TERM "$PID"
i=0
while kill -0 "$PID" 2>/dev/null; do
	i=$((i + 1)); [ $i -gt 40 ] && fail "picowl did not exit within 2 s"
	sleep 0.05
done
wait "$PID"; RC=$?
PID=
[ "$RC" -eq 0 ] || fail "picowl exit status $RC"
rm -rf "$DIR"
echo "pan-e2e: ok ($MODE)"
