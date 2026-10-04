# picowl OpenEmbedded layer

Mini-layer (`picowl-layer`) to build picowl for the iPAQs with Angstrom.
Compatible with the OE releases `wrynose` and `blacksail`
(`LAYERSERIES_COMPAT_picowl-layer = "wrynose blacksail"`, depends on `core`).

Contents:

| Recipe | Why |
|---|---|
| `recipes-graphics/wlroots/wlroots_0.19.0.bb` | oe-core, meta-openembedded and meta-angstrom ship no wlroots recipe for these releases. Minimal build: DRM and libinput backends, pixman renderer only, no GLES2/Vulkan/GBM/Xwayland. |
| `recipes-graphics/picowl/picowl_git.bb` | picowl itself, built from the `picowl` subfolder of the research repository. Installs `picowl.service` (not auto-enabled) and `/etc/picowl.ini` (conffile), and builds `picowl-panel` (PACKAGECONFIG `panel`, on by default, needs `alsa-lib`) into the package `picowl-panel`, which an image installs and `[autostart] cmd = picowl-panel` starts; the package recommends `liberation-fonts` for the panel text, without which the panel falls back to a built-in bitmap font. |
| `recipes-graphics/wvkbd/wvkbd-ipaq_git.bb` | The on-screen keyboard: upstream wvkbd plus an eleven patch series, built with the `ipaq` layout. See the wvkbd-ipaq section below. |
| `recipes-graphics/havoc/files/` | A patch only, no recipe: the havoc recipe lives in meta-handhelds. The patch binds text-input-v3 so the terminal brings up the on-screen keyboard. See `recipes-graphics/havoc/README.md`. |

## Adding the layer

```sh
bitbake-layers add-layer /path/to/Tiny-UI-research/picowl/oe
# or add the path to BBLAYERS in conf/bblayers.conf
bitbake picowl
```

## Machine and distro notes

* The distro must have the `wayland` DISTRO_FEATURE (both recipes use
  `REQUIRED_DISTRO_FEATURES = "wayland"`). `systemd` is needed for the unit file
  to be packaged. Example `local.conf`:
  `DISTRO_FEATURES:append = " wayland systemd"`.
* The `video` group must exist on the target system for backlight brightness
  control. The systemd unit runs picowl with `SupplementaryGroups=video` and the
  udev rule (90-picowl-backlight.rules) grants group write access to the
  backlight brightness sysfs attribute.
* Do not build with Thumb: the render paths (pixman and picowl) are tuned for
  ARM state and the SA-1110/PXA25x have no benefit from Thumb. picowl sets
  `ARM_INSTRUCTION_SET:arm = "arm"`; for the machine/distro also set
  `ARM_INSTRUCTION_SET = "arm"` globally, or at least
  `ARM_INSTRUCTION_SET:pn-pixman = "arm"` and `...:pn-wlroots = "arm"`.
* No GPU: no `virtual/egl` or mesa is needed. wlroots is configured with
  `-Drenderers=[] -Dallocators=[]`, leaving only the built-in pixman renderer
  and dumb-buffer (DRM) allocator.
* Enable the service on the target with `systemctl enable picowl`, or set
  `SYSTEMD_AUTO_ENABLE:pn-picowl = "enable"`.
* wlroots 0.19 puts no upper bound on libdisplay-info, but oe-core carries
  libdisplay-info 0.4.0; if the wlroots build fails there, pin an older
  libdisplay-info (weston needed a patch for the same reason).
* The wlroots shared library has no soversion (`libwlroots-0.19.so`); the recipe
  moves it from `-dev` into the runtime package (`SOLIBS`/`FILES_SOLIBSDEV`).

## Recipe conventions: S, UNPACKDIR and WORKDIR (source research)

Checked against openembedded-core branches `wrynose` and `blacksail`
(`blacksail` equals `master` at the time of writing, `LAYERSERIES_CORENAMES =
"blacksail"`) and bitbake branches `2.18` (wrynose) and `2.20` (blacksail).

