SUMMARY = "picowl, a tiny wlroots-based Wayland compositor for old iPAQ PDAs"
DESCRIPTION = "Single-purpose Wayland compositor with a pixman renderer, \
RGB565 output and damage-only commits, aimed at GPU-less, FPU-less devices."
HOMEPAGE = "https://github.com/Angstrom-distribution/Tiny-UI-research"
SECTION = "graphics"
LICENSE = "MIT"
# Relative to S, i.e. the picowl directory itself.
LIC_FILES_CHKSUM = "file://LICENSE;md5=982d87f7020e9e27c00c0a116489f7ef"

# picowl currently lives in the 'picowl' subfolder of the research repository.
# The git fetcher's subpath parameter checks out only that subtree, and (see
# bitbake fetch2/git.py unpack) it then unpacks into
# ${UNPACKDIR}/<basename of subpath>, i.e. ${UNPACKDIR}/picowl.
SRC_URI = "git://github.com/Angstrom-distribution/Tiny-UI-research.git;protocol=https;branch=main;subpath=picowl"
# Once picowl moves to its own repository use this instead, and drop the S
# assignment below (the default S = ${UNPACKDIR}/${BP} then works):
# SRC_URI = "git://github.com/Angstrom-distribution/picowl.git;protocol=https;branch=main"
S = "${UNPACKDIR}/picowl"

# Tracks the branch head. Pin SRCREV to a commit hash (and PV to a real
# version) for releases and reproducible builds.
PV = "0.1+git"
SRCREV = "${AUTOREV}"

inherit meson pkgconfig features_check systemd

REQUIRED_DISTRO_FEATURES = "wayland"

DEPENDS = " \
    wlroots \
    wayland \
    wayland-native \
    wayland-protocols \
    libxkbcommon \
    pixman \
    libdrm \
    libinput \
"
# libwlroots, libwayland-server, libxkbcommon, libpixman and libdrm are picked
# up at runtime through the automatic shlibs dependencies. libinput, libseat
# and udev are used inside wlroots only, so wlroots pulls them in.
RDEPENDS:${PN} += "seatd xkeyboard-config"

# meson.options: 'tests' (boolean) and 'systemd' (feature). The unit file is
# installed by meson itself (into systemdsystemunitdir of systemd.pc), so no
# manual install is done for it below.
EXTRA_OEMESON += "-Dtests=false -Dudev=disabled -Dudevrulesdir=${nonarch_base_libdir}/udev/rules.d"
PACKAGECONFIG ??= "${@bb.utils.filter('DISTRO_FEATURES', 'systemd', d)}"
PACKAGECONFIG[systemd] = "-Dsystemd=enabled,-Dsystemd=disabled,systemd"

# The render paths are hand-tuned for ARM state; do not build with Thumb.
ARM_INSTRUCTION_SET:arm = "arm"

do_install:append() {
    install -d ${D}${sysconfdir}
    install -m 0644 ${S}/data/picowl.ini.example ${D}${sysconfdir}/picowl.ini
}

# Backlight udev rule (installed by meson into the udev rules dir).
FILES:${PN} += "${nonarch_base_libdir}/udev/rules.d/90-picowl-backlight.rules"

CONFFILES:${PN} += "${sysconfdir}/picowl.ini"

SYSTEMD_SERVICE:${PN} = "picowl.service"
# Not enabled by default: it takes over the display (conflicts with other
# session managers). Enable with 'systemctl enable picowl' or set
# SYSTEMD_AUTO_ENABLE:pn-picowl = "enable" in the distro/machine config.
SYSTEMD_AUTO_ENABLE = "disable"
