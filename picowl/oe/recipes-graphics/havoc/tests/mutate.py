#!/usr/bin/env python3
"""Break the damage code of a patched havoc tree in one named way.

    mutate.py HAVOC_SOURCE_DIR NAME     (edits main.c in place)
    mutate.py --list

tests/mutations.sh builds each of them and runs the tests, which must fail.
"""
import sys

CURSOR = ("tsm_screen_get_cursor_y(term.screen) * term.col + "
          "(tsm_screen_get_cursor_x(term.screen) < term.col "
          "? tsm_screen_get_cursor_x(term.screen) : term.col - 1)")

MUTATIONS = {
    # the cell the cursor moved to is not damaged
    "skip-cursor-cell": [(
        "	if (!full) {\n		for (i = 0; i < n && !term.flags[i]; ++i)",
        "	if (!full)\n		term.flags[" + CURSOR + "] &= ~CELL_DAMAGE;\n"
        "	if (!full) {\n		for (i = 0; i < n && !term.flags[i]; ++i)")],
    # the cell the cursor left is not damaged
    "skip-previous-cursor-cell": [(
        "	if (!full) {\n		for (i = 0; i < n && !term.flags[i]; ++i)",
        "	{\n		static int prev = -1;\n"
        "		if (!full && prev >= 0 && prev < n)\n"
        "			term.flags[prev] &= ~CELL_DAMAGE;\n"
        "		prev = " + CURSOR + ";\n	}\n"
        "	if (!full) {\n		for (i = 0; i < n && !term.flags[i]; ++i)")],
    # damage is what changed in the buffer drawn into, not on the screen: the
    # mistake of trusting a per buffer age for what the compositor shows
    "damage-from-buffer-age": [(
        "	wl_surface_attach(term.surf, buffer->b, 0, 0);\n",
        "	for (i = 0; i < n; ++i)\n"
        "		term.flags[i] = (term.flags[i] & CELL_PAINT)\n"
        "				? CELL_PAINT | CELL_DAMAGE : 0;\n"
        "	wl_surface_attach(term.surf, buffer->b, 0, 0);\n")],
    # the buffer is not marked busy, so it is drawn into while the compositor
    # holds it
    "draw-into-held-buffer": [(
        "	buffer->busy = true;\n", "	buffer->busy = false;\n")],
    # a released buffer is never reused
    "never-reuse-released-buffer": [(
        "	buffer->busy = false;\n}\n\nstatic const struct wl_buffer_listener",
        "}\n\nstatic const struct wl_buffer_listener")],
    # a new buffer size or scale does not invalidate what the compositor shows
    "no-full-damage-on-resize": [(
        "		term.shown_valid = false;\n		term.need_redraw = true;\n	}\n\n"
        "	if (term.col != col",
        "		term.need_redraw = true;\n	}\n\n	if (term.col != col"), (
        "		term.buf[0].valid = term.buf[1].valid = false;\n"
        "		term.shown_valid = false;\n",
        "		term.buf[0].valid = term.buf[1].valid = false;\n")],
    # the second half of a wide cell is not repainted or damaged with its
    # first
    "no-wide-cell-spreading": [(
        "static void spread_wide(uint8_t bit, const struct cell_key *old)\n{\n",
        "static void spread_wide(uint8_t bit, const struct cell_key *old)\n"
        "{\n	return;\n")],
    # a damage rectangle one cell short
    "rect-one-cell-short": [(
        "			x1 = x;\n		}\n\n		if (x0 < 0) {",
        "			x1 = x > x0 ? x - 1 : x;\n		}\n\n		if (x0 < 0) {")],
}


def main():
    if sys.argv[1:] == ["--list"]:
        print("\n".join(MUTATIONS))
        return 0
    src, name = sys.argv[1:3]
    path = src + "/main.c"
    s = open(path).read()
    for old, new in MUTATIONS[name]:
        if s.count(old) != 1:
            print("cannot apply %s: %d matches" % (name, s.count(old)),
                  file=sys.stderr)
            return 2
        s = s.replace(old, new)
    open(path, "w").write(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
