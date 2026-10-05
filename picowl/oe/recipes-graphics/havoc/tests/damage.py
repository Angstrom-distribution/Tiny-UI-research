#!/usr/bin/env python3
"""Tests for havoc's damage reporting (patch 0002).

    damage.py FAKE_COMPOSITOR HAVOC [group ...]

The fake compositor keeps its own copy of the window and updates it only where
the damage it is told says the buffer changed. On every commit it checks that
the copy equals the buffer, so any pixel that changed outside the damage fails
the run, in every test below, as does a write to a buffer the compositor still
holds and a new buffer size without damage for all of it.

Groups: damage (how much is damaged for what), lifecycle (buffers that stay
held), opaque (the opaque region and buffer format), golden (a scripted and a
random sequence of terminal operations whose every intermediate screen must be
byte identical to a full redraw of the same state).
"""
import os
import random
import shutil
import struct
import sys
import tempfile

from harness import Comp, damaged

E = "\x1b"
checks = 0
failures = []


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)
        print("FAIL: " + msg, file=sys.stderr)


def finish(c, what):
    """The checks the compositor makes itself."""
    s = c.stats()
    check(s["stale"] == "0", "%s: %s commits left pixels outside the damage"
          % (what, s["stale"]))
    check(s["held_writes"] == "0", "%s: havoc wrote to a held buffer" % what)
    check(s["no_full"] == "0", "%s: a new buffer size without full damage"
          % what)
    check(s["failed"] == "0", "%s: the compositor reported a failure" % what)
    return s


def clear(c):
    c.feed(E + "[2J" + E + "[H")
    c.wait()


# ---------------------------------------------------------------- damage

