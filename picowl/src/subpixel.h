/* subpixel.h - which way the colour stripes of a panel run as a client sees
 * them. Shared by the compositor (what it advertises) and by picowl-panel (how
 * it draws text), so it knows neither wlroots nor libwayland: the numbers are
 * those of wl_output.subpixel and wl_output.transform. */
#ifndef PW_SUBPIXEL_H
#define PW_SUBPIXEL_H

#include <stdbool.h>

/* wl_output.subpixel */
enum {
	PW_SUBPIXEL_UNKNOWN = 0,
	PW_SUBPIXEL_NONE = 1,
	PW_SUBPIXEL_HORIZONTAL_RGB = 2,
	PW_SUBPIXEL_HORIZONTAL_BGR = 3,
	PW_SUBPIXEL_VERTICAL_RGB = 4,
	PW_SUBPIXEL_VERTICAL_BGR = 5,
};

/* Order of the stripes along the horizontal axis of what a client draws. */
enum pw_stripes { PW_STRIPES_NONE, PW_STRIPES_RGB, PW_STRIPES_BGR };

/* The config and wl_output names, "horizontal_rgb" and so on. False if s is
 * not one of them. */
bool pw_subpixel_parse(const char *s, int *out);
const char *pw_subpixel_name(int subpixel);

/* The layout of a panel as it is in the frame of a client whose buffer is
 * shown with wl_output.transform t (0 normal, 1..3 rotated 90/180/270, 4
 * flipped, 5..7 flipped and rotated); native is wl_output.subpixel of the
 * panel itself. Unknown and none stay as they are.
 *
 * wl_output.transform t says how the compositor turns the buffer on its way to
 * the display; 90 is a rotation by 90 degrees counter-clockwise, and the
 * flipped ones mirror the buffer around a vertical axis first. So with 90 the
 * buffer's right edge ends up at the top of the panel and its bottom edge at
 * the panel's right. A panel that is horizontal RGB (red left) therefore has
 * its stripes run from the top of the buffer to its bottom: vertical RGB. One
 * that is vertical RGB (red at the top) has red at the buffer's right edge and
 * blue at its left: horizontal BGR. */
int pw_subpixel_in_client_frame(int native, int transform);

/* Order along a client's horizontal axis of a panel of native layout shown
 * with transform. Vertical stacks, none and unknown give PW_STRIPES_NONE: no
 * horizontal subpixel text. */
enum pw_stripes pw_subpixel_stripes(int native, int transform);

/* What the compositor puts in wl_output.geometry for a panel of this native
 * layout that is rotated by rotation, when it sends sent_transform as the
 * transform (the rotation, or normal when the display hardware does the
 * rotation). The Wayland protocol defines the subpixel as that of the panel
 * itself, which clients combine with the transform they are sent, so it is the
 * native one; only when the transform is not sent while the picture is
 * rotated does the client frame have to be what is advertised. */
int pw_subpixel_advertise(int native, int rotation, int sent_transform);

#endif
