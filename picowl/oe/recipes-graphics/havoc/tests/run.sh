#!/bin/sh
# Build a patched havoc tree with AddressSanitizer and UBSan and run it
# against the fake compositor, once with and once without text-input-v3.
#
#     tests/run.sh /path/to/patched/havoc
#
# Needs gcc, make, pkg-config, wayland-scanner, wayland-protocols and the
# development files of libwayland and libxkbcommon. The tree is copied, so the
# source directory stays clean.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=${1:?usage: run.sh HAVOC_SOURCE_DIR}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

cp -R "$src" "$work/havoc"
make -C "$work/havoc" clean >/dev/null
make -C "$work/havoc" CFLAGS="-g -O1 -Wall -Wextra -Wno-unused-parameter $san" LDFLAGS="$san" 2>"$work/build.log" >/dev/null || { cat "$work/build.log"; exit 1; }
# Upstream has warnings under -Wall -Wextra that are not ours (sprintf sizes,
# libtsm), so only the text input code is held to being warning-free.
if grep -E 'warning.*(ti_|text_input|text-input)' "$work/build.log"; then
	echo "FAIL: the text input code builds with warnings" >&2
	exit 1
fi

pd=$(pkg-config --variable=pkgdatadir wayland-protocols)
for p in stable/xdg-shell/xdg-shell unstable/text-input/text-input-unstable-v3; do
	b=$(basename "$p")
	wayland-scanner server-header <"$pd/$p.xml" >"$work/$b-server.h"
	wayland-scanner private-code <"$pd/$p.xml" >"$work/$b-protocol.c"
done
gcc -g -O1 -Wall $san -I"$work" -o "$work/fake-compositor" "$here/fake-compositor.c" \
	"$work/xdg-shell-protocol.c" "$work/text-input-unstable-v3-protocol.c" \
	$(pkg-config --cflags --libs wayland-server xkbcommon)

export XDG_RUNTIME_DIR=$work
export ASAN_OPTIONS=detect_leaks=0
"$work/fake-compositor" "$work/havoc/havoc" "$work/typed"
"$work/fake-compositor" --no-text-input "$work/havoc/havoc" "$work/typed"
