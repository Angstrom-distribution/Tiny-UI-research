/* test-rotate.c - unit tests for rotation mode and geometry helpers. */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <wayland-server-protocol.h>
#include "../src/rotate.h"

int main(void)
{
	/* Test pw_rot_mode_parse - all spellings and case-insensitive */
	enum pw_rot_mode m;
	assert(pw_rot_mode_parse("auto", &m) && m == PW_ROT_AUTO);
	assert(pw_rot_mode_parse("Auto", &m) && m == PW_ROT_AUTO);
	assert(pw_rot_mode_parse("AUTO", &m) && m == PW_ROT_AUTO);
	assert(pw_rot_mode_parse("hardware", &m) && m == PW_ROT_HARDWARE);
	assert(pw_rot_mode_parse("Hardware", &m) && m == PW_ROT_HARDWARE);
	assert(pw_rot_mode_parse("HARDWARE", &m) && m == PW_ROT_HARDWARE);
	assert(pw_rot_mode_parse("software", &m) && m == PW_ROT_SOFTWARE);
	assert(pw_rot_mode_parse("Software", &m) && m == PW_ROT_SOFTWARE);
	assert(pw_rot_mode_parse("SOFTWARE", &m) && m == PW_ROT_SOFTWARE);
	assert(!pw_rot_mode_parse("invalid", &m));
	assert(!pw_rot_mode_parse("", &m));
	assert(!pw_rot_mode_parse(NULL, &m));
	assert(!pw_rot_mode_parse("auto", NULL));
	printf("✓ pw_rot_mode_parse\n");

	/* Test pw_rot_mode_name */
	assert(__builtin_strcmp(pw_rot_mode_name(PW_ROT_AUTO), "auto") == 0);
	assert(__builtin_strcmp(pw_rot_mode_name(PW_ROT_HARDWARE), "hardware") == 0);
	assert(__builtin_strcmp(pw_rot_mode_name(PW_ROT_SOFTWARE), "software") == 0);
	printf("✓ pw_rot_mode_name\n");

	/* Test pw_rot_swaps_axes for all 8 transforms */
	assert(!pw_rot_swaps_axes(WL_OUTPUT_TRANSFORM_NORMAL));        /* 0 */
	assert(pw_rot_swaps_axes(WL_OUTPUT_TRANSFORM_90));             /* 1 */
	assert(!pw_rot_swaps_axes(WL_OUTPUT_TRANSFORM_180));           /* 2 */
	assert(pw_rot_swaps_axes(WL_OUTPUT_TRANSFORM_270));            /* 3 */
	assert(!pw_rot_swaps_axes(WL_OUTPUT_TRANSFORM_FLIPPED));       /* 4 */
	assert(pw_rot_swaps_axes(WL_OUTPUT_TRANSFORM_FLIPPED_90));     /* 5 */
	assert(!pw_rot_swaps_axes(WL_OUTPUT_TRANSFORM_FLIPPED_180));   /* 6 */
	assert(pw_rot_swaps_axes(WL_OUTPUT_TRANSFORM_FLIPPED_270));    /* 7 */
	printf("✓ pw_rot_swaps_axes\n");

	/* Test pw_rot_logical_size for all 8 transforms on 240x320 and 320x240 */
	int w, h;

	/* 240x320 portrait */
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_NORMAL, 240, 320, &w, &h);
	assert(w == 240 && h == 320);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_90, 240, 320, &w, &h);
	assert(w == 320 && h == 240);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_180, 240, 320, &w, &h);
	assert(w == 240 && h == 320);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_270, 240, 320, &w, &h);
	assert(w == 320 && h == 240);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_FLIPPED, 240, 320, &w, &h);
	assert(w == 240 && h == 320);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_FLIPPED_90, 240, 320, &w, &h);
	assert(w == 320 && h == 240);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_FLIPPED_180, 240, 320, &w, &h);
	assert(w == 240 && h == 320);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_FLIPPED_270, 240, 320, &w, &h);
	assert(w == 320 && h == 240);

	/* 320x240 landscape */
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_NORMAL, 320, 240, &w, &h);
	assert(w == 320 && h == 240);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_90, 320, 240, &w, &h);
	assert(w == 240 && h == 320);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_180, 320, 240, &w, &h);
	assert(w == 320 && h == 240);
	pw_rot_logical_size(WL_OUTPUT_TRANSFORM_270, 320, 240, &w, &h);
	assert(w == 240 && h == 320);
	printf("✓ pw_rot_logical_size\n");

	/* Test pw_rot_touch_matrix - verify identity matrix for NORMAL */
	float matrix[6];
	pw_rot_touch_matrix(WL_OUTPUT_TRANSFORM_NORMAL, matrix);
	assert(matrix[0] == 1.0f && matrix[1] == 0.0f && matrix[2] == 0.0f);
	assert(matrix[3] == 0.0f && matrix[4] == 1.0f && matrix[5] == 0.0f);
	printf("✓ pw_rot_touch_matrix identity\n");

	/* Test that 90 rotation matrix maps unit square corners correctly:
	 * (0,0) -> (1,0), (1,0) -> (1,1), (1,1) -> (0,1), (0,1) -> (0,0) */
	pw_rot_touch_matrix(WL_OUTPUT_TRANSFORM_90, matrix);
	/* Apply matrix: (x', y') = (ax + by + c, dx + ey + f) */
	float x, y;
	x = matrix[0]*0 + matrix[1]*0 + matrix[2];
	y = matrix[3]*0 + matrix[4]*0 + matrix[5];
	assert(fabsf(x - 1.0f) < 0.001f && fabsf(y - 0.0f) < 0.001f);

	x = matrix[0]*1 + matrix[1]*0 + matrix[2];
	y = matrix[3]*1 + matrix[4]*0 + matrix[5];
	assert(fabsf(x - 1.0f) < 0.001f && fabsf(y - 1.0f) < 0.001f);

	x = matrix[0]*1 + matrix[1]*1 + matrix[2];
	y = matrix[3]*1 + matrix[4]*1 + matrix[5];
	assert(fabsf(x - 0.0f) < 0.001f && fabsf(y - 1.0f) < 0.001f);

	x = matrix[0]*0 + matrix[1]*1 + matrix[2];
	y = matrix[3]*0 + matrix[4]*1 + matrix[5];
	assert(fabsf(x - 0.0f) < 0.001f && fabsf(y - 0.0f) < 0.001f);
	printf("✓ pw_rot_touch_matrix 90 rotation\n");

	/* Test all transforms map the 4 unit-square corners to a permutation of corners */
	for (int t = 0; t < 8; t++) {
		pw_rot_touch_matrix((enum wl_output_transform)t, matrix);
		float points[4][2] = {{0,0}, {1,0}, {1,1}, {0,1}};
		float results[4][2];
		for (int i = 0; i < 4; i++) {
			results[i][0] = matrix[0]*points[i][0] + matrix[1]*points[i][1] + matrix[2];
			results[i][1] = matrix[3]*points[i][0] + matrix[4]*points[i][1] + matrix[5];
		}
		/* Check all results are corners (near 0 or 1 in both dims) */
		for (int i = 0; i < 4; i++) {
			assert((fabsf(results[i][0]) < 0.001f || fabsf(results[i][0] - 1.0f) < 0.001f) &&
			       (fabsf(results[i][1]) < 0.001f || fabsf(results[i][1] - 1.0f) < 0.001f));
		}
	}
	printf("✓ pw_rot_touch_matrix all transforms\n");

	/* pw_rot_matrix_mul: identity keeps the default, rotation composes */
	{
		float id[6] = {1, 0, 0, 0, 1, 0}, d[6] = {0.5f, 0, 0.25f, 0, 2, 0.125f}, o[6], r[6];
		pw_rot_matrix_mul(id, d, o);
		for (int i = 0; i < 6; i++)
			assert(o[i] == d[i]);
		pw_rot_touch_matrix(WL_OUTPUT_TRANSFORM_90, r);
		pw_rot_matrix_mul(r, id, o);
		for (int i = 0; i < 6; i++)
			assert(o[i] == r[i]);
		/* x' = -y + 1 of D(x,y) = (0.5x+0.25, 2y+0.125) -> -2y + 0.875 */
		pw_rot_matrix_mul(r, d, o);
		assert(o[0] == 0 && o[1] == -2 && o[2] == 0.875f);
		assert(o[3] == 0.5f && o[4] == 0 && o[5] == 0.25f);
		printf("ok pw_rot_matrix_mul\n");
	}

	return 0;
}
