/*
 * Tests for the pixman render pass fast paths (patch 0002): memcpy texture
 * copies, pixman_fill rects, and absence of per-frame allocations.
 */
#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/box.h>

/* ---- allocation counting via interposition ---- */

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

static volatile int counting;
static unsigned long n_malloc, n_calloc, n_realloc;

void *malloc(size_t n) {
	if (counting) {
		n_malloc++;
	}
	return __libc_malloc(n);
}

void *calloc(size_t a, size_t b) {
	if (counting) {
		n_calloc++;
	}
	return __libc_calloc(a, b);
}

void *realloc(void *p, size_t n) {
	if (counting) {
		n_realloc++;
	}
	return __libc_realloc(p, n);
}

void free(void *p) {
	__libc_free(p);
}

static unsigned long alloc_calls(void) {
	return n_malloc + n_calloc + n_realloc;
}

static void counters_reset(void) {
	n_malloc = n_calloc = n_realloc = 0;
}

/* ---- test buffers ---- */

struct tbuf {
	struct wlr_buffer base;
	uint16_t *data;
	size_t stride; // bytes
};

static void tbuf_destroy(struct wlr_buffer *b) {
	struct tbuf *t = wl_container_of(b, t, base);
	wlr_buffer_finish(b);
	free(t->data);
	free(t);
}

static bool tbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
		uint32_t *format, size_t *stride) {
	(void)flags;
	struct tbuf *t = wl_container_of(b, t, base);
	*data = t->data;
	*format = DRM_FORMAT_RGB565;
	*stride = t->stride;
	return true;
}

static void tbuf_end(struct wlr_buffer *b) {
	(void)b;
}

static const struct wlr_buffer_impl tbuf_impl = {
	.destroy = tbuf_destroy,
	.begin_data_ptr_access = tbuf_begin,
	.end_data_ptr_access = tbuf_end,
};

static struct tbuf *tbuf_create(int w, int h) {
	struct tbuf *t = calloc(1, sizeof(*t));
	t->stride = (size_t)w * 2;
	t->data = calloc((size_t)w * h, 2);
	wlr_buffer_init(&t->base, &tbuf_impl, w, h);
	return t;
}

