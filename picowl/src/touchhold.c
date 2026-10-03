/* touchhold.c - tap-and-hold state machine (see touchhold.h). Integer only. */
#include "touchhold.h"

void pw_touchhold_init(struct pw_touchhold *th, enum pw_th_hold_action action,
	int delay_ms, int hold_ms, int slop_px)
{
	if (delay_ms < 0)
		delay_ms = 0;
	if (hold_ms < delay_ms)
		hold_ms = delay_ms;
	if (slop_px < 0)
		slop_px = 0;
	th->state = PW_TH_IDLE;
	th->action = action;
	th->delay_ms = delay_ms;
	th->hold_ms = hold_ms;
	th->slop_px = slop_px;
	th->down_x = th->down_y = 0;
	th->down_ms = 0;
}

unsigned pw_touchhold_cancel(struct pw_touchhold *th)
{
	unsigned r = 0;

	switch (th->state) {
	case PW_TH_ANIMATING:
		r = PW_TH_STOP_ANIMATION;
		break;
	case PW_TH_DRAGGING:
		r = PW_TH_SEND_LEFT_RELEASE;
		break;
	default:
		break;
	}
	th->state = PW_TH_IDLE;
	return r;
}

unsigned pw_touchhold_down(struct pw_touchhold *th, int x, int y, int64_t now_ms)
{
	unsigned r = 0;

	if (th->state != PW_TH_IDLE)
		r = pw_touchhold_cancel(th);
	th->down_x = x;
	th->down_y = y;
	th->down_ms = now_ms;
	if (th->action == PW_TH_ACTION_NONE) {
		th->state = PW_TH_DRAGGING;
		r |= PW_TH_SEND_LEFT_PRESS;
	} else {
		th->state = PW_TH_PENDING;
	}
	return r;
}

unsigned pw_touchhold_motion(struct pw_touchhold *th, int x, int y, int64_t now_ms)
{
	(void)now_ms;
	switch (th->state) {
	case PW_TH_PENDING:
	case PW_TH_ANIMATING: {
		int64_t dx = (int64_t)x - th->down_x;
		int64_t dy = (int64_t)y - th->down_y;
		int64_t s = th->slop_px;
		unsigned r = 0;

		if (dx * dx + dy * dy <= s * s)
			return 0;
		if (th->state == PW_TH_ANIMATING)
			r |= PW_TH_STOP_ANIMATION;
		th->state = PW_TH_DRAGGING;
		return r | PW_TH_SEND_LEFT_PRESS | PW_TH_SEND_MOTION;
	}
	case PW_TH_DRAGGING:
		return PW_TH_SEND_MOTION;
	case PW_TH_TRIGGERED:
		return PW_TH_SWALLOW;
	default:
		return 0;
	}
}

unsigned pw_touchhold_up(struct pw_touchhold *th, int64_t now_ms)
{
	enum pw_th_state s = th->state;

	(void)now_ms;
	th->state = PW_TH_IDLE;
	switch (s) {
	case PW_TH_PENDING:
		return PW_TH_SEND_LEFT_PRESS | PW_TH_SEND_LEFT_RELEASE;
	case PW_TH_ANIMATING:
		return PW_TH_STOP_ANIMATION | PW_TH_SEND_LEFT_PRESS |
			PW_TH_SEND_LEFT_RELEASE;
	case PW_TH_DRAGGING:
		return PW_TH_SEND_LEFT_RELEASE;
	case PW_TH_TRIGGERED:
		return PW_TH_SWALLOW;
	default:
		return 0;
	}
}

unsigned pw_touchhold_tick(struct pw_touchhold *th, int64_t now_ms)
{
	if (th->state != PW_TH_PENDING && th->state != PW_TH_ANIMATING)
		return 0;
	if (now_ms >= th->down_ms + th->hold_ms) {
		unsigned r = PW_TH_SEND_RIGHT_CLICK | PW_TH_SWALLOW;

		if (th->state == PW_TH_ANIMATING)
			r |= PW_TH_STOP_ANIMATION;
		th->state = PW_TH_TRIGGERED;
		return r;
	}
	if (th->state == PW_TH_PENDING && now_ms >= th->down_ms + th->delay_ms) {
		th->state = PW_TH_ANIMATING;
		return PW_TH_START_ANIMATION;
	}
	return 0;
}

int64_t pw_touchhold_next_deadline(const struct pw_touchhold *th)
{
	if (th->state == PW_TH_PENDING)
		return th->down_ms + th->delay_ms;
	if (th->state == PW_TH_ANIMATING)
		return th->down_ms + th->hold_ms;
	return -1;
}
