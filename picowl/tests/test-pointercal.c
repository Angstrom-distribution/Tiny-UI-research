/* test-pointercal.c - tslib pointercal parsing and libinput matrix conversion. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/pointercal.h"

#define BOARD "22841 -68 -3658260 -491 -28324 25242144 65536 240 320 0"

static bool parse(const char *s, struct pw_pointercal *pc)
{
	char err[160];
	bool ok = pw_pointercal_parse(s, strlen(s), pc, err, sizeof(err));
	if (!ok)
		printf("  refused '%.40s': %s\n", s, err);
	return ok;
}

/* What libinput does with a matrix for raw (x, y): normalize with
 * max - min + 1, apply, then scale to the screen. Mirrors evdev.c
 * evdev_device_calibrate, so the test checks the whole chain. */
static void libinput_screen(const float m[6], int minx, int maxx, int miny,
	int maxy, const struct pw_pointercal *pc, double x, double y,
	double *sx, double *sy)
{
	double rx = maxx - minx + 1.0, ry = maxy - miny + 1.0;
	double nx = (x - minx) / rx, ny = (y - miny) / ry;
	double ox = m[0] * nx + m[1] * ny + m[2];
	double oy = m[3] * nx + m[4] * ny + m[5];
	*sx = ox * pc->xres;
	*sy = oy * pc->yres;
}

/* Build a pointercal that maps the raw box [rx0,rx1] x [ry0,ry1] onto the
 * screen corners, optionally inverted per axis. */
static void synth(struct pw_pointercal *pc, int xres, int yres, double rx0,
	double rx1, double ry0, double ry1, bool inv_x, bool inv_y)
{
	double sc = 65536.0;
	double ax = sc * (xres - 1) / (rx1 - rx0) * (inv_x ? -1 : 1);
	double ay = sc * (yres - 1) / (ry1 - ry0) * (inv_y ? -1 : 1);
	double cx = inv_x ? -ax * rx1 : -ax * rx0;
	double cy = inv_y ? -ay * ry1 : -ay * ry0;
	pc->a = llround(ax); pc->b = 0; pc->c = llround(cx);
	pc->d = 0; pc->e = llround(ay); pc->f = llround(cy);
	pc->scale = 65536; pc->xres = xres; pc->yres = yres;
}

