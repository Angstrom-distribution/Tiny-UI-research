/* backlight.c - sysfs backlight access. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "backlight.h"

/* Conditional logging for test vs. picowl builds */
#ifdef __has_include
#if __has_include(<wlr/util/log.h>)
#include <wlr/util/log.h>
#define bl_log_info(...) wlr_log(WLR_INFO, __VA_ARGS__)
#define bl_log_debug(...) wlr_log(WLR_DEBUG, __VA_ARGS__)
#else
#define bl_log_info(...)
#define bl_log_debug(...)
#endif
#else
#define bl_log_info(...)
#define bl_log_debug(...)
#endif

struct pw_backlight {
	char name[256];
	int max;
	int user;
	const char *root;
	int max_brightness_written; /* for detecting read-only files */
};

const char *pw_sysfs_root(void)
{
	const char *r = getenv("PICOWL_SYSFS_ROOT");
	return (r && *r) ? r : "/sys";
}

/* Read an integer from a sysfs file. Returns -1 on error. */
static int read_sysfs_int(const char *root, const char *dev, const char *attr)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/class/backlight/%s/%s", root, dev, attr);

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	char buf[32];
	ssize_t nread = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (nread <= 0)
		return -1;

	buf[nread] = '\0';
	char *endp;
	long val = strtol(buf, &endp, 10);

	if (val >= 0 && val <= INT_MAX)
		return (int)val;
	return -1;
}

/* While the panel sits at a level picowl wrote that differs from the user
 * level (dimmed, LOW cap), the user level is kept in a file in the runtime
 * dir, so a restart after a crash does not take the reduced level for the
 * user's. The file is removed when the user level is written back. */
static void state_path(const struct pw_backlight *bl, char *path, size_t len)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	snprintf(path, len, "%s/picowl-backlight-%s",
		(dir && *dir) ? dir : "/run", bl->name);
}

static void state_save(const struct pw_backlight *bl, int level)
{
	char path[512];
	state_path(bl, path, sizeof(path));
	if (level == bl->user) {
		unlink(path);
		return;
	}
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	char buf[16];
	int len = snprintf(buf, sizeof(buf), "%d\n", bl->user);
	if (write(fd, buf, len) != len)
		unlink(path);
	close(fd);
}

/* Saved user level (1..max), or -1. */
static int state_load(const struct pw_backlight *bl)
{
	char path[512], buf[16];
	state_path(bl, path, sizeof(path));
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	long v = strtol(buf, NULL, 10);
	return (v >= 1 && v <= bl->max) ? (int)v : -1;
}

/* Read a string from a sysfs file. Returns 0 on success. */
static int read_sysfs_string(const char *root, const char *dev, const char *attr,
	char *buf, size_t len)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/class/backlight/%s/%s", root, dev, attr);

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	ssize_t nread = read(fd, buf, len - 1);
	close(fd);

	if (nread <= 0)
		return -1;

	buf[nread] = '\0';
	/* Strip trailing newline */
	if (nread > 0 && buf[nread - 1] == '\n')
		buf[nread - 1] = '\0';

	return 0;
}

/* Compare backlight device types for preference. Returns < 0 if a is better. */
static int compare_types(const char *a_type, const char *b_type)
{
	static const char *order[] = { "firmware", "platform", "raw" };

	int a_rank = 3, b_rank = 3;
	for (int i = 0; i < 3; i++) {
		if (strcmp(a_type, order[i]) == 0)
			a_rank = i;
		if (strcmp(b_type, order[i]) == 0)
			b_rank = i;
	}

	if (a_rank != b_rank)
		return a_rank - b_rank;

	/* Same type: compare by name */
	return strcmp(a_type, b_type);
}

/* Find the best backlight device matching the preference.
 * Returns the device name in 'best', or NULL if none found. */