def t_damage(comp, havoc):
    with Comp(comp, havoc) as c:
        cell = c.cw * c.ch * 4
        whole = c.w * c.h * 4

        def small(commits, cells, what, rows=None):
            check(commits, "%s: nothing was drawn" % what)
            total = damaged(commits)
            check(total <= cells * cell,
                  "%s: %d bytes damaged, more than %d cells (%d)"
                  % (what, total, cells, cells * cell))
            check(total * 100 <= whole,
                  "%s: %d bytes is not 100 times less than %d" %
                  (what, total, whole))
            if rows is not None:
                for cm in commits:
                    for (x, y, w, h) in cm.rects:
                        check(y >= rows[0] * c.ch and
                              y + h <= (rows[1] + 1) * c.ch,
                              "%s: damage %d,%d %dx%d outside rows %s"
                              % (what, x, y, w, h, rows))

        clear(c)
        c.keys("a")
        small(c.wait(), 3, "typing one character", (0, 0))

        c.keys("bcd")
        small(c.wait(), 6, "typing three characters", (0, 0))

        # at the end of a line, through the pending wrap into the next
        c.feed(E + "[2J" + E + "[1;79H")
        c.wait()
        c.keys("a")
        small(c.wait(), 3, "typing the 79th character", (0, 0))
        c.keys("b")
        small(c.wait(), 3, "typing the 80th character", (0, 0))
        c.keys("c")
        small(c.wait(), 4, "typing past the end of the line", (0, 1))

        # cursor movement, by a few sequences
        clear(c)
        c.feed(E + "[10;20H")
        small(c.wait(), 3, "cursor move (CUP)", (0, 24))
        for seq, name in ((E + "[A", "up"), (E + "[B", "down"),
                          (E + "[C", "right"), (E + "[D", "left")):
            c.feed(seq)
            small(c.wait(), 3, "cursor " + name)
        c.feed("\n")
        small(c.wait(), 3, "newline")

        # selection
        clear(c)
        c.feed(E + "[4;1Hhello selection test text" + E + "[?25l")
        c.wait()
        c.ptr("enter", *c.cell_px(2, 3))
        c.wait()
        c.ptr("button", 1)
        c.wait()
        c.ptr("motion", *c.cell_px(9, 3))
        small(c.wait(), 10, "selection start and drag", (3, 3))
        c.ptr("motion", *c.cell_px(14, 3))
        small(c.wait(), 10, "selection extended", (3, 3))
        c.ptr("button", 0)
        c.wait()
        c.ptr("button", 1)
        small(c.wait(), 16, "selection stop (a click)", (3, 3))
        c.ptr("button", 0)
        c.wait()

        # colours: only the cells that had the colour change
        clear(c)
        c.feed(E + "[6;1H" + E + "[31mRED" + E + "[0m and not red")
        c.wait()
        c.feed(E + "]4;1;rgb:00/ff/00" + E + "\\")
        small(c.wait(), 3, "palette change under three cells", (5, 5))
        c.feed(E + "[6;1H" + E + "[44mblue" + E + "[0m")
        small(c.wait(), 5, "background colour of four cells", (5, 5))

        # idle: output that changes nothing must not cost a commit
        clear(c)
        c.feed(E + "[?25h")
        c.wait()
        c.feed(E + "[?25h")
        check(not c.wait(), "output that changes nothing was committed")
        c.feed("")
        check(not c.wait(), "no output was committed")

        # a scroll changes every row
        clear(c)
        lines = "".join("line %02d %s\r\n" % (i, "x" * (i % 40))
                        for i in range(60))
        c.feed(lines)
        commits = c.wait()
        check(commits, "scrolling a screenful was not drawn")
        c.feed("one more line\r\n")
        commits = c.wait()
        # every row shifts, but a cell that gets the character it had is not
        # damaged, so the damage is some part of the window and never more
        check(commits and damaged(commits) > cell,
              "a scroll of a screenful of text damaged only %d bytes" %
              damaged(commits))
        check(all(cm.sum_px <= cm.w * cm.h for cm in commits),
              "a scroll damaged some pixels twice")

        # blank lines scrolling over blank lines change nothing at all
        clear(c)
        c.feed("\r\n" * 40)
        c.wait()
        c.feed("\r\n")
        check(damaged(c.wait()) <= 4 * cell, "scrolling blank lines")
        finish(c, "damage")

        # resize: new buffer, all of it damaged, shrink then grow
        for (w, h, cols, rows) in ((600, 360, 60, 20), (400, 270, 40, 15),
                                   (800, 432, 80, 24), (810, 440, 81, 24)):
            c.cmd("configure %d %d" % (w, h))
            commits = c.wait()
            check(commits and commits[-1].full,
                  "resize to %dx%d did not damage everything" % (w, h))
            check(commits and commits[-1].w == cols * c.cw and
                  commits[-1].h == rows * c.ch,
                  "resize to %dx%d gave a %s buffer" %
                  (w, h, commits and (commits[-1].w, commits[-1].h)))
            c.keys("a")
            cw = c.cw
            commits = c.wait()
            check(commits and damaged(commits) <= 3 * cell and
                  damaged(commits) * 100 <= commits[0].w * commits[0].h * 4,
                  "typing after resize to %dx%d damaged %d bytes of %s" %
                  (w, h, damaged(commits), commits and (commits[0].w, commits[0].h)))
        finish(c, "resize")

    # padding: the grid is offset by the padding and the margin is not damaged
    with Comp(comp, havoc, config="[window]\npadding=6\n") as c:
        cell = c.cw * c.ch * 4
        clear(c)
        c.keys("a")
        commits = c.wait()
        check(commits and damaged(commits) <= 3 * cell,
              "typing with padding damaged %d bytes" % damaged(commits))
        for cm in commits:
            for (x, y, w, h) in cm.rects:
                check(x >= 6 and y >= 6 and x + w <= cm.w - 6 and
                      y + h <= cm.h - 6,
                      "damage %d,%d %dx%d reaches into the padding" %
                      (x, y, w, h))
        finish(c, "padding")

    # scale: a new buffer with all of it damaged, then small damage again
    with Comp(comp, havoc, fractional=True) as c:
        old = (c.w, c.h)
        c.cmd("scale 180")
        commits = c.wait()
        check(commits and commits[-1].full and commits[-1].w > old[0],
              "scale change did not give a larger, fully damaged buffer")
        if not commits:
            return
        w, h = commits[-1].w, commits[-1].h
        cell = (w // 80) * (h // 24) * 4
        c.keys("a")
        commits = c.wait()
        check(commits and damaged(commits) <= 3 * cell,
              "typing at scale 1.5 damaged %d bytes (cell %d)" %
              (damaged(commits), cell))
        c.cmd("scale 120")
        commits = c.wait()
        check(commits and commits[-1].full and (commits[-1].w, commits[-1].h)
              == old, "going back to scale 1 did not damage everything")
        finish(c, "scale")


# ------------------------------------------------------------- lifecycle

def t_lifecycle(comp, havoc):
    # a compositor that never gives a buffer back: havoc has two and must stop
    # drawing when both are held, not draw into one of them
    with Comp(comp, havoc, release="never") as c:
        c.keys("a")
        n = len(c.wait())
        check(n == 1, "the second buffer was not used (%d commits)" % n)
        c.keys("b")
        check(not c.wait(), "havoc drew while both buffers were held")
        c.keys("cd")
        check(not c.wait(), "havoc drew while both buffers were held (2)")
        finish(c, "never released")

    # a compositor that holds each buffer for a while: havoc alternates
    # between its buffers and one of them is always older than the screen
    with Comp(comp, havoc, release="timer") as c:
        clear(c)
        for ch in "the quick brown fox":
            c.keys(ch if ch != " " else " ")
        c.wait()
        c.feed(E + "[3;5Hmore" + E + "[1;1H")
        c.wait()
        finish(c, "timed release")

    # one buffer held until the next commit
    with Comp(comp, havoc, release="next") as c:
        clear(c)
        for i in range(12):
            c.keys("ab"[i % 2])
            c.wait()
        finish(c, "release at the next commit")


# ---------------------------------------------------------------- opaque

def t_opaque(comp, havoc):
    XRGB, ARGB = 1, 0

    def region(s):
        a, b = s["opaque"].split(":")
        return tuple(int(v) for v in a.split(",")), \
            tuple(int(v) for v in b.split(","))

    with Comp(comp, havoc) as c:
        s = c.stats()
        (isset, n), r = region(s)
        check(isset == 1 and n == 1 and r == (0, 0, c.w, c.h),
              "opacity 255: opaque region %s, expected the %dx%d window" %
              ((isset, n, r), c.w, c.h))
        check(int(s["format"]) == XRGB, "opacity 255: buffer format %s" %
              s["format"])
        c.cmd("configure 600 360")
        commits = c.wait()
        s = c.stats()
        (isset, n), r = region(s)
        check(r == (0, 0, 600, 360) and commits and commits[-1].w == 600,
              "opaque region %s after a resize to 600x360" % (r,))
        finish(c, "opaque")

    with Comp(comp, havoc, config="[window]\nopacity=230\n") as c:
        s = c.stats()
        (isset, n), r = region(s)
        check(isset == 0, "opacity 230: an opaque region was set: %s" % (r,))
        check(int(s["format"]) == ARGB, "opacity 230: buffer format %s" %
              s["format"])
        clear(c)
        c.keys("a")
        commits = c.wait()
        check(commits and damaged(commits) <= 3 * c.cw * c.ch * 4,
              "opacity 230: typing damaged %d bytes" % damaged(commits))
        finish(c, "translucent")

    with Comp(comp, havoc, fractional=True) as c:
        c.cmd("scale 180")
        c.wait()
        s = c.stats()
        (isset, n), r = region(s)
        vw, vh = (int(v) for v in s["vp"].split("x"))
        check(r == (0, 0, vw, vh) and vw > 700 and vh == 432,
              "opaque region %s with viewport %s at scale 1.5" %
              (r, s["vp"]))
        finish(c, "opaque at scale")

    with Comp(comp, havoc, config="[window]\npadding=6\n") as c:
        s = c.stats()
        (isset, n), r = region(s)
        check(r == (0, 0, c.w, c.h) and c.w == 812,
              "opaque region %s with padding, window %dx%d" % (r, c.w, c.h))
        finish(c, "opaque with padding")


# ---------------------------------------------------------------- golden

def fill_screen():
    return "".join(E + "[%d;1H" % (r + 1) + chr(65 + r % 26) * 80
                   if r < 23 else E + "[24;1H" + "z" * 79
                   for r in range(24))


def golden_steps():
    S = []

    def f(data):
        S.append(("feed", data))

    def k(text):
        S.append(("keys", text))

    def cmd(line):
        S.append(("cmd", line))

    f(E + "[2J" + E + "[H")
    k("hello")
    f("\r\nsecond line")
    f(E + "[1;31mred" + E + "[32m green" + E + "[0;7m inverse" + E + "[0m")
    f(E + "[38;5;196mc196" + E + "[48;5;21m on 21" + E + "[0m")
    f(E + "[38;2;10;200;30mtruecolour" + E + "[48;2;90;0;120m on" + E + "[0m")
    f(E + "[4mul" + E + "[5mblink" + E + "[1mbold" + E + "[0m")
    f(E + "[10;10H" + E + "[Aup" + E + "[2Bdown" + E + "[5Cright" + E +
      "[3Dleft")
    f(fill_screen())
    f(E + "[5;10H" + E + "[K")
    f(E + "[6;10H" + E + "[1K")
    f(E + "[7;1H" + E + "[2K")
    f(E + "[9;5H" + E + "[J")
    f(E + "[20;5H" + E + "[1J")
    f(fill_screen())
    f(E + "[5;1H" + E + "[2L")
    f(E + "[8;1H" + E + "[3M")
    f(E + "[12;10H" + E + "[4@")
    f(E + "[13;10H" + E + "[5P")
    f(E + "[14;10H" + E + "[6X")
    f(fill_screen())
    f(E + "[5;12r" + E + "[12;1H" + "\r\nscrolled in region" + "\r\nagain")
    f(E + "[5;1H" + E + "M" + E + "M" + "reverse index")
    f(E + "[2S" + E + "[1T")
    f(E + "[r" + E + "[24;1H" + "\r\nfull scroll\r\n")
    f(fill_screen())
    # wide characters, then narrow ones over each half of them
    f(E + "[2J" + E + "[3;5H" + "日本語" + "x")
    f(E + "[3;5Hq")
    f(E + "[3;8Hr")
    f(E + "[4;5H" + "日本語" + E + "[4;6Hs")
    f(E + "[5;5H" + "日本語" + E + "[5;5H" + "日")
    f(E + "[6;78H" + "日本")
    f(E + "[7;5H" + "日本語" + E + "[7;5H" + E + "[2P")
    f(E + "[8;5H" + "日本語" + E + "[8;5H" + E + "[2@")
    f(E + "[9;5H" + "日本語" + E + "[9;6H" + E + "[1X")
    # selection, cursor and what is under it
    f(E + "[2J" + E + "[3;1H" + "select this text, and some more of it")
    cmd("ptr enter 25 63")
    cmd("ptr button 1")
    cmd("ptr motion 205 63")
    cmd("ptr motion 305 100")
    f(E + "[?25l")
    f(E + "[?25h")
    f(E + "[?25l")
    f(E + "[?25h")
    k("typing under it")
    cmd("ptr motion 105 63")
    cmd("ptr button 0")
    f(E + "[3;5H" + E + "[?25l" + "overwrite" + E + "[?25h")
    cmd("ptr button 1")
    cmd("ptr button 0")
    cmd("ptr button 1")
    cmd("ptr button 0")
    cmd("ptr button 1")
    cmd("ptr button 0")
    cmd("ptr motion 45 19")
    # scroll then edit
    f("".join("scroll %d\r\n" % i for i in range(40)))
    f(E + "[5;3H" + "edit")
    f(E + "[A" + E + "[A" + "edit2")
    # alternate screen, palette, reverse video
    f(E + "[?1049h" + E + "[2J" + E + "[H" + "alt screen" + E + "[5;5Hhere")
    f(E + "]4;1;rgb:00/ff/00" + E + "\\")
    f(E + "[?1049l")
    f(E + "[31mred" + E + "[0m")
    f(E + "]104" + E + "\\")
    f(E + "[?5h")
    f(E + "[?5l")
    f("tab\there\tand\tthere\b\b\b")
    f(E + "[?7l" + "x" * 90 + E + "[?7h" + "y" * 90)
    f(E + "7" + E + "[20;20Hsaved" + E + "8" + "restored")
    # resize shrink, grow, with content
    cmd("configure 600 360")
    f(fill_screen())
    cmd("configure 300 180")
    f("small")
    cmd("configure 800 432")
    f(fill_screen())
    f(E + "[2J" + E + "[H" + "end of the script")

    # random sequences of the same kinds of operations
    rnd = random.Random(20240607)
    sgr = ["0", "1", "4", "7", "31", "32", "33", "34", "41", "42", "44",
           "90", "97", "100", "38;5;%d" % rnd.randrange(256),
           "48;5;%d" % rnd.randrange(256), "38;2;200;100;50", "48;2;5;50;90"]
    words = ["alpha", "b", "gamma ray", "日本", "x" * 40, "0123456789",
             "wide日", "   ", "\t", "\b"]

    def op():
        r = rnd.randrange(24)
        if r < 6:
            return rnd.choice(words)
        if r == 6:
            return "\r\n" * rnd.randrange(1, 4)
        if r == 7:
            return E + "[%d;%dH" % (rnd.randrange(1, 25), rnd.randrange(1, 82))
        if r == 8:
            return E + "[%sm" % rnd.choice(sgr)
        if r == 9:
            return E + "[%s" % rnd.choice(["K", "1K", "2K", "J", "1J"])
        if r == 10:
            return E + "[%d%s" % (rnd.randrange(1, 5),
                                  rnd.choice(["L", "M", "@", "P", "X"]))
        if r == 11:
            return E + "[%d%s" % (rnd.randrange(1, 4), rnd.choice("STABCD"))
        if r == 12:
            return rnd.choice([E + "M", E + "D", E + "E", E + "7", E + "8"])
        if r == 13:
            t = rnd.randrange(1, 12)
            return E + "[%d;%dr" % (t, rnd.randrange(t + 2, 25))
        if r == 14:
            return E + "[r"
        if r == 15:
            return rnd.choice([E + "[?25l", E + "[?25h"])
        if r == 16:
            return rnd.choice([E + "[?7l", E + "[?7h"])
        if r == 17:
            return E + "]4;%d;rgb:%02x/%02x/%02x%s" % (
                rnd.randrange(8), rnd.randrange(256), rnd.randrange(256),
                rnd.randrange(256), E + "\\")
        if r == 18:
            return rnd.choice([E + "[?5h", E + "[?5l", E + "]104" + E + "\\"])
        if r == 19:
            return rnd.choice([E + "[?1049h", E + "[?1049l"])
        if r == 20:
            return E + "[4h" if rnd.random() < .5 else E + "[4l"
        return fill_screen() if rnd.random() < .15 else rnd.choice(words)

    for _ in range(70):
        f("".join(op() for _ in range(rnd.randrange(1, 7))))
        if rnd.random() < .15:
            x, y = rnd.randrange(800), rnd.randrange(432)
            cmd("ptr enter %d %d" % (x, y))
            cmd("ptr button 1")
            cmd("ptr motion %d %d" % (rnd.randrange(800), rnd.randrange(432)))
            if rnd.random() < .7:
                cmd("ptr button 0")
        elif rnd.random() < .1:
            k("".join(rnd.choice("abcdefgh ") for _ in range(3)))
    return S


def run_golden(comp, havoc, steps, outdir, name, **kw):
    """Run the steps, a snapshot of the window after each; returns the paths."""
    paths = []
    with Comp(comp, havoc, **kw) as c:
        for i, (kind, arg) in enumerate(steps):
            if kind == "feed":
                c.feed(arg)
            elif kind == "keys":
                c.keys(arg)
            else:
                c.cmd(arg)
            c.wait()
            p = os.path.join(outdir, "%s-%03d.raw" % (name, i))
            c.snap(p)
            paths.append(p)
        finish(c, name)
    return paths


def describe(step):
    kind, arg = step
    return "%s %r" % (kind, arg if len(arg) < 60 else arg[:57] + "...")


def diff_report(a, b):
    da, db = open(a, "rb").read(), open(b, "rb").read()
    if len(da) != len(db):
        return "sizes differ: %d and %d bytes" % (len(da), len(db))
    n = len(da) // 4
    ua, ub = struct.unpack("<%dI" % n, da), struct.unpack("<%dI" % n, db)
    bad = [i for i in range(n) if ua[i] != ub[i]]
    return "%d pixels differ, first at pixel index %d" % (len(bad), bad[0])


def t_golden(comp, havoc):
    steps = golden_steps()
    out = tempfile.mkdtemp(prefix="havoc-golden-")
    ok = True
    # the reference is a full redraw of every frame into a cleared buffer; the
    # others are incremental, with the compositor giving buffers back at once,
    # at the next commit and after a while
    configs = [("", "default"),
               ("[terminal]\nscrollback=200\n[window]\npadding=5\n",
                "scrollback and padding")]
    for config, label in configs:
        ref = run_golden(comp, havoc, steps, out, "full", config=config,
                         release="immediate", full_redraw=True)
        modes = ["next", "timer", "immediate"] if not config else ["next"]
        for mode in modes:
            got = run_golden(comp, havoc, steps, out, mode, config=config,
                             release=mode)
            for i, (a, b) in enumerate(zip(got, ref)):
                same = open(a, "rb").read() == open(b, "rb").read()
                check(same, "%s, release %s, step %d (%s): the incremental "
                      "window differs from a full redraw: %s" %
                      (label, mode, i, describe(steps[i]),
                       "" if same else diff_report(a, b)))
                ok = ok and same
                if same:
                    os.unlink(a)
        for p in ref:
            if os.path.exists(p) and ok:
                os.unlink(p)
    if ok:
        shutil.rmtree(out, ignore_errors=True)
    else:
        print("snapshots kept in " + out, file=sys.stderr)
    return len(steps)


GROUPS = {"damage": t_damage, "lifecycle": t_lifecycle, "opaque": t_opaque,
          "golden": t_golden}


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    comp, havoc = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    names = sys.argv[3:] or list(GROUPS)
    for n in names:
        before, failed = checks, len(failures)
        r = GROUPS[n](comp, havoc)
        print("%s: %d checks, %d failed%s" %
              (n, checks - before, len(failures) - failed,
               " over %d steps" % r if r else ""))
    print("%s: %d checks, %d failed" % ("FAILED" if failures else "PASSED",
                                        checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
