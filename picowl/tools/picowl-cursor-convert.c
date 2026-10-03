/*
 * picowl-cursor-convert - convert cursor images to platform-compliant format.
 *
 * Usage:
 *   picowl-cursor-convert [--check] [--frame-width N] [--fg #rrggbb] [--bg #rrggbb]
 *                         [--premultiplied] [--hotspot X,Y] in.pam|ppm out.pam
 *   picowl-cursor-convert --check [--frame-width N] in.pam|ppm
 *   picowl-cursor-convert --export-builtin [--fg #rrggbb] [--bg #rrggbb] out.pam
 *
 * Exit codes:
 *   0 - success
 *   1 - input non-compliant in --check mode
 *   2 - usage error or I/O failure
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "cursorfit.h"
#include "cursor-builtin.h"

struct options {
	bool check;
	bool export_builtin;
	int frame_width;
	bool premultiplied;
	int hotspot_x, hotspot_y;
	bool has_hotspot;
	uint32_t fg, bg;
	bool force_colours;
	const char *input_path;
	const char *output_path;
};

static void usage(FILE *f)
{
	fprintf(f,
	"usage: picowl-cursor-convert [--check] [--frame-width N] [--fg #rrggbb]\n"
	"                             [--bg #rrggbb] [--premultiplied] [--hotspot X,Y]\n"
	"                             in.pam|ppm out.pam\n"
	"       picowl-cursor-convert --check [--frame-width N] in.pam|ppm\n"
	"       picowl-cursor-convert --export-builtin [--fg #rrggbb] [--bg #rrggbb]\n"
	"                             out.pam\n");
}

/* Parse a hex colour string like "#rrggbb" into 0xrrggbb.
 * Returns true on success, false on invalid format. */
static bool parse_hex_colour(const char *str, uint32_t *colour)
{
	if (!str || str[0] != '#' || strlen(str) != 7)
		return false;
	for (int i = 1; i < 7; i++) {
		if (!isxdigit((unsigned char)str[i]))
			return false;
	}
	*colour = strtoul(str + 1, NULL, 16);
	return true;
}

/* Parse command-line arguments into options. Returns true on success. */
static bool parse_args(int argc, char *argv[], struct options *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->frame_width = 0;  /* 0 means frame_width = height */
	opts->hotspot_x = 0;
	opts->hotspot_y = 0;
	opts->has_hotspot = false;
	opts->fg = 0x2050c0;  /* default fill colour */
	opts->bg = 0xffffff;  /* default outline colour */
	opts->force_colours = false;

	int i = 1;
	for (; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			return false;  /* trigger usage message */
		} else if (strcmp(arg, "--check") == 0) {
			opts->check = true;
		} else if (strcmp(arg, "--export-builtin") == 0) {
			opts->export_builtin = true;
		} else if (strcmp(arg, "--frame-width") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "error: --frame-width requires an argument\n");
				return false;
			}
			opts->frame_width = atoi(argv[++i]);
		} else if (strcmp(arg, "--fg") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "error: --fg requires a colour argument\n");
				return false;
			}
			if (!parse_hex_colour(argv[++i], &opts->fg)) {
				fprintf(stderr, "error: invalid fg colour format (use #rrggbb)\n");
				return false;
			}
			opts->force_colours = true;
		} else if (strcmp(arg, "--bg") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "error: --bg requires a colour argument\n");
				return false;
			}
			if (!parse_hex_colour(argv[++i], &opts->bg)) {
				fprintf(stderr, "error: invalid bg colour format (use #rrggbb)\n");
				return false;
			}
			opts->force_colours = true;
		} else if (strcmp(arg, "--premultiplied") == 0) {
			opts->premultiplied = true;
		} else if (strcmp(arg, "--hotspot") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "error: --hotspot requires an argument\n");
				return false;
			}
			const char *hs = argv[++i];
			if (sscanf(hs, "%d,%d", &opts->hotspot_x, &opts->hotspot_y) != 2) {
				fprintf(stderr, "error: invalid hotspot format (use X,Y)\n");
				return false;
			}
			opts->has_hotspot = true;
		} else if (arg[0] == '-') {
			fprintf(stderr, "error: unknown option: %s\n", arg);
			return false;
		} else {
			break;  /* non-option argument, should be input path */
		}
	}

	if (opts->export_builtin) {
		if (i != argc - 1) {
			fprintf(stderr, "error: --export-builtin expects exactly one output file\n");
			return false;
		}
		opts->output_path = argv[i];
		opts->input_path = NULL;
	} else {
		if (opts->check) {
			if (i + 1 != argc) {
				fprintf(stderr, "error: --check expects exactly one input file\n");
				return false;
			}
			opts->input_path = argv[i];
			opts->output_path = NULL;
		} else {
			if (i + 2 != argc) {
				fprintf(stderr, "error: expected input.pam output.pam\n");
				return false;
			}
			opts->input_path = argv[i];
			opts->output_path = argv[i + 1];
		}
	}

	return true;
}

