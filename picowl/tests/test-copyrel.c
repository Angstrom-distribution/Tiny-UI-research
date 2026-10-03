/* test-copyrel.c - unit tests for copied/retained serial bookkeeping. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "../src/copyrel.h"

int main(void)
{
	struct pw_copyrel r;
	uint32_t serial;

	/* direct scanout: commit, commit, direct, present -> copied(2) once */
	pw_copyrel_init(&r);
	assert(r.commit_serial == 0 && r.sampled_serial == 0 && r.sent_serial == 0);
	assert(pw_copyrel_on_commit(&r) == 1);
	assert(pw_copyrel_on_commit(&r) == 2);
	pw_copyrel_on_direct(&r);
	assert(r.sampled_serial == 2);
	assert(pw_copyrel_on_present(&r, &serial) && serial == 2);
	assert(!pw_copyrel_on_present(&r, &serial));
	assert(!pw_copyrel_pending(&r));
	printf("ok direct + present\n");

	/* a commit after the direct sample: copied is for the sampled one */
	pw_copyrel_init(&r);
	pw_copyrel_on_commit(&r);
	pw_copyrel_on_direct(&r);
	pw_copyrel_on_commit(&r);
	assert(pw_copyrel_on_present(&r, &serial) && serial == 1);
	assert(pw_copyrel_pending(&r)); /* commit 2 unanswered */
	printf("ok commit while pending\n");

	/* composited / idle: retained, never copied */
	pw_copyrel_init(&r);
	pw_copyrel_on_commit(&r);
	assert(!pw_copyrel_on_present(&r, &serial));
	assert(pw_copyrel_on_retain(&r, false, &serial) && serial == 1);
	assert(!pw_copyrel_on_retain(&r, false, &serial));
	assert(!pw_copyrel_on_present(&r, &serial));
	assert(!pw_copyrel_pending(&r));
	printf("ok retained\n");

	/* a direct commit awaiting its present is not retained, unless forced */
	pw_copyrel_init(&r);
	pw_copyrel_on_commit(&r);
	pw_copyrel_on_direct(&r);
	assert(!pw_copyrel_on_retain(&r, false, &serial));
	assert(pw_copyrel_on_retain(&r, true, &serial) && serial == 1);
	assert(!pw_copyrel_on_present(&r, &serial));
	printf("ok retain vs pending direct\n");

	/* retained for a newer commit cancels the copied of the older one */
	pw_copyrel_init(&r);
	pw_copyrel_on_commit(&r);
	pw_copyrel_on_direct(&r);
	pw_copyrel_on_commit(&r);
	assert(pw_copyrel_on_retain(&r, false, &serial) && serial == 2);
	assert(!pw_copyrel_on_present(&r, &serial));
	printf("ok retain cancels older copied\n");

	/* nothing committed: nothing to say */
	pw_copyrel_init(&r);
	assert(!pw_copyrel_on_retain(&r, true, &serial));
	assert(!pw_copyrel_pending(&r));

	/* wrap at UINT32_MAX skips 0 */
	pw_copyrel_init(&r);
	r.commit_serial = UINT32_MAX - 1;
	assert(pw_copyrel_on_commit(&r) == UINT32_MAX);
	assert(pw_copyrel_on_commit(&r) == 1);
	printf("ok wrap\n");
	return 0;
}
