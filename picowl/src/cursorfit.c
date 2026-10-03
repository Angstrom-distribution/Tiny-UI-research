/* cursorfit.c - cursor image compliance checking and fitting. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cursorfit.h"

bool pw_image_alloc(struct pw_image *img, int w, int h)
{
	img->width = img->height = 0;
	img->pixels = NULL;
	if (w <= 0 || h <= 0)
		return false;
	img->pixels = calloc((size_t)w * h, sizeof(uint32_t));
	if (!img->pixels)
		return false;
	img->width = w;
	img->height = h;
	return true;
}

void pw_image_free(struct pw_image *img)
{
	if (!img)
		return;
	free(img->pixels);
	img->pixels = NULL;
	img->width = img->height = 0;
}

/* 6-bit quantization: for color reduction, compare at 6 bits per channel.
 * This matches the MediaQ MQ1132/MQ1188 hardware cursor depth. */
#define QUANT_BITS 6
#define QUANT_MASK ((1 << QUANT_BITS) - 1)
#define QUANT(c) (((c) >> (8 - QUANT_BITS)) & QUANT_MASK)

/* Helper: get 6-bit quantized color from ARGB8888 (ignore alpha). */
static uint32_t color_quantized(uint32_t argb)
{
	uint32_t r = QUANT((argb >> 16) & 0xff);
	uint32_t g = QUANT((argb >> 8) & 0xff);
	uint32_t b = QUANT(argb & 0xff);
	return (r << 12) | (g << 6) | b;  /* Pack into 18 bits */
}

/* Helper: RGB distance squared for color matching (integer only). */
static int rgb_distance_sq(uint32_t c1, uint32_t c2)
{
	int r = (int)((c1 >> 16) & 0xff) - (int)((c2 >> 16) & 0xff);
	int g = (int)((c1 >> 8) & 0xff) - (int)((c2 >> 8) & 0xff);
	int b = (int)(c1 & 0xff) - (int)(c2 & 0xff);
	return r*r + g*g + b*b;
}

/* Helper: compute luminance (Y = 0.299*R + 0.587*G + 0.114*B, scaled by 1000) */
static int luminance(uint32_t argb)
{
	int r = (argb >> 16) & 0xff;
	int g = (argb >> 8) & 0xff;
	int b = argb & 0xff;
	return 299*r + 587*g + 114*b;
}

bool pw_cursorfit_check(const struct pw_image *img, char *why, size_t why_len)
{
	if (!img || !img->pixels)
		goto bad_args;

	if (img->width < 1 || img->width > PW_CURSOR_MAX_SIZE ||
	    img->height < 1 || img->height > PW_CURSOR_MAX_SIZE) {
		if (why)
			snprintf(why, why_len, "size %dx%d out of range 1..%d",
				img->width, img->height, PW_CURSOR_MAX_SIZE);
		return false;
	}

	/* Scan for opaque colors and check alpha. */
	uint32_t quant_colors[2];
	int color_count = 0;
	for (int y = 0; y < img->height; y++) {
		for (int x = 0; x < img->width; x++) {
			uint32_t pixel = img->pixels[y * img->width + x];
			uint32_t alpha = (pixel >> 24) & 0xff;

			/* Check: fully opaque or fully transparent. */
			if (alpha != 0x00 && alpha != 0xff) {
				if (why)
					snprintf(why, why_len,
						"translucent pixel at (%d,%d)",
						x, y);
				return false;
			}

			if (alpha == 0x00 && pixel != 0x00000000) {
				if (why)
					snprintf(why, why_len,
						"transparent pixel at (%d,%d) is not 0x00000000",
						x, y);
				return false;
			}

			/* If opaque, collect color. */
			if (alpha == 0xff) {
				uint32_t quant = color_quantized(pixel);

				/* Check if this color is already in our list. */
				bool found = false;
				for (int i = 0; i < color_count; i++) {
					if (quant_colors[i] == quant) {
						found = true;
						break;
					}
				}
				if (!found) {
					if (color_count >= 2) {
						if (why)
							snprintf(why, why_len,
								"more than 2 distinct colors");
						return false;
					}
					quant_colors[color_count++] = quant;
				}
			}
		}
	}

	return true;

bad_args:
	if (why)
		snprintf(why, why_len, "null image");
	return false;
}