/* Print a per-frame report. */
static void print_report(int frame_idx, const struct pw_cursorfit_report *report)
{
	if (!report)
		return;

	printf("frame %d: %dx%d", frame_idx, report->out_w, report->out_h);

	if (report->compliant_input) {
		printf(" (already compliant)\n");
		return;
	}

	if (report->clipped_alpha > 0)
		printf(" alpha_clipped=%d", report->clipped_alpha);
	if (report->merged_colours > 0)
		printf(" colours_merged=%d", report->merged_colours);
	if (report->cropped)
		printf(" cropped_to_%dx%d", report->out_w, report->out_h);
	if (report->remapped_pixels > 0)
		printf(" remapped_pixels=%d", report->remapped_pixels);
	if (report->input_colours > 0)
		printf(" input_colours=%d", report->input_colours);

	printf("\n");
}

/* Convert and write a single frame, processing it through cursorfit. */
static bool convert_frame(const struct pw_image *in_frame,
	struct pw_image *out_frame, const struct options *opts,
	int frame_idx, struct pw_cursorfit_report *report)
{
	struct pw_cursorfit_opts fit_opts = {
		.premultiplied = opts->premultiplied,
		.max_size = PW_CURSOR_MAX_SIZE,
		.force_colours = opts->force_colours,
		.fg = opts->fg,
		.bg = opts->bg,
	};

	int hot_x = opts->has_hotspot ? opts->hotspot_x : 0;
	int hot_y = opts->has_hotspot ? opts->hotspot_y : 0;

	if (!pw_cursorfit_fit(in_frame, out_frame, &hot_x, &hot_y, &fit_opts, report)) {
		fprintf(stderr, "error: failed to fit frame %d\n", frame_idx);
		return false;
	}

	print_report(frame_idx, report);
	return true;
}

/* --export-builtin mode: generate builtin animation and save it. */
static int export_builtin(const struct options *opts)
{
	struct pw_image frames[PW_BUILTIN_FRAMES];

	if (!pw_cursor_builtin_frames(opts->fg, opts->bg, frames)) {
		fprintf(stderr, "error: failed to generate builtin frames\n");
		return 2;
	}

	/* Combine all frames into a horizontal strip. */
	struct pw_image strip;
	int strip_width = PW_BUILTIN_SIZE * PW_BUILTIN_FRAMES;
	int strip_height = PW_BUILTIN_SIZE;

	if (!pw_image_alloc(&strip, strip_width, strip_height)) {
		fprintf(stderr, "error: failed to allocate strip image\n");
		for (int i = 0; i < PW_BUILTIN_FRAMES; i++)
			pw_image_free(&frames[i]);
		return 2;
	}

	/* Copy each frame into the strip horizontally. */
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++) {
		uint32_t *src = frames[i].pixels;
		uint32_t *dst = strip.pixels + i * PW_BUILTIN_SIZE;
		for (int y = 0; y < PW_BUILTIN_SIZE; y++) {
			memcpy(dst + y * strip_width, src + y * PW_BUILTIN_SIZE,
				PW_BUILTIN_SIZE * sizeof(uint32_t));
		}
	}

	/* Write the strip to the output file. */
	if (!pw_pam_write(opts->output_path, &strip)) {
		fprintf(stderr, "error: failed to write %s\n", opts->output_path);
		pw_image_free(&strip);
		for (int i = 0; i < PW_BUILTIN_FRAMES; i++)
			pw_image_free(&frames[i]);
		return 2;
	}

	printf("exported builtin animation: %d frames, %dx%d\n",
		PW_BUILTIN_FRAMES, strip_width, strip_height);

	pw_image_free(&strip);
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++)
		pw_image_free(&frames[i]);

	return 0;
}

