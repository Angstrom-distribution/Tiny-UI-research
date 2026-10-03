SUMMARY = "Modular Wayland compositor library"
DESCRIPTION = "wlroots is a library for building Wayland compositors. This \
recipe builds a minimal configuration (DRM and libinput backends, pixman \
renderer only, no GLES2/Vulkan/GBM, no Xwayland) for GPU-less devices."
HOMEPAGE = "https://gitlab.freedesktop.org/wlroots/wlroots"
BUGTRACKER = "https://gitlab.freedesktop.org/wlroots/wlroots/-/issues"
SECTION = "graphics"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=89e064f90bcb87796ca335cbd2ce4179"

# S defaults to ${UNPACKDIR}/${BP} and BB_GIT_DEFAULT_DESTSUFFIX is ${BP}, so
# no S assignment is needed (see ../../../README.md).
SRC_URI = "git://gitlab.freedesktop.org/wlroots/wlroots.git;protocol=https;branch=0.19;tag=${PV}"

# picowl patches (byte-identical copies of picowl/subprojects/packagefiles/wlroots)
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += " \
    file://0001-pixman-read-dmabuf-client-buffers-via-mmap.patch \
    file://0002-pixman-pass-memcpy-and-fill-fast-paths.patch \
    file://0003-drm-hardware-rotation-and-copy-type.patch \
    file://0004-drm-lease-overlay-planes.patch \
"
SRCREV = "13a62a23a258d96f902c740310d5c7c59784a4d1"

UPSTREAM_CHECK_GITTAGREGEX = "^(?P<pver>\d+\.\d+\.\d+)$"

inherit meson pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"

# hwdata is only needed at build time (pnp.ids), and meson looks it up as a
# native dependency.
DEPENDS = " \
    wayland \
    wayland-native \
    wayland-protocols \
    libdrm \
    libxkbcommon \
    pixman \
    libinput \
    seatd \
    udev \
    libdisplay-info \
    hwdata-native \
"

# wlroots builds with -Werror by default; do not break on newer compilers.
EXTRA_OEMESON += " \
    -Dwerror=false \
    -Dexamples=false \
    -Dxwayland=disabled \
    -Drenderers=[] \
    -Dallocators=[] \
    -Dbackends=drm,libinput \
    -Dsession=enabled \
    -Dcolor-management=disabled \
    -Dlibliftoff=disabled \
    -Dxcb-errors=disabled \
"

# The shared library is installed as libwlroots-0.19.so with no soversion
# suffix; keep it in the runtime package instead of -dev.
WLROOTS_API = "${@'.'.join(d.getVar('PV').split('.')[0:2])}"
SOLIBS = "-${WLROOTS_API}.so"
FILES_SOLIBSDEV = ""
INSANE_SKIP:${PN} += "dev-so"
