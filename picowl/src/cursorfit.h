/*
 * cursorfit.h - cursor image compliance checking and fitting. Pure C, no
 * wlroots dependency; shared by picowl and tools/picowl-cursor-convert.
 *
 * PLATFORM CURSOR RULES (a "compliant" frame):
 *  - ARGB8888 (0xAARRGGBB, stride = width pixels), width,height in 1..64.
 *  - Every pixel is either fully transparent, i.e. exactly 0x00000000 (all
 *    32 bits zero; other colour bits with alpha 0 are NOT allowed), or fully opaque (alpha == 0xff). No translucency.
 *  - At most TWO distinct opaque colours per frame. Colours are compared at
 *    6 bits per component (c >> 2 per channel), the MediaQ MQ1132/MQ1188
 *    colour register depth: colours differing only in the low 2 bits of
 *    each channel count as one colour.
 * The mq11xx DRM cursor plane rejects violations with -EINVAL.
 */
#ifndef PICOWL_CURSORFIT_H
#define PICOWL_CURSORFIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PW_CURSOR_MAX_SIZE 64

struct pw_image {
	int width, height;
	uint32_t *pixels;   /* ARGB8888, stride = width; owned by the holder */
};

struct pw_cursorfit_opts {
	bool premultiplied;   /* input alpha is premultiplied (un-premultiply
	                       * colour before quantising) */
	int max_size;         /* clamp size; <= 0 means PW_CURSOR_MAX_SIZE.
	                       * Larger images are centre-cropped. */
	bool force_colours;   /* use fg/bg (0xRRGGBB) as the two colours instead
	                       * of picking dominant ones */
	uint32_t fg, bg;      /* fg = lighter/lead colour slot, bg = other */
};

struct pw_cursorfit_report {
	bool compliant_input;  /* input already satisfied every rule; output is
	                        * an exact copy and all counts are zero */
	int clipped_alpha;     /* pixels whose alpha was snapped to 0 or 255 */
	int merged_colours;    /* distinct 6-bit colours beyond two (exact) */
	bool cropped;          /* image was centre-cropped to max_size */
	int out_w, out_h;
	int input_colours;     /* exact distinct 6-bit opaque colours before
	                        * fitting (not capped) */
	int remapped_pixels;   /* opaque pixels whose colour changed */
};

/* Allocate a zeroed (all transparent) w*h image. Returns false on failure
 * or w/h <= 0. pw_image_free is NULL-safe and clears the struct. */
bool pw_image_alloc(struct pw_image *img, int w, int h);
void pw_image_free(struct pw_image *img);

/* Return true if img is compliant (see rules above). Otherwise return false
 * and, if why != NULL, write a one-line reason into why[why_len]. */
bool pw_cursorfit_check(const struct pw_image *img, char *why, size_t why_len);

/* Convert any RGBA image to a compliant one:
 *  1. optional un-premultiply; alpha >= 128 -> opaque 0xff, else the pixel
 *     becomes exactly 0x00000000;
 *  2. centre-crop to max_size (hot_x/hot_y adjusted, may end up outside the
 *     image; caller clamps) -- reject (return false) only on bad args/OOM;
 *  3. reduce opaque pixels to <= 2 colours (opts->force_colours, else the
 *     two most frequent 6-bit colours, splitting by luminance if only one
 *     dominates), mapping each pixel to the nearer colour in RGB distance.
 * in and out must differ; out is allocated here (free with pw_image_free).
 * hot_x/hot_y may be NULL; otherwise in/out hotspot. report may be NULL.
 * Returns false on failure (out untouched). Integer arithmetic only. */
bool pw_cursorfit_fit(const struct pw_image *in, struct pw_image *out,
	int *hot_x, int *hot_y, const struct pw_cursorfit_opts *opts,
	struct pw_cursorfit_report *report);

/* PAM (P7, TUPLTYPE RGB_ALPHA or RGB, maxval 255) or binary PPM (P6,
 * maxval 255) reader into ARGB8888 (RGB inputs are fully opaque; straight
 * alpha). pw_pam_read opens path; pw_pam_read_fp reads from f. Returns
 * false and fills err (may be NULL) on failure. */
bool pw_pam_read(const char *path, struct pw_image *img, char *err, size_t err_len);
bool pw_pam_read_fp(FILE *f, struct pw_image *img, char *err, size_t err_len);
/* Write img as PAM P7 RGB_ALPHA 8-bit. Returns false on I/O error. */
bool pw_pam_write(const char *path, const struct pw_image *img);
bool pw_pam_write_fp(FILE *f, const struct pw_image *img);

/* Copy frame `index` (columns index*frame_w .. +frame_w) of a horizontal
 * strip into a newly allocated out image of frame_w x strip->height.
 * frame_w <= 0 means frame_w = strip height. Returns false if the frame is
 * out of range. */
bool pw_strip_frame(const struct pw_image *strip, int frame_w, int index,
	struct pw_image *out);
/* Number of frames in the strip (width / frame_w), same frame_w rule. */
int pw_strip_count(const struct pw_image *strip, int frame_w);

#endif
