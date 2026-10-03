#include "copyrel.h"

void pw_copyrel_init(struct pw_copyrel *r)
{
	r->commit_serial = r->sampled_serial = r->sent_serial = 0;
}

uint32_t pw_copyrel_on_commit(struct pw_copyrel *r)
{
	if (++r->commit_serial == 0)
		r->commit_serial = 1;
	return r->commit_serial;
}

void pw_copyrel_on_direct(struct pw_copyrel *r)
{
	r->sampled_serial = r->commit_serial;
}

bool pw_copyrel_on_present(struct pw_copyrel *r, uint32_t *serial)
{
	if (r->sampled_serial == r->sent_serial)
		return false;
	*serial = r->sampled_serial;
	r->sent_serial = r->sampled_serial;
	return true;
}

bool pw_copyrel_on_retain(struct pw_copyrel *r, bool force, uint32_t *serial)
{
	if (r->commit_serial == r->sent_serial)
		return false;
	if (r->sampled_serial == r->commit_serial && !force)
		return false;
	*serial = r->commit_serial;
	r->sampled_serial = r->sent_serial = r->commit_serial;
	return true;
}

bool pw_copyrel_pending(const struct pw_copyrel *r)
{
	return r->commit_serial != r->sent_serial;
}
