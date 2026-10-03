/* test-cursorfit.c - tests for cursor fitting library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/cursorfit.h"

#define TEST(name) printf("test: %s... ", (name))
#define PASS() printf("PASS\n")
#define FAIL() (printf("FAIL\n"), exit(1))

static void test_alloc_free(void)
{
	TEST("alloc_free");
	struct pw_image img;
	assert(pw_image_alloc(&img, 32, 32));
	assert(img.width == 32);
	assert(img.height == 32);
	assert(img.pixels != NULL);
	/* Check that image is zeroed (transparent). */
	for (int i = 0; i < 32*32; i++)
		assert(img.pixels[i] == 0x00000000);
	pw_image_free(&img);
	assert(img.pixels == NULL);
	assert(img.width == 0);
	assert(img.height == 0);
	PASS();
}

static void test_alloc_invalid(void)
{
	TEST("alloc_invalid");
	struct pw_image img;
	/* Zero or negative size should fail. */
	assert(!pw_image_alloc(&img, 0, 32));
	assert(!pw_image_alloc(&img, 32, 0));
	assert(!pw_image_alloc(&img, -1, 32));
	PASS();
}

static void test_check_compliant(void)
{
	TEST("check_compliant");
	struct pw_image img;
	assert(pw_image_alloc(&img, 16, 16));

	/* Create a simple compliant image: 2 colors, fully opaque/transparent. */
	for (int i = 0; i < 16*16; i++) {
		if (i < 128)
			img.pixels[i] = 0xff0000ff;  /* opaque blue */
		else if (i < 256)
			img.pixels[i] = 0xffff0000;  /* opaque red */
		else
			img.pixels[i] = 0x00000000;  /* transparent */
	}

	char why[256];
	assert(pw_cursorfit_check(&img, why, sizeof(why)));
	PASS();
	pw_image_free(&img);
}

static void test_check_too_large(void)
{
	TEST("check_too_large");
	struct pw_image img;
	assert(pw_image_alloc(&img, 128, 16));  /* Width too large */
	char why[256];
	assert(!pw_cursorfit_check(&img, why, sizeof(why)));
	assert(strstr(why, "out of range") != NULL);
	PASS();
	pw_image_free(&img);
}

static void test_check_translucent(void)
{
	TEST("check_translucent");
	struct pw_image img;
	assert(pw_image_alloc(&img, 16, 16));
	img.pixels[0] = 0x800000ff;  /* Semi-transparent */
	char why[256];
	assert(!pw_cursorfit_check(&img, why, sizeof(why)));
	assert(strstr(why, "translucent") != NULL);
	PASS();
	pw_image_free(&img);
}

static void test_check_too_many_colors(void)
{
	TEST("check_too_many_colors");
	struct pw_image img;
	assert(pw_image_alloc(&img, 16, 16));
	for (int i = 0; i < 16*16; i++) {
		if (i < 256)
			img.pixels[i] = 0xff000000 | (i & 0xffffff);  /* Different colors */
		else
			img.pixels[i] = 0x00000000;
	}
	char why[256];
	assert(!pw_cursorfit_check(&img, why, sizeof(why)));
	assert(strstr(why, "more than 2") != NULL);
	PASS();
	pw_image_free(&img);
}

static void test_fit_compliant_unchanged(void)
{
	TEST("fit_compliant_unchanged");
	struct pw_image img, out;
	assert(pw_image_alloc(&img, 16, 16));
	/* Create a compliant image. */
	for (int i = 0; i < 16*16; i++) {
		if (i < 128)
			img.pixels[i] = 0xff0000ff;  /* blue */
		else if (i < 256)
			img.pixels[i] = 0xffff0000;  /* red */
		else
			img.pixels[i] = 0x00000000;  /* transparent */
	}

	struct pw_cursorfit_opts opts = {0};
	struct pw_cursorfit_report report = {0};
	assert(pw_cursorfit_fit(&img, &out, NULL, NULL, &opts, &report));
	assert(report.compliant_input);
	assert(out.width == img.width);
	assert(out.height == img.height);
	/* Check pixels match (may be reordered colors but should match). */
	for (int i = 0; i < 16*16; i++) {
		uint32_t orig = img.pixels[i];
		uint32_t fitted = out.pixels[i];
		assert((orig == 0x00000000) ? (fitted == 0x00000000) : ((fitted >> 24) == 0xff));
	}

	PASS();
	pw_image_free(&img);
	pw_image_free(&out);
}

