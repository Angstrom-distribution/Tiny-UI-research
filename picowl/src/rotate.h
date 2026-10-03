/* rotate.h - rotation mode and geometry helpers (stub: identity). */
#ifndef PW_ROTATE_H
#define PW_ROTATE_H

#include <stdbool.h>
#include <wayland-server-protocol.h>

enum pw_rot_mode { PW_ROT_AUTO, PW_ROT_HARDWARE, PW_ROT_SOFTWARE };

bool pw_rot_mode_parse(const char *s, enum pw_rot_mode *out);
const char *pw_rot_mode_name(enum pw_rot_mode m);
bool pw_rot_swaps_axes(enum wl_output_transform t);
void pw_rot_logical_size(enum wl_output_transform t, int native_w, int native_h, int *w, int *h);
void pw_rot_touch_matrix(enum wl_output_transform t, float m[6]);

/* out = r * d (both 2x3 affine, d applied first). out may not alias. */
void pw_rot_matrix_mul(const float r[6], const float d[6], float out[6]);

#endif
