/* subpixel.c - see subpixel.h. */
#include <string.h>
#include "subpixel.h"

static const char *const names[] = { "unknown", "none", "horizontal_rgb",
	"horizontal_bgr", "vertical_rgb", "vertical_bgr" };

bool pw_subpixel_parse(const char *s, int *out)
{
	for (int i = 0; i < 6; i++)
		if (s && !strcmp(s, names[i])) {
			*out = i;
			return true;
		}
	return false;
}

const char *pw_subpixel_name(int subpixel)
{
	return subpixel >= 0 && subpixel < 6 ? names[subpixel] : "unknown";
}

int pw_subpixel_in_client_frame(int native, int transform)
{
	if (native < PW_SUBPIXEL_HORIZONTAL_RGB || native > PW_SUBPIXEL_VERTICAL_BGR ||
			transform < 0 || transform > 7)
		return native;

	/* Screen coordinates, y down. s runs from the red to the blue stripe on
	 * the panel; ex and ey are the buffer's axes on the panel. */
	int s[2] = { 0, 0 };
	switch (native) {
	case PW_SUBPIXEL_HORIZONTAL_RGB: s[0] = 1; break;
	case PW_SUBPIXEL_HORIZONTAL_BGR: s[0] = -1; break;
	case PW_SUBPIXEL_VERTICAL_RGB: s[1] = 1; break;
	default: s[1] = -1; break;
	}
	int ex[2] = { transform & 4 ? -1 : 1, 0 }, ey[2] = { 0, 1 };

	/* A rotation by 90 degrees counter-clockwise takes right to up and down
	 * to right: (x, y) -> (y, -x). */
	for (int k = 0; k < (transform & 3); k++) {
		int t = ex[0];
		ex[0] = ex[1];
		ex[1] = -t;
		t = ey[0];
		ey[0] = ey[1];
		ey[1] = -t;
	}
	int along_x = s[0] * ex[0] + s[1] * ex[1];
	int along_y = s[0] * ey[0] + s[1] * ey[1];

	if (along_x)
		return along_x > 0 ? PW_SUBPIXEL_HORIZONTAL_RGB : PW_SUBPIXEL_HORIZONTAL_BGR;
	return along_y > 0 ? PW_SUBPIXEL_VERTICAL_RGB : PW_SUBPIXEL_VERTICAL_BGR;
}

enum pw_stripes pw_subpixel_stripes(int native, int transform)
{
	switch (pw_subpixel_in_client_frame(native, transform)) {
	case PW_SUBPIXEL_HORIZONTAL_RGB:
		return PW_STRIPES_RGB;
	case PW_SUBPIXEL_HORIZONTAL_BGR:
		return PW_STRIPES_BGR;
	default:
		return PW_STRIPES_NONE;
	}
}

int pw_subpixel_advertise(int native, int rotation, int sent_transform)
{
	if (sent_transform == 0 && rotation != 0)
		return pw_subpixel_in_client_frame(native, rotation);
	return native;
}