int main(void)
{
	struct pw_pointercal pc;
	char err[160];
	float m[6];

	/* the board's file, exactly as it is on disk: no trailing newline */
	assert(parse(BOARD, &pc));
	assert(pc.a == 22841 && pc.b == -68 && pc.c == -3658260);
	assert(pc.d == -491 && pc.e == -28324 && pc.f == 25242144);
	assert(pc.scale == 65536 && pc.xres == 240 && pc.yres == 320);

	/* xinput_calibrator's result on the board, ABS 0..1023 */
	assert(pw_pointercal_to_matrix(&pc, 0, 1023, 0, 1023, m, err, sizeof(err)));
	static const float want[6] = { 1.487044f, -0.004427f, -0.232586f,
		-0.023975f, -1.383008f, 1.203639f };
	for (int i = 0; i < 6; i++) {
		printf("  m[%d] = %f (want %f)\n", i, m[i], want[i]);
		assert(fabsf(m[i] - want[i]) < 1e-4f);
	}
	printf("ok board matrix\n");

	/* the matrix and the tslib formula agree for arbitrary raw points */
	for (int x = 0; x <= 1023; x += 127)
		for (int y = 0; y <= 1023; y += 131) {
			double sx, sy;
			libinput_screen(m, 0, 1023, 0, 1023, &pc, x, y, &sx, &sy);
			double tx = ((double)pc.a * x + pc.b * y + pc.c) / pc.scale;
			double ty = ((double)pc.d * x + pc.e * y + pc.f) / pc.scale;
			assert(fabs(sx - tx) < 0.01);
			assert(fabs(sy - ty) < 0.01);
		}
	printf("ok formula agreement\n");

	/* the four corners of the calibrated area land on the screen corners */
	static const struct { int minx, maxx, miny, maxy; } rg[] = {
		{ 0, 1023, 0, 1023 }, { 0, 4095, 0, 4095 }, { 50, 3950, 80, 4000 },
	};
	for (unsigned r = 0; r < sizeof(rg) / sizeof(rg[0]); r++)
		for (int inv = 0; inv < 4; inv++) {
			double w = rg[r].maxx - rg[r].minx, h = rg[r].maxy - rg[r].miny;
			double x0 = rg[r].minx + w * 0.10, x1 = rg[r].minx + w * 0.90;
			double y0 = rg[r].miny + h * 0.15, y1 = rg[r].miny + h * 0.85;
			synth(&pc, 240, 320, x0, x1, y0, y1, inv & 1, inv & 2);
			assert(pw_pointercal_to_matrix(&pc, rg[r].minx, rg[r].maxx,
				rg[r].miny, rg[r].maxy, m, err, sizeof(err)));
			for (int c = 0; c < 4; c++) {
				double rx = (c & 1) ? x1 : x0, ry = (c & 2) ? y1 : y0;
				double ex = ((c & 1) != 0) != ((inv & 1) != 0) ? 239 : 0;
				double ey = ((c & 2) != 0) != ((inv & 2) != 0) ? 319 : 0;
				double sx, sy;
				libinput_screen(m, rg[r].minx, rg[r].maxx, rg[r].miny,
					rg[r].maxy, &pc, rx, ry, &sx, &sy);
				assert(fabs(sx - ex) < 1.0);
				assert(fabs(sy - ey) < 1.0);
			}
		}
	printf("ok corner mapping\n");

	/* accepted spellings */
	assert(parse(BOARD "\n", &pc));
	assert(parse(BOARD "  \t\r\n\n", &pc));
	assert(parse("  22841\t-68 -3658260\n-491 -28324 25242144 65536 240 320 0 ", &pc));
	assert(parse("22841 -68 -3658260 -491 -28324 25242144 65536 240 320", &pc));
	assert(pc.xres == 240 && pc.yres == 320);
	/* 7 fields are syntactically valid but lack the resolution */
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536", &pc));

	/* rejections */
	assert(!parse("", &pc));
	assert(!parse("   \n", &pc));
	assert(!parse("garbage", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536 240 320 x", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536 240.5 320 0", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536 240 320 0 7", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324", &pc));          /* 5 */
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144", &pc)); /* 6 */
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 0 240 320 0", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 -65536 240 320 0", &pc));
	assert(!parse("99999999999999999999 -68 -3658260 -491 -28324 25242144 65536 240 320 0", &pc));
	assert(!parse("2000000000000 -68 -3658260 -491 -28324 25242144 65536 240 320 0", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 4294967296 240 320 0", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536 0 320 0", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536 240 0 0", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536 240 320 1", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536 240 320 90", &pc));
	assert(!parse("22841 -68 -3658260 -491 -28324 25242144 65536 99999 320 0", &pc));
	/* an embedded NUL must not hide trailing garbage */
	{
		const char z[] = "22841 -68 -3658260 -491 -28324 25242144 65536 240 320 0\0 1";
		assert(!pw_pointercal_parse(z, sizeof(z) - 1, &pc, err, sizeof(err)));
	}
	assert(!pw_pointercal_load("/nonexistent/pointercal", &pc, err, sizeof(err)));
	printf("ok rejections\n");

	/* conversion refusals */
	assert(parse(BOARD, &pc));
	assert(!pw_pointercal_to_matrix(&pc, 0, 0, 0, 1023, m, err, sizeof(err)));
	assert(!pw_pointercal_to_matrix(&pc, 0, 1023, 5, 4, m, err, sizeof(err)));
	pc.a = pc.b = pc.d = pc.e = 0;
	assert(!pw_pointercal_to_matrix(&pc, 0, 1023, 0, 1023, m, err, sizeof(err)));
	printf("ok conversion refusals\n");
	return 0;
}
