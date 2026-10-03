/* test-backlight.c - unit tests for backlight device access. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "backlight.h"

static int test_count = 0;
static int fail_count = 0;

#define ASSERT(cond, msg) do { \
	test_count++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s\n", msg); \
		fail_count++; \
	} else { \
		printf("✓ %s\n", msg); \
	} \
} while (0)

#define ASSERT_EQ(got, want, msg) do { \
	test_count++; \
	if ((got) != (want)) { \
		fprintf(stderr, "FAIL: %s (got %d, want %d)\n", msg, (int)(got), (int)(want)); \
		fail_count++; \
	} else { \
		printf("✓ %s\n", msg); \
	} \
} while (0)

#define ASSERT_STR_EQ(got, want, msg) do { \
	test_count++; \
	if (strcmp((got), (want)) != 0) { \
		fprintf(stderr, "FAIL: %s (got '%s', want '%s')\n", msg, got, want); \
		fail_count++; \
	} else { \
		printf("✓ %s\n", msg); \
	} \
} while (0)

#define ASSERT_NULL(ptr, msg) do { \
	test_count++; \
	if ((ptr) != NULL) { \
		fprintf(stderr, "FAIL: %s (expected NULL)\n", msg); \
		fail_count++; \
	} else { \
		printf("✓ %s\n", msg); \
	} \
} while (0)

#define ASSERT_NOT_NULL(ptr, msg) do { \
	test_count++; \
	if ((ptr) == NULL) { \
		fprintf(stderr, "FAIL: %s (expected non-NULL)\n", msg); \
		fail_count++; \
	} else { \
		printf("✓ %s\n", msg); \
	} \
} while (0)

/* Helper: write a file atomically */
static void write_file(const char *path, const char *content)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror("write_file");
		abort();
	}

	ssize_t len = strlen(content);
	if (write(fd, content, len) != len) {
		perror("write");
		close(fd);
		abort();
	}
	close(fd);
}

/* Helper: create directory recursively */
static void mkdir_p(const char *path)
{
	char tmp[256];
	strcpy(tmp, path);

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

/* Helper: remove a directory tree */
static void rmdir_r(const char *path)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
	system(cmd);
}

/* Helper: write file to path/attr */
static void write_attr(const char *path, const char *attr, const char *content)
{
	char fullpath[512];
	snprintf(fullpath, sizeof(fullpath), "%s/%s", path, attr);
	write_file(fullpath, content);
}