bool pw_cursorfit_fit(const struct pw_image *in, struct pw_image *out,
	int *hot_x, int *hot_y, const struct pw_cursorfit_opts *opts,
	struct pw_cursorfit_report *report)
{
	if (!in || !in->pixels || !out)
		return false;
	if (in == out)
		return false;  /* in and out must differ */

	struct pw_cursorfit_opts default_opts = {0};
	if (!opts)
		opts = &default_opts;

	int max_size = opts->max_size <= 0 ? PW_CURSOR_MAX_SIZE : opts->max_size;
	if (max_size < 1 || max_size > PW_CURSOR_MAX_SIZE)
		return false;

	/* Already compliant (and nothing forced): exact copy. */
	if (!opts->force_colours && in->width <= max_size &&
	    in->height <= max_size && pw_cursorfit_check(in, NULL, 0)) {
		if (!pw_image_alloc(out, in->width, in->height))
			return false;
		memcpy(out->pixels, in->pixels,
			(size_t)in->width * in->height * sizeof(uint32_t));
		if (hot_x)
			*hot_x = *hot_x < 0 ? 0 : (*hot_x >= out->width ? out->width - 1 : *hot_x);
		if (hot_y)
			*hot_y = *hot_y < 0 ? 0 : (*hot_y >= out->height ? out->height - 1 : *hot_y);
		if (report) {
			memset(report, 0, sizeof(*report));
			report->compliant_input = true;
			report->out_w = out->width;
			report->out_h = out->height;
		}
		return true;
	}

	int report_input_colors = 0;
	int report_clipped_alpha = 0;
	int report_remapped_pixels = 0;
	int report_merged_colours = 0;
	bool report_cropped = false;

	/* Step 1: Un-premultiply (if needed) and threshold alpha. */
	struct pw_image temp;
	if (!pw_image_alloc(&temp, in->width, in->height))
		return false;

	for (int y = 0; y < in->height; y++) {
		for (int x = 0; x < in->width; x++) {
			uint32_t pixel = in->pixels[y * in->width + x];
			uint32_t alpha = (pixel >> 24) & 0xff;
			uint32_t r = (pixel >> 16) & 0xff;
			uint32_t g = (pixel >> 8) & 0xff;
			uint32_t b = pixel & 0xff;

			if (alpha < 128) {
				temp.pixels[y * in->width + x] = 0x00000000;
				if (alpha > 0)
					report_clipped_alpha++;
			} else {
				/* Make fully opaque and un-premultiply if needed. */
				if (opts->premultiplied && alpha < 255) {
					r = (r * 255 + alpha/2) / alpha;
					g = (g * 255 + alpha/2) / alpha;
					b = (b * 255 + alpha/2) / alpha;
					r = r > 255 ? 255 : r;
					g = g > 255 ? 255 : g;
					b = b > 255 ? 255 : b;
				}
				temp.pixels[y * in->width + x] = 0xff000000 | (r << 16) | (g << 8) | b;
				if (alpha != 255)
					report_clipped_alpha++;
			}
		}
	}

	/* Step 2: Centre-crop to max_size (if needed). */
	struct pw_image cropped;
	int crop_x = 0, crop_y = 0;
	if (temp.width > max_size || temp.height > max_size) {
		report_cropped = true;
		int crop_w = temp.width > max_size ? max_size : temp.width;
		int crop_h = temp.height > max_size ? max_size : temp.height;
		crop_x = (temp.width - crop_w) / 2;
		crop_y = (temp.height - crop_h) / 2;

		if (!pw_image_alloc(&cropped, crop_w, crop_h)) {
			pw_image_free(&temp);
			return false;
		}

		for (int y = 0; y < crop_h; y++) {
			for (int x = 0; x < crop_w; x++) {
				cropped.pixels[y * crop_w + x] =
					temp.pixels[(crop_y + y) * temp.width + (crop_x + x)];
			}
		}
		pw_image_free(&temp);
	} else {
		cropped = temp;
	}

