test_config_exe = executable(
  'test-config',
  'test-config.c',
  '../src/config.c',
  '../src/rotate.c',
  '../src/copytype.c',
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

pb_xml = files('../protocols/picowl-buffer-v1.xml')
pb_client_h = custom_target('picowl-buffer-client-header',
  input: pb_xml, output: 'picowl-buffer-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
pb_client_c = custom_target('picowl-buffer-client-code',
  input: pb_xml, output: 'picowl-buffer-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
dmabuf_xml = wl_proto_dir / 'unstable' / 'linux-dmabuf' / 'linux-dmabuf-unstable-v1.xml'
dmabuf_client_h = custom_target('linux-dmabuf-client-header',
  input: dmabuf_xml, output: 'linux-dmabuf-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
dmabuf_client_c = custom_target('linux-dmabuf-client-code',
  input: dmabuf_xml, output: 'linux-dmabuf-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

inhibit_xml = wl_proto_dir / 'unstable' / 'idle-inhibit' / 'idle-inhibit-unstable-v1.xml'
inhibit_client_h = custom_target('idle-inhibit-client-header',
  input: inhibit_xml, output: 'idle-inhibit-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
inhibit_client_c = custom_target('idle-inhibit-client-code',
  input: inhibit_xml, output: 'idle-inhibit-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

lease_xml = wl_proto_dir / 'staging' / 'drm-lease' / 'drm-lease-v1.xml'
lease_client_h = custom_target('drm-lease-client-header',
  input: lease_xml, output: 'drm-lease-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
lease_client_c = custom_target('drm-lease-client-code',
  input: lease_xml, output: 'drm-lease-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

pw_test_client = executable('pw-test-client',
  'pw-test-client.c', xdg_client_h, xdg_client_c,
  pb_client_h, pb_client_c, dmabuf_client_h, dmabuf_client_c,
  inhibit_client_h, inhibit_client_c,
  dependencies: [wayland_client])

# Needs root, the vkms module and /dev/dri; skips (exit 77) without them.
pw_lease_client = executable('pw-lease-client',
  'pw-lease-client.c', xdg_client_h, xdg_client_c,
  lease_client_h, lease_client_c,
  dependencies: [wayland_client, libdrm])

test('smoke', find_program('sh'),
  args: [files('smoke.sh'), picowl_exe, pw_test_client],
  timeout: 30)

test_touchhold_exe = executable('test-touchhold',
  'test-touchhold.c',
  '../src/touchhold.c',
  include_directories: include_directories('../src'),
)
test('touchhold', test_touchhold_exe)

test_cursorfit_exe = executable('test-cursorfit',
  'test-cursorfit.c',
  link_with: pwcursorfit,
  include_directories: include_directories('../src'),
)
test('cursorfit', test_cursorfit_exe)

test_cursor_builtin_exe = executable('test-cursor-builtin',
  'test-cursor-builtin.c',
  link_with: pwcursorfit,
  include_directories: include_directories('../src'),
)
test('cursor-builtin', test_cursor_builtin_exe)

test_inc = include_directories('../src')

test('leasepolicy', executable('test-leasepolicy', 'test-leasepolicy.c',
  '../src/leasepolicy.c', protocol_headers, dependencies: [wlroots, wayland_server],
  include_directories: test_inc))
test('rotate', executable('test-rotate', 'test-rotate.c', '../src/rotate.c',
  dependencies: [wayland_server], include_directories: test_inc))
test('copytype', executable('test-copytype', 'test-copytype.c', '../src/copytype.c',
  include_directories: test_inc))
test('copyrel', executable('test-copyrel', 'test-copyrel.c', '../src/copyrel.c',
  include_directories: test_inc))
test('pixman-pass', executable('test-pixman-pass', 'test-pixman-pass.c',
  dependencies: [wlroots, pixman, wayland_server, libdrm], include_directories: test_inc))
test('pixman-dmabuf', executable('test-pixman-dmabuf', 'test-pixman-dmabuf.c',
  dependencies: [wlroots, pixman, wayland_server, libdrm], include_directories: test_inc))

test('rss', find_program('sh'),
  args: [files('rss.sh'), picowl_exe, pw_test_client],
  env: {'PW_RSS_CEILING_KB': get_option('rss_ceiling_kb').to_string()},
  timeout: 30)

foreach t : ['backlight', 'powersupply', 'dim']
  test(t, executable('test-' + t, 'test-' + t + '.c', '../src/' + t + '.c',
    dependencies: [wlroots, pixman, wayland_server, libdrm], include_directories: test_inc))
endforeach

test('power-e2e', find_program('sh'),
  args: [files('power-e2e.sh'), picowl_exe, pw_test_client],
  timeout: 90)

test('lease-vkms', find_program('sh'),
  args: [files('lease-vkms.sh'), picowl_exe, pw_lease_client],
  suite: 'vkms',
  timeout: 120)
