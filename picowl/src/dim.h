/*
 * dim.h - ACTIVE -> DIMMED -> BLANKED idle state machine. Pure: no wlroots,
 * no timers, time is injected (ms, monotonic, any epoch), like touchhold.h.
 * The caller owns the timer: after every call it asks pw_dim_next_deadline()
 * and arranges a pw_dim_tick() at that time.
 *
 * Every function returns a bitmask of PW_DIM_* actions:
 *  DIM            enter DIMMED: set the dimmed backlight.
 *  UNDIM          leave DIMMED to ACTIVE: restore user brightness.
 *  BLANK          enter BLANKED: pw_output_blank(true).
 *  UNBLANK        leave BLANKED to ACTIVE: pw_output_blank(false), then
 *                 restore brightness.
 *  SWALLOW_INPUT  the input event that caused this must not reach clients
 *                 (only set with UNBLANK).
 * Order when several are set: BLANK/UNBLANK, then DIM/UNDIM brightness work.
 *
 * Rules (dim_ms/blank_ms are relative to the last activity; 0 = disabled;
 * blank_ms <= dim_ms with both set means dimming is skipped and the screen
 * blanks at blank_ms):
 *  activity: ACTIVE -> restart timers, 0. DIMMED -> ACTIVE, UNDIM (input is
 *    delivered). BLANKED -> ACTIVE, UNBLANK|SWALLOW_INPUT.
 *  tick:     ACTIVE and dim enabled and now >= last+dim_ms -> DIMMED, DIM
 *    (if blank is also due, go straight to BLANKED: BLANK only). ACTIVE or
 *    DIMMED and blank enabled and now >= last+blank_ms -> BLANKED, BLANK.
 *  set_timeouts: replace timeouts (profile change), keep state and last
 *    activity time; re-evaluates like tick(now) and returns its actions.
 *    Never un-dims or un-blanks by itself.
 *  force_blank / force_unblank: manual toggle (TOGGLE_BLANK, output power
 *    protocol). BLANKED state without timers; returns BLANK / UNBLANK (0 if
 *    already there). unblank restarts timers, no SWALLOW_INPUT.
 *  next_deadline: absolute ms of the next transition, -1 if none (BLANKED,
 *    or all timeouts disabled).
 */
#ifndef PICOWL_DIM_H
#define PICOWL_DIM_H

#include <stdint.h>

enum pw_dim_state {
	PW_DIM_ACTIVE,
	PW_DIM_DIMMED,
	PW_DIM_BLANKED,
};

enum {
	PW_DIM_ACT_DIM = 1 << 0,
	PW_DIM_ACT_UNDIM = 1 << 1,
	PW_DIM_ACT_BLANK = 1 << 2,
	PW_DIM_ACT_UNBLANK = 1 << 3,
	PW_DIM_ACT_SWALLOW_INPUT = 1 << 4,
};

struct pw_dim {
	enum pw_dim_state state;
	int64_t dim_ms;
	int64_t blank_ms;
	int64_t last_activity;
};

/* Starts ACTIVE with last_activity = 0 (call pw_dim_activity(now) to set). */
void pw_dim_init(struct pw_dim *d, int64_t dim_ms, int64_t blank_ms);
unsigned pw_dim_activity(struct pw_dim *d, int64_t now);
unsigned pw_dim_tick(struct pw_dim *d, int64_t now);
unsigned pw_dim_set_timeouts(struct pw_dim *d, int64_t dim_ms, int64_t blank_ms, int64_t now);
unsigned pw_dim_force_blank(struct pw_dim *d);
unsigned pw_dim_force_unblank(struct pw_dim *d, int64_t now);
int64_t pw_dim_next_deadline(const struct pw_dim *d);

#endif
