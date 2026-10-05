#!/bin/sh
# Break the damage code of a patched havoc tree in each of the ways listed by
# mutate.py and check that the tests notice.
#
#     tests/mutations.sh /path/to/patched/havoc
#
# Prints, for each mutation, whether it was caught and by how many failed
# checks. Exits non-zero if one survives or cannot be applied. A mutation that
# does not build does not count as caught.
set -u

here=$(cd "$(dirname "$0")" && pwd)
src=${1:?usage: mutations.sh HAVOC_SOURCE_DIR}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rc=0

for m in $(python3 "$here/mutate.py" --list); do
	cp -R "$src" "$work/tree"
	if ! python3 "$here/mutate.py" "$work/tree" "$m"; then
		echo "$m: ERROR, could not be applied"
		rc=1
		rm -rf "$work/tree"
		continue
	fi
	if sh "$here/run.sh" "$work/tree" >"$work/out" 2>&1; then
		echo "$m: SURVIVED"
		rc=1
	elif grep -q '^FAILED: [0-9]* checks' "$work/out" ||
	     grep -q '^FAIL: ' "$work/out"; then
		echo "$m: caught, $(grep -c '^FAIL: ' "$work/out") failure lines in $(grep -E '^[a-z]+: [0-9]+ checks, [1-9][0-9]* failed' "$work/out" | cut -d: -f1 | tr '\n' ' ')"
		echo "    first: $(grep -m1 '^FAIL: ' "$work/out")"
	else
		echo "$m: ERROR, did not run"
		tail -5 "$work/out"
		rc=1
	fi
	rm -rf "$work/tree"
done
exit $rc
