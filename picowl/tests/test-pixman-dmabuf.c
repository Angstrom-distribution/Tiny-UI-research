/*
 * Tests for pixman renderer DMA-BUF reads via mmap (patch 0001).
 */
#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>

#define W 40
#define H 30
#define NAME "picowl-test-dmabuf"
#define DW 64
#define DH 48

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* destination: plain data-ptr buffer */
struct dbuf {
	struct wlr_buffer base;
	uint16_t data[DW * DH];
};

static void dbuf_destroy(struct wlr_buffer *b) {
	(void)b;
}
static bool dbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
		uint32_t *format, size_t *stride) {
	(void)flags;
	struct dbuf *d = wl_container_of(b, d, base);
	*data = d->data;
	*format = DRM_FORMAT_RGB565;
	*stride = DW * 2;
	return true;
}
static void dbuf_end(struct wlr_buffer *b) {
	(void)b;
}
static const struct wlr_buffer_impl dbuf_impl = {
	.destroy = dbuf_destroy,
	.begin_data_ptr_access = dbuf_begin,
	.end_data_ptr_access = dbuf_end,
};

/* source: only get_dmabuf */
struct mbuf {
	struct wlr_buffer base;
	struct wlr_dmabuf_attributes attribs;
	bool destroyed;
};

static void mbuf_destroy(struct wlr_buffer *b) {
	struct mbuf *m = wl_container_of(b, m, base);
	wlr_buffer_finish(b);
	close(m->attribs.fd[0]);
	m->destroyed = true;
}
static bool mbuf_get_dmabuf(struct wlr_buffer *b, struct wlr_dmabuf_attributes *a) {
	struct mbuf *m = wl_container_of(b, m, base);
	*a = m->attribs;
	return true;
}
static const struct wlr_buffer_impl mbuf_impl = {
	.destroy = mbuf_destroy,
	.get_dmabuf = mbuf_get_dmabuf,
};

static uint16_t pattern(int x, int y) {
	return (uint16_t)((x * 2579 + y * 7919 + 0x1234) ^ (x << 5) ^ (y << 11));
}

static void mbuf_init(struct mbuf *m, uint64_t modifier, int n_planes) {
	memset(m, 0, sizeof(*m));
	int fd = memfd_create(NAME, 0);
	if (fd < 0 || ftruncate(fd, W * H * 2) < 0) {
		perror("memfd");
		exit(77);
	}
	uint16_t *p = mmap(NULL, W * H * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(77);
	}
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			p[y * W + x] = pattern(x, y);
		}
	}
	munmap(p, W * H * 2);
	m->attribs = (struct wlr_dmabuf_attributes){
		.width = W, .height = H, .format = DRM_FORMAT_RGB565,
		.modifier = modifier, .n_planes = n_planes,
		.offset = { 0, 0 }, .stride = { W * 2, W * 2 }, .fd = { fd, -1 },
	};
	wlr_buffer_init(&m->base, &mbuf_impl, W, H);
}

static int count_maps(void) {
	FILE *f = fopen("/proc/self/maps", "r");
	if (f == NULL) {
		return -1;
	}
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, NAME)) {
			n++;
		}
	}
	fclose(f);
	return n;
}

static void render(struct wlr_renderer *r, struct dbuf *d, struct wlr_texture *t) {
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(r, &d->base, NULL);
	CHECK(pass != NULL, "begin pass");
	if (pass == NULL) {
		return;
	}
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = t, .dst_box = { 3, 2, W, H },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	CHECK(wlr_render_pass_submit(pass), "submit");
}

static void check_pattern(struct dbuf *d) {
	size_t bad = 0;
	for (int y = 0; y < DH; y++) {
		for (int x = 0; x < DW; x++) {
			bool in = x >= 3 && x < 3 + W && y >= 2 && y < 2 + H;
			uint16_t want = in ? pattern(x - 3, y - 2) : 0xABCD;
			bad += d->data[y * DW + x] != want;
		}
	}
	CHECK(bad == 0, "%zu pixels differ from pattern", bad);
}

int main(void) {
	struct wlr_renderer *r = wlr_pixman_renderer_create();
	if (r == NULL) {
		fprintf(stderr, "no pixman renderer\n");
		return 1;
	}
	int base = count_maps();
	CHECK(base == 0, "baseline maps = %d", base);

	struct dbuf *d = calloc(1, sizeof(*d));
	wlr_buffer_init(&d->base, &dbuf_impl, DW, DH);
	for (int i = 0; i < DW * DH; i++) {
		d->data[i] = 0xABCD;
	}

	struct mbuf m;
	mbuf_init(&m, DRM_FORMAT_MOD_LINEAR, 1);
	struct wlr_texture *t = wlr_texture_from_buffer(r, &m.base);
	CHECK(t != NULL, "texture from linear dmabuf");
	if (t == NULL) {
		return 1;
	}
	render(r, d, t);
	check_pattern(d);
	int after1 = count_maps();
	CHECK(after1 >= 1, "no mapping seen (%d)", after1);

	for (int i = 0; i < DW * DH; i++) {
		d->data[i] = 0xABCD;
	}
	render(r, d, t);
	check_pattern(d);
	int after2 = count_maps();
	CHECK(after2 == after1, "second render changed mappings: %d -> %d", after1, after2);

	wlr_buffer_drop(&m.base);
	wlr_texture_destroy(t);
	CHECK(m.destroyed, "buffer not destroyed");
	int end = count_maps();
	CHECK(end == base, "maps not released: %d vs baseline %d", end, base);

	// INVALID modifier is accepted
	mbuf_init(&m, DRM_FORMAT_MOD_INVALID, 1);
	t = wlr_texture_from_buffer(r, &m.base);
	CHECK(t != NULL, "texture from INVALID-modifier dmabuf");
	if (t) {
		wlr_texture_destroy(t);
	}
	wlr_buffer_drop(&m.base);

	// Unsupported modifier
	mbuf_init(&m, DRM_FORMAT_MOD_LINEAR + 1, 1);
	t = wlr_texture_from_buffer(r, &m.base);
	CHECK(t == NULL, "tiled modifier gave a texture");
	if (t) {
		wlr_texture_destroy(t);
	}
	wlr_buffer_drop(&m.base);

	// Two planes
	mbuf_init(&m, DRM_FORMAT_MOD_LINEAR, 2);
	t = wlr_texture_from_buffer(r, &m.base);
	CHECK(t == NULL, "2-plane buffer gave a texture");
	if (t) {
		wlr_texture_destroy(t);
	}
	wlr_buffer_drop(&m.base);

	CHECK(count_maps() == base, "maps leaked after rejected buffers");

	free(d);
	wlr_renderer_destroy(r);
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
