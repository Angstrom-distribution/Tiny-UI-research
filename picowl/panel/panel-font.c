/* panel-font.c - text of picowl-panel: glyphs rasterized once from a TrueType
 * file with stb_truetype, or the 5x7 bitmap font when there is none. */
#include <fcntl.h>
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
		for (int g = 0; g < PL_FONT_NCHARS; g++)
			pl_mask_free(&f->face[i].g[g].m);
		memset(&f->face[i], 0, sizeof(f->face[i]));
	}
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

		fc->px = px[i];
		for (int g = 0; g < PL_FONT_NCHARS; g++) {
			int cp = PL_FONT_CHARS[g], adv, lsb, w = 0, h = 0, xo = 0, yo = 0;
			stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
			uint8_t *bm = stbtt_GetCodepointBitmap(&info, 0, scale, cp, &w, &h, &xo, &yo);
			fc->g[g].adv = (int)(adv * scale + 0.5f);
			if (bm && w > 0 && h > 0) {
				fc->g[g].m = (struct pl_mask){ w, h, bm };
				fc->g[g].xoff = xo;
				fc->g[g].yoff = yo;
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
	int x, int y, int h, const char *s, uint32_t rgb)
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
	int base = y + (h - fc->digit_h) / 2 + fc->digit_top;

	for (; *s; s++) {
		const struct pl_glyph *g = glyph_of(fc, *s);
		if (!g)
			continue;
		pl_blit(c, &g->m, x + g->xoff, base + g->yoff, rgb, 255, NULL);
		x += g->adv;
	}
}