	/* Step 3: Collect all unique 6-bit colors. */
	uint32_t color_map[256];  /* unique 6-bit quantized colors (RGB only) */
	int color_freq[256];
	int num_colors = 0;
	/* Exact distinct-colour count via an 18-bit presence bitmap. */
	uint8_t seen[(1 << 18) / 8];
	int distinct = 0;
	memset(seen, 0, sizeof(seen));

	for (int i = 0; i < cropped.width * cropped.height; i++) {
		uint32_t pixel = cropped.pixels[i];
		uint32_t alpha = (pixel >> 24) & 0xff;

		if (alpha == 0xff) {
			uint32_t color_rgb = pixel & 0xffffff;
			uint32_t quant = color_quantized(pixel);

			if (!(seen[quant >> 3] & (1u << (quant & 7)))) {
				seen[quant >> 3] |= (uint8_t)(1u << (quant & 7));
				distinct++;
			}

			/* Find or add this color. */
			bool found = false;
			for (int j = 0; j < num_colors; j++) {
				if (color_quantized(color_map[j]) == quant) {
					color_freq[j]++;
					found = true;
					break;
				}
			}
			if (!found && num_colors < 256) {
				color_map[num_colors] = color_rgb;
				color_freq[num_colors] = 1;
				num_colors++;
			}
		}
	}

	report_input_colors = distinct;

	/* Step 4: Select two colors (forced or dominant). */
	uint32_t color1, color2;
	if (opts->force_colours) {
		color1 = opts->fg;
		color2 = opts->bg;
	} else if (num_colors <= 2) {
		if (num_colors >= 1)
			color1 = color_map[0];
		else
			color1 = 0x000000;  /* default black */
		if (num_colors >= 2)
			color2 = color_map[1];
		else
			color2 = 0xffffff;  /* default white */
	} else {
		/* Pick the two most frequent colours. */
		int best1 = 0, best2 = 1;
		if (color_freq[1] > color_freq[0]) {
			best1 = 1;
			best2 = 0;
		}
		int best_freq1 = color_freq[best1], best_freq2 = color_freq[best2];

		for (int i = 2; i < num_colors; i++) {
			if (color_freq[i] > best_freq1) {
				best2 = best1;
				best_freq2 = best_freq1;
				best1 = i;
				best_freq1 = color_freq[i];
			} else if (color_freq[i] > best_freq2) {
				best2 = i;
				best_freq2 = color_freq[i];
			}
		}

		color1 = color_map[best1];
		color2 = color_map[best2];

		/* One colour dominates: split by luminance, i.e. take as the
		 * second colour the one furthest in luminance from the first. */
		if (best_freq1 > best_freq2 * 4) {
			int lum1 = luminance(color1);
			int far_d = -1;
			for (int i = 0; i < num_colors; i++) {
				if (i == best1)
					continue;
				int d = luminance(color_map[i]) - lum1;
				if (d < 0)
					d = -d;
				if (d > far_d) {
					far_d = d;
					color2 = color_map[i];
				}
			}
		}
	}

	report_merged_colours = distinct > 2 ? distinct - 2 : 0;

	/* Step 5: Map opaque pixels to nearest color and create output. */
	if (!pw_image_alloc(out, cropped.width, cropped.height)) {
		pw_image_free(&cropped);
		return false;
	}

