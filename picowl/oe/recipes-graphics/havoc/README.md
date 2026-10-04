# havoc

The havoc recipe itself lives in meta-handhelds, not in this layer. This directory only carries the patch that makes havoc show picowl's on-screen keyboard.

`files/0001-Bind-text-input-v3-so-an-on-screen-keyboard-can-appe.patch` goes into the `SRC_URI` of that recipe (`file://0001-Bind-text-input-v3-so-an-on-screen-keyboard-can-appe.patch`, with this directory on `FILESEXTRAPATHS` in a bbappend). It has `Upstream-Status: Pending`.

It applies to upstream havoc (https://github.com/ii8/havoc) master at commit `73e3467149fdc394c72b22dee6df6aeac601746c` ("Add configurable window padding"). To apply it by hand:

```sh
git clone https://github.com/ii8/havoc
cd havoc
git checkout 73e3467149
git am /path/to/0001-Bind-text-input-v3-so-an-on-screen-keyboard-can-appe.patch
```

## What it does

havoc did not bind `zwp_text_input_manager_v3`, so the compositor never learned that the terminal takes text and the on-screen keyboard (`wvkbd-ipaq --auto`, driven by picowl's text-input to input-method relay, `doc/design/osk.md` part 3) stayed hidden. With the patch havoc creates a text input for its seat and, when the compositor sends `enter` for the surface, sends `enable`, content type terminal, the cursor cell as cursor rectangle and `commit`. On `leave` it sends `disable` and `commit`.

It enables once per focus, never per key. Hiding the keyboard with the toggle button therefore stays in effect until the focus leaves and comes back. Typing is unchanged: keys, including those from the on-screen keyboard, still arrive through `wl_keyboard`, and the text-input text events are ignored. Without a text-input manager nothing is created and nothing changes. The patch also adds `text-input-unstable-v3.xml` to the Makefile's generated protocols, copied from wayland-protocols like the others, so the recipe needs no new dependency.

## Test

`tests/run.sh PATCHED_HAVOC_SOURCE_DIR` builds a patched havoc tree with AddressSanitizer and UBSan and runs it against `tests/fake-compositor.c`, a small libwayland-server compositor (wl_compositor, wl_shm, xdg-shell, a seat with a real keymap and text-input-v3). It checks that nothing is enabled before the focus arrives, that enter is answered by exactly one enable with purpose terminal, a cursor rectangle and a commit, that typing causes no further text-input requests and still reaches the shell, that leave is answered by disable and commit, and that the next enter enables once more. It then repeats the typing check against a compositor without text-input. It needs gcc, make, pkg-config, wayland-scanner, wayland-protocols, libwayland-dev and libxkbcommon-dev (plus xkb-data at run time).

Not covered: a real compositor. Whether picowl and wvkbd show the keyboard on focus and keep it hidden after the toggle button needs a board.
