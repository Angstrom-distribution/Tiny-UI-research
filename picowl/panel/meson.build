# picowl-panel: a layer-shell client with a clock, the battery and two touch
# sliders (backlight, volume).

# The parts that need neither wayland nor alsa, which are most of it: value
# mapping, text, layout, the touch state machine, the 5x7 font, drawing into a
# pixel buffer and sysfs access. A static library so that the unit test and
# the program use the same code.
pwpanel_lib = static_library('pwpanel',
  'panel-logic.c', 'panel-draw.c', 'panel-sys.c')

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
    dependencies: [panel_wl, panel_alsa],
    install: true,
  )
  panel_built = true
endif
