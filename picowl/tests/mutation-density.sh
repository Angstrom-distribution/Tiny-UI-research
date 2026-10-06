#!/bin/sh
# Mutation check, not part of the suite (it builds the tree a second time): the
# tests of the density rule must notice when it is wrong.
#  edge: the threshold is moved from "at 150 ppi" to "above 150 ppi". Only the
#        unit test of the panel looks at the exact edge, so only that must fail.
#  diag: the density is worked out from the width of the mode alone instead of
#        the diagonals. The unit test and the end to end test (section X, which
#        expects smooth on the 480x640 mode of a 60x80 mm panel) must both fail.
# A test that still passes means it does not look at the rule.
#
# usage: tests/mutation-density.sh [MESON-SETUP-OPTION...]
# run from the picowl directory; the options are those of the normal setup.
set -e
SRC=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rc=0

# mutate NAME FROM TO TESTS...: build a copy with FROM replaced by TO in
# panel-logic.c, and every one of TESTS must fail.
mutate() {
	name=$1; from=$2; to=$3; shift 3
	rm -rf "$TMP/src"
	cp -R "$SRC" "$TMP/src"
	rm -rf "$TMP/src/build"
	F=$TMP/src/panel/panel-logic.c
	grep -qF "$from" "$F" || { echo "mutation $name: the line to change is gone"; exit 2; }
	# sed would need the text escaped; awk compares it as a string
	awk -v from="$from" -v to="$to" 'index($0, from) { i = index($0, from); $0 = substr($0, 1, i - 1) to substr($0, i + length(from)) } { print }' "$F" >"$F.new"
	mv "$F.new" "$F"
	(cd "$TMP/src" &&
		meson setup build -Dtests=true "$@" >"$TMP/setup.log" 2>&1) || { cat "$TMP/setup.log"; exit 2; }
	ninja -C "$TMP/src/build" >"$TMP/ninja.log" 2>&1 || { tail -30 "$TMP/ninja.log"; exit 2; }
}

check() {
	name=$1; shift
	for t in "$@"; do
		if meson test -C "$TMP/src/build" "$t" >"$TMP/$name-$t.log" 2>&1; then
			echo "mutation $name: $t PASSED with the rule broken: it does not check the rule"
			rc=1
		else
			echo "mutation $name: $t fails with the rule broken, as it should"
		fi
	done
}

mutate edge 'out->vga = ppi >= PL_DPI_SMOOTH_MIN;' 'out->vga = ppi > PL_DPI_SMOOTH_MIN;' "$@"
check edge panel
mutate diag 'double px = sqrt((double)mode_w * mode_w + (double)mode_h * mode_h);' 'double px = mode_w;' "$@"
check diag panel panel-e2e
exit $rc
