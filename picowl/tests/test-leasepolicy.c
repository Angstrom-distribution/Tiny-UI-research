/*
 * test-leasepolicy: unit tests for the DRM lease grant policy (the verdict
 * for every reject reason, the allow list syntax) and the key policy.
 */
#include <stdio.h>
#include <string.h>
#include "leasepolicy.h"

static int fails;
#define EQ(got, want) do { int g_ = (got), w_ = (want); if (g_ != w_) { \
	fprintf(stderr, "%s:%d: got %d want %d\n", __FILE__, __LINE__, g_, w_); fails++; } } while (0)

/* Facts which grant for app_id "mediaplayer". */
static struct pw_lease_facts good(void)
{
	return (struct pw_lease_facts){
		.enabled = true,
		.session_active = true,
		.requester_focused = true,
		.app_id = "mediaplayer",
	};
}

static void test_decide(void)
{
	struct pw_lease_facts f = good();
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_GRANT);

	f = good(); f.enabled = false;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_DISABLED);
	f = good(); f.leased = true;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_BUSY);
	f = good(); f.session_active = false;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_SESSION);
	f = good(); f.exclusive_focus = true;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_LOCKED);
	f = good(); f.requester_focused = false;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_NOT_FOCUSED);
	f = good();
	EQ(pw_lease_decide(&f, "terminal"), PW_LEASE_REJECT_APP_ID);
	EQ(pw_lease_decide(&f, ""), PW_LEASE_REJECT_APP_ID);
	EQ(pw_lease_decide(&f, NULL), PW_LEASE_REJECT_APP_ID);

	/* The first failing check wins, in enum order. */
	f = (struct pw_lease_facts){ .app_id = "x" };
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_DISABLED);
	f.enabled = true; f.leased = true;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_BUSY);
	f.leased = false;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_SESSION);
	f.session_active = true; f.exclusive_focus = true;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_LOCKED);
	f.exclusive_focus = false;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_NOT_FOCUSED);
	f.requester_focused = true;
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_APP_ID);

	/* "*" accepts any focused client, also one without an app_id; the
	 * other checks still apply. */
	f = good(); f.app_id = NULL;
	EQ(pw_lease_decide(&f, "*"), PW_LEASE_GRANT);
	EQ(pw_lease_decide(&f, "mediaplayer"), PW_LEASE_REJECT_APP_ID);
	f.requester_focused = false;
	EQ(pw_lease_decide(&f, "*"), PW_LEASE_REJECT_NOT_FOCUSED);

	for (int v = PW_LEASE_GRANT; v <= PW_LEASE_REJECT_APP_ID; v++)
		EQ(strcmp(pw_lease_verdict_name(v), "?") != 0, 1);
}

static void test_allow_list(void)
{
	EQ(pw_lease_app_allowed("mediaplayer", "mediaplayer"), 1);
	EQ(pw_lease_app_allowed("mediaplayer", "mediaplayer2"), 0); /* whole entry */
	EQ(pw_lease_app_allowed("mediaplayer", "media"), 0);
	EQ(pw_lease_app_allowed("mediaplayer", "Mediaplayer"), 0); /* case matters */
	EQ(pw_lease_app_allowed("mediaplayer", NULL), 0);
	EQ(pw_lease_app_allowed("mediaplayer", ""), 0);

	/* lists, blanks and empty entries */
	EQ(pw_lease_app_allowed("a,mediaplayer,b", "mediaplayer"), 1);
	EQ(pw_lease_app_allowed("a,mediaplayer,b", "a"), 1);
	EQ(pw_lease_app_allowed("a,mediaplayer,b", "b"), 1);
	EQ(pw_lease_app_allowed("a,mediaplayer,b", "c"), 0);
	EQ(pw_lease_app_allowed("  a ,\tmediaplayer , b  ", "mediaplayer"), 1);
	EQ(pw_lease_app_allowed("  a ,\tmediaplayer , b  ", "b"), 1);
	EQ(pw_lease_app_allowed(",,a,,", "a"), 1);
	EQ(pw_lease_app_allowed("a b", "a"), 0); /* blanks do not separate */
	EQ(pw_lease_app_allowed("a b", "a b"), 1);

	/* * */
	EQ(pw_lease_app_allowed("*", "anything"), 1);
	EQ(pw_lease_app_allowed("*", NULL), 1);
	EQ(pw_lease_app_allowed("*", ""), 1);
	EQ(pw_lease_app_allowed("a, *", "zzz"), 1);
	EQ(pw_lease_app_allowed("a*", "abc"), 0); /* no globbing */
	EQ(pw_lease_app_allowed("**", "abc"), 0);

	/* empty list: nothing, not even an empty app_id */
	EQ(pw_lease_app_allowed("", "mediaplayer"), 0);
	EQ(pw_lease_app_allowed("", NULL), 0);
	EQ(pw_lease_app_allowed(" , ", ""), 0);
	EQ(pw_lease_app_allowed(NULL, "mediaplayer"), 0);
}

static void test_key_policy(void)
{
	EQ(pw_lease_key_policy(PW_ACTION_TOGGLE_BLANK), PW_LEASE_KEY_REVOKE_FIRST);
	EQ(pw_lease_key_policy(PW_ACTION_CYCLE_VIEWS), PW_LEASE_KEY_REVOKE_FIRST);
	EQ(pw_lease_key_policy(PW_ACTION_SPAWN), PW_LEASE_KEY_REVOKE_FIRST);
	EQ(pw_lease_key_policy(PW_ACTION_TOGGLE_PANEL), PW_LEASE_KEY_REVOKE_FIRST);
	EQ(pw_lease_key_policy(PW_ACTION_ROTATE), PW_LEASE_KEY_DROP);
	EQ(pw_lease_key_policy(PW_ACTION_CLOSE_VIEW), PW_LEASE_KEY_PASS);
	EQ(pw_lease_key_policy(PW_ACTION_QUIT), PW_LEASE_KEY_PASS);
}

int main(void)
{
	test_decide();
	test_allow_list();
	test_key_policy();
	if (!fails)
		printf("leasepolicy: ok\n");
	return fails ? 1 : 0;
}
