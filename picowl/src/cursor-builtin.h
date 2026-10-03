/*
 * cursor-builtin.h - built-in hold animation, Pocket PC 2003 style: 8 dots
 * on a ring, 32x32, 8 frames; the lead dot moves one position per frame
 * with a short trail. Pure C, integer arithmetic only (precomputed 8-entry
 * sin/cos table). Output frames are compliant (see cursorfit.h): dots are
 * filled with `fill`, outlined with `outline` (0xRRGGBB); two colours only.
 * Hotspot is the centre (16,16).
 */
#ifndef PICOWL_CURSOR_BUILTIN_H
#define PICOWL_CURSOR_BUILTIN_H

#include <stdint.h>
#include "cursorfit.h"

#define PW_BUILTIN_FRAMES 8
#define PW_BUILTIN_SIZE 32

/* Allocate and render the 8 frames. On failure returns false with all
 * frames freed. Caller frees each with pw_image_free. */
bool pw_cursor_builtin_frames(uint32_t fill, uint32_t outline,
	struct pw_image frames[PW_BUILTIN_FRAMES]);

#endif
