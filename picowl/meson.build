project(
  'picowl', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.3',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'],
)

add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
  '-DPICOWL_VERSION="@0@"'.format(meson.project_version()),
  language: 'c'
)

cc = meson.get_compiler('c')

wlroots = dependency('wlroots-0.19', fallback: ['wlroots', 'wlroots'],
  default_options: ['renderers=', 'allocators=', 'backends=drm,libinput',
    'xwayland=disabled', 'examples=false'])

# The subproject build applies the patches itself and meson cannot run compiler
# checks against an internal dependency, so only probe an installed wlroots.
if wlroots.type_name() != 'internal'
  # Verify that wlroots has the picowl patches (checks for wlr_drm_connector_set_copy_type from patch 0003)
  if not cc.has_header_symbol('wlr/backend/drm.h', 'wlr_drm_connector_set_copy_type',
      dependencies: wlroots, prefix: '#define WLR_USE_UNSTABLE\n#include <wlr/backend/drm.h>')
    error('wlroots-0.19 lacks the picowl patches (0001-pixman, 0002-memcpy, 0003-rotation). ' +
          'Use --force-fallback-for=wlroots or install the patched build.')
  endif
  # Patch 0004 fixes a use-after-free in the lease grant, so leasing needs it.
  if not cc.has_header_symbol('wlr/backend/drm.h', 'WLR_DRM_LEASE_OVERLAY_PLANES',
      dependencies: wlroots, prefix: '#define WLR_USE_UNSTABLE\n#include <wlr/backend/drm.h>')
    error('wlroots-0.19 lacks the picowl patch 0004-drm-lease-overlay-planes. ' +
          'Use --force-fallback-for=wlroots or install the patched build.')
  endif
endif
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols', version: '>=1.32')
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
libdrm = dependency('libdrm')
libinput = dependency('libinput', required: false)
# Touch calibration reads the ABS ranges from the device node libinput found.
libudev = dependency('libudev', required: libinput.found())
if libinput.found()
  add_project_arguments('-DPW_HAVE_LIBINPUT', language: 'c')
endif
wayland_scanner_dep = dependency('wayland-scanner', native: true)

wayland_scanner = find_program(
  wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner'),
  native: true,
)
wl_proto_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')

# Server protocol headers required by the wlroots 0.19 public headers we use
# (xdg-shell, layer-shell, output-power-management, content-type, cursor-shape, tablet,
# text-input, input-method). The other interfaces
# (virtual-keyboard, foreign-toplevel, idle-notify, presentation-time,
# viewporter, single-pixel-buffer, xdg-activation, xdg-decoration) are
# implemented entirely inside libwlroots and need no headers here.
protocol_xmls = {
  'xdg-shell': wl_proto_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml',
  'wlr-layer-shell-unstable-v1': files('protocols/wlr-layer-shell-unstable-v1.xml'),
  'wlr-output-power-management-unstable-v1': files('protocols/wlr-output-power-management-unstable-v1.xml'),
  'picowl-buffer-v1': files('protocols/picowl-buffer-v1.xml'),
  'tablet-v2': wl_proto_dir / 'unstable' / 'tablet' / 'tablet-unstable-v2.xml',
  'cursor-shape-v1': wl_proto_dir / 'staging' / 'cursor-shape' / 'cursor-shape-v1.xml',
  'content-type-v1': wl_proto_dir / 'staging' / 'content-type' / 'content-type-v1.xml',
  'text-input-unstable-v3': wl_proto_dir / 'unstable' / 'text-input' / 'text-input-unstable-v3.xml',
  # Not in wayland-protocols (only input-method-unstable-v1 is); wlroots ships
  # v2 in its own protocol/ directory, so picowl carries a copy of that file.
  'input-method-unstable-v2': files('protocols/input-method-unstable-v2.xml'),
}
# wlr-screencopy-unstable-v1 is not listed: wlr_screencopy_v1.h includes no
# generated header and libwlroots provides the interface.

protocol_headers = []
foreach name, xml : protocol_xmls
  protocol_headers += custom_target(
    name + '-server-header',
    input: xml,
    output: name + '-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

picowl_buffer_code = custom_target(
  'picowl-buffer-v1-server-code',
  input: files('protocols/picowl-buffer-v1.xml'),
  output: 'picowl-buffer-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

sources = files(
  'src/main.c',
  'src/server.c',
  'src/config.c',
  'src/output.c',
  'src/view.c',
  'src/tile.c',
  'src/layer.c',
  'src/input.c',
  'src/grab.c',
  'src/pointercal.c',
  'src/imrelay.c',
  'src/implace.c',
  'src/tablet.c',
  'src/idle.c',
  'src/touchhold.c',
  'src/cursor.c',
  'src/cursorshape.c',
  'src/rotate.c',
  'src/copytype.c',
  'src/copyrel.c',
  'src/zerocopy.c',
  'src/zbproto.c',
  'src/zbquota.c',
  'src/mem.c',
  'src/backlight.c',
  'src/powersupply.c',
  'src/dim.c',
  'src/power.c',
  'src/lease.c',
  'src/leasepolicy.c',
  'src/osk.c',
  'src/oskstate.c',
  'src/home.c',
)

pwcursorfit = static_library(
  'pwcursorfit',
  'src/cursorfit.c', 'src/cursor-builtin.c',
  include_directories: include_directories('src'),
)

picowl_exe = executable(
  'picowl',
  sources, protocol_headers, picowl_buffer_code,
  link_with: pwcursorfit,
  dependencies: [wlroots, wayland_server, xkbcommon, pixman, libdrm, libinput, libudev,
    cc.find_library('m', required: false)],
  include_directories: include_directories('src'),
  install: true,
)

install_data('data/picowl.ini.example', install_dir: get_option('datadir') / 'picowl')

systemd = dependency('systemd', required: get_option('systemd'))
if systemd.found()
  unitdir = systemd.get_variable(pkgconfig: 'systemdsystemunitdir', default_value: get_option('prefix') / 'lib/systemd/system')
  install_data('data/picowl.service', install_dir: unitdir)
endif

udev = dependency('udev', required: get_option('udev'))
udevrulesdir = get_option('udevrulesdir')
if udevrulesdir == ''
  udevrulesdir = '/usr/lib/udev/rules.d'
  if udev.found()
    udevdir = udev.get_variable(pkgconfig: 'udevdir', default_value: '')
    if udevdir != ''
      udevrulesdir = udevdir / 'rules.d'
    endif
  endif
endif
if udev.found() or get_option('udevrulesdir') != ''
  install_data('data/90-picowl-backlight.rules', install_dir: udevrulesdir)
endif

if get_option('tools')
  subdir('tools')
endif

if get_option('tests')
  subdir('tests')
endif

# wvkbd-ipaq is a separate package, so its own tests only run when asked for.
# The patched tree is built with the bitmap text backend (what the image
# uses) and without the man page so that scdoc is not needed. meson runs the
# registered tests of a subproject together with ours.
if get_option('osk_tests').enabled()
  subproject('wvkbd', default_options: [
    'kbd_layout=ipaq',
    'text=bitmap',
    'tests=true',
    'man=disabled',
  ])
endif
