/*
 * leasepolicy.h - who may lease the display, and what a key press does while
 * it is leased. Pure: no wlroots calls and no state, like touchhold.h. Only
 * enum pw_action comes from picowl.h.
 */
#ifndef PICOWL_LEASEPOLICY_H
#define PICOWL_LEASEPOLICY_H

#include <stdbool.h>
#include "picowl.h"

enum pw_lease_verdict {
	PW_LEASE_GRANT,
	PW_LEASE_REJECT_DISABLED,    /* [lease] enable = false */
	PW_LEASE_REJECT_BUSY,        /* a lease is active already */
	PW_LEASE_REJECT_SESSION,     /* the session is inactive: no DRM master */
	PW_LEASE_REJECT_LOCKED,      /* a layer surface holds exclusive keyboard focus */
	PW_LEASE_REJECT_NOT_FOCUSED, /* requester does not own the focused toplevel */
	PW_LEASE_REJECT_APP_ID,      /* that toplevel's app_id is not on the list */
};

struct pw_lease_facts {
	bool enabled;
	bool leased;
	bool session_active;
	bool exclusive_focus;
	bool requester_focused;
	const char *app_id;          /* of the focused toplevel, may be NULL */
};

/* First failing check wins, in the order of the enum. allow is the
 * [lease] allow list. */
enum pw_lease_verdict pw_lease_decide(const struct pw_lease_facts *facts,
	const char *allow);

/* Is app_id on the comma-separated list? Blanks around entries are ignored,
 * "*" matches any app_id (also NULL), an empty list matches nothing. */
bool pw_lease_app_allowed(const char *list, const char *app_id);

const char *pw_lease_verdict_name(enum pw_lease_verdict v);

enum pw_lease_key {
	PW_LEASE_KEY_PASS,           /* run the action, the lease goes on */
	PW_LEASE_KEY_REVOKE_FIRST,   /* end the lease, then run the action */
	PW_LEASE_KEY_DROP,           /* ignore it: the lessee owns scanout */
};

/* What a keybinding's action does while the display is leased. */
enum pw_lease_key pw_lease_key_policy(enum pw_action action);

#endif
