/* panel-font.c - text of picowl-panel: glyphs rasterized once from a TrueType
 * file with stb_truetype, or the 5x7 bitmap font when there is none. */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "panel-gfx.h"
#include "stb_truetype.h"

const char *const pl_font_search[] = {
	"/usr/share/fonts/ttf/LiberationSans-Bold.ttf",
	"/usr/share/fonts/ttf/LiberationSans-Regular.ttf",
	"/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
	"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
	NULL,
};

static void faces_free(struct pl_font *f)
{
	for (int i = 0; i < PL_FACES; i++) {
		for (int g = 0; g < PL_FONT_NCHARS; g++) {
			pl_mask_free(&f->face[i].g[g].m);
			pl_mask_free(&f->face[i].g[g].lcd);
		}
		memset(&f->face[i], 0, sizeof(f->face[i]));
	}
}

/* Rasterizes cp at three times the horizontal resolution and filters it into
 * three coverages per pixel. The pen is at the left edge of a pixel, which is
 * where the glyphs are drawn, so the subpixel phase is the same for every
 * occurrence of a glyph. */
static void rasterize_lcd(const stbtt_fontinfo *info, float sx, float sy, int cp,
	struct pl_glyph *gl)
{
	int w = 0, h = 0, xo = 0, yo = 0;
	uint8_t *bm = stbtt_GetCodepointBitmap(info, 3 * sx, sy, cp, &w, &h, &xo, &yo);

	if (!bm || w <= 0 || h <= 0) {
		free(bm);
		return;
	}
	/* The filter reaches 2 subpixels beyond the outline on both sides. */
	int first = xo - (PL_LCD_TAPS - 1) / 2, last = xo + w - 1 + (PL_LCD_TAPS - 1) / 2;
	int p0 = first >= 0 ? first / 3 : -((2 - first) / 3);
	int p1 = last >= 0 ? last / 3 : -((2 - last) / 3);
	int pw = p1 - p0 + 1;
	uint8_t *out = calloc((size_t)pw * 3 * h, 1);
	uint8_t *line = malloc((size_t)(pw * 3 + PL_LCD_TAPS));

	if (!out || !line) {
		free(out);
		free(line);
		free(bm);
		return;
	}
	for (int y = 0; y < h; y++) {
		/* The filtered row starts 2 subpixels before the bitmap. */
		pl_lcd_filter(bm + (size_t)y * w, w, line);
		uint8_t *dst = out + (size_t)y * pw * 3;
		for (int i = 0; i < w + PL_LCD_TAPS - 1; i++) {
			int s = xo - (PL_LCD_TAPS - 1) / 2 + i - 3 * p0;
			if (s >= 0 && s < pw * 3)
				dst[s] = line[i];
		}
	}
	free(line);
	free(bm);
	gl->lcd = (struct pl_mask){ pw * 3, h, out };
	gl->lcd_xoff = p0;
}

/* The font file is mapped, read and unmapped again: only the bitmaps stay. */
static bool rasterize(struct pl_font *f, const char *path, const int px[PL_FACES])
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;

	if (fd < 0)
		return false;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 64 ||
			st.st_size > (64 << 20)) {
		close(fd);
		return false;
	}
	void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return false;

	stbtt_fontinfo info;
	bool ok = false;
	int off = stbtt_GetFontOffsetForIndex(map, 0);
	if (off < 0 || !stbtt_InitFont(&info, map, off))
		goto out;
	for (int i = 0; i < PL_FACES; i++) {
		struct pl_face *fc = &f->face[i];
		float scale = stbtt_ScaleForMappingEmToPixels(&info, (float)px[i]);

		/* There is no hinting, so the height of the digits is made a whole
		 * number of pixels instead: their flat tops and bottoms then fall on
		 * pixel edges and stay sharp. The width is left alone. */
		int bx0, by0, bx1, by1;
		float sy = scale;
		if (stbtt_GetCodepointBox(&info, '1', &bx0, &by0, &bx1, &by1) && by1 > 0) {
			float hpx = by1 * scale, whole = floorf(hpx + 0.5f);
			if (whole >= 4)
				sy = scale * whole / hpx;
		}
		fc->px = px[i];
		for (int g = 0; g < PL_FONT_NCHARS; g++) {
			int cp = PL_FONT_CHARS[g], adv, lsb, w = 0, h = 0, xo = 0, yo = 0;
			stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
			uint8_t *bm = stbtt_GetCodepointBitmap(&info, scale, sy, cp, &w, &h, &xo, &yo);
			fc->g[g].adv = (int)(adv * scale + 0.5f);
			if (bm && w > 0 && h > 0) {
				fc->g[g].m = (struct pl_mask){ w, h, bm };
				fc->g[g].xoff = xo;
				fc->g[g].yoff = yo;
				rasterize_lcd(&info, scale, sy, cp, &fc->g[g]);
			} else {
				free(bm);
			}
		}
		const struct pl_glyph *zero = &fc->g[0];
		if (!zero->m.a)
			goto out;
		fc->digit_h = zero->m.h;
		fc->digit_top = -zero->yoff;
	}
	ok = true;
