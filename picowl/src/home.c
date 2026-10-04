/* home.c - HOME of the compositor and the clients it starts.
 *
 * A service manager without a login session hands its units HOME=/ (or none),
 * and every client then looks for ~/.config in the wrong place: a terminal
 * silently loses its font and shell settings. The account database says where
 * the user's home really is, so use it only when the inherited value is useless
 * and leave a deliberate HOME alone. */
#include "home.h"

#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usable(const char *p)
{
	return p && p[0] == '/' && strcmp(p, "/") != 0;
}

const char *pw_home_choose(const char *env_home, const char *pw_dir)
{
	if (usable(env_home))
		return env_home;
	if (usable(pw_dir))
		return pw_dir;
	return env_home;
}

const char *pw_home_init(void)
{
	const char *cur = getenv("HOME");
	struct passwd *pw = getpwuid(geteuid());
	const char *want = pw_home_choose(cur, pw ? pw->pw_dir : NULL);

	if (!want || (cur && strcmp(want, cur) == 0))
		return NULL;
	setenv("HOME", want, 1);
	return want;
}