int main(void)
{
	printf("test-backlight: unit tests\n");

	/* Test: sysfs_root defaults to /sys */
	ASSERT_STR_EQ(pw_sysfs_root(), "/sys", "sysfs_root defaults to /sys");

	/* Test: scale 100% of max */
	ASSERT_EQ(pw_backlight_scale(64, 100), 64, "scale 64x100%");
	ASSERT_EQ(pw_backlight_scale(100, 100), 100, "scale 100x100%");

	/* Test: scale 30% of max (e.g., dim level) */
	ASSERT_EQ(pw_backlight_scale(64, 30), 19, "scale 64x30% = 19");
	ASSERT_EQ(pw_backlight_scale(100, 30), 30, "scale 100x30% = 30");

	/* Test: scale minimum is 1 */
	ASSERT_EQ(pw_backlight_scale(64, 1), 1, "scale 64x1% = 1");
	ASSERT_EQ(pw_backlight_scale(10, 5), 1, "scale 10x5% min 1");
	ASSERT_EQ(pw_backlight_scale(1, 50), 1, "scale 1x50% = 1");

	/* Test: scale_level */
	ASSERT_EQ(pw_backlight_scale_level(100, 30), 30, "scale_level 100x30%");
	ASSERT_EQ(pw_backlight_scale_level(19, 100), 19, "scale_level 19x100%");
	ASSERT_EQ(pw_backlight_scale_level(10, 10), 1, "scale_level minimum 1");

	/* Create a temporary test directory with fake backlight devices */
	char tmpdir[200];
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/picowl-backlight-test.%d", getpid());

	rmdir_r(tmpdir);
	mkdir_p(tmpdir);
	setenv("XDG_RUNTIME_DIR", tmpdir, 1); /* user-level state file */

	/* Test: no devices in empty tree */
	char root[256];
	snprintf(root, sizeof(root), "%s/sys1", tmpdir);
	mkdir_p(root);

	struct pw_backlight *bl = pw_backlight_open(root, "auto");
	ASSERT_NULL(bl, "open empty tree returns NULL");

	/* Test: single firmware device */
	snprintf(root, sizeof(root), "%s/sys2", tmpdir);
	mkdir_p(root);
	char path[384];
	snprintf(path, sizeof(path), "%s/class/backlight/lcd0", root);
	mkdir_p(path);

	write_attr(path, "type", "firmware\n");
	write_attr(path, "max_brightness", "255\n");
	write_attr(path, "actual_brightness", "128\n");
	write_attr(path, "brightness", "128\n");

	bl = pw_backlight_open(root, "auto");
	ASSERT_NOT_NULL(bl, "open with one device");
	if (bl) {
		ASSERT_STR_EQ(pw_backlight_name(bl), "lcd0", "device name is lcd0");
		ASSERT_EQ(pw_backlight_get_max(bl), 255, "max_brightness is 255");
		ASSERT_EQ(pw_backlight_get_user(bl), 128, "user level is 128");
		pw_backlight_close(bl);
	}

	/* Test: prefer firmware > platform > raw */
	snprintf(root, sizeof(root), "%s/sys3", tmpdir);
	mkdir_p(root);

	/* Create platform, firmware, and raw devices */
	const char *devices[] = { "platform_dev", "firmware_dev", "raw_dev" };
	const char *types[] = { "platform", "firmware", "raw" };

	for (int i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%s/class/backlight/%s", root, devices[i]);
		mkdir_p(path);
		write_attr(path, "type", types[i]);
		write_attr(path, "max_brightness", "100\n");
		write_attr(path, "actual_brightness", "50\n");
		write_attr(path, "brightness", "50\n");
	}

	bl = pw_backlight_open(root, "auto");
	ASSERT_NOT_NULL(bl, "open with multiple devices");
	if (bl) {
		ASSERT_STR_EQ(pw_backlight_name(bl), "firmware_dev", "prefer firmware");
		pw_backlight_close(bl);
	}

	/* Test: tiebreak by name order */
	snprintf(root, sizeof(root), "%s/sys4", tmpdir);
	mkdir_p(root);

	for (int i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%s/class/backlight/lcd%d", root, i);
		mkdir_p(path);
		write_attr(path, "type", "platform");
		write_attr(path, "max_brightness", "100\n");
		write_attr(path, "actual_brightness", "50\n");
		write_attr(path, "brightness", "50\n");
	}

	bl = pw_backlight_open(root, "auto");
	ASSERT_NOT_NULL(bl, "open with same type devices");
	if (bl) {
		ASSERT_STR_EQ(pw_backlight_name(bl), "lcd0", "tiebreak by name order");
		pw_backlight_close(bl);
	}

	/* Test: explicit device name */
	snprintf(root, sizeof(root), "%s/sys5", tmpdir);
	mkdir_p(root);

	snprintf(path, sizeof(path), "%s/class/backlight/my_backlight", root);
	mkdir_p(path);
	write_attr(path, "type", "firmware\n");
	write_attr(path, "max_brightness", "200\n");
	write_attr(path, "actual_brightness", "100\n");
	write_attr(path, "brightness", "100\n");

	bl = pw_backlight_open(root, "my_backlight");
	ASSERT_NOT_NULL(bl, "open with explicit name");
	if (bl) {
		ASSERT_STR_EQ(pw_backlight_name(bl), "my_backlight", "name matches");
		ASSERT_EQ(pw_backlight_get_max(bl), 200, "max is 200");
		pw_backlight_close(bl);
	}

	/* Test: explicit nonexistent device */
	bl = pw_backlight_open(root, "nonexistent");
	ASSERT_NULL(bl, "open nonexistent device returns NULL");

	/* Test: on/off backlight (max <= 1) */
	snprintf(root, sizeof(root), "%s/sys6", tmpdir);
	mkdir_p(root);

	snprintf(path, sizeof(path), "%s/class/backlight/gpio_backlight", root);
	mkdir_p(path);
	write_attr(path, "type", "raw\n");
	write_attr(path, "max_brightness", "1\n");
	write_attr(path, "actual_brightness", "1\n");
	write_attr(path, "brightness", "1\n");

	bl = pw_backlight_open(root, "auto");
	ASSERT_NOT_NULL(bl, "open on/off backlight");
	if (bl) {
		ASSERT_EQ(pw_backlight_get_max(bl), 1, "max is 1");
		pw_backlight_close(bl);
	}

	/* Test: read actual_brightness as fallback */
	snprintf(root, sizeof(root), "%s/sys7", tmpdir);
	mkdir_p(root);

	snprintf(path, sizeof(path), "%s/class/backlight/lcd", root);
	mkdir_p(path);
	write_attr(path, "type", "firmware\n");
	write_attr(path, "max_brightness", "100\n");
	write_attr(path, "actual_brightness", "75\n");
	write_attr(path, "brightness", "75\n");

	bl = pw_backlight_open(root, "auto");
	ASSERT_NOT_NULL(bl, "open with fallback brightness");
	if (bl) {
		ASSERT_EQ(pw_backlight_get_user(bl), 75, "user level from actual_brightness");
		pw_backlight_close(bl);
	}

	/* Test: missing actual_brightness falls back to brightness */
	snprintf(root, sizeof(root), "%s/sys7a", tmpdir);
	mkdir_p(root);
	snprintf(path, sizeof(path), "%s/class/backlight/lcd", root);
	mkdir_p(path);
	write_attr(path, "type", "firmware\n");
	write_attr(path, "max_brightness", "100\n");
	write_attr(path, "brightness", "40\n");
	bl = pw_backlight_open(root, "auto");
	ASSERT_NOT_NULL(bl, "open without actual_brightness");
	if (bl) {
		ASSERT_EQ(pw_backlight_get_user(bl), 40, "user from brightness");
		pw_backlight_close(bl);
	}

	/* Test: actual_brightness=0 uses brightness, never 0 */
	snprintf(root, sizeof(root), "%s/sys7b", tmpdir);
	mkdir_p(root);
	snprintf(path, sizeof(path), "%s/class/backlight/lcd", root);
	mkdir_p(path);
	write_attr(path, "type", "firmware\n");
	write_attr(path, "max_brightness", "100\n");
	write_attr(path, "actual_brightness", "0\n");
	write_attr(path, "brightness", "60\n");
	bl = pw_backlight_open(root, "auto");
	ASSERT_NOT_NULL(bl, "open with actual_brightness=0");
	if (bl) {
		ASSERT_EQ(pw_backlight_get_user(bl), 60, "zero actual falls back to brightness");
		pw_backlight_close(bl);
	}

	/* Test: both zero uses max */
	write_attr(path, "brightness", "0\n");
	bl = pw_backlight_open(root, "auto");
	ASSERT_NOT_NULL(bl, "open with all zero");
	if (bl) {
		ASSERT_EQ(pw_backlight_get_user(bl), 100, "all zero falls back to max");
		pw_backlight_close(bl);
	}

	/* Test: write brightness */
	snprintf(root, sizeof(root), "%s/sys8", tmpdir);
	mkdir_p(root);

	snprintf(path, sizeof(path), "%s/class/backlight/lcd", root);
	mkdir_p(path);
	write_attr(path, "type", "firmware\n");
	write_attr(path, "max_brightness", "100\n");
	write_attr(path, "actual_brightness", "50\n");
	write_attr(path, "brightness", "50\n");

	bl = pw_backlight_open(root, "auto");
	if (bl) {
		int ret = pw_backlight_set(bl, 75);
		ASSERT_EQ(ret, 0, "write brightness succeeds");

		/* Verify the value was written */
		char brightness_path[512];
		snprintf(brightness_path, sizeof(brightness_path), "%s/brightness", path);
		int fd = open(brightness_path, O_RDONLY);
		char buf[32];
		ssize_t nread = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (nread > 0) {
			buf[nread] = '\0';
			int written = atoi(buf);
			ASSERT_EQ(written, 75, "brightness value written");
		}

		pw_backlight_close(bl);
	}

	/* Test: clamping and minimum 1 */
	snprintf(root, sizeof(root), "%s/sys9", tmpdir);
	mkdir_p(root);

	snprintf(path, sizeof(path), "%s/class/backlight/lcd", root);
	mkdir_p(path);
	write_attr(path, "type", "firmware\n");
	write_attr(path, "max_brightness", "100\n");
	write_attr(path, "actual_brightness", "50\n");
	write_attr(path, "brightness", "50\n");

	bl = pw_backlight_open(root, "auto");
	if (bl) {
		/* Write 0 (should be clamped) */
		pw_backlight_set(bl, 0);

		/* Write above max (should be clamped) */
		pw_backlight_set(bl, 200);

		/* User level management */
		pw_backlight_set_user(bl, 80);
		ASSERT_EQ(pw_backlight_get_user(bl), 80, "set_user updates level");

		pw_backlight_close(bl);
	}

	/* Test: a crash while dimmed must not make the dimmed level the user's.
	 * The panel sits at 20 and picowl dies (close without restoring); the
	 * next open reads actual_brightness 20 but gets the saved level 50. */
	snprintf(root, sizeof(root), "%s/sys11", tmpdir);
	snprintf(path, sizeof(path), "%s/class/backlight/crash", root);
	mkdir_p(path);
	write_attr(path, "type", "raw\n");
	write_attr(path, "max_brightness", "100\n");
	write_attr(path, "actual_brightness", "50\n");
	write_attr(path, "brightness", "50\n");
	bl = pw_backlight_open(root, "crash");
	ASSERT_NOT_NULL(bl, "open crash device");
	if (bl) {
		ASSERT_EQ(pw_backlight_get_user(bl), 50, "user level before dim");
		ASSERT_EQ(pw_backlight_set(bl, 20), 0, "dim write");
		write_attr(path, "actual_brightness", "20\n");
		pw_backlight_close(bl); /* crash: no restore */

		bl = pw_backlight_open(root, "crash");
		ASSERT_NOT_NULL(bl, "reopen after crash");
		if (bl) {
			ASSERT_EQ(pw_backlight_get_user(bl), 50,
				"user level survives a crash while dimmed");
			/* Writing the user level back clears the state. */
			ASSERT_EQ(pw_backlight_set(bl, 50), 0, "restore write");
			write_attr(path, "actual_brightness", "50\n");
			pw_backlight_close(bl);
		}
		write_attr(path, "actual_brightness", "33\n");
		bl = pw_backlight_open(root, "crash");
		if (bl) {
			ASSERT_EQ(pw_backlight_get_user(bl), 33,
				"no state after a restore: live level is the user level");
			pw_backlight_close(bl);
		}
	}

	/* Test: read-only file handling (skip if running as root) */
	if (getuid() != 0) {
		snprintf(root, sizeof(root), "%s/sys10", tmpdir);
		mkdir_p(root);

		snprintf(path, sizeof(path), "%s/class/backlight/readonly", root);
		mkdir_p(path);
		write_attr(path, "type", "firmware\n");
		write_attr(path, "max_brightness", "100\n");
		write_attr(path, "actual_brightness", "50\n");
		write_attr(path, "brightness", "50\n");

		/* Make brightness read-only */
		char brightness_path[512];
		snprintf(brightness_path, sizeof(brightness_path), "%s/brightness", path);
		chmod(brightness_path, 0444);

		bl = pw_backlight_open(root, "auto");
		if (bl) {
			int ret = pw_backlight_set(bl, 75);
			ASSERT_EQ(ret, -1, "write to read-only file returns error");
			ASSERT_EQ(errno, EACCES, "errno is EACCES");

			pw_backlight_close(bl);
		}
	} else {
		printf("✓ skipping read-only file test (running as root)\n");
		test_count += 2;
	}

	/* Clean up */
	rmdir_r(tmpdir);

	printf("\ntest-backlight: %d tests, %d failed\n", test_count, fail_count);
	return (fail_count > 0) ? 1 : 0;
}
