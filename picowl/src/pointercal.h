/* pointercal.h - tslib pointercal files and their libinput calibration matrix.
 * Pure C, no wlroots or libinput, so it can be unit tested. */
#ifndef PW_POINTERCAL_H
#define PW_POINTERCAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* tslib: screen x' = (a*x + b*y + c) / scale, y' = (d*x + e*y + f) / scale for
 * raw device x, y; the screen is xres by yres pixels. */
struct pw_pointercal {
	int64_t a, b, c, d, e, f;
	int64_t scale;
	int32_t xres, yres;
};

/* Parse the text of a pointercal file (7 to 10 integers: a b c d e f scale
 * [xres yres [rotation]]). Strict: anything else is refused with a message in
 * err. xres and yres are required because the normalization needs them, and a
 * non-zero rotation is refused rather than guessed. */
bool pw_pointercal_parse(const char *text, size_t len, struct pw_pointercal *out,
	char *err, size_t errlen);

/* Read and parse a file (at most a few KiB are read). */
bool pw_pointercal_load(const char *path, struct pw_pointercal *out,
	char *err, size_t errlen);

/* Convert to the 2x3 matrix 'a b c d e f' of
 * libinput_device_config_calibration_set_matrix for a device whose ABS_X and
 * ABS_Y range over [minx, maxx] and [miny, maxy]. */
bool pw_pointercal_to_matrix(const struct pw_pointercal *pc,
	int32_t minx, int32_t maxx, int32_t miny, int32_t maxy,
	float m[6], char *err, size_t errlen);

#endif
