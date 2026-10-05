#!/bin/sh
# Build a patched havoc tree with AddressSanitizer and UBSan and run it
# against the fake compositor: with and without text-input-v3, and then
# through the damage tests (damage.py).
#
#     tests/run.sh /path/to/patched/havoc
#
# HAVOC_TEST_GROUPS="damage golden" in the environment runs only those groups
# of damage.py (damage, lifecycle, opaque, golden).
#
#     tests/run.sh --measure /path/to/havoc/without/0002 /path/to/patched/havoc
#
# instead builds both without the sanitizers and prints the damaged bytes of
# some scenarios for each (measure.py).
#
# Needs gcc, make, pkg-config, wayland-scanner, wayland-protocols, the
# development files of libwayland and libxkbcommon (plus xkb-data at run time)
# and python3. The trees are copied, so the source directories stay clean.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
measure=false
if [ "${1:-}" = --measure ]; then
	measure=true
	shift
	before=${1:?usage: run.sh --measure HAVOC_BEFORE HAVOC_AFTER}
	after=${2:?usage: run.sh --measure HAVOC_BEFORE HAVOC_AFTER}
else
	src=${1:?usage: run.sh HAVOC_SOURCE_DIR}
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

pd=$(pkg-config --variable=pkgdatadir wayland-protocols)
for p in stable/xdg-shell/xdg-shell unstable/text-input/text-input-unstable-v3 \
	stable/viewporter/viewporter staging/fractional-scale/fractional-scale-v1; do
	b=$(basename "$p")
	wayland-scanner server-header <"$pd/$p.xml" >"$work/$b-server.h"
	wayland-scanner private-code <"$pd/$p.xml" >"$work/$b-protocol.c"
done
gcc -g -O1 -Wall $san -I"$work" -o "$work/fake-compositor" "$here/fake-compositor.c" \
	"$work/xdg-shell-protocol.c" "$work/text-input-unstable-v3-protocol.c" \
	"$work/viewporter-protocol.c" "$work/fractional-scale-v1-protocol.c" \
	$(pkg-config --cflags --libs wayland-server xkbcommon)

export XDG_RUNTIME_DIR=$work
export ASAN_OPTIONS=detect_leaks=0

if $measure; then
	for t in before after; do
		if [ $t = before ]; then from=$before; else from=$after; fi
		cp -R "$from" "$work/$t"
		make -C "$work/$t" clean >/dev/null
		make -C "$work/$t" CFLAGS="-O2" >/dev/null 2>&1
	done
	python3 "$here/measure.py" "$work/fake-compositor" "$work/before/havoc" "$work/after/havoc"
	exit
fi

cp -R "$src" "$work/havoc"
make -C "$work/havoc" clean >/dev/null
make -C "$work/havoc" CFLAGS="-g -O1 -Wall -Wextra -Wno-unused-parameter $san" LDFLAGS="$san" 2>"$work/build.log" >/dev/null || { cat "$work/build.log"; exit 1; }
# Upstream has warnings under -Wall -Wextra that are not ours (sprintf sizes,
# libtsm), so only our code is held to being warning-free.
if grep -E 'warning.*(ti_|text_input|text-input|cell_key|collect_cell|paint_cell|spread_wide|damage_rects|send_damage|redraw|swap_buffers|buffer_)' "$work/build.log"; then
	echo "FAIL: the text input or damage code builds with warnings" >&2
	exit 1
fi

# every stage runs even if an earlier one failed, so that a failure shows
# everything it breaks
rc=0
"$work/fake-compositor" "$work/havoc/havoc" "$work/typed" || rc=1
"$work/fake-compositor" --no-text-input "$work/havoc/havoc" "$work/typed" || rc=1
python3 "$here/damage.py" "$work/fake-compositor" "$work/havoc/havoc" ${HAVOC_TEST_GROUPS:-} || rc=1
exit $rc