	for (int i = 0; i < cropped.width * cropped.height; i++) {
		uint32_t pixel = cropped.pixels[i];
		uint32_t alpha = (pixel >> 24) & 0xff;

		if (alpha == 0x00) {
			out->pixels[i] = 0x00000000;
		} else {
			/* Map to nearest color. */
			uint32_t rgb = pixel & 0xffffff;
			int dist1 = rgb_distance_sq(rgb, color1);
			int dist2 = rgb_distance_sq(rgb, color2);
			uint32_t mapped = (dist1 <= dist2) ? color1 : color2;
			out->pixels[i] = 0xff000000 | mapped;

			if ((pixel & 0xffffff) != mapped)
				report_remapped_pixels++;
		}
	}

	pw_image_free(&cropped);

	/* Adjust hotspot for cropping. */
	if (hot_x && report_cropped)
		*hot_x -= crop_x;
	if (hot_y && report_cropped)
		*hot_y -= crop_y;
	if (hot_x)
		*hot_x = *hot_x < 0 ? 0 : (*hot_x >= out->width ? out->width - 1 : *hot_x);
	if (hot_y)
		*hot_y = *hot_y < 0 ? 0 : (*hot_y >= out->height ? out->height - 1 : *hot_y);

	/* Fill report. */
	if (report) {
		report->compliant_input = false;
		report->clipped_alpha = report_clipped_alpha;
		report->merged_colours = report_merged_colours;
		report->cropped = report_cropped;
		report->out_w = out->width;
		report->out_h = out->height;
		report->input_colours = report_input_colors;
		report->remapped_pixels = report_remapped_pixels;
	}

	return true;
}

/* PAM/PPM reading and writing. */

#define PW_MAX_DIM 4096

static void set_err(char *err, size_t len, const char *msg)
{
	if (err && len)
		snprintf(err, len, "%s", msg);
}

/* Parse a non-negative decimal into *out, rejecting junk and overflow. */
static bool parse_uint(const char *s, int *out)
{
	char *end;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno || end == s || *end != '\0' || v < 0 || v > 1000000)
		return false;
	*out = (int)v;
	return true;
}

/* Scale a sample (clamped to maxval) to 0..255. */
static uint32_t scale_sample(uint32_t v, uint32_t maxval)
{
	if (v > maxval)
		v = maxval;
	return maxval == 255 ? v : (v * 255 + maxval / 2) / maxval;
}

/* Read one header line (without the newline). Returns 0 on EOF, -1 if the
 * line is longer than len-1, 1 otherwise. */
static int read_hdr_line(FILE *f, char *line, size_t len)
{
	size_t n = 0;
	int c;
	while ((c = getc(f)) != EOF && c != '\n') {
		if (n + 1 >= len)
			return -1;
		line[n++] = (char)c;
	}
	if (c == EOF && n == 0)
		return 0;
	line[n] = '\0';
	return 1;
}

