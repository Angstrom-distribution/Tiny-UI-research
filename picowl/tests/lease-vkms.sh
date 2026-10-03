#!/bin/sh
# DRM lease end to end on the vkms virtual KMS device.
# usage: lease-vkms.sh PICOWL PW_LEASE_CLIENT
#
# Needs root, the vkms module and /dev/dri; skips (exit 77) otherwise, so it
# is harmless in a normal `meson test`. To run it:
#   modprobe vkms enable_overlay=1        (overlay planes are only in vkms 6.x)
#   meson test -C build --suite vkms --print-errorlogs
# The wlroots patch 0004 and the lessee's overlay plane are checked too: set
# PW_LEASE_EXPECT_OVERLAY=1 when vkms has the overlay. LIBSEAT_BACKEND defaults
# to builtin, which needs a free VT; use `noop` or a seatd otherwise. Build
# picowl and wlroots with -Db_sanitize=address to also check the lease grant
# for the use-after-free that patch 0004 fixes: any ASan report fails the test.
PICOWL=$1
CLIENT=$2

skip() { echo "lease-vkms: SKIP: $*"; exit 77; }
[ -d /dev/dri ] || skip "no /dev/dri"
[ "$(id -u)" = 0 ] || skip "needs root"
grep -qw vkms /proc/modules 2>/dev/null || modprobe vkms enable_overlay=1 2>/dev/null ||
	skip "no vkms module"

CARD=
for d in /sys/class/drm/card[0-9]*; do
	[ "$(basename "$(readlink -f "$d/device/driver" 2>/dev/null)")" = vkms ] &&
		CARD=/dev/dri/$(basename "$d") && break
done
[ -n "$CARD" ] && [ -c "$CARD" ] || skip "no vkms card node"

DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=drm WLR_DRM_DEVICES=$CARD WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export LIBSEAT_BACKEND=${LIBSEAT_BACKEND:-builtin}
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
unset DISPLAY WAYLAND_DISPLAY
LOG=$DIR/picowl.log
OVERLAY=
[ "$PW_LEASE_EXPECT_OVERLAY" = 1 ] && OVERLAY=--expect-overlay

PID= CPID=
cleanup() { kill -9 $CPID $PID 2>/dev/null; rm -rf "$DIR"; }
fail() { echo "lease-vkms: FAIL: $*"; echo "--- picowl.log"; cat "$LOG"; cleanup; exit 1; }
count() { grep -c "$1" "$LOG"; }
wait_log() { # PATTERN COUNT: wait up to 5 s until PATTERN appears COUNT times
	i=0
	while [ "$(count "$1")" -lt "$2" ]; do
		kill -0 "$PID" 2>/dev/null || fail "picowl died waiting for '$1'"
		i=$((i + 1)); [ $i -gt 100 ] && fail "no '$1' ($2 times) in the picowl log"
		sleep 0.05
	done
}

"$PICOWL" -d 2 >"$LOG" 2>&1 &
PID=$!
i=0
while ! ls "$DIR"/wayland-* >/dev/null 2>&1; do
	kill -0 "$PID" 2>/dev/null || { cat "$LOG"; rm -rf "$DIR"; skip "picowl cannot run on $CARD (seat?)"; }
	i=$((i + 1)); [ $i -gt 100 ] && fail "socket never appeared"
	sleep 0.05
done
export WAYLAND_DISPLAY=$(basename "$(ls "$DIR"/wayland-* | grep -v '\.lock$' | head -n1)")
wait_log 'lease: offering' 1

# 1. A request the policy rejects leaves everything alone.
"$CLIENT" --app-id someone-else reject >"$DIR/c0.out" 2>&1 || fail "reject case: $(cat "$DIR/c0.out")"
grep -q 'lease: rejected (app_id not allowed)' "$LOG" || fail "no policy rejection logged"
[ "$(count 'lease: granting')" -eq 0 ] || fail "a rejected request was granted"

# 2. The player's exit path: close the fd, destroy the lease object.
"$CLIENT" $OVERLAY cycle >"$DIR/c1.out" 2>&1 || fail "cycle: $(cat "$DIR/c1.out")"
cat "$DIR/c1.out"
wait_log 'lease: ended' 1
wait_log 'lease: offering' 2 # the output is back and offered again
grep -q 'connector offered again' "$DIR/c1.out" || fail "client saw no new connector event"

# 3. The player dies while leased: the Wayland disconnect ends the lease.
"$CLIENT" $OVERLAY hang >"$DIR/c2.out" 2>&1 &
CPID=$!
i=0
until grep -q '^pw-lease-client: leased' "$DIR/c2.out"; do
	kill -0 "$CPID" 2>/dev/null || fail "hang client died: $(cat "$DIR/c2.out")"
	i=$((i + 1)); [ $i -gt 100 ] && fail "hang client never leased"
	sleep 0.05
done
kill -9 "$CPID"
wait "$CPID" 2>/dev/null
CPID=
wait_log 'lease: ended' 2
wait_log 'lease: offering' 3

# 4. The player closes the fd but keeps the object: only the kernel's LEASE
# uevent tells picowl. Needs a udev monitor, so only where udev runs.
if [ -d /run/udev ]; then
	"$CLIENT" $OVERLAY close-fd-only >"$DIR/c3.out" 2>&1 || fail "close-fd-only: $(cat "$DIR/c3.out")"
	wait_log 'lease: ended' 3
	wait_log 'lease: offering' 4
fi

grep -q 'AddressSanitizer' "$LOG" && fail "AddressSanitizer report"
kill -0 "$PID" 2>/dev/null || fail "picowl died"
kill -TERM "$PID"
i=0
while kill -0 "$PID" 2>/dev/null; do
	i=$((i + 1)); [ $i -gt 100 ] && fail "picowl did not exit within 5 s"
	sleep 0.05
done
wait "$PID"; RC=$?
[ "$RC" -eq 0 ] || fail "picowl exit status $RC"
grep -q 'AddressSanitizer' "$LOG" && fail "AddressSanitizer report at exit"
rm -rf "$DIR"
echo "lease-vkms: ok"
