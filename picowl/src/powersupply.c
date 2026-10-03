/* powersupply.c - power supply profile from sysfs and kernel uevent socket. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include "backlight.h"
#include "powersupply.h"

/* Read a small sysfs attribute into buf (NUL terminated, newline stripped).
 * No heap allocation. Returns the length, or -1 on error. */
static int read_attr(const char *dir, const char *name, const char *attr,
	char *buf, size_t size)
{
	char path[1024];
	int n = snprintf(path, sizeof(path), "%s/%s/%s", dir, name, attr);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -1;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t r = read(fd, buf, size - 1);
	close(fd);
	if (r <= 0)
		return -1;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
		r--;
	buf[r] = '\0';
	return (int)r;
}

struct ps_dirent64 {
	uint64_t d_ino;
	int64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

enum pw_power_profile pw_ps_read_profile(const char *root, int low_capacity,
	struct pw_ps_info *info)
{
	if (!root)
		root = pw_sysfs_root();

	enum pw_power_profile profile = PW_PROFILE_BATTERY;
	bool any_supply = false;
	bool ac_online = false;
	int lowest_capacity = -1;

	/* Scan /sys/class/power_supply/ with getdents64 into a stack buffer
	 * (no opendir/fopen: no heap allocation per event). */
	char ps_path[512];
	snprintf(ps_path, sizeof(ps_path), "%s/class/power_supply", root);

	int dfd = open(ps_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		goto done;

	char dbuf[2048] __attribute__((aligned(8)));
	long nread;
	while ((nread = syscall(SYS_getdents64, dfd, dbuf, sizeof(dbuf))) > 0) {
		for (long off = 0; off < nread;) {
			struct ps_dirent64 *entry = (struct ps_dirent64 *)(dbuf + off);
			off += entry->d_reclen;
			if (entry->d_name[0] == '.')
				continue;

			char type[32];
			if (read_attr(ps_path, entry->d_name, "type", type, sizeof(type)) < 0)
				continue;

			any_supply = true;

			if (strcmp(type, "Mains") == 0 || strcmp(type, "USB") == 0) {
				char val[16];
				if (read_attr(ps_path, entry->d_name, "online", val, sizeof(val)) > 0
						&& atoi(val) != 0)
					ac_online = true;
			} else if (strcmp(type, "Battery") == 0) {
				char val[16];
				if (read_attr(ps_path, entry->d_name, "capacity", val, sizeof(val)) > 0) {
					char *endp;
					long cap = strtol(val, &endp, 10);
					if (endp != val && cap >= 0 && cap <= 1000 &&
							(lowest_capacity < 0 || cap < lowest_capacity))
						lowest_capacity = (int)cap;
				}
			}
		}
	}

	close(dfd);

done:
	/* Determine profile */
	if (ac_online) {
		profile = PW_PROFILE_AC;
	} else if (lowest_capacity >= 0 && lowest_capacity <= low_capacity) {
		profile = PW_PROFILE_LOW;
	} else {
		profile = PW_PROFILE_BATTERY;
	}

	if (info) {
		info->profile = profile;
		info->any_supply = any_supply;
		info->ac_online = ac_online;
		info->capacity = lowest_capacity;
	}

	return profile;
}

bool pw_ps_uevent_is_power_supply(const char *buf, size_t len)
{
	/* Parse "ACTION@DEVPATH\0KEY=VALUE\0..." format.
	 * We need to find SUBSYSTEM=power_supply in the KEY=VALUE pairs. */

	if (len < 2)
		return false;

	/* Skip the ACTION@DEVPATH\0 part */
	const char *pos = buf;
	const char *end = buf + len;

	/* Find the first NUL terminator */
	while (pos < end && *pos != '\0')
		pos++;

	if (pos >= end)
		return false;

	/* Skip the NUL */
	pos++;

	/* Now parse KEY=VALUE pairs */
	while (pos < end) {
		/* Find the next NUL or end of buffer */
		const char *pair_end = pos;
		while (pair_end < end && *pair_end != '\0')
			pair_end++;

		/* Parse this KEY=VALUE pair */
		size_t pair_len = pair_end - pos;
		if (pair_len > 0) {
			/* Look for SUBSYSTEM=power_supply */
			if (pair_len >= sizeof("SUBSYSTEM=") - 1 && strncmp(pos, "SUBSYSTEM=", 10) == 0) {
				const char *value = pos + 10;
				size_t value_len = pair_len - 10;
				if (value_len == 12 && strncmp(value, "power_supply", 12) == 0) {
					return true;
				}
			}
		}

		pos = pair_end + 1;
	}

	return false;
}

int pw_ps_uevent_open(struct pw_ps_uevent *u)
{
	u->fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
		NETLINK_KOBJECT_UEVENT);
	if (u->fd < 0)
		return -1;

	/* Bind to group 1 */
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
		.nl_groups = 1,
	};

	if (bind(u->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(u->fd);
		u->fd = -1;
		return -1;
	}

	return 0;
}

void pw_ps_uevent_close(struct pw_ps_uevent *u)
{
	if (u->fd >= 0)
		close(u->fd);
	u->fd = -1;
}

int pw_ps_uevent_fd(const struct pw_ps_uevent *u)
{
	return u->fd;
}

bool pw_ps_uevent_drain(struct pw_ps_uevent *u)
{
	bool saw_power_supply = false;

	if (u->fd < 0)
		return false;

	/* Read messages from the socket until EAGAIN */
	char buf[4096];
	ssize_t n;

	for (;;) {
		n = recv(u->fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ENOBUFS) {
				/* Queue overflowed: events were lost, re-read. */
				saw_power_supply = true;
				continue;
			}
			break; /* EAGAIN or hard error */
		}
		if (n == 0)
			break;
		if (pw_ps_uevent_is_power_supply(buf, (size_t)n))
			saw_power_supply = true;
	}

	return saw_power_supply;
}
