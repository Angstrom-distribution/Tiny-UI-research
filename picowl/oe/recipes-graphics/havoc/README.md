# havoc

The havoc recipe itself lives in meta-handhelds, not in this layer. This directory only carries the patches that make havoc show picowl's on-screen keyboard and keep the bytes it sends the compositor small.

The files in `files/` go into the `SRC_URI` of that recipe, in this order (`file://0001-...patch` and `file://0002-...patch`, with this directory on `FILESEXTRAPATHS` in a bbappend). Both have `Upstream-Status: Pending`.

| Patch | What it is for |
|---|---|
| `0001-Bind-text-input-v3-so-an-on-screen-keyboard-can-appe.patch` | the on-screen keyboard appears when the terminal has the focus |
| `0002-Damage-only-the-cells-that-changed-and-say-the-windo.patch` | a keystroke damages a few cells instead of the whole surface, and an opaque window says so |

They apply to upstream havoc (https://github.com/ii8/havoc) master at commit `73e3467149fdc394c72b22dee6df6aeac601746c` ("Add configurable window padding"). To apply them by hand:

```sh
git clone https://github.com/ii8/havoc
cd havoc
git checkout 73e3467149
git am /path/to/0001-Bind-text-input-v3-so-an-on-screen-keyboard-can-appe.patch /path/to/0002-Damage-only-the-cells-that-changed-and-say-the-windo.patch
```

## 0001: text-input-v3

havoc did not bind `zwp_text_input_manager_v3`, so the compositor never learned that the terminal takes text and the on-screen keyboard (`wvkbd-ipaq --auto`, driven by picowl's text-input to input-method relay, `doc/design/osk.md` part 3) stayed hidden. With the patch havoc creates a text input for its seat and, when the compositor sends `enter` for the surface, sends `enable`, content type terminal, the cursor cell as cursor rectangle and `commit`. On `leave` it sends `disable` and `commit`.

It enables once per focus, never per key. Hiding the keyboard with the toggle button therefore stays in effect until the focus leaves and comes back. Typing is unchanged: keys, including those from the on-screen keyboard, still arrive through `wl_keyboard`, and the text-input text events are ignored. Without a text-input manager nothing is created and nothing changes. The patch also adds `text-input-unstable-v3.xml` to the Makefile's generated protocols, copied from wayland-protocols like the others, so the recipe needs no new dependency.

## 0002: per-cell damage and an opaque region

Before the patch every redraw sent `wl_surface.damage_buffer(0, 0, width, height)`, so one typed character made the compositor take in the whole frame again: 153,600 bytes at 240x320 RGB565, about 17 ms of bus on an iPAQ h2200. libtsm's cell ages cannot tell what changed, because libtsm stamps the whole screen on every erase, scroll, selection and colour change (its own "more sophisticated ageing" TODOs), which is exactly what editing a line at a prompt does.

With the patch havoc reduces each cell to a key of what decides its pixels (symbol, width, and the two colours with the cursor, selection and inverse mode already applied) and keeps two sets of keys:

- Per buffer, the keys the buffer was painted with. Only cells whose key differs there are painted again, so a buffer that is older than the screen catches up.
- Per commit, the keys of the frame the compositor shows. Only cells that differ from that are damaged, one rectangle per run of changed rows, merged into one bounding box when there would be more than eight, and the whole surface when more than three quarters of the cells changed.

Everything that makes the compositor's picture unknown damages and paints everything: the first frame, a new buffer size, a font or scale change (zoom, fractional scale), a changed margin colour. A redraw in which nothing changed sends no commit. Under a fractional scale every rectangle grows by one pixel so that the compositor's rounding of the viewport cannot leave a seam. Buffers are used only after the compositor released them (`wl_buffer.release`), and each of the two is checked for its own size and memory, which the old shared resize counter did not do: with a compositor that released at once it re-created the first buffer twice and left the second without memory.

Cells that cover pixels together are handled together. A narrow character written over half of a wide one used to leave the other half on screen, and a wide cell in the last column wrote into the next row of pixels; both are fixed.

When the configured opacity is 255 (the code default, not the 230 of the sample `havoc.cfg`) the window is opaque, so havoc sets the opaque region to the whole window, in surface coordinates like the viewport destination, after every configure, and uses `WL_SHM_FORMAT_XRGB8888` buffers. With any other opacity nothing changes.

### Debug switch

`HAVOC_FULL_REDRAW=1` in the environment paints every cell into a cleared buffer and damages the whole surface on every redraw. It is off unless set and exists for the tests: the picture the incremental redraw must equal.

### Test

`tests/run.sh PATCHED_HAVOC_SOURCE_DIR` builds a patched havoc tree with AddressSanitizer and UBSan and runs it against `tests/fake-compositor.c`, a small libwayland-server compositor (wl_compositor with regions, wl_shm, xdg-shell, a seat with a real keymap and a pointer, text-input-v3, and with `--fractional` viewporter and fractional-scale). It runs:

1. The text-input checks of patch 0001, with and without text-input in the compositor: nothing is enabled before the focus arrives, enter is answered by exactly one enable with purpose terminal, a cursor rectangle and a commit, typing causes no further text-input requests and still reaches the shell, leave is answered by disable and commit, and the next enter enables once more.
2. `tests/damage.py`, which drives the compositor with `--driver` (commands on stdin, see `tests/harness.py`) and feeds the terminal through a fifo, so the screen content is under the test's control. In `--driver` mode the compositor keeps its own copy of the window and updates it only where the damage it was told says the buffer changed. After every commit it checks that the copy equals the committed buffer (any pixel that changed outside the damage fails the run), that nothing wrote to a buffer it still holds, and that a new buffer size came with damage for all of it. Release policies are immediate, at the next commit, after 40 ms, and never. The groups are:
   - `damage`: typing one character, at the end of a line and past it, cursor moves, a newline, selection start, drag and stop, a palette change, output that changes nothing (no commit), a scroll, scrolling blank lines, padding, resize shrink then grow, and a scale change. Each is held to a bound computed from the cell size (a character is at most 3 cells, a selection step at most 10, and always 100 times less than the buffer).
   - `lifecycle`: a compositor that never releases a buffer (havoc must stop drawing after both are held), one that releases after a delay, and one that releases at the next commit.
   - `opaque`: the opaque region and buffer format for opacity 255 and 230, after a resize, at scale 1.5 and with padding.
   - `golden`: a scripted sequence (typing, colours and attributes, cursor movement, erase, insert and delete of lines and characters, scroll regions, wide characters and overwriting half of them, selection with the cursor hidden and shown over it, scroll then edit, alternate screen, palette changes, reverse video, shrink then grow) followed by 70 seeded random steps, 194 steps in all, each with a snapshot of the window. The sequence runs with `HAVOC_FULL_REDRAW=1` as the reference and again incrementally with each release policy, and every snapshot must be byte identical to the reference's. A second configuration (scrollback and padding) runs the reference and one incremental run.
   `HAVOC_TEST_GROUPS="damage golden" tests/run.sh DIR` runs only some groups. The whole run takes about 3 minutes.

`tests/mutations.sh PATCHED_HAVOC_SOURCE_DIR` breaks the damage code in the ways listed in `tests/mutate.py` (the cursor cell, the cell the cursor left, damage from the buffer's own state instead of what is shown, drawing into a held buffer, never reusing a released buffer, no full damage on resize or scale, no handling of wide cells, a rectangle one cell short) and checks that the tests fail for each.

`tests/run.sh --measure HAVOC_WITHOUT_0002 PATCHED_HAVOC_SOURCE_DIR` builds both without the sanitizers and prints the damaged bytes of some scenarios for each. For the 80x24 window of the container (800x432 pixels, 1,382,400 bytes per full frame):

| scenario | commits before | bytes before | commits after | bytes after | reduction |
|---|---:|---:|---:|---:|---:|
| type 100 characters, one by one | 100 | 138,240,000 | 100 | 144,000 | 960x |
| seq 1 3000 | 2 | 2,764,800 | 2 | 100,800 | 27x |
| cursor blink, 10 cycles (hide, show) | 20 | 27,648,000 | 20 | 14,400 | 1,920x |
| top-like refresh, 20 times | 40 | 55,296,000 | 40 | 3,774,240 | 15x |
| scroll of one line | 1 | 1,382,400 | 1 | 1,002,240 | 1.4x |

One typed character is 1,440 bytes (the cell it fills and the cell the cursor moves to). A scroll changes almost every cell, so there is nothing to save, and the commit counts of `seq` depend on how the output is split into reads, so its row says more about the commit count than the damage. havoc has no cursor blink timer; hide and show (DECTCEM) is what a blinking program or a future timer would send.

The tests need gcc, make, pkg-config, wayland-scanner, wayland-protocols, libwayland-dev, libxkbcommon-dev (plus xkb-data at run time) and python3 (python3-pil is not needed). On a Mac they are meant to run in a container (`container run`, debian:trixie), with the sources copied into the container.

### Not covered

A real compositor and a board. Whether picowl's wlroots accepts the damage as sent, releases shm buffers at commit or holds them, takes a fractional-scale viewport's rounding at the one pixel the patch adds, and what the damage costs on the panel's bus are not tested; the fake compositor takes the protocol at its word. The opacity the board runs with is not known (the code default is 255, the sample configuration 230): at 230 only the damage applies, not the opaque region. The tests use havoc's built-in font, so what it draws for wide characters is whatever that font has.
