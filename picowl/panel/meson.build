# picowl-panel: a layer-shell client with a slim bar (clock, battery, backlight
# and volume buttons) and a slider row that pops out under it.

# stb_truetype is third-party and has its own warnings, so it is built apart.
pwpanel_stb = static_library('pwpanel-stb', 'panel-stb.c',
  override_options: ['warning_level=0', 'werror=false'])

# The parts that need neither wayland nor alsa, which are most of it: value
# mapping, layout, the touch and pop-out state machines, the pixel layer (masks,
# blending, text, the 5x7 fallback font), the drawing of the widgets and sysfs
# access. A static library so that the unit test and the program use the same
# code.
libm = cc.find_library('m', required: false)
pwpanel_lib = static_library('pwpanel',
  'panel-logic.c', 'panel-gfx.c', 'panel-font.c', 'panel-draw.c', 'panel-sys.c',
  '../src/subpixel.c',
  link_with: pwpanel_stb, dependencies: libm,
  include_directories: include_directories('../src'))

# The program needs libwayland-client and alsa-lib, not wlroots. The protocol
# code is private to it.
panel_alsa = dependency('alsa', required: get_option('panel'))
panel_wl = dependency('wayland-client', required: get_option('panel'))
panel_built = false

if panel_alsa.found() and panel_wl.found()
  panel_xdg_xml = wl_proto_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'
  panel_ls_xml = files('../protocols/wlr-layer-shell-unstable-v1.xml')
  panel_proto = []
  foreach p : [['xdg-shell', panel_xdg_xml], ['wlr-layer-shell-unstable-v1', panel_ls_xml]]
    panel_proto += custom_target('panel-' + p[0] + '-client-header',
      input: p[1], output: p[0] + '-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
    panel_proto += custom_target('panel-' + p[0] + '-client-code',
      input: p[1], output: p[0] + '-client-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  endforeach

  picowl_panel_exe = executable('picowl-panel',
    'panel.c', panel_proto,
    link_with: pwpanel_lib,
    dependencies: [panel_wl, panel_alsa, libm],
    install: true,
  )
  panel_built = true
endif
