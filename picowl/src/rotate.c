#include "rotate.h"

#include <string.h>
#include <ctype.h>

/* Case-insensitive comparison */
static bool strcaseeq(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
		a++;
		b++;
	}
	return *a == *b;
}

bool pw_rot_mode_parse(const char *s, enum pw_rot_mode *out)
{
	if (!s || !out)
		return false;
	if (strcaseeq(s, "auto"))
		*out = PW_ROT_AUTO;
	else if (strcaseeq(s, "hardware"))
		*out = PW_ROT_HARDWARE;
	else if (strcaseeq(s, "software"))
		*out = PW_ROT_SOFTWARE;
	else
		return false;
	return true;
}

const char *pw_rot_mode_name(enum pw_rot_mode m)
{
	switch (m) {
	case PW_ROT_HARDWARE: return "hardware";
	case PW_ROT_SOFTWARE: return "software";
	default: return "auto";
	}
}

bool pw_rot_swaps_axes(enum wl_output_transform t)
{
	/* Transforms 1, 3, 5, 7 (90, 270, FLIPPED_90, FLIPPED_270) swap axes. */
	return (t & 1) != 0;
}

void pw_rot_logical_size(enum wl_output_transform t, int native_w, int native_h, int *w, int *h)
{
	if (pw_rot_swaps_axes(t)) {
		*w = native_h;
		*h = native_w;
	} else {
		*w = native_w;
		*h = native_h;
	}
}

void pw_rot_touch_matrix(enum wl_output_transform t, float m[6])
{
	/* Precomputed libinput calibration matrices for touch input mapping.
	 * Format: row-major [a b c; d e f] where x' = ax + by + c, y' = dx + ey + f.
	 * FLIPPED_x matrices: rotation_x composed after the x-flip {-1,0,1, 0,1,0}.
	 */
	static const float matrices[8][6] = {
		/* NORMAL */ {1, 0, 0, 0, 1, 0},
		/* 90 */ {0, -1, 1, 1, 0, 0},
		/* 180 */ {-1, 0, 1, 0, -1, 1},
		/* 270 */ {0, 1, 0, -1, 0, 1},
		/* FLIPPED */ {-1, 0, 1, 0, 1, 0},
		/* FLIPPED_90 */ {0, -1, 1, -1, 0, 1},
		/* FLIPPED_180 */ {1, 0, 0, 0, -1, 1},
		/* FLIPPED_270 */ {0, 1, 0, 1, 0, 0},
	};
	for (int i = 0; i < 6; i++)
		m[i] = matrices[t & 7][i];
}

void pw_rot_matrix_mul(const float r[6], const float d[6], float out[6])
{
	out[0] = r[0] * d[0] + r[1] * d[3];
	out[1] = r[0] * d[1] + r[1] * d[4];
	out[2] = r[0] * d[2] + r[1] * d[5] + r[2];
	out[3] = r[3] * d[0] + r[4] * d[3];
	out[4] = r[3] * d[1] + r[4] * d[4];
	out[5] = r[3] * d[2] + r[4] * d[5] + r[5];
}
