/* test-subpixel.c - the subpixel layout of a panel as a client sees it, and
 * what the compositor advertises. */
#include <stdio.h>
#include <string.h>
#include "subpixel.h"

static int test_count, fail_count;

#define CHECK(cond, msg) do { \
	test_count++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
		fail_count++; \
	} \
} while (0)

#define CHECK_EQ(got, want, msg) do { \
	long long g_ = (long long)(got), w_ = (long long)(want); \
	test_count++; \
	if (g_ != w_) { \
		fprintf(stderr, "FAIL: %s (got %lld, want %lld, line %d)\n", msg, g_, w_, __LINE__); \
		fail_count++; \
	} \
} while (0)

#define CHECK_STR(got, want, msg) do { \
	test_count++; \
	if (strcmp((got), (want)) != 0) { \
		fprintf(stderr, "FAIL: %s (got '%s', want '%s', line %d)\n", msg, got, want, __LINE__); \
		fail_count++; \
	} \
} while (0)

static void test_order(void)
{
	enum { N = 0, R90 = 1, R180 = 2, R270 = 3, F = 4, F90 = 5, F180 = 6, F270 = 7 };

	/* The h2200: red at the left. */
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_HORIZONTAL_RGB, N), PW_STRIPES_RGB, "h2200 normal: RGB");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_HORIZONTAL_RGB, R180), PW_STRIPES_BGR, "h2200 180: BGR");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_HORIZONTAL_RGB, R90), PW_STRIPES_NONE, "h2200 90: a vertical stack");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_HORIZONTAL_RGB, R270), PW_STRIPES_NONE, "h2200 270: a vertical stack");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_HORIZONTAL_RGB, F), PW_STRIPES_BGR, "h2200 flipped: mirrored");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_HORIZONTAL_RGB, F180), PW_STRIPES_RGB, "h2200 flipped 180: mirrored twice");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_HORIZONTAL_BGR, N), PW_STRIPES_BGR, "a BGR panel normal: BGR");
	/* The h3900: red at the top of the panel as it is built. With 90 the
	 * buffer's right edge is at the top of the panel, so along the buffer's
	 * x the colours go from blue (left, bottom of the panel) to red. */
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_VERTICAL_RGB, N), PW_STRIPES_NONE, "h3900 normal: a vertical stack");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_VERTICAL_RGB, R90), PW_STRIPES_BGR, "h3900 90: BGR");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_VERTICAL_RGB, R270), PW_STRIPES_RGB, "h3900 270: RGB");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_VERTICAL_RGB, R180), PW_STRIPES_NONE, "h3900 180: a vertical stack");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_VERTICAL_BGR, R90), PW_STRIPES_RGB, "vertical BGR 90: RGB");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_VERTICAL_RGB, F90), PW_STRIPES_RGB, "h3900 flipped 90: the mirrored x runs the other way than at 90");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_VERTICAL_RGB, F270), PW_STRIPES_BGR, "h3900 flipped 270");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_NONE, N), PW_STRIPES_NONE, "none stays none");
	CHECK_EQ(pw_subpixel_stripes(PW_SUBPIXEL_UNKNOWN, N), PW_STRIPES_NONE, "unknown is no layout to draw for");
	CHECK_EQ(pw_subpixel_in_client_frame(PW_SUBPIXEL_HORIZONTAL_RGB, R90), PW_SUBPIXEL_VERTICAL_RGB, "client frame at 90");
	CHECK_EQ(pw_subpixel_in_client_frame(PW_SUBPIXEL_HORIZONTAL_RGB, R270), PW_SUBPIXEL_VERTICAL_BGR, "client frame at 270");
	CHECK_EQ(pw_subpixel_in_client_frame(PW_SUBPIXEL_UNKNOWN, R90), PW_SUBPIXEL_UNKNOWN, "unknown has no frame");

	/* What the compositor advertises: the panel's own layout with the
	 * rotation as the transform; in the client frame only when the transform
	 * is not sent. */
	CHECK_EQ(pw_subpixel_advertise(PW_SUBPIXEL_HORIZONTAL_RGB, R90, R90), PW_SUBPIXEL_HORIZONTAL_RGB,
		"software rotation: native, with the transform");
	CHECK_EQ(pw_subpixel_advertise(PW_SUBPIXEL_HORIZONTAL_RGB, R90, N), PW_SUBPIXEL_VERTICAL_RGB,
		"hardware rotation: the transform is normal, so the layout is the one the client sees");
	CHECK_EQ(pw_subpixel_advertise(PW_SUBPIXEL_HORIZONTAL_RGB, N, N), PW_SUBPIXEL_HORIZONTAL_RGB, "not rotated");
	CHECK_EQ(pw_subpixel_stripes(pw_subpixel_advertise(PW_SUBPIXEL_VERTICAL_RGB, R90, N), N),
		pw_subpixel_stripes(pw_subpixel_advertise(PW_SUBPIXEL_VERTICAL_RGB, R90, R90), R90),
		"a client reaches the same order either way");

	int sp = -1;
	CHECK(pw_subpixel_parse("horizontal_rgb", &sp) && sp == PW_SUBPIXEL_HORIZONTAL_RGB, "the name parses");
	CHECK(pw_subpixel_parse("vertical_bgr", &sp) && sp == PW_SUBPIXEL_VERTICAL_BGR, "vertical_bgr parses");
	CHECK(pw_subpixel_parse("unknown", &sp) && sp == PW_SUBPIXEL_UNKNOWN, "unknown parses");
	CHECK(!pw_subpixel_parse("rgb", &sp), "a short name is not one");
	CHECK(!pw_subpixel_parse(NULL, &sp), "NULL is not one");
	CHECK_STR(pw_subpixel_name(PW_SUBPIXEL_HORIZONTAL_BGR), "horizontal_bgr", "and prints back");
}

int main(void)
{
	test_order();
	printf("test-subpixel: %d checks, %d failed\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
