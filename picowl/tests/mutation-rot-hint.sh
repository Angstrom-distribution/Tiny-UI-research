#!/bin/sh
# Mutation check, not part of the suite (it builds the tree a second time): the
# tests of the hardware rotation hint must notice when the panel stops using it.
# pl_rot_resolve is changed to ignore the hint, and the unit test of the panel
# and the end to end test (section W) must both fail with that. A test that
# still passes means it does not look at the hint.
#
# usage: tests/mutation-rot-hint.sh [MESON-SETUP-OPTION...]
# run from the picowl directory; the options are those of the normal setup.
set -e
SRC=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp -R "$SRC" "$TMP/src"
rm -rf "$TMP/src/build"

F=$TMP/src/panel/panel-logic.c
grep -q 'bool hw = h && h->present && h->hardware;' "$F" || { echo "mutation: the line to change is gone"; exit 2; }
sed 's/bool hw = h && h->present && h->hardware;/bool hw = false;/' "$F" >"$F.new"
mv "$F.new" "$F"

cd "$TMP/src"
meson setup build -Dtests=true "$@" >"$TMP/setup.log" 2>&1 || { cat "$TMP/setup.log"; exit 2; }
ninja -C build >"$TMP/ninja.log" 2>&1 || { tail -30 "$TMP/ninja.log"; exit 2; }
rc=0
for t in panel panel-e2e; do
	if meson test -C build "$t" >"$TMP/$t.log" 2>&1; then
		echo "mutation: $t PASSED with the hint ignored: it does not check the hint"
		rc=1
	else
		echo "mutation: $t fails with the hint ignored, as it should"
	fi
done
exit $rc