static bool read_pam_body(FILE *f, struct pw_image *img, char *err, size_t err_len)
{
	int width = 0, height = 0, depth = 0, maxval = 0;
	char tupltype[32] = "";
	bool ended = false;
	char line[256];
	int r;

	while ((r = read_hdr_line(f, line, sizeof(line))) != 0) {
		if (r < 0) {
			set_err(err, err_len, "PAM header line too long");
			return false;
		}
		char *save = NULL;
		char *key = strtok_r(line, " \t\r", &save);
		if (!key || key[0] == '#')
			continue;
		char *val = strtok_r(NULL, " \t\r", &save);
		if (strcmp(key, "ENDHDR") == 0) {
			ended = true;
			break;
		}
		if (!val) {
			set_err(err, err_len, "malformed PAM header");
			return false;
		}
		bool ok = true;
		if (strcmp(key, "WIDTH") == 0) ok = parse_uint(val, &width);
		else if (strcmp(key, "HEIGHT") == 0) ok = parse_uint(val, &height);
		else if (strcmp(key, "DEPTH") == 0) ok = parse_uint(val, &depth);
		else if (strcmp(key, "MAXVAL") == 0) ok = parse_uint(val, &maxval);
		else if (strcmp(key, "TUPLTYPE") == 0)
			snprintf(tupltype, sizeof(tupltype), "%s", val);
		if (!ok) {
			set_err(err, err_len, "invalid number in PAM header");
			return false;
		}
	}
	if (!ended) {
		set_err(err, err_len, "PAM header has no ENDHDR");
		return false;
	}
	if (width < 1 || width > PW_MAX_DIM || height < 1 || height > PW_MAX_DIM) {
		set_err(err, err_len, "image size out of range");
		return false;
	}
	if (maxval < 1 || maxval > 255) {
		set_err(err, err_len, "unsupported MAXVAL (need 1..255)");
		return false;
	}
	bool rgba = depth == 4 && strcmp(tupltype, "RGB_ALPHA") == 0;
	bool rgb = depth == 3 && (tupltype[0] == '\0' || strcmp(tupltype, "RGB") == 0);
	if (!rgba && !rgb) {
		set_err(err, err_len,
			"unsupported PAM (need DEPTH 4 RGB_ALPHA or DEPTH 3 RGB)");
		return false;
	}
	if (!pw_image_alloc(img, width, height)) {
		set_err(err, err_len, "allocation failed");
		return false;
	}
	size_t rowlen = (size_t)width * depth;
	unsigned char *row = malloc(rowlen);
	if (!row) {
		pw_image_free(img);
		set_err(err, err_len, "allocation failed");
		return false;
	}
	for (int y = 0; y < height; y++) {
		if (fread(row, 1, rowlen, f) != rowlen) {
			free(row);
			pw_image_free(img);
			set_err(err, err_len, "truncated data");
			return false;
		}
		for (int x = 0; x < width; x++) {
			const unsigned char *px = row + (size_t)x * depth;
			uint32_t rr = scale_sample(px[0], maxval);
			uint32_t gg = scale_sample(px[1], maxval);
			uint32_t bb = scale_sample(px[2], maxval);
			uint32_t aa = rgba ? scale_sample(px[3], maxval) : 0xff;
			img->pixels[(size_t)y * width + x] =
				(aa << 24) | (rr << 16) | (gg << 8) | bb;
		}
	}
	free(row);
	return true;
}

/* Read a PPM header integer, skipping whitespace and '#' comments. The one
 * terminating whitespace byte is consumed. */
static bool read_ppm_int(FILE *f, int *out)
{
	int c;
	for (;;) {
		c = getc(f);
		if (c == '#') {
			while ((c = getc(f)) != EOF && c != '\n') {}
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
		    c == '\v' || c == '\f')
			continue;
		break;
	}
	if (c < '0' || c > '9')
		return false;
	long v = 0;
	while (c >= '0' && c <= '9') {
		v = v * 10 + (c - '0');
		if (v > 1000000)
			return false;
		c = getc(f);
	}
	if (c == '#')
		ungetc(c, f);
	else if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	           c == '\v' || c == '\f'))
		return false;
	*out = (int)v;
	return true;
}

static bool read_ppm_body(FILE *f, struct pw_image *img, char *err, size_t err_len)
{
	int w, h, maxv;
	if (!read_ppm_int(f, &w) || !read_ppm_int(f, &h) || !read_ppm_int(f, &maxv)) {
		set_err(err, err_len, "invalid PPM header");
		return false;
	}
	if (w < 1 || w > PW_MAX_DIM || h < 1 || h > PW_MAX_DIM) {
		set_err(err, err_len, "image size out of range");
		return false;
	}
	if (maxv < 1 || maxv > 255) {
		set_err(err, err_len, "unsupported PPM maxval (need 1..255)");
		return false;
	}
	if (!pw_image_alloc(img, w, h)) {
		set_err(err, err_len, "allocation failed");
		return false;
	}
	size_t rowlen = (size_t)w * 3;
	unsigned char *row = malloc(rowlen);
	if (!row) {
		pw_image_free(img);
		set_err(err, err_len, "allocation failed");
		return false;
	}
	for (int y = 0; y < h; y++) {
		if (fread(row, 1, rowlen, f) != rowlen) {
			free(row);
			pw_image_free(img);
			set_err(err, err_len, "truncated PPM data");
			return false;
		}
		for (int x = 0; x < w; x++) {
			const unsigned char *px = row + (size_t)x * 3;
			img->pixels[(size_t)y * w + x] = 0xff000000 |
				(scale_sample(px[0], maxv) << 16) |
				(scale_sample(px[1], maxv) << 8) |
				scale_sample(px[2], maxv);
		}
	}
	free(row);
	return true;
}

