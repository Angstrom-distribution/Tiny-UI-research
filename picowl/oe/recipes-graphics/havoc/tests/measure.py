#!/usr/bin/env python3
"""Print how many bytes havoc damages in some everyday scenarios.

    measure.py FAKE_COMPOSITOR HAVOC_BEFORE HAVOC_AFTER

HAVOC_BEFORE is a build without patch 0002 (which damages the whole surface
on every redraw), HAVOC_AFTER one with it. Both run the same scenarios in the
fake compositor, which counts the damaged pixels of every commit. A pixel
damaged by two rectangles of a commit counts once.
"""
import os
import sys

from harness import Comp, damaged

E = "\x1b"
QUIET = 60


def type100(c):
    c.feed(E + "[2J" + E + "[H")
    c.wait()
    out = []
    for i in range(100):
        c.keys("abcdefghij"[i % 10])
        out += c.wait()
    return out


def seq(c):
    c.feed(E + "[2J" + E + "[H")
    c.wait()
    c.feed("".join("%d\r\n" % i for i in range(1, 3001)))
    return c.wait()


def blink(c):
    c.feed(E + "[2J" + E + "[H" + "prompt$ ")
    c.wait()
    out = []
    for i in range(10):
        c.feed(E + "[?25l")
        out += c.wait()
        c.feed(E + "[?25h")
        out += c.wait()
    return out


def top(c):
    def frame(n):
        rows = ["top - 12:%02d:%02d up 3 days,  2 users,  load average: 0.%02d"
                % (n // 60, n % 60, n % 100)]
        rows.append("Tasks: 123 total,   1 running, 122 sleeping")
        rows.append("%%Cpu(s): %4.1f us,  1.0 sy,  0.0 ni, 97.0 id" % (n % 9))
        rows.append("")
        rows.append("  PID USER      PR  NI    VIRT    RES  %CPU  %MEM COMMAND")
        for p in range(18):
            rows.append("%5d root      20   0  %6d  %5d  %4.1f   0.%d proc%d"
                        % (100 + p, 20000 + 100 * p, 4000 + p,
                           ((n * (p + 3)) % 100) / 10 if p < 6 else 0.0,
                           p % 10, p))
        return "".join(E + "[%d;1H%s%s[K" % (i + 1, r, E)
                       for i, r in enumerate(rows[:24]))

    c.feed(E + "[?1049h" + E + "[2J" + frame(0))
    c.wait()
    out = []
    for n in range(1, 21):
        c.feed(frame(n))
        out += c.wait()
    return out


def scroll1(c):
    c.feed(E + "[2J" + E + "[H")
    c.feed("".join("line %02d: %s\r\n" % (i, "some text " * (i % 6))
                   for i in range(30)))
    c.wait()
    c.feed("one more line")
    c.wait()
    c.feed("\r\nnew line at the bottom")
    return c.wait()


SCENARIOS = [
    ("type 100 characters, one by one", type100),
    ("seq 1 3000", seq),
    ("cursor blink, 10 cycles (hide, show)", blink),
    ("top-like refresh, 20 times", top),
    ("scroll of one line", scroll1),
]


def run(comp, havoc, fn):
    with Comp(comp, havoc, quiet=QUIET) as c:
        commits = fn(c)
        return len(commits), damaged(commits), c.w * c.h * 4


def main():
    comp, before, after = (os.path.abspath(a) for a in sys.argv[1:4])
    rows = []
    for name, fn in SCENARIOS:
        cb, bb, whole = run(comp, before, fn)
        ca, ba, _ = run(comp, after, fn)
        rows.append((name, cb, bb, ca, ba))
    print("Window %d bytes (%d KiB) per full frame. Bytes are damaged "
          "pixels * 4, summed over all commits of the scenario.\n" %
          (whole, whole // 1024))
    print("| scenario | commits before | bytes before | commits after "
          "| bytes after | reduction |")
    print("|---|---:|---:|---:|---:|---:|")
    for name, cb, bb, ca, ba in rows:
        print("| %s | %d | %d | %d | %d | %s |" %
              (name, cb, bb, ca, ba,
               "%.1fx" % (bb / ba) if ba else "all"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
