/* copyrel.h - serial bookkeeping for picowl_buffer_v1.copied. No allocation. */
#ifndef PW_COPYREL_H
#define PW_COPYREL_H

#include <stdbool.h>
#include <stdint.h>

struct pw_copyrel {
	uint32_t commit_serial, sampled_serial, sent_serial;
};

void pw_copyrel_init(struct pw_copyrel *r);
/* ++commit_serial (skipping 0 on wrap); returns the new serial. */
uint32_t pw_copyrel_on_commit(struct pw_copyrel *r);
/* The current commit was consumed by a direct scanout on a copy-type output:
 * sampled_serial = commit_serial (copied is due once it is presented). */
void pw_copyrel_on_direct(struct pw_copyrel *r);
/* true with *serial = sampled_serial iff it differs from sent_serial: copied. */
bool pw_copyrel_on_present(struct pw_copyrel *r, uint32_t *serial);
/* The current commit is held by the compositor (composited, occluded, idle,
 * scanout-type output): true with *serial = commit_serial iff not answered
 * yet: retained. A direct commit awaiting its present is left alone unless
 * force (outputs off). Cancels a pending copied of an older commit. */
bool pw_copyrel_on_retain(struct pw_copyrel *r, bool force, uint32_t *serial);
bool pw_copyrel_pending(const struct pw_copyrel *r);

#endif
