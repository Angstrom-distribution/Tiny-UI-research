#include <stdio.h>
#include <stdlib.h>
#include "touchhold.h"

#define L PW_TH_SEND_LEFT_PRESS
#define R PW_TH_SEND_LEFT_RELEASE
#define M PW_TH_SEND_MOTION
#define SA PW_TH_START_ANIMATION
#define SO PW_TH_STOP_ANIMATION
#define RC PW_TH_SEND_RIGHT_CLICK
#define SW PW_TH_SWALLOW
#define MC PW_TH_SEND_MIDDLE_CLICK

static int fails;
#define EQ(got, want) do { unsigned g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got 0x%x want 0x%x\n", __FILE__, __LINE__, g_, w_); fails++; } } while (0)
#define EQ64(got, want) do { long long g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got %lld want %lld\n", __FILE__, __LINE__, g_, w_); fails++; } } while (0)

static struct pw_touchhold th;
static void fresh(void) { pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 300, 900, 8); }

int main(void)
{
	fresh();
	EQ(th.state, PW_TH_IDLE);
	EQ64(pw_touchhold_next_deadline(&th), -1);

	/* tap */
	EQ(pw_touchhold_down(&th, 10, 10, 1000), 0);
	EQ(th.state, PW_TH_PENDING);
	EQ64(pw_touchhold_next_deadline(&th), 1300);
	EQ(pw_touchhold_tick(&th, 1100), 0);
	EQ(pw_touchhold_up(&th, 1100), L | R);
	EQ(th.state, PW_TH_IDLE);
	EQ64(pw_touchhold_next_deadline(&th), -1);

	/* drag */
	pw_touchhold_down(&th, 10, 10, 0);
	EQ(pw_touchhold_motion(&th, 10, 25, 50), L | M);
	EQ(th.state, PW_TH_DRAGGING);
	EQ(pw_touchhold_motion(&th, 10, 30, 60), M);
	EQ(pw_touchhold_tick(&th, 400), 0);
	EQ(pw_touchhold_tick(&th, 5000), 0);
	EQ64(pw_touchhold_next_deadline(&th), -1);
	EQ(pw_touchhold_up(&th, 500), R);
	EQ(th.state, PW_TH_IDLE);

	/* jitter within slop (exactly slop is inside) */
	pw_touchhold_down(&th, 100, 100, 0);
	EQ(pw_touchhold_motion(&th, 108, 100, 10), 0);
	EQ(pw_touchhold_motion(&th, 95, 94, 20), 0);
	EQ(th.state, PW_TH_PENDING);
	EQ(pw_touchhold_motion(&th, 106, 106, 30), L | M); /* 72 > 64 */
	pw_touchhold_up(&th, 40);

	/* hold */
	pw_touchhold_down(&th, 50, 50, 2000);
	EQ(pw_touchhold_tick(&th, 2299), 0);
	EQ(pw_touchhold_tick(&th, 2300), SA);
	EQ(th.state, PW_TH_ANIMATING);
	EQ64(pw_touchhold_next_deadline(&th), 2900);
	EQ(pw_touchhold_tick(&th, 2400), 0);
	EQ(pw_touchhold_motion(&th, 52, 52, 2500), 0);
	EQ(pw_touchhold_tick(&th, 2900), SO | RC | SW);
	EQ(th.state, PW_TH_TRIGGERED);
	EQ64(pw_touchhold_next_deadline(&th), -1);
	EQ(pw_touchhold_motion(&th, 200, 200, 3000), SW);
	EQ(pw_touchhold_tick(&th, 4000), 0);
	EQ(pw_touchhold_up(&th, 4100), SW);
	EQ(th.state, PW_TH_IDLE);

	/* both thresholds in one tick: no START */
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_tick(&th, 1000), RC | SW);
	EQ(pw_touchhold_cancel(&th), 0);

	/* lift during animation */
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_tick(&th, 300);
	EQ(pw_touchhold_up(&th, 500), SO | L | R);
	EQ(th.state, PW_TH_IDLE);

	/* move beyond slop during animation */
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_tick(&th, 300);
	EQ(pw_touchhold_motion(&th, 20, 0, 400), SO | L | M);
	EQ(th.state, PW_TH_DRAGGING);
	EQ(pw_touchhold_up(&th, 500), R);

	/* cancel in every state */
	EQ(pw_touchhold_cancel(&th), 0); /* idle */
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_cancel(&th), 0); /* pending */
	EQ(th.state, PW_TH_IDLE);
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_tick(&th, 300);
	EQ(pw_touchhold_cancel(&th), SO); /* animating */
	EQ(th.state, PW_TH_IDLE);
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_motion(&th, 50, 0, 10);
	EQ(pw_touchhold_cancel(&th), R); /* dragging */
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_tick(&th, 900);
	EQ(pw_touchhold_cancel(&th), 0); /* triggered */
	EQ(th.state, PW_TH_IDLE);

	/* up / motion / tick while idle */
	EQ(pw_touchhold_up(&th, 0), 0);
	EQ(pw_touchhold_motion(&th, 1, 1, 0), 0);
	EQ(pw_touchhold_tick(&th, 99999), 0);

	/* down while not idle = cancel then down */
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_tick(&th, 300);
	EQ(pw_touchhold_down(&th, 5, 5, 400), SO);
	EQ(th.state, PW_TH_PENDING);
	EQ64(pw_touchhold_next_deadline(&th), 700);
	pw_touchhold_cancel(&th);

	/* action NONE */
	pw_touchhold_init(&th, PW_TH_ACTION_NONE, PW_TH_BTN_RIGHT, 300, 900, 8);
	EQ(pw_touchhold_down(&th, 0, 0, 0), L);
	EQ(th.state, PW_TH_DRAGGING);
	EQ(pw_touchhold_tick(&th, 5000), 0);
	EQ(pw_touchhold_motion(&th, 1, 1, 10), M);
	EQ64(pw_touchhold_next_deadline(&th), -1);
	EQ(pw_touchhold_up(&th, 20), R);

	/* button middle: the hold emits a middle click, never a right click */
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_MIDDLE, 300, 900, 8);
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_tick(&th, 300), SA);
	EQ(pw_touchhold_tick(&th, 900), SO | MC | SW);
	EQ(pw_touchhold_up(&th, 1000), SW);
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_tick(&th, 1000), MC | SW); /* both thresholds in one tick */
	pw_touchhold_cancel(&th);
	/* a tap or a drag is unaffected by the button */
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_up(&th, 100), L | R);
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_motion(&th, 20, 0, 100), L | M);
	pw_touchhold_up(&th, 200);
	/* set_params switches the button between touches */
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 300, 900, 8), 0);
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_tick(&th, 900), RC | SW);
	pw_touchhold_cancel(&th);
	/* an unknown value falls back to right */
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, (enum pw_th_hold_button)7, 300, 900, 8);
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_tick(&th, 900), RC | SW);
	pw_touchhold_cancel(&th);

	/* invalid configs */
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, -5, 100, -3);
	EQ(th.delay_ms, 0); EQ(th.slop_px, 0);
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 500, 200, 8);
	EQ(th.hold_ms, 500);
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_motion(&th, 1, 0, 1), 0);
	EQ(pw_touchhold_tick(&th, 500), RC | SW); /* delay == hold: no START */

	/* set_params tests */
	fresh();
	/* set_params(NONE) in IDLE returns 0 */
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_NONE, PW_TH_BTN_RIGHT, 300, 900, 8), 0);
	EQ(th.action, PW_TH_ACTION_NONE);
	EQ(pw_touchhold_down(&th, 0, 0, 0), L);
	EQ(th.state, PW_TH_DRAGGING);
	EQ(pw_touchhold_motion(&th, 1, 1, 10), M);
	EQ(pw_touchhold_up(&th, 20), R);
	EQ(pw_touchhold_tick(&th, 5000), 0);

	/* set_params in PENDING returns 0 */
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 300, 900, 8);
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(th.state, PW_TH_PENDING);
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_NONE, PW_TH_BTN_RIGHT, 300, 900, 8), 0);
	EQ(th.state, PW_TH_IDLE);
	EQ64(pw_touchhold_next_deadline(&th), -1);

	/* set_params in ANIMATING returns SO */
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 300, 900, 8);
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_tick(&th, 300);
	EQ(th.state, PW_TH_ANIMATING);
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_NONE, PW_TH_BTN_RIGHT, 300, 900, 8), SO);
	EQ(th.state, PW_TH_IDLE);

	/* set_params in DRAGGING returns R */
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 300, 900, 8);
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_motion(&th, 50, 0, 10);
	EQ(th.state, PW_TH_DRAGGING);
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_NONE, PW_TH_BTN_RIGHT, 300, 900, 8), R);
	EQ(th.state, PW_TH_IDLE);

	/* set_params in TRIGGERED returns 0 */
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 300, 900, 8);
	pw_touchhold_down(&th, 0, 0, 0);
	pw_touchhold_tick(&th, 900);
	EQ(th.state, PW_TH_TRIGGERED);
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_NONE, PW_TH_BTN_RIGHT, 300, 900, 8), 0);
	EQ(th.state, PW_TH_IDLE);

	/* Clamping in set_params */
	fresh();
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, -5, 100, -3), 0);
	EQ(th.delay_ms, 0); EQ(th.slop_px, 0);
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 500, 200, 8), 0);
	EQ(th.hold_ms, 500);

	/* Parameters are fixed for the gesture */
	pw_touchhold_init(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 300, 900, 8);
	pw_touchhold_down(&th, 0, 0, 0);
	EQ(pw_touchhold_tick(&th, 300), SA);
	EQ(pw_touchhold_tick(&th, 900), SO | RC | SW);
	/* Now set new parameters and down again */
	EQ(pw_touchhold_set_params(&th, PW_TH_ACTION_RIGHT_CLICK, PW_TH_BTN_RIGHT, 500, 1500, 8), 0);
	EQ(pw_touchhold_down(&th, 0, 0, 2000), 0);
	EQ64(pw_touchhold_next_deadline(&th), 2500);

	return fails ? 1 : 0;
}