* `meta/conf/bitbake.conf` (both branches):
  `UNPACKDIR ??= "${WORKDIR}/sources"`,
  `BB_GIT_DEFAULT_DESTSUFFIX = "${BP}"`,
  `S = "${UNPACKDIR}/${BP}"`.
  Sources are unpacked into `UNPACKDIR`, never directly into `WORKDIR`, and a
  git checkout is placed in `${UNPACKDIR}/${BP}`, so a recipe fetching a plain
  git repository needs no `S` at all.
* `meta/classes-global/insane.bbclass`, `do_qa_unpack`/`do_recipe_qa`:
  * `if sourcedir == workdir: bb.fatal("Using S = ${WORKDIR} is no longer supported")`
    (likewise `B` and `UNPACKDIR` equal to `WORKDIR`).
  * `S = "${WORKDIR}/git"` or `"${UNPACKDIR}/git"` is fatal: remove the
    assignment, the bitbake.conf default works.
  * Any `${WORKDIR}` in the unexpanded `S` is fatal: "S should be set relative
    to UNPACKDIR".
* `meta/classes-global/base.bbclass` `base_do_unpack` calls
  `fetcher.unpack(d.getVar('UNPACKDIR'))`; `do_unpack[cleandirs] = "${UNPACKDIR}"`.
* bitbake `lib/bb/fetch2/git.py` (2.18; renamed `lib/bb/fetch/git.py` in 2.20,
  same logic), `unpack()`:
  * default destination is
    `(BB_GIT_DEFAULT_DESTSUFFIX or "git") + "/"` below the unpack dir.
  * `subpath=<path>` limits the checkout to that subtree (implemented as
    `git read-tree <rev>:<path>` + `checkout-index`) and changes the default
    destination to `basename(subpath)/`, ignoring `BB_GIT_DEFAULT_DESTSUFFIX`.
  * `destsuffix=<dir>` overrides the destination; `subdir=<dir>` sets it to
    `<dir>` itself.

Consequence for picowl: with `subpath=picowl` the tree lands in
`${UNPACKDIR}/picowl`, which differs from the default `S`
(`${UNPACKDIR}/picowl-0.1+git`), so `picowl_git.bb` sets
`S = "${UNPACKDIR}/picowl"`. `LIC_FILES_CHKSUM` is relative to `S`. Once picowl
has its own repository, switch `SRC_URI` (see the comment in the recipe) and
delete the `S` line. `SRCPV` is empty in both releases, so `PV = "0.1+git"` with
`SRCREV = "${AUTOREV}"` is sufficient; pin `SRCREV` for releases.

## Status

Not built (no cross toolchain here), but both recipes were parse-checked
with bitbake against each release, using only `openembedded-core/meta` plus
this layer, `MACHINE = "qemuarm"`, `DISTRO_FEATURES:append = " wayland systemd"`:

* wrynose: openembedded-core `wrynose` + bitbake `2.18` (2.18.0)
* blacksail: openembedded-core `master` + bitbake `master` (2.20.0)

`bitbake -p` parses without errors or warnings in both, and
`bitbake-getvar -r <recipe>` gives:

| Variable | picowl | wlroots |
|---|---|---|
| `S` | `${WORKDIR}/sources/picowl` | `${WORKDIR}/sources/wlroots-0.19.0` |
| `PV` | `0.1+git` | `0.19.0` |
| `SYSTEMD_AUTO_ENABLE` | `disable` | n/a |
| `ARM_INSTRUCTION_SET` | `arm` | machine default |
| `FILES:${PN}` | default | includes `${libdir}/*-0.19.so` |

The wlroots SRCREV `13a62a23a258d96f902c740310d5c7c59784a4d1` is the `0.19.0`
tag commit and is on the `0.19` branch (checked in a local wlroots clone;
the fetch itself was not run because gitlab.freedesktop.org was not reachable
from the sandbox). A fetch, compile and package run has not been done, so check
the `SOLIBS` packaging of `libwlroots-0.19.so` on a real build.
If `picowl/LICENSE` changes, update the `LIC_FILES_CHKSUM` md5 in `picowl_git.bb`.

