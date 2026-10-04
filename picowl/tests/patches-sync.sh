#!/bin/sh
# The wvkbd patch series exists twice: oe/recipes-graphics/wvkbd/files (for
# bitbake) and subprojects/packagefiles/wvkbd (for the meson wrap). The only
# allowed difference is the line 'Upstream-Status: Pending' that OpenEmbedded's
# patch-status QA demands, which only the OE copies carry, directly before the
# '---' separator. Anything else, in either direction, fails the test.
#
# usage: patches-sync.sh OE_DIR PACKAGEFILES_DIR
set -u

oe=${1:?usage: patches-sync.sh OE_DIR PACKAGEFILES_DIR}
pkg=${2:?usage: patches-sync.sh OE_DIR PACKAGEFILES_DIR}
status='Upstream-Status: Pending'
tmp=$(mktemp -d) || exit 99
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
fail=0

bad() {
    echo "FAIL: $*" >&2
    fail=1
}

(cd "$oe" && ls -- *.patch 2>/dev/null) > "$tmp/oe.list"
(cd "$pkg" && ls -- *.patch 2>/dev/null) > "$tmp/pkg.list"

[ -s "$tmp/oe.list" ] || bad "no patches in $oe"
if ! cmp -s "$tmp/oe.list" "$tmp/pkg.list"; then
    bad "the two directories hold different patch files:"
    diff "$tmp/oe.list" "$tmp/pkg.list" >&2
fi

# Other file types would escape the comparison (and the wrap's diff_files).
for d in "$oe" "$pkg"; do
    for f in "$d"/*; do
        case $f in
        *.patch) ;;
        *) bad "unexpected file $f (only .patch files belong here)" ;;
        esac
    done
done

while read -r p; do
    [ -f "$pkg/$p" ] && [ -f "$oe/$p" ] || continue
    if grep -qx "$status" "$pkg/$p"; then
        bad "$p: the packagefiles copy must not carry '$status'"
    fi
    n=$(grep -cx "$status" "$oe/$p")
    if [ "$n" -ne 1 ]; then
        bad "$p: the OE copy needs exactly one '$status' line, found $n"
        continue
    fi
    # Directly before the first '---' separator line, nowhere else.
    awk -v s="$status" '
        $0 == "---" { found = 1; exit (prev == s) ? 0 : 1 }
        { prev = $0 }
        END { if (!found) exit 1 }
    ' "$oe/$p" || bad "$p: '$status' is not directly before the '---' separator"
    grep -vx "$status" "$oe/$p" > "$tmp/stripped"
    if ! cmp -s "$tmp/stripped" "$pkg/$p"; then
        bad "$p: differs beyond the '$status' line:"
        diff "$tmp/stripped" "$pkg/$p" | head -20 >&2
    fi
done < "$tmp/oe.list"

if [ "$fail" -eq 0 ]; then
    echo "ok: $(wc -l < "$tmp/oe.list" | tr -d ' ') patches in sync"
fi
exit "$fail"