static uint32_t rng_state = 0x12345678;
static uint32_t rng(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define DW 100
#define DH 80
#define SW 64
#define SH 48

static pixman_region32_t make_clip(void) {
	pixman_region32_t clip;
	pixman_region32_init_rect(&clip, 12, 9, 20, 15);
	pixman_region32_union_rect(&clip, &clip, 40, 20, 30, 10);
	// partly outside the destination and the dst_box
	pixman_region32_union_rect(&clip, &clip, 80, 60, 40, 40);
	return clip;
}

static size_t count_diff(const uint16_t *a, const uint16_t *b) {
	size_t d = 0;
	for (int i = 0; i < DW * DH; i++) {
		if (a[i] != b[i]) {
			d++;
		}
	}
	return d;
}

static struct tbuf *fill_dst(void) {
	struct tbuf *d = tbuf_create(DW, DH);
	for (int i = 0; i < DW * DH; i++) {
		d->data[i] = 0xABCD;
	}
	return d;
}

static void render_texture(struct wlr_renderer *r, struct tbuf *dst,
		struct wlr_texture *tex, const struct wlr_render_texture_options *opts) {
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(r, &dst->base, NULL);
	CHECK(pass != NULL, "begin_buffer_pass");
	if (pass == NULL) {
		return;
	}
	struct wlr_render_texture_options o = *opts;
	o.texture = tex;
	wlr_render_pass_add_texture(pass, &o);
	CHECK(wlr_render_pass_submit(pass), "submit");
}

// Plain C reference for the unscaled copy
static void ref_copy(uint16_t *dst, const uint16_t *src, const struct wlr_box *srcb,
		const struct wlr_box *dstb, pixman_region32_t *clip) {
	for (int y = 0; y < DH; y++) {
		for (int x = 0; x < DW; x++) {
			if (!pixman_region32_contains_point(clip, x, y, NULL)) {
				continue;
			}
			if (x < dstb->x || y < dstb->y || x >= dstb->x + dstb->width ||
					y >= dstb->y + dstb->height) {
				continue;
			}
			int sx = x - dstb->x + srcb->x, sy = y - dstb->y + srcb->y;
			dst[y * DW + x] = src[sy * SW + sx];
		}
	}
}

static void test_copy(struct wlr_renderer *r, struct tbuf *src, bool crop) {
	struct wlr_texture *tex = wlr_texture_from_buffer(r, &src->base);
	CHECK(tex != NULL, "texture_from_buffer");
	if (tex == NULL) {
		return;
	}
	struct tbuf *dst = fill_dst();
	uint16_t *ref = malloc(DW * DH * 2);
	memcpy(ref, dst->data, DW * DH * 2);
	pixman_region32_t clip = make_clip();

	struct wlr_box srcb = { 0, 0, SW, SH };
	struct wlr_box dstb = { 10, 7, SW, SH };
	if (crop) {
		srcb = (struct wlr_box){ 5, 3, 40, 30 };
		dstb = (struct wlr_box){ 10, 7, 40, 30 };
	}
	struct wlr_render_texture_options o = {
		.src_box = { srcb.x, srcb.y, srcb.width, srcb.height },
		.dst_box = dstb,
		.clip = &clip,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	render_texture(r, dst, tex, &o);
	ref_copy(ref, src->data, &srcb, &dstb, &clip);
	CHECK(count_diff(dst->data, ref) == 0, "%s copy mismatch: %zu pixels",
		crop ? "cropped" : "plain", count_diff(dst->data, ref));

	pixman_region32_fini(&clip);
	free(ref);
	wlr_buffer_drop(&dst->base);
	wlr_texture_destroy(tex);
}

// Independent pixman reference for fallback paths
static void ref_composite(uint16_t *dst, const uint16_t *src, pixman_op_t op,
		double alpha, bool scaled, pixman_region32_t *clip) {
	pixman_image_t *s = pixman_image_create_bits_no_clear(PIXMAN_r5g6b5, SW, SH,
		(uint32_t *)(void *)src, SW * 2);
	pixman_image_t *d = pixman_image_create_bits_no_clear(PIXMAN_r5g6b5, DW, DH,
		(uint32_t *)(void *)dst, DW * 2);
	pixman_image_t *mask = NULL;
	if (alpha != 1) {
		mask = pixman_image_create_solid_fill(
			&(pixman_color_t){ .alpha = 0xFFFF * alpha });
	}
	int w = SW, h = SH;
	if (scaled) {
		w = 50;
		h = 40;
		pixman_transform_t t;
		pixman_transform_init_identity(&t);
		pixman_transform_scale(&t, NULL, pixman_double_to_fixed(SW / (double)w),
			pixman_double_to_fixed(SH / (double)h));
		pixman_image_set_transform(s, &t);
		pixman_image_set_filter(s, PIXMAN_FILTER_BILINEAR, NULL, 0);
	}
	pixman_image_set_clip_region32(d, clip);
	pixman_image_composite32(op, s, mask, d, 0, 0, 0, 0, 10, 7, w, h);
	if (mask) {
		pixman_image_unref(mask);
	}
	pixman_image_unref(s);
	pixman_image_unref(d);
}

static void test_fallback(struct wlr_renderer *r, struct tbuf *src, const char *name,
		float alpha, enum wlr_render_blend_mode blend, bool scaled) {
	struct wlr_texture *tex = wlr_texture_from_buffer(r, &src->base);
	struct tbuf *dst = fill_dst();
	uint16_t *ref = malloc(DW * DH * 2);
	memcpy(ref, dst->data, DW * DH * 2);
	pixman_region32_t clip = make_clip();

	struct wlr_render_texture_options o = {
		.dst_box = { 10, 7, scaled ? 50 : SW, scaled ? 40 : SH },
		.clip = &clip,
		.alpha = &alpha,
		.blend_mode = blend,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
	};
	render_texture(r, dst, tex, &o);
	ref_composite(ref, src->data,
		blend == WLR_RENDER_BLEND_MODE_NONE ? PIXMAN_OP_SRC : PIXMAN_OP_OVER,
		alpha, scaled, &clip);
	size_t d = count_diff(dst->data, ref);
	CHECK(d == 0, "%s: %zu pixels differ from pixman reference", name, d);

	pixman_region32_fini(&clip);
	free(ref);
	wlr_buffer_drop(&dst->base);
	wlr_texture_destroy(tex);
}

static void test_rects(struct wlr_renderer *r) {
	struct tbuf *dst = fill_dst();
	uint16_t *ref = malloc(DW * DH * 2);
	memcpy(ref, dst->data, DW * DH * 2);
	pixman_region32_t clip = make_clip();
	struct wlr_box box = { 8, 5, 70, 60 };

	pixman_image_t *rimg = pixman_image_create_bits_no_clear(PIXMAN_r5g6b5, DW, DH,
		(uint32_t *)(void *)ref, DW * 2);

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(r, &dst->base, NULL);
	CHECK(pass != NULL, "begin pass");
	if (pass == NULL) {
		return;
	}

	// Opaque
	struct wlr_render_rect_options ro = {
		.box = box,
		.color = { 0.8f, 0.3f, 0.1f, 1.0f },
		.clip = &clip,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_rect(pass, &ro);
	pixman_color_t c = { .red = 0.8f * 0xFFFF, .green = 0.3f * 0xFFFF,
		.blue = 0.1f * 0xFFFF, .alpha = 0xFFFF };
	pixman_region32_t area;
	pixman_region32_init_rect(&area, box.x, box.y, box.width, box.height);
	pixman_region32_intersect(&area, &area, &clip);
	int n;
	pixman_box32_t *bx = pixman_region32_rectangles(&area, &n);
	pixman_rectangle16_t rects[8];
	CHECK(n <= 8, "too many rects");
	for (int i = 0; i < n && i < 8; i++) {
		rects[i] = (pixman_rectangle16_t){ bx[i].x1, bx[i].y1,
			bx[i].x2 - bx[i].x1, bx[i].y2 - bx[i].y1 };
	}
	pixman_image_fill_rectangles(PIXMAN_OP_SRC, rimg, &c, n, rects);

	// Translucent
	struct wlr_render_rect_options rt = {
		.box = { 20, 12, 60, 50 },
		.color = { 0.0f, 0.4f, 0.4f, 0.5f },
		.clip = &clip,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_rect(pass, &rt);
	CHECK(wlr_render_pass_submit(pass), "submit");

	pixman_color_t c2 = { .red = 0, .green = 0.4f * 0xFFFF, .blue = 0.4f * 0xFFFF,
		.alpha = 0.5f * 0xFFFF };
	pixman_image_t *fill = pixman_image_create_solid_fill(&c2);
	pixman_image_set_clip_region32(rimg, &clip);
	pixman_image_composite32(PIXMAN_OP_OVER, fill, NULL, rimg, 0, 0, 0, 0,
		20, 12, 60, 50);
	pixman_image_unref(fill);

	size_t d = count_diff(dst->data, ref);
	CHECK(d == 0, "rect mismatch: %zu pixels", d);

	pixman_image_unref(rimg);
	pixman_region32_fini(&area);
	pixman_region32_fini(&clip);
	free(ref);
	wlr_buffer_drop(&dst->base);
}

static void test_allocs(struct wlr_renderer *r, struct tbuf *src) {
	struct wlr_texture *tex = wlr_texture_from_buffer(r, &src->base);
	struct tbuf *dst = fill_dst();
	pixman_region32_t clip = make_clip();

	// Sanity check: interposition works
	counters_reset();
	counting = 1;
	void *p = malloc(16);
	counting = 0;
	CHECK(alloc_calls() == 1, "interposed malloc not counted (%lu)", alloc_calls());
	free(p);

	struct wlr_render_rect_options ro = {
		.box = { 0, 0, 30, 30 }, .color = { 1, 0, 0, 1 }, .clip = &clip,
	};
	struct wlr_render_texture_options to = {
		.texture = tex, .dst_box = { 10, 7, SW, SH }, .clip = &clip,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};

	for (int phase = 0; phase < 2; phase++) {
		unsigned long limit = phase == 0 ? 0 : 2000;
		for (int i = 0; i < 50; i++) {
			struct wlr_render_pass *p2 = wlr_renderer_begin_buffer_pass(r, &dst->base, NULL);
			if (phase == 0) {
				for (int k = 0; k < 20; k++) {
					ro.box.x = k;
					wlr_render_pass_add_rect(p2, &ro);
				}
			} else {
				wlr_render_pass_add_texture(p2, &to);
			}
			wlr_render_pass_submit(p2);
		}
		counters_reset();
		counting = 1;
		for (int i = 0; i < 2000; i++) {
			struct wlr_render_pass *p2 = wlr_renderer_begin_buffer_pass(r, &dst->base, NULL);
			if (p2 == NULL) {
				break;
			}
			if (phase == 0) {
				for (int k = 0; k < 20; k++) {
					ro.box.x = k;
					wlr_render_pass_add_rect(p2, &ro);
				}
			} else {
				wlr_render_pass_add_texture(p2, &to);
			}
			wlr_render_pass_submit(p2);
		}
		counting = 0;
		CHECK(alloc_calls() <= limit,
			"%s loop: malloc=%lu calloc=%lu realloc=%lu (limit %lu)",
			phase == 0 ? "rect" : "texture", n_malloc, n_calloc, n_realloc, limit);
		printf("%s loop: malloc=%lu calloc=%lu realloc=%lu\n",
			phase == 0 ? "rect" : "texture", n_malloc, n_calloc, n_realloc);
	}

	pixman_region32_fini(&clip);
	wlr_buffer_drop(&dst->base);
	wlr_texture_destroy(tex);
}

int main(void) {
	struct wlr_renderer *r = wlr_pixman_renderer_create();
	if (r == NULL) {
		fprintf(stderr, "no pixman renderer\n");
		return 1;
	}

	struct tbuf *src = tbuf_create(SW, SH);
	for (int i = 0; i < SW * SH; i++) {
		src->data[i] = rng() & 0xFFFF;
	}

	test_copy(r, src, false);
	test_copy(r, src, true);
	test_fallback(r, src, "alpha 0.5", 0.5f, WLR_RENDER_BLEND_MODE_PREMULTIPLIED, false);
	test_fallback(r, src, "premultiplied", 1.0f, WLR_RENDER_BLEND_MODE_PREMULTIPLIED, false);
	test_fallback(r, src, "scaled", 1.0f, WLR_RENDER_BLEND_MODE_NONE, true);
	test_fallback(r, src, "scaled alpha", 0.5f, WLR_RENDER_BLEND_MODE_PREMULTIPLIED, true);
	test_rects(r);
	test_allocs(r, src);

	wlr_buffer_drop(&src->base);
	wlr_renderer_destroy(r);
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
