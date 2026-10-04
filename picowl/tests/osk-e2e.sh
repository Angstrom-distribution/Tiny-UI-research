#!/bin/sh
# Headless end-to-end test of the on-screen keyboard supervision, with
# pw-osk-fake standing in for wvkbd and pw-key-client pressing the keys.
#  A: the keyboard is picowl's child and starts with the compositor; the osk
#     toggle, show and hide keys reach it as SIGRTMIN, SIGUSR2 and SIGUSR1, in
#     order; [autostart] still works next to it; SIGTERM stops it and it is
#     not restarted.
#  B: a keyboard that dies at once is restarted after 1, 2, 4 and 8 s, then
#     picowl gives up and stays quiet; an osk key starts it again and the
#     backoff starts over.
#  C: osk keys without an [osk] cmd are ignored.
#  D: a keyboard command that does not exist is retried and the compositor
#     keeps answering meanwhile.
#  E: restart = no leaves a dead keyboard dead until an osk key.
# usage: osk-e2e.sh PICOWL PW_KEY_CLIENT PW_OSK_FAKE
PICOWL=$1
KEYS=$2
FAKE=$3
DIR=$(mktemp -d)
chmod 700 "$DIR"
export XDG_RUNTIME_DIR=$DIR
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
unset DISPLAY WAYLAND_DISPLAY
PID=
cleanup() {
	[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
	for p in $(sed -n 's/^start //p' "$DIR"/*.log 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
	rm -rf "$DIR"
}
fail() { echo "osk-e2e: FAIL: $*"; cat "$DIR/picowl.log" 2>/dev/null; echo "--- fake logs"; tail -n 20 "$DIR"/*.log 2>/dev/null; cleanup; exit 1; }

# start INI: run picowl with INI and wait for its socket.
start() {
	rm -f "$DIR"/wayland-*
	"$PICOWL" -d 2 -c "$1" >"$DIR/picowl.log" 2>&1 &
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

# stop: SIGTERM picowl, it must exit 0 within 3 s.
stop() {
	kill -TERM "$PID"
	i=0
	while kill -0 "$PID" 2>/dev/null; do
		i=$((i + 1)); [ $i -gt 60 ] && fail "$1: picowl did not exit within 3 s"
		sleep 0.05
	done
	wait "$PID"; RC=$?
	PID=
	[ "$RC" -eq 0 ] || fail "$1: picowl exit status $RC"
}

lines() { if [ -f "$1" ]; then wc -l <"$1" | tr -d ' '; else echo 0; fi; }

# wait_lines FILE N SECONDS WHAT
wait_lines() {
	i=0
	while [ "$(lines "$1")" -lt "$2" ]; do
		kill -0 "$PID" 2>/dev/null || fail "$4: picowl died"
		i=$((i + 1)); [ $i -gt $(($3 * 20)) ] && fail "$4: $1 has $(lines "$1") lines, wanted $2"
		sleep 0.05
	done
}

# wait_log PATTERN SECONDS WHAT: wait for a line in picowl's log.
wait_log() {
	i=0
	while ! grep -q "$1" "$DIR/picowl.log"; do
		kill -0 "$PID" 2>/dev/null || fail "$3: picowl died"
		i=$((i + 1)); [ $i -gt $(($2 * 20)) ] && fail "$3: no '$1' in the picowl log"
		sleep 0.05
	done
}

press() { "$KEYS" "$@" || fail "key client failed for $*"; }

# ---- A: signals, ownership, autostart, shutdown ----
cat >"$DIR/a.ini" <<EOF
[osk]
cmd = $FAKE $DIR/a.log
[autostart]
cmd = echo autostart-ok > $DIR/autostart.out
[keybindings]
code:397 = osk toggle
code:398 = osk show
code:399 = osk hide
EOF
start "$DIR/a.ini"
wait_lines "$DIR/a.log" 1 5 A
FAKEPID=$(sed -n 's/^start //p' "$DIR/a.log")
kill -0 "$FAKEPID" 2>/dev/null || fail "A: keyboard is not running"
PPID_OF=$(awk '{print $4}' "/proc/$FAKEPID/stat")
[ "$PPID_OF" = "$PID" ] || fail "A: keyboard parent is $PPID_OF, not picowl ($PID)"
n=1
for step in "397 toggle" "398 show" "399 hide" "397 toggle"; do
	set -- $step
	press "$1"
	n=$((n + 1))
	wait_lines "$DIR/a.log" $n 5 "A key $1"
	[ "$(sed -n "${n}p" "$DIR/a.log")" = "$2" ] || fail "A: key $1 gave '$(sed -n "${n}p" "$DIR/a.log")', wanted '$2'"
done
i=0
while [ ! -s "$DIR/autostart.out" ]; do
	i=$((i + 1)); [ $i -gt 100 ] && fail "A: autostart did not run next to the keyboard"
	sleep 0.05
done
[ "$(cat "$DIR/autostart.out")" = "autostart-ok" ] || fail "A: autostart output wrong"
stop A
kill -0 "$FAKEPID" 2>/dev/null && fail "A: keyboard still running after picowl exited"
[ "$(lines "$DIR/a.log")" -eq 6 ] || fail "A: expected 6 log lines, got $(lines "$DIR/a.log")"
[ "$(tail -n1 "$DIR/a.log")" = "term" ] || fail "A: keyboard did not get SIGTERM"
if grep -q 'restarting' "$DIR/picowl.log"; then fail "A: picowl restarted the keyboard on shutdown"; fi
echo "osk-e2e: A ok"

# ---- B: backoff, give up, start again ----
cat >"$DIR/b.ini" <<EOF
[osk]
cmd = $FAKE $DIR/b.log --exit-after 50
[keybindings]
code:397 = osk toggle
code:399 = osk hide
EOF
start "$DIR/b.ini"
wait_lines "$DIR/b.log" 5 25 B
wait_log 'giving up' 3 B
[ "$(grep -c 'restarting in' "$DIR/picowl.log")" -eq 4 ] || fail "B: expected 4 restarts"
[ "$(sed -n 's/.*restarting in \([0-9]*\) ms.*/\1/p' "$DIR/picowl.log" | tr '\n' ' ')" = "1000 2000 4000 8000 " ] ||
	fail "B: backoff delays are not 1, 2, 4, 8 s"
sleep 2
[ "$(lines "$DIR/b.log")" -eq 5 ] || fail "B: keyboard restarted after picowl gave up"
# hide never starts it, an osk key that shows does
press 399
sleep 0.3
[ "$(lines "$DIR/b.log")" -eq 5 ] || fail "B: osk hide started the keyboard"
press 397
wait_lines "$DIR/b.log" 6 3 "B restart by key"
wait_lines "$DIR/b.log" 7 5 "B backoff after the key"
[ "$(sed -n 's/.*restarting in \([0-9]*\) ms.*/\1/p' "$DIR/picowl.log" | sed -n 5p)" = "1000" ] ||
	fail "B: the backoff did not start over after the key"
stop B
echo "osk-e2e: B ok"

# ---- C: no [osk] cmd ----
cat >"$DIR/c.ini" <<EOF
[keybindings]
code:397 = osk toggle
EOF
start "$DIR/c.ini"
press 397
wait_log 'no \[osk\] cmd configured' 3 C
stop C
echo "osk-e2e: C ok"

# ---- D: a command that does not exist ----
cat >"$DIR/d.ini" <<EOF
[osk]
cmd = $DIR/no-such-keyboard
[keybindings]
code:399 = osk hide
EOF
start "$DIR/d.ini"
wait_log 'exited with status 127' 3 D
wait_log 'restarting in 1000 ms' 1 D
press 399
stop D
echo "osk-e2e: D ok"

# ---- E: restart = no ----
cat >"$DIR/e.ini" <<EOF
[osk]
cmd = $FAKE $DIR/e.log --exit-after 50
restart = no
[keybindings]
code:397 = osk toggle
EOF
start "$DIR/e.ini"
wait_lines "$DIR/e.log" 1 5 E
wait_log 'exited with status 1' 3 E
sleep 1.5
[ "$(lines "$DIR/e.log")" -eq 1 ] || fail "E: restarted although restart = no"
press 397
wait_lines "$DIR/e.log" 2 3 "E key"
stop E
echo "osk-e2e: E ok"

cleanup
echo "osk-e2e: ok"
