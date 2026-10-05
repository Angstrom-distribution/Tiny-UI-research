/* panel-sys.c - sysfs access of picowl-panel. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "panel-sys.h"

const char *pl_sysfs_root(void)
{
	const char *r = getenv("PICOWL_SYSFS_ROOT");
	return (r && *r) ? r : "/sys";
}

/* Read a small sysfs attribute, trailing whitespace stripped. */
static bool read_attr(const char *dir, const char *attr, char *buf, size_t len)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	ssize_t n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return false;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = '\0';
	return true;
}

static int read_int(const char *dir, const char *attr)
{
	char buf[32];
	if (!read_attr(dir, attr, buf, sizeof(buf)))
		return -1;
	char *end;
	long v = strtol(buf, &end, 10);
	if (end == buf || v < 0 || v > INT_MAX)
		return -1;
	return (int)v;
}

/* A signed attribute, PL_ABSENT if it is missing or not a number. */
static int64_t read_s64(const char *dir, const char *attr)
{
	char buf[32];

	if (!read_attr(dir, attr, buf, sizeof(buf)))
		return PL_ABSENT;
	char *end;
	errno = 0;
	long long v = strtoll(buf, &end, 10);
	/* Nothing in a battery is anywhere near 10^13: that is garbage. */
	if (end == buf || errno || v > 10000000000000LL || v < -10000000000000LL)
		return PL_ABSENT;
	return v;
}

/* A charge, energy or time: negative is garbage. */
static int64_t read_pos(const char *dir, const char *attr)
{
	int64_t v = read_s64(dir, attr);

	return v < 0 ? PL_ABSENT : v;
}

bool pl_backlight_find(struct pl_backlight *bl, const char *root)
{
	char class_dir[300];
	char best[256] = "";
	int best_max = 0;

	if (!root)
		root = pl_sysfs_root();
	snprintf(class_dir, sizeof(class_dir), "%s/class/backlight", root);
	DIR *d = opendir(class_dir);
	if (!d)
		return false;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(best))
			continue;
		char dev[560];
		snprintf(dev, sizeof(dev), "%s/%s", class_dir, e->d_name);
		int max = read_int(dev, "max_brightness");
		if (max < 1)
			continue;
		if (!best[0] || strcmp(e->d_name, best) < 0) {
			snprintf(best, sizeof(best), "%s", e->d_name);
			best_max = max;
		}
	}
	closedir(d);
	if (!best[0])
		return false;
	if ((size_t)snprintf(bl->path, sizeof(bl->path), "%s/%s", class_dir, best)
			>= sizeof(bl->path))
		return false;
	bl->max = best_max;
	return true;
}

int pl_backlight_read(const struct pl_backlight *bl)
{
	return read_int(bl->path, "brightness");
}

bool pl_backlight_write(const struct pl_backlight *bl, int raw)
{
	char path[512], buf[16];

	if (raw < 1)
		raw = 1;
	if (raw > bl->max)
		raw = bl->max;
	snprintf(path, sizeof(path), "%s/brightness", bl->path);
	/* O_TRUNC changes nothing on sysfs but keeps a fake tree honest. */
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return false;
	int len = snprintf(buf, sizeof(buf), "%d", raw);
	ssize_t n = write(fd, buf, len);
	int err = errno;
	close(fd);
	if (n != len) {
		errno = n < 0 ? err : EIO;
		return false;
	}
	return true;
}

void pl_battery_read(const char *root, enum pl_bat_status *st, int *pct)
{
	struct pl_batt_raw r;

	pl_battery_read_raw(root, &r);
	*st = r.st;
	*pct = r.pct;
}

void pl_battery_read_raw(const char *root, struct pl_batt_raw *r)
{
	char dir[300];
	char bat[256] = "";
	char ac[256] = "";
	int chargers = 0;

	pl_batt_raw_clear(r);
	if (!root)
		root = pl_sysfs_root();
	snprintf(dir, sizeof(dir), "%s/class/power_supply", root);
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(bat))
			continue;
		char sup[560], type[32], scope[32];
		snprintf(sup, sizeof(sup), "%s/%s", dir, e->d_name);
		if (!read_attr(sup, "type", type, sizeof(type)))
			continue;
		if (!strcmp(type, "Battery")) {
			/* A battery of an input device is not the system's. */
			if (read_attr(sup, "scope", scope, sizeof(scope)) &&
					!strcmp(scope, "Device"))
				continue;
			if (read_int(sup, "capacity") < 0)
				continue;
			if (!bat[0] || strcmp(e->d_name, bat) < 0)
				snprintf(bat, sizeof(bat), "%s", e->d_name);
		} else if (!strcmp(type, "Mains") || !strncmp(type, "USB", 3)) {
			chargers++;
			if (read_int(sup, "online") == 1 &&
					(!ac[0] || strcmp(e->d_name, ac) < 0))
				snprintf(ac, sizeof(ac), "%s", e->d_name);
		}
	}
	closedir(d);

	if (bat[0]) {
		char sup[560], status[32];
		snprintf(sup, sizeof(sup), "%s/%s", dir, bat);
		int v = read_int(sup, "capacity");
		r->pct = v > 100 ? 100 : v;
		r->st = PL_BAT_DISCHARGING;
		if (read_attr(sup, "status", status, sizeof(status))) {
			if (!strcmp(status, "Charging"))
				r->st = PL_BAT_CHARGING;
			else if (!strcmp(status, "Full"))
				r->st = PL_BAT_FULL;
			else if (!strcmp(status, "Not charging"))
				r->not_charging = true;
		}
		r->charge_now = read_pos(sup, "charge_now");
		r->charge_full = read_pos(sup, "charge_full");
		r->charge_full_design = read_pos(sup, "charge_full_design");
		r->charge_empty = read_pos(sup, "charge_empty");
		r->energy_now = read_pos(sup, "energy_now");
		r->energy_full = read_pos(sup, "energy_full");
		r->energy_full_design = read_pos(sup, "energy_full_design");
		r->current_now = read_s64(sup, "current_now");
		r->current_avg = read_s64(sup, "current_avg");
		r->power_now = read_s64(sup, "power_now");
		r->time_to_empty_now = read_pos(sup, "time_to_empty_now");
	} else if (ac[0]) {
		r->st = PL_BAT_AC;
	}
	if (chargers)
		r->charger = ac[0] ? 1 : 0;
}
