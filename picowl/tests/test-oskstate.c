#include <stdio.h>
#include "oskstate.h"

static int fails;
#define EQ(got, want) do { long long g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got %lld want %lld\n", __FILE__, __LINE__, g_, w_); fails++; } } while (0)

static struct pw_oskstate st;

/* Start at t, let it die after run_ms, return exited()'s delay. */
static int run_and_die(int64_t t, int64_t run_ms)
{
	pw_oskstate_started(&st, t);
	EQ(st.phase, PW_OSK_RUNNING);
	return pw_oskstate_exited(&st, t + run_ms);
}

int main(void)
{
	/* backoff series 1, 2, 4, 8 s, then give up on the fifth quick exit */
	pw_oskstate_init(&st, true);
	EQ(st.phase, PW_OSK_STOPPED);
	EQ(run_and_die(0, 50), 1000);
	EQ(st.phase, PW_OSK_BACKOFF);
	EQ(run_and_die(1050, 50), 2000);
	EQ(run_and_die(3100, 50), 4000);
	EQ(run_and_die(7150, 50), 8000);
	EQ(st.phase, PW_OSK_BACKOFF);
	EQ(run_and_die(15200, 50), -1);
	EQ(st.phase, PW_OSK_GAVE_UP);
	EQ(st.quick_exits, PW_OSK_QUICK_LIMIT);
	/* nothing more happens by itself */
	EQ(pw_oskstate_exited(&st, 99999), -1);
	EQ(st.phase, PW_OSK_GAVE_UP);

	/* a show or toggle starts it again with a fresh count; hide does not */
	EQ(pw_oskstate_request(&st, PW_OSK_HIDE), PW_OSK_DO_NOTHING);
	EQ(st.phase, PW_OSK_GAVE_UP);
	EQ(pw_oskstate_request(&st, PW_OSK_TOGGLE), PW_OSK_DO_START);
	EQ(st.quick_exits, 0);
	EQ(run_and_die(100000, 50), 1000);

	/* the edge: exactly PW_OSK_QUICK_MS is not quick, one ms less is */
	pw_oskstate_init(&st, true);
	EQ(run_and_die(0, PW_OSK_QUICK_MS - 1), 1000);
	EQ(st.quick_exits, 1);
	EQ(run_and_die(0, PW_OSK_QUICK_MS), PW_OSK_RESTART_MS);
	EQ(st.quick_exits, 0);

	/* a long run resets the count: the series starts over */
	pw_oskstate_init(&st, true);
	EQ(run_and_die(0, 50), 1000);
	EQ(run_and_die(0, 50), 2000);
	EQ(run_and_die(0, 50), 4000);
	EQ(run_and_die(0, 60000), 1000);
	EQ(st.quick_exits, 0);
	EQ(run_and_die(0, 50), 1000);
	EQ(run_and_die(0, 50), 2000);

	/* four quick exits, a long run, four more: never gives up */
	pw_oskstate_init(&st, true);
	for (int round = 0; round < 3; round++) {
		for (int i = 0; i < 4; i++)
			EQ(run_and_die(0, 50) > 0, 1);
		EQ(run_and_die(0, 20000), 1000);
		EQ(st.phase, PW_OSK_BACKOFF);
	}

	/* restart = no: an exit is final, and the keyboard can still be started */
	pw_oskstate_init(&st, false);
	EQ(run_and_die(0, 50), -1);
	EQ(st.phase, PW_OSK_STOPPED);
	EQ(run_and_die(0, 60000), -1);
	EQ(st.phase, PW_OSK_STOPPED);
	EQ(pw_oskstate_request(&st, PW_OSK_SHOW), PW_OSK_DO_START);
	pw_oskstate_started(&st, 0);
	EQ(st.phase, PW_OSK_RUNNING);

	/* exited() only means something while running */
	pw_oskstate_init(&st, true);
	EQ(pw_oskstate_exited(&st, 0), -1);
	EQ(st.phase, PW_OSK_STOPPED);
	EQ(st.quick_exits, 0);

	/* requests while running signal and track the visibility guess */
	pw_oskstate_init(&st, true);
	pw_oskstate_started(&st, 0);
	EQ(st.visible, 0);
	EQ(pw_oskstate_request(&st, PW_OSK_TOGGLE), PW_OSK_DO_SIGNAL_TOGGLE);
	EQ(st.visible, 1);
	EQ(pw_oskstate_request(&st, PW_OSK_TOGGLE), PW_OSK_DO_SIGNAL_TOGGLE);
	EQ(st.visible, 0);
	EQ(pw_oskstate_request(&st, PW_OSK_SHOW), PW_OSK_DO_SIGNAL_SHOW);
	EQ(st.visible, 1);
	/* show and hide are sent even when the guess already says so: the guess
	 * is wrong when something else signalled the keyboard */
	EQ(pw_oskstate_request(&st, PW_OSK_SHOW), PW_OSK_DO_SIGNAL_SHOW);
	EQ(pw_oskstate_request(&st, PW_OSK_HIDE), PW_OSK_DO_SIGNAL_HIDE);
	EQ(st.visible, 0);
	EQ(pw_oskstate_request(&st, PW_OSK_HIDE), PW_OSK_DO_SIGNAL_HIDE);

	/* a (re)start and an exit reset the guess to hidden */
	pw_oskstate_request(&st, PW_OSK_SHOW);
	EQ(st.visible, 1);
	EQ(pw_oskstate_exited(&st, 10), 1000);
	EQ(st.visible, 0);
	pw_oskstate_started(&st, 2000);
	EQ(st.visible, 0);
	pw_oskstate_request(&st, PW_OSK_SHOW);
	pw_oskstate_started(&st, 3000);
	EQ(st.visible, 0);

	/* requests in backoff start it now; hide does not */
	pw_oskstate_init(&st, true);
	EQ(run_and_die(0, 50), 1000);
	EQ(pw_oskstate_request(&st, PW_OSK_HIDE), PW_OSK_DO_NOTHING);
	EQ(st.phase, PW_OSK_BACKOFF);
	EQ(pw_oskstate_request(&st, PW_OSK_SHOW), PW_OSK_DO_START);
	EQ(st.quick_exits, 0);

	/* never started: a request starts it */
	pw_oskstate_init(&st, true);
	EQ(pw_oskstate_request(&st, PW_OSK_TOGGLE), PW_OSK_DO_START);
	EQ(pw_oskstate_request(&st, PW_OSK_HIDE), PW_OSK_DO_NOTHING);

	/* shutdown: no restart, exit ignored */
	pw_oskstate_init(&st, true);
	pw_oskstate_started(&st, 0);
	pw_oskstate_stop(&st);
	EQ(st.phase, PW_OSK_STOPPED);
	EQ(pw_oskstate_exited(&st, 10), -1);
	EQ(st.phase, PW_OSK_STOPPED);

	if (!fails)
		printf("oskstate: ok\n");
	return fails ? 1 : 0;
}
