/* test-cursor-builtin.c - tests for the built-in cursor animation. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "../src/cursor-builtin.h"

#define TEST(name) printf("test: %s... ", (name))
#define PASS() printf("PASS\n")
#define FAIL() (printf("FAIL\n"), exit(1))

static void test_frames_alloc(void)
{
	TEST("frames_alloc");
	struct pw_image frames[PW_BUILTIN_FRAMES];

	/* Generate the built-in animation. */
	assert(pw_cursor_builtin_frames(0x2050c0, 0xffffff, frames));

	/* Check that all frames are allocated and the right size. */
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++) {
		assert(frames[i].width == PW_BUILTIN_SIZE);
		assert(frames[i].height == PW_BUILTIN_SIZE);
		assert(frames[i].pixels != NULL);
	}

	/* Clean up. */
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++)
		pw_image_free(&frames[i]);

	PASS();
}

static void test_frames_compliant(void)
{
	TEST("frames_compliant");
	struct pw_image frames[PW_BUILTIN_FRAMES];

	/* Generate frames. */
	assert(pw_cursor_builtin_frames(0x2050c0, 0xffffff, frames));

	/* Check that each frame is compliant. */
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++) {
		char why[256];
		assert(pw_cursorfit_check(&frames[i], why, sizeof(why)));
	}

	/* Clean up. */
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++)
		pw_image_free(&frames[i]);

	PASS();
}

static void test_frames_differ(void)
{
	TEST("frames_differ");
	struct pw_image frames[PW_BUILTIN_FRAMES];

	/* Generate frames. */
	assert(pw_cursor_builtin_frames(0x2050c0, 0xffffff, frames));

	/* Check that adjacent frames differ (animation should be non-trivial). */
	for (int k = 0; k < PW_BUILTIN_FRAMES; k++) {
		int next = (k + 1) % PW_BUILTIN_FRAMES;
		int differ = 0;
		for (int i = 0; i < PW_BUILTIN_SIZE * PW_BUILTIN_SIZE; i++) {
			if (frames[k].pixels[i] != frames[next].pixels[i]) {
				differ = 1;
				break;
			}
		}
		assert(differ);  /* Frames should differ */
	}

	/* Clean up. */
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++)
		pw_image_free(&frames[i]);

	PASS();
}

static void test_centre_transparent(void)
{
	TEST("centre_transparent");
	struct pw_image frames[PW_BUILTIN_FRAMES];

	/* Generate frames. */
	assert(pw_cursor_builtin_frames(0x2050c0, 0xffffff, frames));

	/* Check that the centre pixel is transparent (hotspot at 16,16). */
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++) {
		uint32_t centre = frames[i].pixels[16 * PW_BUILTIN_SIZE + 16];
		assert(centre == 0x00000000);  /* Fully transparent */
	}

	/* Clean up. */
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++)
		pw_image_free(&frames[i]);

	PASS();
}

static void test_various_colors(void)
{
	TEST("various_colors");
	struct pw_image frames[PW_BUILTIN_FRAMES];

	/* Test with different colors. */
	assert(pw_cursor_builtin_frames(0xff0000, 0x000000, frames));
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++) {
		char why[256];
		assert(pw_cursorfit_check(&frames[i], why, sizeof(why)));
		pw_image_free(&frames[i]);
	}

	PASS();
}

int main(void)
{
	printf("Running cursor-builtin tests...\n");
	test_frames_alloc();
	test_frames_compliant();
	test_frames_differ();
	test_centre_transparent();
	test_various_colors();
	printf("\nAll tests passed!\n");
	return 0;
}
