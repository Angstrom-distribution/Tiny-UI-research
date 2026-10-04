test_config_exe = executable(
  'test-config',
  'test-config.c',
  '../src/config.c',
  '../src/rotate.c',
  '../src/copytype.c',
  protocol_headers,
  dependencies: [wlroots, wayland_server, xkbcommon, libdrm,
    cc.find_library('m', required: false)],
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

ti_xml = wl_proto_dir / 'unstable' / 'text-input' / 'text-input-unstable-v3.xml'
ti_client_h = custom_target('text-input-client-header',
  input: ti_xml, output: 'text-input-unstable-v3-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
ti_client_c = custom_target('text-input-client-code',
  input: ti_xml, output: 'text-input-unstable-v3-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

im_xml = files('../protocols/input-method-unstable-v2.xml')
im_client_h = custom_target('input-method-client-header',
  input: im_xml, output: 'input-method-unstable-v2-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
im_client_c = custom_target('input-method-client-code',
  input: im_xml, output: 'input-method-unstable-v2-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

pw_test_client = executable('pw-test-client',
  'pw-test-client.c', xdg_client_h, xdg_client_c,
  pb_client_h, pb_client_c, dmabuf_client_h, dmabuf_client_c,
  inhibit_client_h, inhibit_client_c,
  dependencies: [wayland_client])

sc_xml = files('../protocols/wlr-screencopy-unstable-v1.xml')
sc_client_h = custom_target('wlr-screencopy-client-header',
  input: sc_xml, output: 'wlr-screencopy-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
sc_client_c = custom_target('wlr-screencopy-client-code',
  input: sc_xml, output: 'wlr-screencopy-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

pw_capture_client = executable('pw-capture-client',
  'pw-capture-client.c', sc_client_h, sc_client_c,
  dependencies: [wayland_client])

# Opt-in (PW_LEASE_VKMS=1), needs root, the vkms module and /dev/dri; skips
# (exit 77) otherwise.
pw_lease_client = executable('pw-lease-client',
  'pw-lease-client.c', xdg_client_h, xdg_client_c,
  lease_client_h, lease_client_c,
  dependencies: [wayland_client, libdrm])

# One process with two connections: an application and an input method.
pw_im_client = executable('pw-im-client',
  'pw-im-client.c', xdg_client_h, xdg_client_c,
  ti_client_h, ti_client_c, im_client_h, im_client_c,
  dependencies: [wayland_client])

ls_xml = files('../protocols/wlr-layer-shell-unstable-v1.xml')
ls_client_h = custom_target('wlr-layer-shell-client-header',
  input: ls_xml, output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
ls_client_c = custom_target('wlr-layer-shell-client-code',
  input: ls_xml, output: 'wlr-layer-shell-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

vk_xml = files('../protocols/virtual-keyboard-unstable-v1.xml')
vk_client_h = custom_target('virtual-keyboard-client-header',
  input: vk_xml, output: 'virtual-keyboard-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
vk_client_c = custom_target('virtual-keyboard-client-code',
  input: vk_xml, output: 'virtual-keyboard-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# One process, a few toplevels and a keyboard stand-in: the configure sizes of
# the tiled layout.
pw_tile_client = executable('pw-tile-client',
  'pw-tile-client.c', xdg_client_h, xdg_client_c, ls_client_h, ls_client_c,
  vk_client_h, vk_client_c,
  dependencies: [wayland_client, xkbcommon])

# A stand-in for wvkbd that logs the signals it gets, and a client which
# presses keys through virtual-keyboard so that a keybinding fires headless.
pw_osk_fake = executable('pw-osk-fake', 'pw-osk-fake.c')
pw_key_client = executable('pw-key-client',
  'pw-key-client.c', vk_client_h, vk_client_c,
  dependencies: [wayland_client, xkbcommon])

test('osk-e2e', find_program('sh'),
  args: [files('osk-e2e.sh'), picowl_exe, pw_key_client, pw_osk_fake],
  timeout: 90)

test('tile-e2e', find_program('sh'),
  args: [files('tile-e2e.sh'), picowl_exe, pw_tile_client],
  timeout: 60)

foreach mode : ['pixels', 'pixels-zone']
  test('pan-e2e-' + mode, find_program('sh'),
    args: [files('pan-e2e.sh'), picowl_exe, pw_tile_client, mode],
    timeout: 60)
endforeach

test('smoke', find_program('sh'),
  args: [files('smoke.sh'), picowl_exe, pw_test_client, pw_capture_client, pw_im_client, pw_key_client],
  timeout: 30)

test_touchhold_exe = executable('test-touchhold',
  'test-touchhold.c',
  '../src/touchhold.c',
  include_directories: include_directories('../src'),
)
test('touchhold', test_touchhold_exe)

test('home', executable('test-home', 'test-home.c', '../src/home.c',
  include_directories: include_directories('../src')))

test('oskstate', executable('test-oskstate', 'test-oskstate.c', '../src/oskstate.c',
  include_directories: include_directories('../src')))

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
test('cursorshape', executable('test-cursorshape', 'test-cursorshape.c',
  '../src/cursorshape.c', protocol_headers, dependencies: [wayland_server],
  include_directories: [test_inc, include_directories('..')]))
test('implace', executable('test-implace', 'test-implace.c', '../src/implace.c',
  include_directories: test_inc))
test('tile', executable('test-tile', 'test-tile.c', '../src/tile.c',
  include_directories: test_inc))
test('scenehit', executable('test-scenehit', 'test-scenehit.c', '../src/tile.c',
  dependencies: [wlroots, wayland_server], include_directories: test_inc))
test('grab', executable('test-grab', 'test-grab.c', '../src/grab.c',
  dependencies: [wlroots, wayland_server], include_directories: test_inc))
test('rotate', executable('test-rotate', 'test-rotate.c', '../src/rotate.c',
  dependencies: [wayland_server], include_directories: test_inc))
test('pointercal', executable('test-pointercal', 'test-pointercal.c', '../src/pointercal.c',
  dependencies: [cc.find_library('m', required: false)], include_directories: test_inc))
test('copytype', executable('test-copytype', 'test-copytype.c', '../src/copytype.c',
  include_directories: test_inc))
test('zbquota', executable('test-zbquota', 'test-zbquota.c', '../src/zbquota.c',
  include_directories: test_inc))
test('copyrel', executable('test-copyrel', 'test-copyrel.c', '../src/copyrel.c',
  include_directories: test_inc))
test('pixman-pass', executable('test-pixman-pass', 'test-pixman-pass.c',
  dependencies: [wlroots, pixman, wayland_server, libdrm], include_directories: test_inc))
test('pixman-dmabuf', executable('test-pixman-dmabuf', 'test-pixman-dmabuf.c',
  dependencies: [wlroots, pixman, wayland_server, libdrm], include_directories: test_inc))

test('bufproto', executable('test-bufproto', 'test-bufproto.c', '../src/zbproto.c',
  protocol_headers, picowl_buffer_code, pb_client_h,
  dependencies: [wayland_server, wayland_client, libdrm], include_directories: test_inc),
  args: [pw_test_client], timeout: 60)
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

# The two copies of the wvkbd patch series (OE recipe and meson wrap) must stay
# the same apart from the Upstream-Status header.
test('wvkbd-patches-sync', find_program('sh'),
  args: [files('patches-sync.sh'),
         meson.project_source_root() / 'oe/recipes-graphics/wvkbd/files',
         meson.project_source_root() / 'subprojects/packagefiles/wvkbd'])

# picowl-panel without wayland: value mapping, text, layout, touch, font,
# drawing and sysfs access.
test('panel', executable('test-panel', 'test-panel.c',
  link_with: pwpanel_lib, dependencies: libm,
  include_directories: include_directories('../panel')))

# picowl-panel against headless picowl. pw-fake-ctl is an alsa-lib control
# plugin that stands in for a sound card (no kernel driver needed), and
# pw-bare-server a server with a wl_output and nothing else.
if panel_built
  # alsa-lib's plugin macros need PIC to build the dynamic form of the symbol.
  pw_fake_ctl = shared_module('pw-fake-ctl', 'pw-fake-ctl.c',
    name_prefix: '', c_args: ['-DPIC'], dependencies: panel_alsa)
  pw_bare_server = executable('pw-bare-server', 'pw-bare-server.c',
    dependencies: [wayland_server])

  test('panel-e2e', find_program('sh'),
    args: [files('panel-e2e.sh'), picowl_exe, picowl_panel_exe, pw_test_client,
           pw_key_client, pw_fake_ctl, pw_bare_server],
    timeout: 120)
endif
