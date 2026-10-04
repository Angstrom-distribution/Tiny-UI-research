executable('picowl-cursor-convert',
  'picowl-cursor-convert.c',
  link_with: pwcursorfit,
  include_directories: include_directories('../src'),
  install: true,
)

# Client-only: needs libwayland-client and the stable protocol XMLs, not
# wlroots. The generated protocol code is private to this tool.
tools_wayland_client = dependency('wayland-client', required: false)
if tools_wayland_client.found()
  cl_xdg_xml = wl_proto_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'
  cl_xdg_h = custom_target('commit-loop-xdg-shell-client-header',
    input: cl_xdg_xml, output: 'xdg-shell-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  cl_xdg_c = custom_target('commit-loop-xdg-shell-client-code',
    input: cl_xdg_xml, output: 'xdg-shell-client-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  cl_pres_xml = wl_proto_dir / 'stable' / 'presentation-time' / 'presentation-time.xml'
  cl_pres_h = custom_target('commit-loop-presentation-time-client-header',
    input: cl_pres_xml, output: 'presentation-time-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  cl_pres_c = custom_target('commit-loop-presentation-time-client-code',
    input: cl_pres_xml, output: 'presentation-time-client-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

  executable('picowl-commit-loop',
    'picowl-commit-loop.c', cl_xdg_h, cl_xdg_c, cl_pres_h, cl_pres_c,
    dependencies: [tools_wayland_client],
    install: true,
  )
endif