/* Standard mode: read input, process each frame, write output. */
static int convert_input(const struct options *opts)
{
	struct pw_image strip;
	char err_msg[256];

	/* Read the input image (which may be a strip of multiple frames). */
	if (!pw_pam_read(opts->input_path, &strip, err_msg, sizeof(err_msg))) {
		fprintf(stderr, "error: failed to read %s: %s\n", opts->input_path, err_msg);
		return 2;
	}

	/* Determine the number of frames in the strip. */
	int num_frames = pw_strip_count(&strip, opts->frame_width);
	if (num_frames <= 0) {
		fprintf(stderr, "error: invalid frame count\n");
		pw_image_free(&strip);
		return 2;
	}

	int frame_width = opts->frame_width > 0 ? opts->frame_width : strip.height;
	if (frame_width > strip.width) {
		fprintf(stderr, "error: frame width exceeds image width\n");
		pw_image_free(&strip);
		return 2;
	}

	if (strip.width % frame_width != 0) {
		fprintf(stderr, "error: strip width %d is not a multiple of frame width %d\n",
			strip.width, frame_width);
		pw_image_free(&strip);
		return 2;
	}

	/* Output strip is allocated once frame 0 has been fitted (its size
	 * depends on cropping). */
	struct pw_image out_strip = {0};
	int cell = 0;

	bool all_compliant = true;
	int failed_frame = -1;

	/* Process each frame. */
	for (int i = 0; i < num_frames; i++) {
		struct pw_image in_frame, out_frame;
		if (!pw_strip_frame(&strip, frame_width, i, &in_frame)) {
			fprintf(stderr, "error: failed to extract frame %d\n", i);
			pw_image_free(&strip);
			pw_image_free(&out_strip);
			return 2;
		}

		struct pw_cursorfit_report report;
		memset(&report, 0, sizeof(report));

		if (opts->check) {
			/* In check mode, just validate without fitting. */
			char why[160] = "";
			if (!pw_cursorfit_check(&in_frame, why, sizeof(why))) {
				all_compliant = false;
				if (failed_frame < 0)
					failed_frame = i;
				printf("frame %d: non-compliant: %s\n", i, why);
			} else {
				printf("frame %d: compliant\n", i);
			}
			pw_image_free(&in_frame);
		} else {
			/* Process frame through cursorfit. */
			if (!convert_frame(&in_frame, &out_frame, opts, i, &report)) {
				pw_image_free(&in_frame);
				pw_image_free(&strip);
				pw_image_free(&out_strip);
				return 2;
			}

			if (i == 0) {
				/* Square cells (picowl slices width = height). */
				cell = out_frame.width > out_frame.height ?
					out_frame.width : out_frame.height;
				if (!pw_image_alloc(&out_strip, cell * num_frames, cell)) {
					fprintf(stderr, "error: failed to allocate output strip\n");
					pw_image_free(&in_frame);
					pw_image_free(&out_frame);
					pw_image_free(&strip);
					return 2;
				}
			}
			if (out_frame.height > cell ||
			    out_frame.width > cell) {
				fprintf(stderr, "error: frame %d size differs from frame 0\n", i);
				pw_image_free(&in_frame);
				pw_image_free(&out_frame);
				pw_image_free(&strip);
				pw_image_free(&out_strip);
				return 2;
			}

			/* Copy the fitted frame, centred, into its cell. */
			int off_x = (cell - out_frame.width) / 2;
			int off_y = (cell - out_frame.height) / 2;
			for (int y = 0; y < out_frame.height; y++) {
				uint32_t *src = out_frame.pixels + y * out_frame.width;
				uint32_t *dst = out_strip.pixels +
					(size_t)(y + off_y) * out_strip.width + i * cell + off_x;
				memcpy(dst, src, out_frame.width * sizeof(uint32_t));
			}

			pw_image_free(&in_frame);
			pw_image_free(&out_frame);
		}
	}

	pw_image_free(&strip);

	if (opts->check) {
		pw_image_free(&out_strip);
		return all_compliant ? 0 : 1;
	}

	/* Write output. */
	if (!pw_pam_write(opts->output_path, &out_strip)) {
		fprintf(stderr, "error: failed to write %s\n", opts->output_path);
		pw_image_free(&out_strip);
		return 2;
	}

	pw_image_free(&out_strip);
	printf("wrote %s: %d frames\n", opts->output_path, num_frames);
	return 0;
}

int main(int argc, char *argv[])
{
	struct options opts;

	if (!parse_args(argc, argv, &opts)) {
		usage(stderr);
		return 2;
	}

	if (opts.export_builtin) {
		return export_builtin(&opts);
	} else {
		return convert_input(&opts);
	}
}