static void test_fit_alpha_threshold(void)
{
	TEST("fit_alpha_threshold");
	struct pw_image img, out;
	assert(pw_image_alloc(&img, 16, 16));
	for (int i = 0; i < 16*16; i++) {
		if (i < 128)
			img.pixels[i] = 0x7f0000ff;  /* alpha 127 (below 128) */
		else if (i < 256)
			img.pixels[i] = 0x800000ff;  /* alpha 128 (>= 128) */
		else
			img.pixels[i] = 0x00000000;  /* transparent */
	}

	struct pw_cursorfit_opts opts = {0};
	struct pw_cursorfit_report report = {0};
	assert(pw_cursorfit_fit(&img, &out, NULL, NULL, &opts, &report));
	/* Both 0x7f and 0x80 pixels get snapped to 0 or 0xff, so 256 total. */
	assert(report.clipped_alpha == 256);
	/* Check: alpha 127 pixels became transparent, alpha 128 became opaque. */
	for (int i = 0; i < 128; i++) {
		assert(out.pixels[i] == 0x00000000);
	}
	for (int i = 128; i < 256; i++) {
		assert((out.pixels[i] >> 24) == 0xff);
	}

	PASS();
	pw_image_free(&img);
	pw_image_free(&out);
}

static void test_fit_crop_to_max(void)
{
	TEST("fit_crop_to_max");
	struct pw_image img, out;
	assert(pw_image_alloc(&img, 128, 128));  /* Too large */
	for (int i = 0; i < 128*128; i++)
		img.pixels[i] = 0xffff0000;  /* Opaque red */

	struct pw_cursorfit_opts opts = {.max_size = 64};
	struct pw_cursorfit_report report = {0};
	assert(pw_cursorfit_fit(&img, &out, NULL, NULL, &opts, &report));
	assert(report.cropped);
	assert(out.width == 64);
	assert(out.height == 64);

	PASS();
	pw_image_free(&img);
	pw_image_free(&out);
}

static void test_fit_hotspot_crop(void)
{
	TEST("fit_hotspot_crop");
	struct pw_image img, out;
	assert(pw_image_alloc(&img, 128, 128));
	for (int i = 0; i < 128*128; i++)
		img.pixels[i] = 0xffff0000;

	int hot_x = 80, hot_y = 80;
	struct pw_cursorfit_opts opts = {.max_size = 64};
	struct pw_cursorfit_report report = {0};
	assert(pw_cursorfit_fit(&img, &out, &hot_x, &hot_y, &opts, &report));
	/* Centre crop (128-64)/2 = 32 offset. hot (80,80) - (32,32) = (48,48). */
	assert(hot_x == 48);
	assert(hot_y == 48);

	PASS();
	pw_image_free(&img);
	pw_image_free(&out);
}

static void test_fit_color_reduction(void)
{
	TEST("fit_color_reduction");
	struct pw_image img, out;
	assert(pw_image_alloc(&img, 16, 16));
	/* Image with many colors that should reduce to 2 dominant. */
	for (int i = 0; i < 256; i++) {
		if (i < 200)
			img.pixels[i] = 0xff0000ff;  /* Lots of blue */
		else if (i < 240)
			img.pixels[i] = 0xffff0000;  /* Some red */
		else
			img.pixels[i] = 0x00ff00ff;  /* A few green (will map to blue or red) */
	}

	struct pw_cursorfit_opts opts = {0};
	struct pw_cursorfit_report report = {0};
	assert(pw_cursorfit_fit(&img, &out, NULL, NULL, &opts, &report));
	assert(report.input_colours >= 2);
	assert(report.merged_colours == report.input_colours - 2);

	/* Verify output is compliant. */
	char why[256];
	assert(pw_cursorfit_check(&out, why, sizeof(why)));

	PASS();
	pw_image_free(&img);
	pw_image_free(&out);
}

static void test_fit_forced_colours(void)
{
	TEST("fit_forced_colours");
	struct pw_image img, out;
	assert(pw_image_alloc(&img, 16, 16));
	for (int i = 0; i < 16*16; i++) {
		if (i < 128)
			img.pixels[i] = 0xff0000ff;  /* blue */
		else
			img.pixels[i] = 0x00000000;  /* transparent */
	}

	struct pw_cursorfit_opts opts = {
		.force_colours = true,
		.fg = 0x00ff00,  /* Green */
		.bg = 0xffffff,  /* White */
	};
	struct pw_cursorfit_report report = {0};
	assert(pw_cursorfit_fit(&img, &out, NULL, NULL, &opts, &report));

	/* All opaque pixels should map to green or white. */
	for (int i = 0; i < 16*16; i++) {
		uint32_t pix = out.pixels[i];
		if ((pix >> 24) == 0xff) {
			uint32_t rgb = pix & 0xffffff;
			assert(rgb == 0x00ff00 || rgb == 0xffffff);
		}
	}

	PASS();
	pw_image_free(&img);
	pw_image_free(&out);
}

