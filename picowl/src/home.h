/* home.h - HOME of the compositor and the clients it starts. */
#ifndef PICOWL_HOME_H
#define PICOWL_HOME_H

/* The HOME to use: env_home when it is usable, else the account's home
 * directory pw_dir, else env_home unchanged (possibly NULL). Usable means
 * absolute and not "/". Pure, so it can be unit-tested. */
const char *pw_home_choose(const char *env_home, const char *pw_dir);

/* Set HOME in the environment from the passwd entry of the effective user when
 * the inherited value is unusable. Call before anything is spawned. Returns the
 * new HOME, or NULL when nothing was changed. */
const char *pw_home_init(void);

#endif
