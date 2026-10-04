/* oskstate.c - supervision policy for the on-screen keyboard (see oskstate.h). */
#include "oskstate.h"

void pw_oskstate_init(struct pw_oskstate *st, bool restart)
{
	st->phase = PW_OSK_STOPPED;
	st->restart = restart;
	st->visible = false;
	st->quick_exits = 0;
	st->start_ms = 0;
}

void pw_oskstate_started(struct pw_oskstate *st, int64_t now_ms)
{
	st->phase = PW_OSK_RUNNING;
	st->visible = false;
	st->start_ms = now_ms;
}

int pw_oskstate_exited(struct pw_oskstate *st, int64_t now_ms)
{
	int delay;

	if (st->phase != PW_OSK_RUNNING)
		return -1;
	st->visible = false;
	if (now_ms - st->start_ms < PW_OSK_QUICK_MS) {
		st->quick_exits++;
		if (st->quick_exits >= PW_OSK_QUICK_LIMIT) {
			st->phase = st->restart ? PW_OSK_GAVE_UP : PW_OSK_STOPPED;
			return -1;
		}
		delay = PW_OSK_RESTART_MS << (st->quick_exits - 1);
	} else {
		st->quick_exits = 0;
		delay = PW_OSK_RESTART_MS;
	}
	if (!st->restart) {
		st->phase = PW_OSK_STOPPED;
		return -1;
	}
	st->phase = PW_OSK_BACKOFF;
	return delay;
}

enum pw_osk_do pw_oskstate_request(struct pw_oskstate *st, enum pw_osk_op op)
{
	if (st->phase == PW_OSK_RUNNING) {
		switch (op) {
		case PW_OSK_SHOW:
			st->visible = true;
			return PW_OSK_DO_SIGNAL_SHOW;
		case PW_OSK_HIDE:
			st->visible = false;
			return PW_OSK_DO_SIGNAL_HIDE;
		case PW_OSK_TOGGLE:
			st->visible = !st->visible;
			return PW_OSK_DO_SIGNAL_TOGGLE;
		}
		return PW_OSK_DO_NOTHING;
	}
	if (op == PW_OSK_HIDE)
		return PW_OSK_DO_NOTHING;
	st->quick_exits = 0;
	return PW_OSK_DO_START;
}

void pw_oskstate_stop(struct pw_oskstate *st)
{
	st->phase = PW_OSK_STOPPED;
	st->visible = false;
}
