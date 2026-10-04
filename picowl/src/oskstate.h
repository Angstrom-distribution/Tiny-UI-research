/*
 * oskstate.h - supervision policy for the on-screen keyboard process. Pure:
 * no fork, no signals, no timers, time is injected (milliseconds, monotonic,
 * any epoch). osk.c owns the process and the timer and asks this file what to
 * do.
 *
 * Phases:
 *  STOPPED  not running and nothing scheduled (never started, restart = no
 *           after an exit, or shut down).
 *  RUNNING  started and not yet reaped.
 *  BACKOFF  exited, a restart is scheduled after the delay exited() returned.
 *  GAVE_UP  QUICK_LIMIT quick exits in a row; nothing is scheduled until a
 *           show or toggle request asks for a start.
 *
 * An exit within QUICK_MS of the start is quick. The restart delays after
 * quick exit 1, 2, 3, 4 are 1, 2, 4, 8 s; the fifth gives up. An exit after a
 * longer run resets the count and restarts after RESTART_MS.
 *
 * visible is picowl's guess whether the keyboard is shown. A (re)start resets
 * it to hidden, since the keyboard is started hidden. It is wrong when
 * something else signals the keyboard; the next toggle corrects it.
 */
#ifndef PICOWL_OSKSTATE_H
#define PICOWL_OSKSTATE_H

#include <stdbool.h>
#include <stdint.h>

#define PW_OSK_QUICK_MS 10000
#define PW_OSK_QUICK_LIMIT 5
#define PW_OSK_RESTART_MS 1000 /* delay after quick exit 1, doubled each time */

enum pw_osk_phase {
	PW_OSK_STOPPED,
	PW_OSK_RUNNING,
	PW_OSK_BACKOFF,
	PW_OSK_GAVE_UP,
};

enum pw_osk_op {
	PW_OSK_SHOW,
	PW_OSK_HIDE,
	PW_OSK_TOGGLE,
};

/* What osk.c has to do for a request. */
enum pw_osk_do {
	PW_OSK_DO_NOTHING,
	PW_OSK_DO_START,        /* not running: start it (any pending restart is moot) */
	PW_OSK_DO_SIGNAL_SHOW,
	PW_OSK_DO_SIGNAL_HIDE,
	PW_OSK_DO_SIGNAL_TOGGLE,
};

struct pw_oskstate {
	enum pw_osk_phase phase;
	bool restart;           /* [osk] restart */
	bool visible;
	int quick_exits;        /* in a row */
	int64_t start_ms;
};

void pw_oskstate_init(struct pw_oskstate *st, bool restart);
/* The process was started at now_ms: RUNNING, hidden. */
void pw_oskstate_started(struct pw_oskstate *st, int64_t now_ms);
/* The process exited at now_ms. Returns the delay in ms after which to start
 * it again (the phase is BACKOFF), or -1 for no restart (GAVE_UP after the
 * last quick exit, STOPPED when restart is off). Ignored (-1) unless RUNNING. */
int pw_oskstate_exited(struct pw_oskstate *st, int64_t now_ms);
/* A keybinding asked for op. Updates the visibility guess and returns what to
 * do. Hide never starts anything. Show and toggle with nothing running return
 * DO_START and clear the give-up count; the caller then calls started(). */
enum pw_osk_do pw_oskstate_request(struct pw_oskstate *st, enum pw_osk_op op);
/* picowl is exiting: STOPPED, no restarts. */
void pw_oskstate_stop(struct pw_oskstate *st);

#endif
