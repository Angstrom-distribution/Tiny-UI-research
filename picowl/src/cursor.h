/*
 * cursor.h - hold-animation cursor. picowl draws no cursor otherwise.
 * Frames (builtin or a PAM strip from config->hold_animation) are made
 * compliant with pw_cursorfit (warning logged if changed) and pre-rendered
 * into static wlr_buffers at init; nothing is allocated per frame.
 */
#ifndef PICOWL_CURSOR_H
#define PICOWL_CURSOR_H

#include <stdbool.h>

struct pw_server;

/* Load/render frames per server->config and create the animation state in
 * server->hold_cursor. Called by pw_server_init() after pw_input_init().
 * Failure is non-fatal: logged, hold_cursor stays NULL and the other
 * entry points become no-ops. */
void pw_cursor_init(struct pw_server *server);

/* Show frame 0 centred (hotspot) at layout coords x,y and start the
 * config->cursor_frame_ms timer. Idempotent while a touch hold animation
 * runs; takes over one a client asked for (cursor-shape wait). */
void pw_cursor_hold_start(struct pw_server *server, int x, int y);

/* Hide the cursor image and stop the timer. Idempotent. */
void pw_cursor_hold_stop(struct pw_server *server);

/* Free timer, buffers and state. Called by pw_server_finish() before
 * pw_input_finish(). NULL-safe. */
void pw_cursor_finish(struct pw_server *server);

#endif