out:
	munmap(map, (size_t)st.st_size);
	return ok;
}

bool pl_font_load(struct pl_font *f, const char *path, int px_bar, int px_small)
{
	int px[PL_FACES] = { px_bar, px_small };

	memset(f, 0, sizeof(*f));
	for (int i = 0; i < PL_FACES; i++)
		f->scale[i] = px[i] >= 11 ? 2 : 1;
	if (path) {
		if (rasterize(f, path, px)) {
			f->ttf = true;
			snprintf(f->path, sizeof(f->path), "%s", path);
			return true;
		}
		faces_free(f);
		return false;
	}
	for (int i = 0; pl_font_search[i]; i++) {
		if (rasterize(f, pl_font_search[i], px)) {
			f->ttf = true;
			snprintf(f->path, sizeof(f->path), "%s", pl_font_search[i]);
			return true;
		}
		faces_free(f);
	}
	return false;
}

void pl_font_free(struct pl_font *f)
{
	faces_free(f);
	f->ttf = false;
}

static const struct pl_glyph *glyph_of(const struct pl_face *fc, char ch)
{
	const char *at = ch ? strchr(PL_FONT_CHARS, ch) : NULL;
	return at ? &fc->g[at - PL_FONT_CHARS] : NULL;
}

int pl_font_baseline(const struct pl_font *f, int face, int y, int h)
{
	const struct pl_face *fc = &f->face[face];

	return y + (h - fc->digit_h) / 2 + fc->digit_top;
}

int pl_font_text_w(const struct pl_font *f, int face, const char *s)
{
	if (!f->ttf)
		return pl_text_width(s, f->scale[face]);
	int w = 0;
	for (; *s; s++) {
		const struct pl_glyph *g = glyph_of(&f->face[face], *s);
		if (g)
			w += g->adv;
	}
	return w;
}

void pl_font_draw(const struct pl_canvas *c, const struct pl_font *f, int face,
	int x, int y, int h, const char *s, uint32_t rgb, enum pl_sub sub)
{
	if (!f->ttf) {
		int sc = f->scale[face], top = y + (h - 7 * sc) / 2;
		for (; *s; s++, x += 6 * sc) {
			const uint8_t *g = pl_font_glyph(*s);
			if (!g)
				continue;
			for (int row = 0; row < 7; row++)
				for (int col = 0; col < 5; col++)
					if (g[row] & (0x10 >> col))
						pl_fill(c, (struct pl_rect){ x + col * sc,
							top + row * sc, sc, sc }, rgb, 255);
		}
		return;
	}
	const struct pl_face *fc = &f->face[face];
	int base = pl_font_baseline(f, face, y, h);

	for (; *s; s++) {
		const struct pl_glyph *g = glyph_of(fc, *s);
		if (!g)
			continue;
		if (sub != PL_SUB_NONE && g->lcd.a)
			pl_blit_lcd(c, &g->lcd, x + g->lcd_xoff, base + g->yoff, rgb, sub);
		else
			pl_blit(c, &g->m, x + g->xoff, base + g->yoff, rgb, 255, NULL);
		x += g->adv;
	}
}