static const char *find_backlight(const char *root, const char *name, char *best, size_t len)
{
	/* If a specific name is requested, use it directly */
	if (name && name[0] && strcmp(name, "auto") != 0) {
		snprintf(best, len, "%s", name);
		return best;
	}

	/* Auto-discovery: scan /sys/class/backlight */
	char class_path[512];
	snprintf(class_path, sizeof(class_path), "%s/class/backlight", root);

	DIR *dir = opendir(class_path);
	if (!dir)
		return NULL;

	char best_name[256] = "";
	char best_type[64] = "";
	int best_valid = 0;

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;

		/* Read the device type */
		char type[64] = "";
		int max_brightness = read_sysfs_int(root, entry->d_name, "max_brightness");

		if (max_brightness <= 0) {
			continue; /* Skip invalid devices */
		}

		if (read_sysfs_string(root, entry->d_name, "type", type, sizeof(type)) < 0) {
			strcpy(type, "unknown");
		}

		/* Check if this device is better than the current best */
		if (!best_valid || compare_types(type, best_type) < 0) {
			strcpy(best_name, entry->d_name);
			strcpy(best_type, type);
			best_valid = 1;
		} else if (compare_types(type, best_type) == 0 && strcmp(entry->d_name, best_name) < 0) {
			/* Same type: prefer by name order */
			strcpy(best_name, entry->d_name);
		}
	}

	closedir(dir);

	if (!best_valid)
		return NULL;

	snprintf(best, len, "%s", best_name);
	return best;
}

struct pw_backlight *pw_backlight_open(const char *root, const char *name)
{
	if (!root)
		root = pw_sysfs_root();

	char dev_name[256];
	if (!find_backlight(root, name, dev_name, sizeof(dev_name))) {
		return NULL;
	}

	struct pw_backlight *bl = malloc(sizeof(*bl));
	if (!bl)
		return NULL;

	/* Read max_brightness */
	int max = read_sysfs_int(root, dev_name, "max_brightness");
	if (max <= 0) {
		free(bl);
		return NULL;
	}

	/* Read actual_brightness (user level) */
	int user = read_sysfs_int(root, dev_name, "actual_brightness");
	if (user <= 0)
		user = read_sysfs_int(root, dev_name, "brightness");
	if (user <= 0)
		user = max; /* never restore to 0 */
	if (user > max)
		user = max;

	snprintf(bl->name, sizeof(bl->name), "%s", dev_name);
	bl->max = max;
	bl->user = user;
	bl->root = root;
	bl->max_brightness_written = 0;

	/* A previous run died while the panel was dimmed or capped. */
	int saved = state_load(bl);
	if (saved > 0)
		bl->user = user = saved;

	bl_log_debug( "backlight: opened %s (max=%d, user=%d)", dev_name, max, user);

	return bl;
}

const char *pw_backlight_name(const struct pw_backlight *bl)
{
	return bl ? bl->name : "";
}

int pw_backlight_get_max(const struct pw_backlight *bl)
{
	return bl ? bl->max : 0;
}

int pw_backlight_get_user(const struct pw_backlight *bl)
{
	return bl ? bl->user : 0;
}

void pw_backlight_set_user(struct pw_backlight *bl, int level)
{
	if (bl) {
		if (level < 0)
			level = 0;
		if (level > bl->max)
			level = bl->max;
		bl->user = level;
	}
}

int pw_backlight_scale(int max, int pct)
{
	if (max <= 0 || pct <= 0)
		return 1;

	/* Integer math: (max * pct) / 100, minimum 1 */
	int result = (max * pct) / 100;
	if (result < 1)
		result = 1;
	if (result > max)
		result = max;

	return result;
}

int pw_backlight_scale_level(int level, int pct)
{
	if (level <= 0 || pct <= 0)
		return 1;

	/* Integer math: (level * pct) / 100, minimum 1 */
	int result = (level * pct) / 100;
	if (result < 1)
		result = 1;

	return result;
}

int pw_backlight_set(struct pw_backlight *bl, int level)
{
	if (!bl)
		return -1;

	/* Clamp level to valid range */
	if (level < 0)
		level = 0;
	if (level > bl->max)
		level = bl->max;

	/* Never write 0 for non-zero requests */
	if (level > 0 && level < 1)
		level = 1;

	char path[512];
	snprintf(path, sizeof(path), "%s/class/backlight/%s/brightness", bl->root, bl->name);

	/* Open, write, close each time to avoid buffering surprises */
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		return -1; /* errno from open() */
	}

	char buf[32];
	int len = snprintf(buf, sizeof(buf), "%d", level);

	ssize_t nwritten = write(fd, buf, len);
	int write_err = errno;
	close(fd);

	if (nwritten != len) {
		errno = write_err;
		return -1;
	}

	state_save(bl, level);
	return 0;
}

void pw_backlight_close(struct pw_backlight *bl)
{
	free(bl);
}
