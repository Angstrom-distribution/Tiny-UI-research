/*
 * gen-dejavu-bitmaps.c - generates panel-pixfont-dejavu.c, the DejaVu Sans
 * bitmaps of picowl-panel's crisp style, with FreeType. It runs offline, on the
 * developer's machine: the panel itself has no FreeType in it.
 *
 *     gen-dejavu-bitmaps OUT.c          write the C data
 *     gen-dejavu-bitmaps --check OUT.c  fail if OUT.c is not what it would write
 *
 * Reproducible output needs the same hinter and the same font files, so run it
 * in a debian:trixie container (arm64 or amd64, the bitmaps do not depend on
 * the architecture), from this directory:
 *
 *     apt-get install -y gcc pkg-config libfreetype-dev fonts-dejavu-core
 *     gcc -O2 -Wall -o gen gen-dejavu-bitmaps.c $(pkg-config --cflags --libs freetype2)
 *     ./gen ../panel-pixfont-dejavu.c        (or ./gen --check ../panel-pixfont-dejavu.c)
 *
 * Why these settings: the TrueType v35 interpreter with FT_LOAD_TARGET_MONO
 * and no auto-hinter runs the font's own bytecode and gives bi-level glyphs
 * with whole-pixel stems, which is how a Verdana-like font was meant to be
 * shown at 9 to 12 px without anti-aliasing. The v40 interpreter ignores the
 * horizontal hinting, and the auto-hinter does not know the font's intent;
 * both give a less even result in mono.
 *
 * Each font is a table of cells for 0x20..0x7e as panel-pixfont.h says. A cell
 * spans from the leftmost ink (or the pen) to the rightmost ink or advance, with
 * `ox` columns left of the pen, so that a glyph with a negative left bearing
 * stays whole. The advance is the hinted integer advance: text is proportional
 * and every glyph keeps its own bearing. No kerning.
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRST 0x20
#define LAST 0x7e
#define NCH (LAST - FIRST + 1)

#define FONT_DIR "/usr/share/fonts/truetype/dejavu/"

struct spec {
	const char *id, *file;
	int px;
};

/* The bar's face is the bold 11. The text rows step down a ladder of the same
 * weight (the fit logic takes the largest that fits): bold 12, then the bar's 11,
 * 10 and 9. Bold because 2 px stems stay legible as light text on the dark
 * theme, where Regular 12 has 1 px stems and looks thin. To try Regular, add
 * "DejaVuSans.ttf" entries (ids dv_reg_12 and so on) and point rows_dejavu in
 * panel-font.c at them. */
static const struct spec specs[] = {
	{ "dv_bold_11", "DejaVuSans-Bold.ttf", 11 },
	{ "dv_bold_12", "DejaVuSans-Bold.ttf", 12 },
	{ "dv_bold_10", "DejaVuSans-Bold.ttf", 10 },
	{ "dv_bold_9", "DejaVuSans-Bold.ttf", 9 },
};
#define NSPEC ((int)(sizeof(specs) / sizeof(specs[0])))

struct glyph {
	int adv, left, top, w, h;	/* left and top are bitmap_left and bitmap_top */
	unsigned char *bits;		/* h rows of w bytes, 0 or 1 */
};

struct cell {
	int w, h, ascent, bpr, ox;
};

static void die(const char *what, int err)
{
	fprintf(stderr, "gen-dejavu-bitmaps: %s (FreeType error %d)\n", what, err);
	exit(1);
}

static char *out;
static size_t out_len, out_cap;

static void put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void put(const char *fmt, ...)
{
	va_list ap;
	char tmp[512];
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)sizeof(tmp)) {
		fprintf(stderr, "gen-dejavu-bitmaps: line too long\n");
		exit(1);
	}
	if (out_len + (size_t)n + 1 > out_cap) {
		out_cap = (out_len + (size_t)n + 1) * 2;
		out = realloc(out, out_cap);
		if (!out)
			exit(1);
	}
	memcpy(out + out_len, tmp, (size_t)n + 1);
	out_len += (size_t)n;
}

static struct cell emit(FT_Library lib, const struct spec *s)
{
	char path[256];
	FT_Face face;
	struct glyph g[NCH];
	int lo = 0, hi = 0, top = 0, below = 0, err;
	struct cell cl;

	snprintf(path, sizeof(path), FONT_DIR "%s", s->file);
	if ((err = FT_New_Face(lib, path, 0, &face)))
		die(path, err);
	if ((err = FT_Set_Pixel_Sizes(face, 0, (FT_UInt)s->px)))
		die("FT_Set_Pixel_Sizes", err);

