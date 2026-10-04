/*
 * panel-sys.h - sysfs access of picowl-panel: the backlight and the battery.
 * Every path is built from a root prefix so the tests can use a fake tree.
 */
#ifndef PICOWL_PANEL_SYS_H
#define PICOWL_PANEL_SYS_H

#include <stdbool.h>
#include "panel-logic.h"

/* $PICOWL_SYSFS_ROOT if set and non-empty, else "/sys" (as picowl does). */
const char *pl_sysfs_root(void);

struct pl_backlight {
	char path[300];		/* <root>/class/backlight/<dev> */
	int max;
};

/* The first device (strcmp order of the names) with a max_brightness >= 1.
 * root NULL means pl_sysfs_root(). False if there is none. */
bool pl_backlight_find(struct pl_backlight *bl, const char *root);

/* Raw brightness, -1 if unreadable. */
int pl_backlight_read(const struct pl_backlight *bl);

/* Writes the raw value (clamped to 1..max); false and errno on failure. */
bool pl_backlight_write(const struct pl_backlight *bl, int raw);

/* The first supply of type Battery with a readable capacity: percent and
 * charge state. Else, if a Mains or USB supply is online, PL_BAT_AC; else
 * PL_BAT_NONE. *pct is -1 without a battery. */
void pl_battery_read(const char *root, enum pl_bat_status *st, int *pct);

#endif
