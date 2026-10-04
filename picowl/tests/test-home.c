#include <stdio.h>
#include <string.h>
#include "home.h"

static int fails;
#define STR(got, want) do { const char *g_ = (got), *w_ = (want); \
	if ((g_ == NULL) != (w_ == NULL) || (g_ && strcmp(g_, w_) != 0)) { \
	fprintf(stderr, "%s:%d: got '%s' want '%s'\n", __FILE__, __LINE__, \
		g_ ? g_ : "(null)", w_ ? w_ : "(null)"); fails++; } } while (0)

int main(void)
{
	/* a deliberate HOME is kept, even when the account says otherwise */
	STR(pw_home_choose("/home/root", "/root"), "/home/root");
	/* the service manager's useless values fall back to the account */
	STR(pw_home_choose("/", "/home/root"), "/home/root");
	STR(pw_home_choose(NULL, "/home/root"), "/home/root");
	STR(pw_home_choose("", "/home/root"), "/home/root");
	STR(pw_home_choose("relative/dir", "/home/root"), "/home/root");
	/* an account without a usable home changes nothing */
	STR(pw_home_choose("/", NULL), "/");
	STR(pw_home_choose("/", "/"), "/");
	STR(pw_home_choose("/", ""), "/");
	STR(pw_home_choose(NULL, NULL), NULL);
	return fails ? 1 : 0;
}
