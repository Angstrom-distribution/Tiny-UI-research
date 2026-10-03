/*
 * power.h - compositor glue for power-supply aware dimming. Owns the
 * backlight, the power supply state, the uevent socket, the dim state
 * machine and its timer. State lives in server->power. Inactive (all calls
 * harmless) when there is no backlight and no idle timeouts.
 */
#ifndef PICOWL_POWER_H
#define PICOWL_POWER_H

#include <stdbool.h>

struct pw_server;

/* Create the module after pw_idle_init() (needs event_loop, config, outputs
 * API). Never fails the server: problems are logged and the module degrades
 * (no backlight -> blank-only behaviour). Called by pw_server_init(). */
void pw_power_init(struct pw_server *server);

/* Report user input (called from pw_idle_activity). Returns true if the
 * event must be swallowed (it only woke a blanked screen). */
bool pw_power_activity(struct pw_server *server);

/* Manual blank toggles (TOGGLE_BLANK, output power protocol) so the state
 * machine stays in sync with pw_output_blank. */
void pw_power_set_blanked(struct pw_server *server, bool blanked);

/* Resync the state machine after server->blanked was changed outside
 * power.c (output power management protocol). */
void pw_power_sync_blanked(struct pw_server *server);

/* Remove event sources, restore user brightness, free state. NULL-safe,
 * idempotent. Called by pw_server_finish() before the display goes away. */
void pw_power_finish(struct pw_server *server);

#endif
