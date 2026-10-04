/*
 * backlight.h - sysfs backlight access. No wlroots, no wayland; every sysfs
 * path is built from a root prefix so tests can use a fake tree.
 *
 * Layout: <root>/class/backlight/<dev>/{brightness,max_brightness,
 * actual_brightness,type}. root NULL means pw_sysfs_root().
 */
#ifndef PICOWL_BACKLIGHT_H
#define PICOWL_BACKLIGHT_H

#include <stdbool.h>

struct pw_backlight;

/* Root prefix for sysfs: $PICOWL_SYSFS_ROOT if set and non-empty, else "/sys". */
const char *pw_sysfs_root(void);

/* Open a backlight. name NULL or "auto": prefer type firmware > platform >
 * raw, ties broken by first name (strcmp order). Otherwise the named device.
 * Reads max_brightness and the user level (actual_brightness, fallback
 * brightness) once. Returns NULL if no usable device (logs at debug/info).
 * Does not check writability; set() reports that. */
struct pw_backlight *pw_backlight_open(const char *root, const char *name);

/* Device name (valid until close). */
const char *pw_backlight_name(const struct pw_backlight *bl);

/* max_brightness (>= 1). */
int pw_backlight_get_max(const struct pw_backlight *bl);

/* User level read at open (raw units, 0..max), updated by
 * pw_backlight_set_user(). */
int pw_backlight_get_user(const struct pw_backlight *bl);
void pw_backlight_set_user(struct pw_backlight *bl, int level);

/* Another process (the panel's slider) wrote the brightness since picowl last
 * did: if the device shows a level other than the one picowl left it at, that
 * level becomes the user level and true is returned. Only valid while the
 * panel is at the level picowl wrote for the ACTIVE state, i.e. not while
 * dimmed: a dimmed level is not the user's. Level 0 is ignored. */
bool pw_backlight_adopt_external(struct pw_backlight *bl);

/* Scale: level = max(1, max * pct / 100) for pct in 1..100, integer maths,
 * never 0 and never above max. Pure; used for the LOW cap. */
int pw_backlight_scale(int max, int pct);

/* Scale a raw level by pct (dim): max(1, level * pct / 100), min(.., max). */
int pw_backlight_scale_level(int level, int pct);

/* Write brightness (clamped to 0..max). Returns 0 on success, -1 on error
 * with errno set (EACCES: caller should log once and disable dimming). */
int pw_backlight_set(struct pw_backlight *bl, int level);

void pw_backlight_close(struct pw_backlight *bl);

#endif
