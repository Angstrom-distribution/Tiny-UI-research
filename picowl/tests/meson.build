test_config_exe = executable(
  'test-config',
  'test-config.c',
  '../src/config.c',
  protocol_headers,
  dependencies: [wlroots, wayland_server, xkbcommon, libdrm],
  include_directories: include_directories('../src'),
)

test('config', test_config_exe,
  workdir: meson.project_source_root(),
)

wayland_client = dependency('wayland-client')
xdg_xml = wl_proto_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'
xdg_client_h = custom_target('xdg-shell-client-header',
  input: xdg_xml, output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xdg_client_c = custom_target('xdg-shell-client-code',
  input: xdg_xml, output: 'xdg-shell-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

pw_test_client = executable('pw-test-client',
  'pw-test-client.c', xdg_client_h, xdg_client_c,
  dependencies: [wayland_client])

test('smoke', find_program('sh'),
  args: [files('smoke.sh'), picowl_exe, pw_test_client],
  timeout: 30)