bool pw_pam_read(const char *path, struct pw_image *img, char *err, size_t err_len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		if (err)
			snprintf(err, err_len, "open failed: %s", strerror(errno));
		return false;
	}
	bool ret = pw_pam_read_fp(f, img, err, err_len);
	fclose(f);
	return ret;
}

bool pw_pam_read_fp(FILE *f, struct pw_image *img, char *err, size_t err_len)
{
	if (!f || !img)
		return false;
	img->width = img->height = 0;
	img->pixels = NULL;

	char magic[2];
	if (fread(magic, 1, 2, f) != 2) {
		set_err(err, err_len, "read failed");
		return false;
	}
	if (magic[0] == 'P' && magic[1] == '7')
		return read_pam_body(f, img, err, err_len);
	if (magic[0] == 'P' && magic[1] == '6')
		return read_ppm_body(f, img, err, err_len);
	set_err(err, err_len, "not PAM or PPM");
	return false;
}

bool pw_pam_write(const char *path, const struct pw_image *img)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	bool ret = pw_pam_write_fp(f, img);
	fclose(f);
	return ret;
}

bool pw_pam_write_fp(FILE *f, const struct pw_image *img)
{
	if (!f || !img || !img->pixels)
		return false;

	if (fprintf(f, "P7\n") < 0) return false;
	if (fprintf(f, "WIDTH %d\n", img->width) < 0) return false;
	if (fprintf(f, "HEIGHT %d\n", img->height) < 0) return false;
	if (fprintf(f, "DEPTH 4\n") < 0) return false;
	if (fprintf(f, "MAXVAL 255\n") < 0) return false;
	if (fprintf(f, "TUPLTYPE RGB_ALPHA\n") < 0) return false;
	if (fprintf(f, "ENDHDR\n") < 0) return false;

	for (int i = 0; i < img->width * img->height; i++) {
		uint32_t pixel = img->pixels[i];
		unsigned char rgba[4] = {
			(pixel >> 16) & 0xff,
			(pixel >> 8) & 0xff,
			pixel & 0xff,
			(pixel >> 24) & 0xff,
		};
		if (fwrite(rgba, 1, 4, f) != 4)
			return false;
	}
	return true;
}

bool pw_strip_frame(const struct pw_image *strip, int frame_w, int index,
	struct pw_image *out)
{
	if (!strip || !strip->pixels || !out)
		return false;

	if (frame_w <= 0)
		frame_w = strip->height;
	if (frame_w < 1)
		return false;

	int num_frames = (strip->width + frame_w - 1) / frame_w;
	if (index < 0 || index >= num_frames)
		return false;

	int start_x = index * frame_w;
	int frame_width = (start_x + frame_w > strip->width) ?
		(strip->width - start_x) : frame_w;

	if (!pw_image_alloc(out, frame_width, strip->height))
		return false;

	for (int y = 0; y < strip->height; y++) {
		for (int x = 0; x < frame_width; x++) {
			out->pixels[y * frame_width + x] =
				strip->pixels[y * strip->width + (start_x + x)];
		}
	}
	return true;
}

int pw_strip_count(const struct pw_image *strip, int frame_w)
{
	if (!strip)
		return 0;
	if (frame_w <= 0)
		frame_w = strip->height;
	if (frame_w < 1)
		return 0;
	return (strip->width + frame_w - 1) / frame_w;
}
