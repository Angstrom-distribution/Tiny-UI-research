/*
 * Tests for pixman renderer DMA-BUF reads via mmap (patch 0001).
 *
 * The renderer only maps real DMA-BUFs, memfds stand in for them when the
 * WLR_PIXMAN_DMABUF_ALLOW_ANY_FD escape is set. A real DMA-BUF is used too if
 * /dev/dma_heap/system exists.
 */
#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
#define ESCAPE "WLR_PIXMAN_DMABUF_ALLOW_ANY_FD"

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

static void fill_pattern(int fd, size_t len) {
	uint16_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(77);
	}
	for (size_t i = 0; i < len / 2; i++) {
		p[i] = pattern(i % W, i / W);
	}
	munmap(p, len);
}

static void mbuf_from_fd(struct mbuf *m, int fd, uint64_t modifier, int n_planes,
		uint32_t stride, uint32_t offset) {
	memset(m, 0, sizeof(*m));
	m->attribs = (struct wlr_dmabuf_attributes){
		.width = W, .height = H, .format = DRM_FORMAT_RGB565,
		.modifier = modifier, .n_planes = n_planes,
		.offset = { offset, 0 }, .stride = { stride, W * 2 }, .fd = { fd, -1 },
	};
	wlr_buffer_init(&m->base, &mbuf_impl, W, H);
}

/* a memfd of fd_size bytes, the pattern in the first W * H * 2 */
static void mbuf_init_ex(struct mbuf *m, uint64_t modifier, int n_planes,
		uint32_t stride, uint32_t offset, off_t fd_size) {
	int fd = memfd_create(NAME, 0);
	if (fd < 0 || ftruncate(fd, fd_size) < 0) {
		perror("memfd");
		exit(77);
	}
	fill_pattern(fd, fd_size);
	mbuf_from_fd(m, fd, modifier, n_planes, stride, offset);
}

static void mbuf_init(struct mbuf *m, uint64_t modifier, int n_planes) {
	mbuf_init_ex(m, modifier, n_planes, W * 2, 0, W * H * 2);
}

/* a real DMA-BUF from the system heap, false if there is none */
static bool mbuf_init_heap(struct mbuf *m) {
	int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	if (heap < 0) {
		return false;
	}
	struct dma_heap_allocation_data a = {
		.len = W * H * 2, .fd_flags = O_RDWR | O_CLOEXEC,
	};
	int ret = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a);
	close(heap);
	if (ret < 0) {
		return false;
	}
	fill_pattern(a.fd, W * H * 2);
	mbuf_from_fd(m, a.fd, DRM_FORMAT_MOD_LINEAR, 1, W * 2, 0);
	return true;
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

static struct wlr_renderer *new_renderer(bool allow_any_fd) {
	if (allow_any_fd) {
		setenv(ESCAPE, "1", 1);
	} else {
		unsetenv(ESCAPE);
	}
	struct wlr_renderer *r = wlr_pixman_renderer_create();
	if (r == NULL) {
		fprintf(stderr, "no pixman renderer\n");
		exit(1);
	}
	return r;
}

/* the texture is created from the buffer, which is never mapped on failure */
static void check_rejected(struct wlr_renderer *r, struct mbuf *m, const char *what) {
	struct wlr_texture *t = wlr_texture_from_buffer(r, &m->base);
	CHECK(t == NULL, "%s gave a texture", what);
	if (t) {
		wlr_texture_destroy(t);
	}
	wlr_buffer_drop(&m->base);
}

int main(void) {
	int base = count_maps();
	CHECK(base == 0, "baseline maps = %d", base);
	struct mbuf m;

	/* by default only real DMA-BUFs are mapped: a client could truncate a
	 * memfd it passed as a DMA-BUF and SIGBUS the compositor */
	struct wlr_renderer *r = new_renderer(false);
	mbuf_init(&m, DRM_FORMAT_MOD_LINEAR, 1);
	check_rejected(r, &m, "memfd");
	CHECK(count_maps() == base, "memfd was mapped");

	if (mbuf_init_heap(&m)) {
		struct wlr_texture *t = wlr_texture_from_buffer(r, &m.base);
		CHECK(t != NULL, "no texture from a real DMA-BUF");
		if (t) {
			wlr_texture_destroy(t);
		}
		wlr_buffer_drop(&m.base);
	} else {
		printf("skip real DMA-BUF: no /dev/dma_heap/system\n");
	}
	wlr_renderer_destroy(r);

	r = new_renderer(true);

	struct dbuf *d = calloc(1, sizeof(*d));
	wlr_buffer_init(&d->base, &dbuf_impl, DW, DH);
	for (int i = 0; i < DW * DH; i++) {
		d->data[i] = 0xABCD;
	}

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

	// Bad layouts are rejected before anything is mapped: a stride below
	// the row size reads past the end of the mapping
	mbuf_init_ex(&m, DRM_FORMAT_MOD_LINEAR, 1, W * 2 - 4, 0, W * H * 2);
	check_rejected(r, &m, "stride below the row size");
	mbuf_init_ex(&m, DRM_FORMAT_MOD_LINEAR, 1, W * 2 + 2, 0, W * H * 4);
	check_rejected(r, &m, "stride not a multiple of 4");
	mbuf_init_ex(&m, DRM_FORMAT_MOD_LINEAR, 1, W * 2, 2, W * H * 2 + 16);
	check_rejected(r, &m, "offset not a multiple of 4");
	mbuf_init_ex(&m, DRM_FORMAT_MOD_LINEAR, 1, UINT32_MAX - 3, UINT32_MAX - 3, W * H * 2);
	check_rejected(r, &m, "huge stride and offset");
	// fd smaller than offset + stride * height
	mbuf_init_ex(&m, DRM_FORMAT_MOD_LINEAR, 1, W * 2, 0, W * H * 2 - 4);
	check_rejected(r, &m, "fd smaller than the buffer");
	mbuf_init_ex(&m, DRM_FORMAT_MOD_LINEAR, 1, W * 2, 4, W * H * 2);
	check_rejected(r, &m, "offset past the end of the fd");

	// stride larger than the row, offset: accepted, rows at offset + y * stride
	{
		const int stride = W * 2 + 8, offset = 4;
		mbuf_init_ex(&m, DRM_FORMAT_MOD_LINEAR, 1, stride, offset, offset + stride * H);
		uint8_t *p = mmap(NULL, offset + stride * H, PROT_READ | PROT_WRITE,
			MAP_SHARED, m.attribs.fd[0], 0);
		CHECK(p != MAP_FAILED, "mmap");
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				*(uint16_t *)(p + offset + y * stride + x * 2) = pattern(x, y);
			}
		}
		munmap(p, offset + stride * H);
		t = wlr_texture_from_buffer(r, &m.base);
		CHECK(t != NULL, "texture from padded dmabuf");
		if (t) {
			for (int i = 0; i < DW * DH; i++) {
				d->data[i] = 0xABCD;
			}
			render(r, d, t);
			check_pattern(d);
			wlr_texture_destroy(t);
		}
		wlr_buffer_drop(&m.base);
	}

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
