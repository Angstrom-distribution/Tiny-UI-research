/* dim.c - dim/blank state machine. */
#include "dim.h"

void pw_dim_init(struct pw_dim *d, int64_t dim_ms, int64_t blank_ms)
{
	d->state = PW_DIM_ACTIVE;
	d->dim_ms = dim_ms;
	d->blank_ms = blank_ms;
	d->last_activity = 0;
}

unsigned pw_dim_activity(struct pw_dim *d, int64_t now)
{
	unsigned r = 0;

	switch (d->state) {
	case PW_DIM_ACTIVE:
		/* Restart timers from now. */
		d->last_activity = now;
		return 0;
	case PW_DIM_DIMMED:
		/* DIMMED -> ACTIVE: restore brightness and deliver input. */
		r = PW_DIM_ACT_UNDIM;
		d->state = PW_DIM_ACTIVE;
		d->last_activity = now;
		return r;
	case PW_DIM_BLANKED:
		/* BLANKED -> ACTIVE: unblank and swallow the first input event. */
		r = PW_DIM_ACT_UNBLANK | PW_DIM_ACT_SWALLOW_INPUT;
		d->state = PW_DIM_ACTIVE;
		d->last_activity = now;
		return r;
	default:
		return 0;
	}
}

unsigned pw_dim_tick(struct pw_dim *d, int64_t now)
{
	int64_t elapsed;

	if (d->state == PW_DIM_BLANKED)
		return 0;

	elapsed = now - d->last_activity;

	/* Check if both timers are disabled. */
	if (d->dim_ms <= 0 && d->blank_ms <= 0)
		return 0;

	/* Check if blank deadline has passed. */
	if (d->blank_ms > 0 && elapsed >= d->blank_ms) {
		d->state = PW_DIM_BLANKED;
		return PW_DIM_ACT_BLANK;
	}

	/* Check if dim deadline has passed (only if not blanking). */
	if (d->dim_ms > 0 && d->state == PW_DIM_ACTIVE && elapsed >= d->dim_ms) {
		/* If blank_ms <= dim_ms, skip dimming and go straight to blanked. */
		if (d->blank_ms > 0 && d->blank_ms <= d->dim_ms) {
			d->state = PW_DIM_BLANKED;
			return PW_DIM_ACT_BLANK;
		}
		d->state = PW_DIM_DIMMED;
		return PW_DIM_ACT_DIM;
	}

	return 0;
}

unsigned pw_dim_set_timeouts(struct pw_dim *d, int64_t dim_ms, int64_t blank_ms, int64_t now)
{
	d->dim_ms = dim_ms;
	d->blank_ms = blank_ms;
	/* Re-evaluate as if tick was called now. */
	return pw_dim_tick(d, now);
}

unsigned pw_dim_force_blank(struct pw_dim *d)
{
	if (d->state == PW_DIM_BLANKED)
		return 0;
	d->state = PW_DIM_BLANKED;
	return PW_DIM_ACT_BLANK;
}

unsigned pw_dim_force_unblank(struct pw_dim *d, int64_t now)
{
	if (d->state != PW_DIM_BLANKED)
		return 0;
	d->state = PW_DIM_ACTIVE;
	d->last_activity = now;
	return PW_DIM_ACT_UNBLANK;
}

int64_t pw_dim_next_deadline(const struct pw_dim *d)
{
	int64_t deadline = -1;

	if (d->state == PW_DIM_BLANKED)
		return -1;

	/* If both timeouts are disabled, no deadline. */
	if (d->dim_ms <= 0 && d->blank_ms <= 0)
		return -1;

	if (d->state == PW_DIM_ACTIVE) {
		/* In ACTIVE state, return the earlier of dim or blank deadlines. */
		if (d->dim_ms > 0)
			deadline = d->last_activity + d->dim_ms;
		if (d->blank_ms > 0) {
			int64_t blank_deadline = d->last_activity + d->blank_ms;
			if (deadline < 0 || blank_deadline < deadline)
				deadline = blank_deadline;
		}
	} else if (d->state == PW_DIM_DIMMED) {
		/* In DIMMED state, only blank deadline is relevant. */
		if (d->blank_ms > 0)
			deadline = d->last_activity + d->blank_ms;
	}

	return deadline;
}
