# picowl OpenEmbedded layer

Mini-layer (`picowl-layer`) to build picowl for the iPAQs with Angstrom.
Compatible with the OE releases `wrynose` and `blacksail`
(`LAYERSERIES_COMPAT_picowl-layer = "wrynose blacksail"`, depends on `core`).

Contents:

| Recipe | Why |
|---|---|
| `recipes-graphics/wlroots/wlroots_0.19.0.bb` | oe-core, meta-openembedded and meta-angstrom ship no wlroots recipe for these releases. Minimal build: DRM and libinput backends, pixman renderer only, no GLES2/Vulkan/GBM/Xwayland. |
| `recipes-graphics/picowl/picowl_git.bb` | picowl itself, built from the `picowl` subfolder of the research repository. Installs `picowl.service` (not auto-enabled) and `/etc/picowl.ini` (conffile). |

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

* picowl `DEPENDS` now matches `meson.build` (wlroots-0.19, wayland-server, wayland-protocols, xkbcommon, pixman-1, libdrm, native wayland-scanner); libinput was dropped (only wlroots uses it).
* `-Dtests=false` is set; `PACKAGECONFIG[systemd]` maps the `systemd` DISTRO_FEATURE to `-Dsystemd=enabled/disabled`. meson installs the unit itself, so the recipe no longer copies it.
* `/etc/picowl.ini` is still installed from `data/picowl.ini.example`; LICENSE md5 and wlroots 0.19.0 options were re-verified. `bitbake -p` parses cleanly (blacksail, wrynose).
