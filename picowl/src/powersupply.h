/*
 * powersupply.h - power supply profile from sysfs plus kernel uevent
 * socket. No wlroots; the event loop integration lives in power.c, which
 * adds pw_ps_uevent_fd() to the wl_event_loop and calls pw_ps_uevent_drain()
 * when readable.
 *
 * Profile rule: AC if any supply of type Mains or USB has online=1; else LOW
 * if any Battery has capacity <= low_capacity; else BATTERY. No readable
 * supply information at all -> BATTERY.
 */
#ifndef PICOWL_POWERSUPPLY_H
#define PICOWL_POWERSUPPLY_H

#include <stdbool.h>
#include <stddef.h>
#include "powerprofile.h"

struct pw_ps_info {
	enum pw_power_profile profile;
	bool any_supply;       /* at least one supply readable */
	bool ac_online;        /* a Mains/USB supply is online */
	int capacity;          /* lowest Battery capacity %, -1 if none */
};

/* Re-read <root>/class/power_supply/ *. root NULL = pw_sysfs_root(). Fills
 * *info (may be NULL) and returns the profile. */
enum pw_power_profile pw_ps_read_profile(const char *root, int low_capacity,
	struct pw_ps_info *info);

/* Parse one kernel uevent message ("ACTION@DEVPATH\0KEY=VALUE\0...").
 * True iff it carries SUBSYSTEM=power_supply. Pure; buf need not be
 * NUL-terminated at len. */
bool pw_ps_uevent_is_power_supply(const char *buf, size_t len);

struct pw_ps_uevent {
	int fd;                /* -1 when closed */
};

/* Open NETLINK_KOBJECT_UEVENT, group 1, SOCK_NONBLOCK|SOCK_CLOEXEC.
 * Returns 0 or -1 (errno set; caller falls back to polling only). */
int pw_ps_uevent_open(struct pw_ps_uevent *u);
void pw_ps_uevent_close(struct pw_ps_uevent *u);
int pw_ps_uevent_fd(const struct pw_ps_uevent *u);

/* Read every pending message without blocking (static buffer, no
 * allocation). Returns true if at least one power_supply event was seen. */
bool pw_ps_uevent_drain(struct pw_ps_uevent *u);

#endif
