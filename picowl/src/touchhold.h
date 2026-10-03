/*
 * touchhold.h - tap-and-hold state machine. Pure: no wlroots, no timers,
 * time is injected (milliseconds, monotonic, any epoch). The caller owns the
 * timer: after every call it asks pw_touchhold_next_deadline() and arranges
 * a call to pw_touchhold_tick() at that time.
 *
 * Every function returns a bitmask of PW_TH_* actions the caller performs
 * in this order: STOP_ANIMATION, SEND_LEFT_PRESS, SEND_MOTION,
 * SEND_RIGHT_CLICK, SEND_LEFT_RELEASE, START_ANIMATION.
 *
 * States:
 *  IDLE      no touch.
 *  PENDING   down, left press deferred, waiting for slop / delay / up.
 *  DRAGGING  moved beyond slop (or action == NONE): left press sent; motion
 *            is forwarded (SEND_MOTION), up -> SEND_LEFT_RELEASE.
 *  ANIMATING hold_delay elapsed: animation shown, still deferred.
 *  TRIGGERED hold_ms elapsed: right click sent; everything until up/cancel
 *            is swallowed (SWALLOW).
 *
 * Rules:
 *  down:   action NONE -> DRAGGING, SEND_LEFT_PRESS. Else PENDING (focus +
 *          motion is done by the caller, no action bit).
 *  motion: PENDING/ANIMATING and squared distance from the down point >
 *          slop^2 -> DRAGGING, [STOP_ANIMATION if animating,]
 *          SEND_LEFT_PRESS|SEND_MOTION. DRAGGING -> SEND_MOTION.
 *          TRIGGERED -> SWALLOW. Otherwise 0.
 *  up:     PENDING -> IDLE, SEND_LEFT_PRESS|SEND_LEFT_RELEASE (tap).
 *          ANIMATING -> IDLE, STOP_ANIMATION|left press+release (tap).
 *          DRAGGING -> IDLE, SEND_LEFT_RELEASE. TRIGGERED -> IDLE, SWALLOW.
 *  cancel: any -> IDLE; STOP_ANIMATION if animating; DRAGGING yields
 *          SEND_LEFT_RELEASE so no button stays stuck; else nothing.
 *  set_params: if not IDLE, cancel first; returns cancel actions. Replaces
 *          thresholds and action (same clamping as init).
 *  tick:   PENDING and now >= down+delay -> ANIMATING, START_ANIMATION.
 *          PENDING/ANIMATING and now >= down+hold_ms -> TRIGGERED,
 *          [STOP_ANIMATION,] SEND_RIGHT_CLICK|SWALLOW. (Both thresholds
 *          crossed in one tick -> only the second applies, no START.)
 *  next_deadline: absolute ms of the next threshold in PENDING/ANIMATING,
 *          else -1.
 * Invalid configs: delay < 0 -> 0; hold_ms < delay -> hold_ms = delay;
 * slop < 0 -> 0. A down while not IDLE is treated as cancel then down.
 */
#ifndef PICOWL_TOUCHHOLD_H
#define PICOWL_TOUCHHOLD_H

#include <stdint.h>

enum pw_th_action {
	PW_TH_SEND_LEFT_PRESS   = 1 << 0,
	PW_TH_SEND_LEFT_RELEASE = 1 << 1,
	PW_TH_SEND_MOTION       = 1 << 2,
	PW_TH_START_ANIMATION   = 1 << 3,
	PW_TH_STOP_ANIMATION    = 1 << 4,
	PW_TH_SEND_RIGHT_CLICK  = 1 << 5, /* press + release of BTN_RIGHT */
	PW_TH_SWALLOW           = 1 << 6, /* do not deliver this event */
};

enum pw_th_state {
	PW_TH_IDLE,
	PW_TH_PENDING,
	PW_TH_DRAGGING,
	PW_TH_ANIMATING,
	PW_TH_TRIGGERED,
};

/* Must mirror enum pw_hold_action in picowl.h without including it. */
enum pw_th_hold_action {
	PW_TH_ACTION_RIGHT_CLICK = 0,
	PW_TH_ACTION_NONE = 1,
};

struct pw_touchhold {
	enum pw_th_state state;
	enum pw_th_hold_action action;
	int delay_ms, hold_ms, slop_px;
	int down_x, down_y;
	int64_t down_ms;
};

void pw_touchhold_init(struct pw_touchhold *th, enum pw_th_hold_action action,
	int delay_ms, int hold_ms, int slop_px);
/* Replace thresholds/action. Same clamping as init. If not IDLE, cancels
 * first and returns the cancel actions (STOP_ANIMATION or SEND_LEFT_RELEASE),
 * else 0. Not IDLE -> caller must run result through th_exec before next down. */
unsigned pw_touchhold_set_params(struct pw_touchhold *th,
	enum pw_th_hold_action action, int delay_ms, int hold_ms, int slop_px);
unsigned pw_touchhold_down(struct pw_touchhold *th, int x, int y, int64_t now_ms);
unsigned pw_touchhold_motion(struct pw_touchhold *th, int x, int y, int64_t now_ms);
unsigned pw_touchhold_up(struct pw_touchhold *th, int64_t now_ms);
unsigned pw_touchhold_cancel(struct pw_touchhold *th);
unsigned pw_touchhold_tick(struct pw_touchhold *th, int64_t now_ms);
int64_t pw_touchhold_next_deadline(const struct pw_touchhold *th);

#endif
