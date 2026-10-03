/*
 * powerprofile.h - tiny dependency-free header shared by picowl.h (config)
 * and powersupply.h, so unit tests need no wlroots headers.
 */
#ifndef PICOWL_POWERPROFILE_H
#define PICOWL_POWERPROFILE_H

#include <stdbool.h>

/* Power profile, see powersupply.h. */
enum pw_power_profile {
	PW_PROFILE_AC,
	PW_PROFILE_BATTERY,
	PW_PROFILE_LOW,
	PW_PROFILE_COUNT
};

/* Per-profile idle timings, seconds. 0 disables the step. inhibit: honour
 * client idle inhibitors (idle-inhibit protocol) in this profile. */
struct pw_power_timing {
	int dim_after_s;
	int blank_after_s;
	bool inhibit;
};

#endif
