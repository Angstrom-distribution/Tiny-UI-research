/*
 * test-dim: unit tests for the dim/blank state machine.
 * Tests cover all state transitions, disabled timeouts, dim >= blank,
 * timeout changes, and manual blanking/unblanking.
 */
#include <stdio.h>
#include "dim.h"

#define D PW_DIM_ACT_DIM
#define UD PW_DIM_ACT_UNDIM
#define B PW_DIM_ACT_BLANK
#define UB PW_DIM_ACT_UNBLANK
#define SW PW_DIM_ACT_SWALLOW_INPUT

static int fails;
#define EQ(got, want) do { unsigned g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got 0x%x want 0x%x\n", __FILE__, __LINE__, g_, w_); fails++; } } while (0)
#define EQ64(got, want) do { int64_t g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got %lld want %lld\n", __FILE__, __LINE__, (long long)g_, (long long)w_); fails++; } } while (0)
#define EQS(got, want) do { enum pw_dim_state g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got state %d want state %d\n", __FILE__, __LINE__, g_, w_); fails++; } } while (0)

static struct pw_dim d;

static void fresh(void) { pw_dim_init(&d, 100, 200); }

int main(void)
{
	/* Initial state */
	fresh();
	EQS(d.state, PW_DIM_ACTIVE);
	EQ64(pw_dim_next_deadline(&d), 100); /* epoch 0 is a valid time */

	/* Activity in ACTIVE state restarts timers */
	EQ(pw_dim_activity(&d, 1000), 0);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ64(d.last_activity, 1000);
	EQ64(pw_dim_next_deadline(&d), 1100); /* 1000 + 100 */

	/* Tick before dim deadline: no change */
	EQ(pw_dim_tick(&d, 1050), 0);
	EQS(d.state, PW_DIM_ACTIVE);

	/* Tick at dim deadline: DIM */
	EQ(pw_dim_tick(&d, 1100), D);
	EQS(d.state, PW_DIM_DIMMED);
	EQ64(pw_dim_next_deadline(&d), 1200); /* blank deadline */

	/* Activity in DIMMED state: UNDIM and return to ACTIVE */
	EQ(pw_dim_activity(&d, 1150), UD);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ64(d.last_activity, 1150);

	/* Tick to blank deadline */
	EQ(pw_dim_tick(&d, 1150), 0);
	EQ(pw_dim_tick(&d, 1250), D);
	EQS(d.state, PW_DIM_DIMMED);
	EQ(pw_dim_tick(&d, 1350), B);
	EQS(d.state, PW_DIM_BLANKED);
	EQ64(pw_dim_next_deadline(&d), -1); /* no deadline while blanked */

	/* Activity in BLANKED state: UNBLANK with SWALLOW_INPUT */
	EQ(pw_dim_activity(&d, 1400), UB | SW);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ64(d.last_activity, 1400);

	/* Dimming disabled (dim_ms = 0) */
	fresh();
	pw_dim_init(&d, 0, 200);
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ64(pw_dim_next_deadline(&d), 1200); /* only blank deadline */
	EQ(pw_dim_tick(&d, 1100), 0);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ(pw_dim_tick(&d, 1200), B); /* skip dim, go to blank */
	EQS(d.state, PW_DIM_BLANKED);

	/* Blanking disabled (blank_ms = 0) */
	fresh();
	pw_dim_init(&d, 100, 0);
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ64(pw_dim_next_deadline(&d), 1100);
	EQ(pw_dim_tick(&d, 1100), D);
	EQS(d.state, PW_DIM_DIMMED);
	EQ64(pw_dim_next_deadline(&d), -1); /* no blank deadline */
	EQ(pw_dim_activity(&d, 1150), UD);
	EQS(d.state, PW_DIM_ACTIVE);

	/* Both disabled */
	fresh();
	pw_dim_init(&d, 0, 0);
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ64(pw_dim_next_deadline(&d), -1);
	EQ(pw_dim_tick(&d, 5000), 0); /* no-op */
	EQS(d.state, PW_DIM_ACTIVE);

	/* dim_ms >= blank_ms: skip dimming, go straight to blank */
	fresh();
	pw_dim_init(&d, 200, 100);
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ64(pw_dim_next_deadline(&d), 1100); /* blank deadline */
	EQ(pw_dim_tick(&d, 1050), 0); /* no dimming */
	EQS(d.state, PW_DIM_ACTIVE);
	EQ(pw_dim_tick(&d, 1100), B); /* go straight to blank */
	EQS(d.state, PW_DIM_BLANKED);

	/* dim_ms == blank_ms: same behavior */
	fresh();
	pw_dim_init(&d, 100, 100);
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ64(pw_dim_next_deadline(&d), 1100);
	EQ(pw_dim_tick(&d, 1100), B); /* skip dim, go to blank */
	EQS(d.state, PW_DIM_BLANKED);

	/* set_timeouts while ACTIVE */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	/* Change timeouts: dim_ms=50, blank_ms=150 */
	EQ(pw_dim_set_timeouts(&d, 50, 150, 1000), 0); /* no immediate action at exact last_activity */
	EQS(d.state, PW_DIM_ACTIVE);
	EQ64(d.dim_ms, 50);
	EQ64(d.blank_ms, 150);
	EQ64(pw_dim_next_deadline(&d), 1050);
	EQ(pw_dim_tick(&d, 1050), D);

	/* set_timeouts while DIMMED: triggers blank if blank deadline passed */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_tick(&d, 1100), D); /* now dimmed at time 1100 */
	EQS(d.state, PW_DIM_DIMMED);
	/* Change timeouts to blank_ms=50 (already passed since last_activity=1000) */
	EQ(pw_dim_set_timeouts(&d, 100, 50, 1100), B); /* triggers immediate blank */
	EQS(d.state, PW_DIM_BLANKED);

	/* set_timeouts while DIMMED with future blank deadline */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_tick(&d, 1100), D); /* dimmed at time 1100, last_activity=1000 */
	EQ(pw_dim_set_timeouts(&d, 100, 150, 1100), 0); /* future deadline */
	EQS(d.state, PW_DIM_DIMMED);
	EQ64(pw_dim_next_deadline(&d), 1150);

	/* set_timeouts changes deadline early (shortening) */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ64(pw_dim_next_deadline(&d), 1100); /* dim at 1100 */
	EQ(pw_dim_set_timeouts(&d, 50, 200, 1000), 0);
	EQ64(pw_dim_next_deadline(&d), 1050); /* dim at 1050 now */
	EQ(pw_dim_tick(&d, 1050), D);

	/* set_timeouts while DIMMED: can shorten blank deadline for immediate action */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_tick(&d, 1100), D);
	EQS(d.state, PW_DIM_DIMMED);
	/* Tick to 1150, blank still not due (1200) */
	EQ(pw_dim_tick(&d, 1150), 0);
	/* Set blank deadline to 50ms (already passed) */
	EQ(pw_dim_set_timeouts(&d, 100, 50, 1150), B);
	EQS(d.state, PW_DIM_BLANKED);

	/* force_blank in ACTIVE state */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_force_blank(&d), B);
	EQS(d.state, PW_DIM_BLANKED);
	EQ64(pw_dim_next_deadline(&d), -1);

	/* force_blank while already BLANKED: no-op */
	EQ(pw_dim_force_blank(&d), 0);
	EQS(d.state, PW_DIM_BLANKED);

	/* force_blank in DIMMED state */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_tick(&d, 1100), D);
	EQS(d.state, PW_DIM_DIMMED);
	EQ(pw_dim_force_blank(&d), B);
	EQS(d.state, PW_DIM_BLANKED);

	/* force_unblank in BLANKED state */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_force_blank(&d), B);
	EQ(pw_dim_force_unblank(&d, 1500), UB); /* no SWALLOW_INPUT */
	EQS(d.state, PW_DIM_ACTIVE);
	EQ64(d.last_activity, 1500);

	/* force_unblank while ACTIVE: no-op */
	EQ(pw_dim_force_unblank(&d, 1600), 0);
	EQS(d.state, PW_DIM_ACTIVE);

	/* force_unblank while DIMMED: no-op */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_tick(&d, 1100), D);
	EQ(pw_dim_force_unblank(&d, 1200), 0);
	EQS(d.state, PW_DIM_DIMMED);

	/* Activity from BLANKED always swallows */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_force_blank(&d), B);
	EQ(pw_dim_activity(&d, 2000), UB | SW);
	EQS(d.state, PW_DIM_ACTIVE);

	/* Multiple ticks with no progress */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_tick(&d, 1050), 0);
	EQ(pw_dim_tick(&d, 1050), 0); /* same time again */
	EQ(pw_dim_tick(&d, 1050), 0);
	EQS(d.state, PW_DIM_ACTIVE);

	/* Tick just before deadline (off by one) */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_tick(&d, 1099), 0);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ(pw_dim_tick(&d, 1100), D);
	EQS(d.state, PW_DIM_DIMMED);

	/* Activity in ACTIVE followed by tick before deadline */
	fresh();
	EQ(pw_dim_activity(&d, 2000), 0);
	EQ64(pw_dim_next_deadline(&d), 2100);
	EQ(pw_dim_tick(&d, 2050), 0);
	EQ(pw_dim_tick(&d, 2099), 0);
	EQS(d.state, PW_DIM_ACTIVE);

	/* Going through all states: ACTIVE -> DIMMED -> BLANKED -> ACTIVE */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ(pw_dim_tick(&d, 1100), D);
	EQS(d.state, PW_DIM_DIMMED);
	EQ(pw_dim_tick(&d, 1200), B);
	EQS(d.state, PW_DIM_BLANKED);
	EQ(pw_dim_activity(&d, 1300), UB | SW);
	EQS(d.state, PW_DIM_ACTIVE);

	/* Going ACTIVE -> DIMMED -> BLANKED via activity in between */
	fresh();
	EQ(pw_dim_activity(&d, 1000), 0);
	EQ(pw_dim_tick(&d, 1100), D);
	EQS(d.state, PW_DIM_DIMMED);
	EQ(pw_dim_activity(&d, 1150), UD);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ(pw_dim_tick(&d, 1250), D);
	EQS(d.state, PW_DIM_DIMMED);
	EQ(pw_dim_tick(&d, 1350), B);
	EQS(d.state, PW_DIM_BLANKED);

	/* Activity at t=0 must keep its deadlines */
	fresh();
	EQ(pw_dim_activity(&d, 0), 0);
	EQ64(pw_dim_next_deadline(&d), 100);
	EQ(pw_dim_tick(&d, 100), D);
	EQ64(pw_dim_next_deadline(&d), 200);
	EQ(pw_dim_tick(&d, 200), B);

	/* Inhibit while ACTIVE: no timers at all */
	fresh();
	EQ(pw_dim_set_inhibited(&d, true, 0), 0);
	EQ(pw_dim_tick(&d, 10000), 0);
	EQ64(pw_dim_next_deadline(&d), -1);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ(pw_dim_set_inhibited(&d, true, 10), 0); /* same value: no-op */

	/* Inhibit while DIMMED: UNDIM, back to ACTIVE */
	fresh();
	EQ(pw_dim_tick(&d, 100), D);
	EQ(pw_dim_set_inhibited(&d, true, 150), UD);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ64(pw_dim_next_deadline(&d), -1);

	/* Inhibit while BLANKED: stays blank; a client never unblanks */
	fresh();
	EQ(pw_dim_force_blank(&d), B);
	EQ(pw_dim_set_inhibited(&d, true, 300), 0);
	EQS(d.state, PW_DIM_BLANKED);
	EQ(pw_dim_force_unblank(&d, 400), UB); /* power key wins, no timers after */
	EQ64(pw_dim_next_deadline(&d), -1);
	EQS(d.state, PW_DIM_ACTIVE);

	/* Release restarts the timers from the release time */
	fresh();
	EQ(pw_dim_set_inhibited(&d, true, 0), 0);
	EQ(pw_dim_set_inhibited(&d, false, 5000), 0);
	EQ64(pw_dim_next_deadline(&d), 5100);
	EQ(pw_dim_tick(&d, 5100), D);
	EQ(pw_dim_set_inhibited(&d, false, 5200), 0); /* same value: no-op */

	/* force_blank and BLANKED activity while inhibited */
	fresh();
	EQ(pw_dim_set_inhibited(&d, true, 0), 0);
	EQ(pw_dim_force_blank(&d), B);
	EQ(pw_dim_activity(&d, 100), UB | SW);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ64(pw_dim_next_deadline(&d), -1);

	/* set_timeouts while inhibited takes effect on release */
	fresh();
	EQ(pw_dim_set_inhibited(&d, true, 0), 0);
	EQ(pw_dim_set_timeouts(&d, 50, 80, 10000), 0);
	EQ64(pw_dim_next_deadline(&d), -1);
	EQ(pw_dim_set_inhibited(&d, false, 20000), 0);
	EQ64(pw_dim_next_deadline(&d), 20050);

	/* Release while BLANKED: stays blank, no deadline */
	fresh();
	EQ(pw_dim_set_inhibited(&d, true, 0), 0);
	EQ(pw_dim_force_blank(&d), B);
	EQ(pw_dim_set_inhibited(&d, false, 500), 0);
	EQS(d.state, PW_DIM_BLANKED);
	EQ64(pw_dim_next_deadline(&d), -1);

	/* Lease (lease.c): the grant unblanks first, then holds like an inhibitor,
	 * and the end restarts the timers from that moment. */
	fresh();
	EQ(pw_dim_force_blank(&d), B);
	EQ(pw_dim_force_unblank(&d, 1000), UB);
	EQ(pw_dim_set_inhibited(&d, true, 1000), 0);
	EQ(pw_dim_tick(&d, 900000), 0);
	EQ64(pw_dim_next_deadline(&d), -1);
	EQ(pw_dim_activity(&d, 5000), 0); /* touch under the lease: no wake-up swallowed */
	EQS(d.state, PW_DIM_ACTIVE);
	EQ(pw_dim_set_inhibited(&d, false, 60000), 0);
	EQ64(pw_dim_next_deadline(&d), 60100);
	EQ(pw_dim_tick(&d, 60200), B | 0); /* both due: straight to blank */

	/* A DIMMED screen is undimmed by the grant; a profile switch during the
	 * lease does not arm anything. */
	fresh();
	EQ(pw_dim_tick(&d, 100), D);
	EQ(pw_dim_set_inhibited(&d, true, 150), UD);
	EQ(pw_dim_set_timeouts(&d, 10, 20, 200), 0);
	EQ64(pw_dim_next_deadline(&d), -1);
	EQS(d.state, PW_DIM_ACTIVE);
	EQ(pw_dim_set_inhibited(&d, false, 1000), 0);
	EQ64(pw_dim_next_deadline(&d), 1010);

	/* pw_dim_init clears the flag */
	d.inhibited = true;
	pw_dim_init(&d, 100, 200);
	EQ64(pw_dim_next_deadline(&d), 100);

	return fails ? 1 : 0;
}