static void test_strip_frame(void)
{
	TEST("strip_frame");
	struct pw_image strip, frame;
	assert(pw_image_alloc(&strip, 64, 32));  /* 2 frames of 32x32 */
	/* Frame 0 (cols 0-31): red, Frame 1 (cols 32-63): blue. */
	for (int y = 0; y < 32; y++) {
		for (int x = 0; x < 64; x++) {
			if (x < 32)
				strip.pixels[y * 64 + x] = 0xffff0000;  /* red */
			else
				strip.pixels[y * 64 + x] = 0xff0000ff;  /* blue */
		}
	}

	assert(pw_strip_frame(&strip, 32, 0, &frame));
	assert(frame.width == 32);
	assert(frame.height == 32);
	assert(frame.pixels[0] == 0xffff0000);

	pw_image_free(&frame);

	assert(pw_strip_frame(&strip, 32, 1, &frame));
	assert(frame.width == 32);
	assert(frame.height == 32);
	assert(frame.pixels[0] == 0xff0000ff);

	pw_image_free(&frame);

	/* Out of range should fail. */
	assert(!pw_strip_frame(&strip, 32, 2, &frame));

	PASS();
	pw_image_free(&strip);
}

static void test_strip_count(void)
{
	TEST("strip_count");
	struct pw_image strip;
	assert(pw_image_alloc(&strip, 96, 32));  /* 3 frames of 32x32 */
	assert(pw_strip_count(&strip, 32) == 3);
	assert(pw_strip_count(&strip, 16) == 6);
	assert(pw_strip_count(&strip, 0) == 3);  /* frame_w = height (32), so 96/32 = 3 */
	PASS();
	pw_image_free(&strip);
}

static void test_pam_roundtrip(void)
{
	TEST("pam_roundtrip");
	struct pw_image img, loaded;
	assert(pw_image_alloc(&img, 16, 16));
	for (int i = 0; i < 16*16; i++) {
		if (i < 128)
			img.pixels[i] = 0xff0000ff;  /* opaque blue */
		else
			img.pixels[i] = 0x00000000;  /* transparent */
	}

	/* Write and read back. */
	const char *path = "/tmp/test-cursor.pam";
	assert(pw_pam_write(path, &img));
	assert(pw_pam_read(path, &loaded, NULL, 0));
	assert(loaded.width == img.width);
	assert(loaded.height == img.height);
	for (int i = 0; i < 16*16; i++) {
		assert(loaded.pixels[i] == img.pixels[i]);
	}

	pw_image_free(&img);
	pw_image_free(&loaded);
	PASS();
}

static void test_fit_unsorted_freq(void)
{
	TEST("fit keeps two most frequent colours (unsorted 10,5,20)");
	struct pw_image in, out;
	assert(pw_image_alloc(&in, 35, 1));
	for (int i = 0; i < 35; i++)
		in.pixels[i] = 0xff000000 | (i < 10 ? 0xff0000 : i < 15 ? 0x00ff00 : 0x0000ff);
	assert(pw_cursorfit_fit(&in, &out, NULL, NULL, NULL, NULL));
	int red = 0, green = 0, blue = 0;
	for (int i = 0; i < 35; i++) {
		if (out.pixels[i] == 0xffff0000) red++;
		if (out.pixels[i] == 0xff00ff00) green++;
		if (out.pixels[i] == 0xff0000ff) blue++;
	}
	assert(red == 10 && green == 0 && blue == 25);
	pw_image_free(&in);
	pw_image_free(&out);
	PASS();
}

static void test_ppm_leading_ws_byte(void)
{
	TEST("PPM first pixel byte 0x0a is not eaten");
	const char *path = "/tmp/test-cursor-ws.ppm";
	FILE *f = fopen(path, "wb");
	assert(f);
	fwrite("P6\n# c\n2 1\n255\n", 1, 15, f);
	unsigned char px[6] = {0x0a, 1, 2, 3, 4, 5};
	fwrite(px, 1, 6, f);
	fclose(f);
	struct pw_image img;
	assert(pw_pam_read(path, &img, NULL, 0));
	assert(img.pixels[0] == 0xff0a0102 && img.pixels[1] == 0xff030405);
	pw_image_free(&img);
	PASS();
}

int main(void)
{
	printf("Running cursorfit tests...\n");
	test_alloc_free();
	test_alloc_invalid();
	test_check_compliant();
	test_check_too_large();
	test_check_translucent();
	test_check_too_many_colors();
	test_fit_compliant_unchanged();
	test_fit_alpha_threshold();
	test_fit_crop_to_max();
	test_fit_hotspot_crop();
	test_fit_color_reduction();
	test_fit_forced_colours();
	test_strip_frame();
	test_strip_count();
	test_pam_roundtrip();
	test_fit_unsorted_freq();
	test_ppm_leading_ws_byte();
	printf("\nAll tests passed!\n");
	return 0;
}
