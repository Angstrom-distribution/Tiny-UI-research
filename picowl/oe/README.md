# picowl OpenEmbedded layer

Mini-layer (`picowl-layer`) to build picowl and its companion pieces for the iPAQs with Angstrom. The packaging overview is in [README.md](../README.md#openembedded); this file has the details.

`conf/layer.conf` declares `LAYERDEPENDS_picowl-layer = "core"`, `LAYERSERIES_COMPAT_picowl-layer = "wrynose blacksail"` and `BBFILE_PRIORITY_picowl-layer = "6"`.

No build from this layer is recorded in this repository. The last parse check is described under [Status](#status).

## Recipes

| Recipe | Why |
|---|---|
| `recipes-graphics/wlroots/wlroots_0.19.0.bb` | oe-core, meta-openembedded and meta-angstrom ship no wlroots recipe for these releases, and picowl needs the five patches in `recipes-graphics/wlroots/files/`. Minimal build: DRM and libinput backends, pixman renderer only, no GLES2/Vulkan/GBM/Xwayland. |
| `recipes-graphics/picowl/picowl_git.bb` | picowl itself, built from the `picowl` subfolder of the research repository. Packages `picowl` and `picowl-panel`. |
| `recipes-graphics/wvkbd/wvkbd-ipaq_git.bb` | The on-screen keyboard: upstream wvkbd plus an eleven patch series, built with the `ipaq` layout. See [wvkbd-ipaq](#wvkbd-ipaq-on-screen-keyboard). |
| `recipes-graphics/havoc/files/` | Two patches and no recipe. The layer has neither a havoc recipe nor a bbappend. See [recipes-graphics/havoc/README.md](recipes-graphics/havoc/README.md). |

## What picowl_git.bb installs

* `picowl.service`, installed by meson (`PACKAGECONFIG[systemd]` maps the `systemd` DISTRO_FEATURE to `-Dsystemd=enabled` or `disabled`). The recipe sets `SYSTEMD_AUTO_ENABLE = "disable"`, so the unit is not enabled: it takes over the display and conflicts with `getty@tty1`.
* `/etc/picowl.ini`, a conffile copied from `data/picowl.ini.example`. In that file `[osk]` and `[autostart]` are commented out, so a fresh image starts no keyboard and no panel until the file is edited.
* The backlight udev rule `90-picowl-backlight.rules`, installed into `${nonarch_base_libdir}/udev/rules.d` (the recipe passes `-Dudev=disabled -Dudevrulesdir=...`, so the directory does not depend on `udev.pc`).
* The `picowl` binary and the two host tools of the `tools` meson option, which defaults to on and which the recipe does not turn off.
* `picowl-panel` in a package of its own, `picowl-panel`, so an image can leave it out. `PACKAGECONFIG[panel]` (on by default, needs `alsa-lib` for the mixer) builds it. It is a separate client: the image installs it and `[autostart] cmd = picowl-panel` in `picowl.ini` starts it.
* `RRECOMMENDS:picowl-panel = "liberation-fonts"`. Only the smooth panel style reads a TrueType font (`--style auto` picks it from 150 ppi, which is the hx4700's 480x640). It looks for `LiberationSans-Bold.ttf` and `LiberationSans-Regular.ttf` under `/usr/share/fonts/ttf` and `/usr/share/fonts/truetype/liberation`, then `DejaVuSans.ttf` under `/usr/share/fonts/TTF` (`panel/panel-font.c`), and without a file it uses a built-in 5x7 bitmap font. The crisp style that the QVGA boards get has its fonts compiled in and never loads a file. One package serves all the boards, so the recipe keeps the recommendation. An image for QVGA boards only can drop it (`BAD_RECOMMENDATIONS` or `NO_RECOMMENDATIONS`) to save the space. That the `liberation-fonts` package installs the faces at those paths was not verified here.
* `RDEPENDS:picowl = "seatd xkeyboard-config"`. libwlroots, libwayland-server, libxkbcommon, libpixman and libdrm come in through the automatic shared library dependencies. libinput and libudev are linked by picowl itself (touch calibration matrix and the ABS ranges of the touch device); libseat is used inside wlroots only. `DEPENDS` matches `meson.build`: wlroots, wayland, wayland-native, wayland-protocols, libxkbcommon, pixman, libdrm, libinput, udev, plus alsa-lib for the panel.
* `-Dtests=false` is set, so the tests described in [README.md](../README.md#testing) are not built for the target.

## Adding the layer

```sh
bitbake-layers add-layer /path/to/Tiny-UI-research/picowl/oe
# or add the path to BBLAYERS in conf/bblayers.conf
bitbake picowl
```

## Machine and distro notes

* The distro must have the `wayland` DISTRO_FEATURE: `picowl_git.bb` and `wlroots_0.19.0.bb` set `REQUIRED_DISTRO_FEATURES = "wayland"` (`wvkbd-ipaq_git.bb` does not). `systemd` is needed for the unit file to be packaged. Example `local.conf`: `DISTRO_FEATURES:append = " wayland systemd"`.
* The `video` group must exist on the target for backlight control. The unit runs picowl with `SupplementaryGroups=video`, and the udev rule gives that group write access to the backlight `brightness` attribute.
* The sound device must be reachable for the panel's volume row. `picowl.service` runs as root, so the panel started from picowl can use it.
* ARM state: `picowl_git.bb` sets `ARM_INSTRUCTION_SET:arm = "arm"`, with the comment that the render paths are tuned for ARM state and must not be built with Thumb. The other recipes set nothing, so Thumb is the machine default for them. The advice to set `ARM_INSTRUCTION_SET = "arm"` for the machine or distro, or at least `ARM_INSTRUCTION_SET:pn-pixman = "arm"` and `ARM_INSTRUCTION_SET:pn-wlroots = "arm"`, rests on that comment. The reason is not measured: picowl has no ARM assembly of its own and no comparison of a Thumb and an ARM build is kept in the repository.
* No GPU: no `virtual/egl` or mesa is needed. wlroots is configured with `-Drenderers=[] -Dallocators=[]`, which leaves the pixman renderer and the dumb-buffer allocator. The recipe also passes `-Dbackends=drm,libinput`, `-Dsession=enabled`, `-Dxwayland=disabled`, `-Dcolor-management=disabled`, `-Dlibliftoff=disabled`, `-Dxcb-errors=disabled`, `-Dexamples=false` and `-Dwerror=false` (wlroots builds with `-Werror` by default and newer compilers would break it).
* Enable the service on the target with `systemctl enable picowl`, or set `SYSTEMD_AUTO_ENABLE:pn-picowl = "enable"`.
* wlroots 0.19 puts no upper bound on libdisplay-info, but oe-core carries libdisplay-info 0.4.0. If the wlroots build fails there, pin an older libdisplay-info (weston needed a patch for the same reason). Not verified: no build has been run.
* The wlroots shared library has no soversion (`libwlroots-0.19.so`). The recipe sets `SOLIBS = "-0.19.so"` and `FILES_SOLIBSDEV = ""` so that the library stays in the runtime package instead of `-dev`. An earlier version of the recipe also set `INSANE_SKIP:${PN} += "dev-so"`. Commit 8b0aaf8 removed it: the library is installed as a regular `libwlroots-0.19.so` and passes the QA checks without it. No recipe in the layer uses `INSANE_SKIP` or `HOSTTOOLS` now. Treat both as a last resort, only after the cause has been fixed or ruled out.
* `wlroots_0.19.0.bb` sets `PR = "r1"`. The library keeps its file name and `PV` when a patch changes it, and without a new revision opkg sees no upgrade on a board that already has the previous build. Bump `PR` whenever a wlroots patch is added or changed.

## wlroots patches

`recipes-graphics/wlroots/files/` holds the five patches (0001 pixman reads dmabuf client buffers via mmap, 0002 pixman pass memcpy and fill fast paths, 0003 DRM hardware rotation and copy-type, 0004 DRM lease overlay planes and the grant fix, 0005 scene: update a buffer node when its opacity changes), listed as `file://` entries in `SRC_URI` with `FILESEXTRAPATHS:prepend := "${THISDIR}/files:"`. They are byte-identical to `subprojects/packagefiles/wlroots/`, which the meson wrap uses through `diff_files`. No test checks this, so keep both in sync by hand; the `cmp` loop is in [subprojects/packagefiles/wlroots/README.md](../subprojects/packagefiles/wlroots/README.md). What each patch does is described there too.

The recipe fetches the `0.19` branch with `tag=${PV}` and `SRCREV = "13a62a23a258d96f902c740310d5c7c59784a4d1"`, the same release as the wrap's `revision = 0.19.0`. Whether that SRCREV is the `0.19.0` tag commit was checked in a local clone earlier and not repeated; the fetch has not been run.

## Recipe conventions: S, UNPACKDIR and WORKDIR

These notes come from reading openembedded-core branches `wrynose` and `blacksail` (`blacksail` equals `master`, `LAYERSERIES_CORENAMES = "blacksail"`) and bitbake branches `2.18` (wrynose) and `2.20` (blacksail). They were not re-read for this revision.

* `meta/conf/bitbake.conf` (both branches): `UNPACKDIR ??= "${WORKDIR}/sources"`, `BB_GIT_DEFAULT_DESTSUFFIX = "${BP}"`, `S = "${UNPACKDIR}/${BP}"`. Sources are unpacked into `UNPACKDIR`, never directly into `WORKDIR`, and a git checkout is placed in `${UNPACKDIR}/${BP}`, so a recipe fetching a plain git repository needs no `S`. `wlroots_0.19.0.bb` and `wvkbd-ipaq_git.bb` set none.
* `meta/classes-global/insane.bbclass`, `do_qa_unpack` and `do_recipe_qa`: `S = "${WORKDIR}"` (likewise `B` and `UNPACKDIR`) is fatal, `S = "${WORKDIR}/git"` or `"${UNPACKDIR}/git"` is fatal (remove the assignment), and any `${WORKDIR}` in the unexpanded `S` is fatal ("S should be set relative to UNPACKDIR").
* `meta/classes-global/base.bbclass`: `base_do_unpack` calls `fetcher.unpack(d.getVar('UNPACKDIR'))` and `do_unpack[cleandirs] = "${UNPACKDIR}"`.
* bitbake `lib/bb/fetch2/git.py` (2.18; `lib/bb/fetch/git.py` in 2.20, same logic), `unpack()`: the default destination is `(BB_GIT_DEFAULT_DESTSUFFIX or "git") + "/"` below the unpack directory. `subpath=<path>` limits the checkout to that subtree (`git read-tree <rev>:<path>` and `checkout-index`) and changes the default destination to `basename(subpath)/`, ignoring `BB_GIT_DEFAULT_DESTSUFFIX`. `destsuffix=<dir>` overrides the destination, `subdir=<dir>` sets it to `<dir>` itself.

Consequence for picowl: with `subpath=picowl` the tree lands in `${UNPACKDIR}/picowl`, which differs from the default `S` (`${UNPACKDIR}/picowl-0.1+git`), so `picowl_git.bb` sets `S = "${UNPACKDIR}/picowl"`. `LIC_FILES_CHKSUM` is relative to `S`; it is the md5 of `picowl/LICENSE` and has to change with that file (checked today: `982d87f7020e9e27c00c0a116489f7ef`). `SRCPV` is empty in both releases, so `PV = "0.1+git"` with `SRCREV = "${AUTOREV}"` is enough while the recipe tracks the branch head; pin `SRCREV` for releases. The recipe also carries the `SRC_URI` for the day picowl has its own repository; then the `S` line goes. See [roadmap](../doc/design/roadmap.md).

## wvkbd-ipaq (on-screen keyboard)

* `recipes-graphics/wvkbd/wvkbd-ipaq_git.bb` fetches upstream wvkbd at the pinned commit `e14b53aff4fd1f471add6b21b3885c2cff945509` (`PV = "0.21+git"`, the recipe comment calls the base v0.20-9) and applies the eleven patches in `recipes-graphics/wvkbd/files/`. It builds with meson and `-Dkbd_layout=ipaq -Dtests=false`, and installs the binary `wvkbd-ipaq` and its man page. Design: [doc/design/osk.md](../doc/design/osk.md), part 1. It does not set `REQUIRED_DISTRO_FEATURES`; the keyboard needs a compositor with layer-shell and `zwp_virtual_keyboard_v1`, plus `zwp_input_method_v2` for `--auto` (picowl has all three).
* The patches are copies of `subprojects/packagefiles/wvkbd/` (used by `subprojects/wvkbd.wrap`) with one extra line, `Upstream-Status: Pending`, directly before the `---` separator, because the OE patch-status QA rejects patches without it. `tests/patches-sync.sh` (meson test `wvkbd-patches-sync`) fails on any other difference. When the series changes, regenerate the packagefiles copies with `git format-patch` and insert the line again in the OE copies.
* `PACKAGECONFIG[pango]` (off by default) switches the text backend from the built-in bitmap fonts to pango (`-Dtext=pango`, adds pango and fontconfig). With pango the label cache lives in `/var/cache/wvkbd-ipaq`, which the package does not create, so the first start has to be allowed to create it or `--no-cache` is needed. cairo itself may pull in fontconfig depending on the cairo recipe, so check the image if the bitmap build is meant to be free of it.
* Launch with `wvkbd-ipaq --hidden --auto`. picowl starts and supervises it from `[osk] cmd = /usr/bin/wvkbd-ipaq --hidden --auto` and has a built-in `[layer.wvkbd]` rule with `hold_action = none`. `picowl_git.bb` does not recommend the keyboard, and the installed `/etc/picowl.ini` has `[osk]` commented out.
* The keyboard must use layer-shell keyboard interactivity `none`, so that tapping a key keeps the focus in the application. The patched source requests it (`zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, false)` in patch 0006). Not verified on a board; see [roadmap](../doc/design/roadmap.md).
* `LICENSE = "GPL-3.0-only & MIT"` is the conservative reading of an unclear upstream statement; the comment at the top of the recipe lists what is unsettled.
* The wrap and `meson setup -Dosk_tests=enabled` build the patched tree for the picowl test run only (they need the network for the clone). Install with `meson install --skip-subprojects` so that nothing of it is installed.

## Status

The recipes were parse-checked with bitbake against each release before the later changes (the `picowl-panel` package, the `RRECOMMENDS`, `PR`, the removal of `INSANE_SKIP`, the wvkbd-ipaq recipe). That check used only `openembedded-core/meta` plus this layer, `MACHINE = "qemuarm"` and `DISTRO_FEATURES:append = " wayland systemd"`: wrynose with openembedded-core `wrynose` and bitbake 2.18.0, blacksail with openembedded-core `master` and bitbake 2.20.0. Both parsed without errors or warnings, and `bitbake-getvar -r` gave `S = ${WORKDIR}/sources/picowl` for picowl, `${WORKDIR}/sources/wlroots-0.19.0` for wlroots, `PV` `0.1+git` and `0.19.0`, `SYSTEMD_AUTO_ENABLE = disable` and `ARM_INSTRUCTION_SET = arm` for picowl. The parse was not repeated for the current recipes, and no fetch, compile or packaging run has been done for any recipe. Not verified: that the patches apply in `do_patch`, the `SOLIBS` packaging of `libwlroots-0.19.so` on a real build, and the wvkbd-ipaq fetch. See [roadmap](../doc/design/roadmap.md).
