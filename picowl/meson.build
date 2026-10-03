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

wlroots = dependency('wlroots-0.19', fallback: ['wlroots', 'wlroots_dep'])
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols', version: '>=1.32')
xkbcommon = dependency('xkbcommon')
pixman = dependency('pixman-1')
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)
wayland_scanner_dep = dependency('wayland-scanner', native: true)

wayland_scanner = find_program(
  wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner'),
  native: true,
)
wl_proto_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')

# Server protocol headers required by the wlroots 0.19 public headers we use
# (xdg-shell, layer-shell, output-power-management). The other interfaces
# (virtual-keyboard, foreign-toplevel, idle-notify, presentation-time,
# viewporter, single-pixel-buffer, xdg-activation, xdg-decoration) are
# implemented entirely inside libwlroots and need no headers here.
protocol_xmls = {
  'xdg-shell': wl_proto_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml',
  'wlr-layer-shell-unstable-v1': files('protocols/wlr-layer-shell-unstable-v1.xml'),
  'wlr-output-power-management-unstable-v1': files('protocols/wlr-output-power-management-unstable-v1.xml'),
}

protocol_headers = []
foreach name, xml : protocol_xmls
  protocol_headers += custom_target(
    name + '-server-header',
    input: xml,
    output: name + '-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

sources = files(
  'src/main.c',
  'src/server.c',
  'src/config.c',
  'src/output.c',
  'src/view.c',
  'src/layer.c',
  'src/input.c',
  'src/idle.c',
)

pwcursorfit = static_library(
  'pwcursorfit',
  'src/cursorfit.c', 'src/cursor-builtin.c',
  include_directories: include_directories('src'),
)

picowl_exe = executable(
  'picowl',
  sources, protocol_headers,
  dependencies: [wlroots, wayland_server, xkbcommon, pixman, libdrm],
  include_directories: include_directories('src'),
  install: true,
)

install_data('data/picowl.ini.example', install_dir: get_option('datadir') / 'picowl')

systemd = dependency('systemd', required: get_option('systemd'))
if systemd.found()
  unitdir = systemd.get_variable(pkgconfig: 'systemdsystemunitdir', default_value: get_option('prefix') / 'lib/systemd/system')
  install_data('data/picowl.service', install_dir: unitdir)
endif

if get_option('tools')
  subdir('tools')
endif

if get_option('tests')
  subdir('tests')
endif