	for (int c = FIRST; c <= LAST; c++) {
		struct glyph *q = &g[c - FIRST];
		FT_GlyphSlot sl = face->glyph;

		memset(q, 0, sizeof(*q));
		if ((err = FT_Load_Char(face, (FT_ULong)c,
				FT_LOAD_TARGET_MONO | FT_LOAD_NO_AUTOHINT | FT_LOAD_RENDER)))
			die("FT_Load_Char", err);
		if (sl->bitmap.rows && sl->bitmap.pixel_mode != FT_PIXEL_MODE_MONO) {
			fprintf(stderr, "gen-dejavu-bitmaps: not a 1-bit bitmap\n");
			exit(1);
		}
		if (sl->advance.x & 63) {
			fprintf(stderr, "gen-dejavu-bitmaps: fractional advance of '%c'\n", c);
			exit(1);
		}
		q->adv = (int)(sl->advance.x >> 6);
		q->left = sl->bitmap_left;
		q->top = sl->bitmap_top;
		q->w = (int)sl->bitmap.width;
		q->h = (int)sl->bitmap.rows;
		q->bits = calloc((size_t)(q->w * q->h + 1), 1);
		for (int r = 0; r < q->h; r++)
			for (int x = 0; x < q->w; x++)
				q->bits[r * q->w + x] =
					(sl->bitmap.buffer[r * sl->bitmap.pitch + x / 8] >> (7 - x % 8)) & 1;
		if (q->w) {
			if (q->left < lo)
				lo = q->left;
			if (q->left + q->w > hi)
				hi = q->left + q->w;
			if (q->top > top)
				top = q->top;
			if (q->h - q->top > below)
				below = q->h - q->top;
		}
		if (q->adv > hi)
			hi = q->adv;
	}
	cl = (struct cell){ hi - lo, top + below, top, (hi - lo + 7) / 8, -lo };
	if (cl.w > 16) {
		fprintf(stderr, "gen-dejavu-bitmaps: %s has a cell wider than 16 pixels\n", s->id);
		exit(1);
	}

	put("static const uint8_t rows_%s[%d * %d * %d] = {\n", s->id, NCH, cl.h, cl.bpr);
	for (int c = FIRST; c <= LAST; c++) {
		struct glyph *q = &g[c - FIRST];

		put("\t");
		for (int r = 0; r < cl.h; r++) {
			unsigned v = 0;
			int gr = r - (top - q->top);

			if (q->w && gr >= 0 && gr < q->h)
				for (int x = 0; x < q->w; x++)
					if (q->bits[gr * q->w + x])
						v |= 0x8000u >> (q->left + cl.ox + x);
			if (cl.bpr == 1)
				put("0x%02x,%s", v >> 8, r + 1 < cl.h ? " " : "");
			else
				put("0x%02x, 0x%02x,%s", v >> 8, v & 0xff, r + 1 < cl.h ? " " : "");
		}
		put(" /* %s%c%s */\n", c == '\'' ? "\"" : "'", c, c == '\'' ? "\"" : "'");
	}
	put("};\n");
	put("static const uint8_t adv_%s[%d] = {", s->id, NCH);
	for (int c = FIRST; c <= LAST; c++)
		put("%s%d,", (c - FIRST) % 16 ? " " : "\n\t", g[c - FIRST].adv);
	put("\n};\n\n");
	for (int c = FIRST; c <= LAST; c++)
		free(g[c - FIRST].bits);
	FT_Done_Face(face);
	return cl;
}

int main(int argc, char **argv)
{
	FT_Library lib;
	FT_UInt interp = 35;
	struct cell cells[NSPEC];
	int check = argc == 3 && !strcmp(argv[1], "--check"), err;
	const char *dst = argv[argc - 1];

	if (argc != 2 && !check) {
		fprintf(stderr, "usage: %s [--check] OUT.c\n", argv[0]);
		return 2;
	}
	if ((err = FT_Init_FreeType(&lib)))
		die("FT_Init_FreeType", err);
	if ((err = FT_Property_Set(lib, "truetype", "interpreter-version", &interp)))
		die("the v35 TrueType interpreter is not available", err);

	put("/* Generated by panel/fonts/gen-dejavu-bitmaps.c; do not edit.\n"
	    " *\n"
	    " * These are 1-bit renderings of DejaVu Sans and DejaVu Sans Bold (Debian\n"
	    " * fonts-dejavu-core), hinted by FreeType's v35 TrueType interpreter. The fonts\n"
	    " * derive from Bitstream Vera: their copyright, trademark and permission notice\n"
	    " * is in panel/fonts/DEJAVU-LICENSE and goes with this file. The data is not\n"
	    " * distributed as a font and not under the Bitstream or Vera names; see\n"
	    " * panel/fonts/LICENSES. */\n"
	    "#include \"panel-pixfont.h\"\n\n");
	for (int i = 0; i < NSPEC; i++)
		cells[i] = emit(lib, &specs[i]);
	for (int i = 0; i < NSPEC; i++)
		put("const struct pl_pixfont pl_pixfont_%s = { \"DejaVuSans %s %d\", %d, %d, %d, %d, rows_%s, adv_%s, %d };\n",
			specs[i].id, strstr(specs[i].file, "Bold") ? "Bold" : "Regular", specs[i].px,
			cells[i].w, cells[i].h, cells[i].ascent, cells[i].bpr, specs[i].id, specs[i].id,
			cells[i].ox);
	FT_Done_FreeType(lib);

	if (check) {
		FILE *f = fopen(dst, "rb");
		char *have = malloc(out_len + 2);
		size_t n = f && have ? fread(have, 1, out_len + 1, f) : 0;

		if (!f || n != out_len || memcmp(have, out, out_len)) {
			fprintf(stderr, "gen-dejavu-bitmaps: %s is not what the generator writes\n", dst);
			return 1;
		}
		fclose(f);
		free(have);
		printf("gen-dejavu-bitmaps: %s is up to date\n", dst);
		return 0;
	}
	FILE *f = fopen(dst, "wb");

	if (!f || fwrite(out, 1, out_len, f) != out_len || fclose(f)) {
		fprintf(stderr, "gen-dejavu-bitmaps: cannot write %s\n", dst);
		return 1;
	}
	return 0;
}
