/* rotationproto.h - picowl-rotation-v1: what a client cannot see of the turn
 * of an output when the display does it. */
#ifndef PW_ROTATIONPROTO_H
#define PW_ROTATIONPROTO_H

#include <stdbool.h>

struct pw_server;
struct pw_output;

bool pw_rotationproto_init(struct pw_server *s);
void pw_rotationproto_finish(struct pw_server *s);
/* The output's rotation settled or changed: tells the clients that asked,
 * once per change. */
void pw_rotationproto_output_changed(struct pw_output *o);
void pw_rotationproto_output_removed(struct pw_output *o);

#endif
