# picowl-panel: the parts that need neither wayland nor alsa, which are most of
# it: value mapping, text, layout, the touch state machine, the 5x7 font,
# drawing into a pixel buffer and sysfs access. A static library so that the
# unit test and the program use the same code.
pwpanel_lib = static_library('pwpanel',
  'panel-logic.c', 'panel-draw.c', 'panel-sys.c')
