/*
 * pointercal.c - parse tslib pointercal files, convert to a libinput matrix.
 */
#include "pointercal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_MAX_FIELDS 10
#define PC_MAX_COEF ((int64_t)1 << 40)   /* far beyond any real calibration */
#define PC_MAX_SCALE ((int64_t)1 << 31)
#define PC_MAX_RES 32768
#define PC_MAX_FILE 4096

static void fail(char *err, size_t errlen, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void fail(char *err, size_t errlen, const char *fmt, ...)
{
	if (!err || !errlen)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

bool pw_pointercal_parse(const char *text, size_t len, struct pw_pointercal *out,
	char *err, size_t errlen)
{
	if (!text || !out) {
		fail(err, errlen, "no input");
		return false;
	}
	/* Copy first: the caller's buffer need not be NUL terminated, and an
	 * embedded NUL would end strtoll early and hide trailing garbage. */
	if (len > PC_MAX_FILE) {
		fail(err, errlen, "file too large for a pointercal");
		return false;
	}
	char buf[PC_MAX_FILE + 1];
	memcpy(buf, text, len);
	buf[len] = '\0';
	if (strlen(buf) != len) {
		fail(err, errlen, "NUL byte in file");
		return false;
	}

	int64_t v[PC_MAX_FIELDS];
	int n = 0;
	const char *p = buf;
	for (;;) {
		while (isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		if (n == PC_MAX_FIELDS) {
			fail(err, errlen, "more than %d fields", PC_MAX_FIELDS);
			return false;
		}
		char *end;
		errno = 0;
		long long x = strtoll(p, &end, 10);
		if (end == p || errno == ERANGE ||
		    (*end && !isspace((unsigned char)*end))) {
			fail(err, errlen, "field %d is not an integer", n + 1);
			return false;
		}
		v[n++] = x;
		p = end;
	}
	if (n < 7) {
		fail(err, errlen, "%d fields, need at least 7 (a b c d e f scale)", n);
		return false;
	}
	for (int i = 0; i < 6; i++) {
		if (v[i] > PC_MAX_COEF || v[i] < -PC_MAX_COEF) {
			fail(err, errlen, "coefficient %d out of range", i + 1);
			return false;
		}
	}
	if (v[6] == 0) {
		fail(err, errlen, "scale is 0");
		return false;
	}
	if (v[6] < 0 || v[6] > PC_MAX_SCALE) {
		fail(err, errlen, "scale %lld out of range", (long long)v[6]);
		return false;
	}
	if (n < 9 || v[7] <= 0 || v[8] <= 0) {
		fail(err, errlen, "screen resolution (xres yres) missing or 0; "
			"it is needed to normalize, recreate the file with a "
			"current ts_calibrate");
		return false;
	}
	if (v[7] > PC_MAX_RES || v[8] > PC_MAX_RES) {
		fail(err, errlen, "screen resolution out of range");
		return false;
	}
	if (n == 10 && v[9] != 0) {
		fail(err, errlen, "rotation field is %lld, rotated pointercal "
			"files are not supported; recalibrate in the native "
			"orientation", (long long)v[9]);
		return false;
	}
	out->a = v[0]; out->b = v[1]; out->c = v[2];
	out->d = v[3]; out->e = v[4]; out->f = v[5];
	out->scale = v[6];
	out->xres = (int32_t)v[7];
	out->yres = (int32_t)v[8];
	return true;
}

bool pw_pointercal_load(const char *path, struct pw_pointercal *out,
	char *err, size_t errlen)
{
	FILE *f = fopen(path, "re");
	if (!f) {
		fail(err, errlen, "cannot open: %s", strerror(errno));
		return false;
	}
	char buf[PC_MAX_FILE + 1];
	size_t n = fread(buf, 1, sizeof(buf), f);
	bool bad = ferror(f);
	fclose(f);
	if (bad) {
		fail(err, errlen, "cannot read file");
		return false;
	}
	return pw_pointercal_parse(buf, n, out, err, errlen);
}

/*
 * libinput (1.32.0, src/evdev.c evdev_device_calibrate) applies the matrix as
 * M = Unnormalize * Calibration * Normalize on raw values, with
 *     range = absinfo_range() = maximum - minimum + 1    (util-input-event.h)
 *     normalize:   n = (raw - minimum) / range
 *     unnormalize: raw' = n' * range + minimum
 * and afterwards scales raw' by (raw' - minimum) / range again to get 0..1
 * (absinfo_scale_axis). The divisor is therefore max - min + 1, not max - min,
 * so a 10-bit panel (0..1023) has range 1024. Substituting
 *     raw = minimum + range * n
 * into the tslib formula and dividing by the screen size gives the matrix.
 */
bool pw_pointercal_to_matrix(const struct pw_pointercal *pc,
	int32_t minx, int32_t maxx, int32_t miny, int32_t maxy,
	float m[6], char *err, size_t errlen)
{
	if (!pc || pc->scale <= 0 || pc->xres <= 0 || pc->yres <= 0) {
		fail(err, errlen, "invalid pointercal");
		return false;
	}
	if (maxx <= minx || maxy <= miny) {
		fail(err, errlen, "invalid axis range X %d..%d Y %d..%d",
			minx, maxx, miny, maxy);
		return false;
	}
	double rx = (double)maxx - minx + 1.0;
	double ry = (double)maxy - miny + 1.0;
	double a = (double)pc->a, b = (double)pc->b, c = (double)pc->c;
	double d = (double)pc->d, e = (double)pc->e, f = (double)pc->f;
	if (a * e - b * d == 0.0) {
		fail(err, errlen, "degenerate calibration (zero determinant)");
		return false;
	}
	double kx = (double)pc->scale * pc->xres;
	double ky = (double)pc->scale * pc->yres;
	double r[6] = {
		a * rx / kx, b * ry / kx, (a * minx + b * miny + c) / kx,
		d * rx / ky, e * ry / ky, (d * minx + e * miny + f) / ky,
	};
	for (int i = 0; i < 6; i++) {
		if (!isfinite(r[i]) || fabs(r[i]) > 1e6) {
			fail(err, errlen, "matrix element %d out of range", i);
			return false;
		}
		m[i] = (float)r[i];
	}
	return true;
}
