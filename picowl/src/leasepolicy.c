/* leasepolicy.c - lease grant policy and key policy, see leasepolicy.h. */
#include <string.h>
#include "leasepolicy.h"

bool pw_lease_app_allowed(const char *list, const char *app_id)
{
	if (!list)
		return false;
	while (*list) {
		while (*list == ' ' || *list == '\t' || *list == ',')
			list++;
		const char *end = list;
		while (*end && *end != ',')
			end++;
		const char *stop = end;
		while (stop > list && (stop[-1] == ' ' || stop[-1] == '\t'))
			stop--;
		size_t len = stop - list;
		if (len == 1 && *list == '*')
			return true;
		if (len > 0 && app_id && strlen(app_id) == len &&
				strncmp(list, app_id, len) == 0)
			return true;
		list = end;
	}
	return false;
}

enum pw_lease_verdict pw_lease_decide(const struct pw_lease_facts *f,
	const char *allow)
{
	if (!f->enabled)
		return PW_LEASE_REJECT_DISABLED;
	if (f->leased)
		return PW_LEASE_REJECT_BUSY;
	if (!f->session_active)
		return PW_LEASE_REJECT_SESSION;
	if (f->exclusive_focus)
		return PW_LEASE_REJECT_LOCKED;
	if (!f->requester_focused)
		return PW_LEASE_REJECT_NOT_FOCUSED;
	if (!pw_lease_app_allowed(allow, f->app_id))
		return PW_LEASE_REJECT_APP_ID;
	return PW_LEASE_GRANT;
}

const char *pw_lease_verdict_name(enum pw_lease_verdict v)
{
	switch (v) {
	case PW_LEASE_GRANT: return "granted";
	case PW_LEASE_REJECT_DISABLED: return "leasing disabled";
	case PW_LEASE_REJECT_BUSY: return "already leased";
	case PW_LEASE_REJECT_SESSION: return "session inactive";
	case PW_LEASE_REJECT_LOCKED: return "exclusive keyboard focus";
	case PW_LEASE_REJECT_NOT_FOCUSED: return "requester is not the focused client";
	case PW_LEASE_REJECT_APP_ID: return "app_id not allowed";
	}
	return "?";
}

enum pw_lease_key pw_lease_key_policy(enum pw_action action)
{
	switch (action) {
	case PW_ACTION_TOGGLE_BLANK:
	case PW_ACTION_CYCLE_VIEWS:
	case PW_ACTION_SPAWN:
	case PW_ACTION_TOGGLE_PANEL:
	case PW_ACTION_OSK: /* a keyboard shown under the lease would be invisible */
		return PW_LEASE_KEY_REVOKE_FIRST;
	case PW_ACTION_ROTATE:
		return PW_LEASE_KEY_DROP;
	case PW_ACTION_CLOSE_VIEW:
	case PW_ACTION_QUIT:
		break;
	}
	return PW_LEASE_KEY_PASS;
}