## Cross-check against the project (latest)

* picowl `DEPENDS` now matches `meson.build` (wlroots-0.19, wayland-server, wayland-protocols, xkbcommon, pixman-1, libdrm, native wayland-scanner), including libinput (picowl itself uses its touch calibration matrix).
* `-Dtests=false` is set; `PACKAGECONFIG[systemd]` maps the `systemd` DISTRO_FEATURE to `-Dsystemd=enabled/disabled`. meson installs the unit itself, so the recipe no longer copies it.
* `/etc/picowl.ini` is still installed from `data/picowl.ini.example`; LICENSE md5 and wlroots 0.19.0 options were re-verified. `bitbake -p` parses cleanly (blacksail, wrynose).

## wlroots patches and libinput (zero-copy / rotation work)

* `recipes-graphics/wlroots/files/` holds five patches (`0001` pixman dmabuf mmap, `0002` pixman fast paths, `0003` DRM hardware rotation and copy-type, `0004` DRM lease: overlay planes and the grant fix, `0005` scene: update a buffer node when its opacity changes). `wlroots_0.19.0.bb` has `FILESEXTRAPATHS:prepend := "${THISDIR}/files:"` and lists them as `file://` entries in `SRC_URI`. They are byte-identical to `subprojects/packagefiles/wlroots/` (used by the meson wrap `diff_files`); keep both in sync (`diff -r`).
* `libinput` is back in the picowl `DEPENDS` (and `-DPW_HAVE_LIBINPUT` is set by meson): picowl uses the libinput calibration matrix directly for touch rotation. The comment above `RDEPENDS` in `picowl_git.bb` still says libinput is used inside wlroots only; that is stale (libseat and udev are wlroots-only).
* The OE recipes were not built here (no bitbake); check the patches apply in `do_patch`.

## wvkbd-ipaq (on-screen keyboard)

* `recipes-graphics/wvkbd/wvkbd-ipaq_git.bb` fetches upstream wvkbd at the pinned commit `e14b53aff4fd1f471add6b21b3885c2cff945509` and applies the eleven patches in `recipes-graphics/wvkbd/files/`. It builds with meson, `-Dkbd_layout=ipaq`, and installs the binary `wvkbd-ipaq` and its man page. Design: `doc/design/osk.md`, part 1.
* The patches are copies of `subprojects/packagefiles/wvkbd/` (used by `subprojects/wvkbd.wrap`) with one extra line, `Upstream-Status: Pending`, directly before the `---` separator, because the OE patch-status QA rejects patches without it. `tests/patches-sync.sh` (meson test `wvkbd-patches-sync`) fails on any other difference. When the series changes, regenerate the packagefiles copies with `git format-patch` and insert the line again in the OE copies.
* `PACKAGECONFIG[pango]` (off by default) switches the text backend from the built-in bitmap fonts to pango (`-Dtext=pango`, adds pango and fontconfig). cairo itself may pull in fontconfig depending on the cairo recipe, so check the image if the bitmap build is meant to be free of it.
* Launch with `wvkbd-ipaq --hidden --auto`: picowl starts and supervises it from `[osk] cmd = /usr/bin/wvkbd-ipaq --hidden --auto` and has a built-in `[layer.wvkbd]` rule with `hold_action = none`. The keyboard must use layer-shell keyboard interactivity `none` (the patched source requests it; to be confirmed on a board). `picowl_git.bb` does not recommend the keyboard and ships no `[osk]` section yet.
* `LICENSE = "GPL-3.0-only & MIT"` is the conservative reading of an unclear upstream statement; the recipe comment lists what is unsettled.
* The wrap and `meson setup -Dosk_tests=enabled` build the patched tree for the picowl test run only (they need the network for the clone). Install with `meson install --skip-subprojects` so that nothing of it is installed.
* Not verified: the recipe was not parsed or built with bitbake (no bitbake here), the fetch of the upstream repository was not run, and the patches were applied only in the earlier verification checkout, not by `do_patch`.
