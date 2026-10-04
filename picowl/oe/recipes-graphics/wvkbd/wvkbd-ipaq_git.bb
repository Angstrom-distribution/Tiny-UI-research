# wvkbd-ipaq: the on-screen keyboard for picowl (doc/design/osk.md, part 1).
#
# Launching it: start it hidden, and let it show itself when a text field gets
# the focus (--auto needs the zwp_input_method_v2 relay, which picowl has).
# picowl starts and restarts it from its [osk] section and shows or hides it
# with the "osk" key action (SIGUSR2 shows, SIGUSR1 hides, SIGRTMIN toggles):
#
#     [osk]
#     cmd = /usr/bin/wvkbd-ipaq --hidden --auto
#
# picowl's built-in [layer.wvkbd] rule sets hold_action = none, because
# picowl's own tap-and-hold would fight the keyboard's long press.
#
# The keyboard must never take the keyboard focus away from the application
# it types into (that would also hide the keyboard again when the input
# method relay is used).
#
# The layer-shell keyboard interactivity of the keyboard must be none. The
# patched main.c requests none, but that is to be confirmed on a board: check
# that tapping a key keeps the focus in the application.
SUMMARY = "wvkbd on-screen keyboard patched for 240x320 handhelds (iPAQ layout, RGB565, bitmap text)"
DESCRIPTION = "wvkbd, a Wayland on-screen keyboard, built with the ipaq layout \
and a patch series that renders in RGB565, repaints only the changed keys and \
draws its labels from built-in bitmap fonts, so that it needs neither pango \
nor fontconfig at run time."
HOMEPAGE = "https://github.com/jjsullivan5196/wvkbd"
BUGTRACKER = "https://github.com/jjsullivan5196/wvkbd/issues"
SECTION = "graphics"

# What is not settled about the license, from the audit of the patched tree:
# - COPYING says "GPL v3.0" without "only" or "or later" and LICENSE is the
#   plain GPLv3 text, so -only is the safe reading; claiming or-later would
#   need a statement from upstream. The patched meson.build says GPL-3.0-only.
# - os-compatibility.[ch] and most protocol XML files are MIT (COPYING_WESTON
#   has the text), but proto/wlr-layer-shell-unstable-v1.xml carries a
#   different permissive "permission to use, copy, modify, distribute and sell"
#   notice. It is called MIT here for lack of a better single token; strictly it
#   is a separate SPDX identifier (HPND-sell-variant style, not verified).
# - COPYING lists only os-compatibility.[ch] as non-GPL and still mentions a
#   wld submodule that is gone, so it is stale; the generated protocol header
#   proto/input-method-unstable-v2-protocol.h is MIT as well.
# - Most source files (including shm_open.[ch], whose origin was not traced,
#   and all files the patches add) have no license header, so the repository
#   statement in COPYING is all there is.
# - The label font table (bmfont-data.c) is generated from the X.Org misc-fixed
#   BDF fonts 6x13 and 10x20, which say "Public domain font". The upstream
#   misc-misc license file was not checked, so this rests on that one line.
# COPYING (md5 45aad51fb536678b1b4906aa7a74d171) is deliberately left out of
# LIC_FILES_CHKSUM as asked, so a change to that statement is not noticed.
LICENSE = "GPL-3.0-only & MIT"
LIC_FILES_CHKSUM = " \
    file://LICENSE;md5=1ebbd3e34237af26da5dc08a4e440464 \
    file://COPYING_WESTON;md5=f21c9af4de068fb53b83f0b37d262ec3 \
"

# The git fetcher unpacks into ${UNPACKDIR}/${BP}, which is the default S in
# wrynose and blacksail, so no S is set here (see ../../README.md).
SRC_URI = "git://github.com/jjsullivan5196/wvkbd.git;protocol=https;branch=master"

# wvkbd-ipaq patch series on top of upstream e14b53a (v0.20-9), byte-identical
# to subprojects/packagefiles/wvkbd apart from the Upstream-Status line, which
# tests/patches-sync.sh checks.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += " \
    file://0001-Add-a-meson-build-alongside-the-Makefile.patch \
    file://0002-Block-signals-first-and-stop-leaking-keymap-fds.patch \
    file://0003-Render-the-keyboard-in-RGB565-when-the-compositor-of.patch \
    file://0004-Draw-labels-through-coverage-masks-with-built-in-bit.patch \
    file://0005-Pick-the-key-preview-popup-default-per-layout-and-ad.patch \
    file://0006-Keep-the-surfaces-on-hide-and-unmap-with-a-NULL-buff.patch \
    file://0007-Repaint-only-the-keys-that-changed-on-a-shift-or-lay.patch \
    file://0008-Add-the-ipaq-layout-for-240x320-handhelds.patch \
    file://0009-Add-a-persistent-label-mask-cache-for-the-pango-back.patch \
    file://0010-Take-the-pointer-position-from-wl_pointer.enter.patch \
    file://0011-Keep-a-pressed-key-highlighted-for-a-minimum-time.patch \
"
SRCREV = "e14b53aff4fd1f471add6b21b3885c2cff945509"
PV = "0.21+git"

inherit meson pkgconfig

# All protocol XML files wvkbd needs are vendored in its proto/ directory, so
# wayland-protocols is not needed. wayland-native provides wayland-scanner and
# scdoc-native builds the man page.
# cairo may pull in fontconfig by itself, depending on the cairo recipe and its
# PACKAGECONFIG; that is not under this recipe's control, so a bitmap build can
# still end up with fontconfig on the target through cairo.
DEPENDS = "wayland wayland-native libxkbcommon cairo scdoc-native"

# Off by default: the bitmap text backend needs neither pango nor fontconfig.
# With pango the labels are rendered once at start-up (or from the label cache
# in /var/cache/wvkbd-ipaq, which the package does not create, so the first
# start has to be allowed to create it or --no-cache is needed).
PACKAGECONFIG ??= ""
PACKAGECONFIG[pango] = "-Dtext=pango,-Dtext=bitmap,pango fontconfig"

# meson_options.txt: kbd_layout (the binary is named wvkbd-<layout>), text and
# tests. The tests are not built for the target: they run on the build host
# through picowl's osk_tests option.
EXTRA_OEMESON += "-Dkbd_layout=ipaq -Dtests=false"

# Runtime notes (nothing here is packaged or enforced):
# - A Wayland compositor with wlr-layer-shell and zwp_virtual_keyboard_v1 is
#   needed (picowl has both), plus zwp_input_method_v2 for --auto; RGB565 is
#   used when the compositor offers it.
# - The keymap comes from libxkbcommon, which needs xkeyboard-config at run
#   time: picowl_git.bb already has it in RDEPENDS.
