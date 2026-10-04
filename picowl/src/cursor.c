/* cursor.c - hold-animation cursor: pre-rendered static wlr_buffers
 * stepped by a wl_event_loop timer. No allocation after init. */
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/util/log.h>
#include "picowl.h"
#include "cursor-builtin.h"
#include "cursorfit.h"
#include "cursorshape.h"

#define PW_MAX_FRAMES 64

struct pw_frame {
	struct wlr_buffer base;
	uint32_t *pixels;
	int stride;
};

struct pw_cursor {
	struct pw_server *server;
	struct pw_frame *frames[PW_MAX_FRAMES];
	int nframes, cur;
	int hot_x, hot_y;
	struct pw_cursor_owner own;
	struct wl_event_source *timer;
};

/* File-local: there is one cursor-shape manager per process. */
static struct {
	struct pw_server *server;
	struct wl_listener request_set_shape;
	struct wl_listener focus_change;
	bool added;
} shape;

static struct pw_frame *frame_from_buffer(struct wlr_buffer *b)
{
	return wl_container_of(b, (struct pw_frame *)NULL, base);
}

static void frame_destroy(struct wlr_buffer *b)
{
	struct pw_frame *f = frame_from_buffer(b);
	wlr_buffer_finish(b);
	free(f->pixels);
	free(f);
}

static bool frame_begin(struct wlr_buffer *b, uint32_t flags, void **data,
	uint32_t *format, size_t *stride)
{
	struct pw_frame *f = frame_from_buffer(b);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = f->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)f->stride;
	return true;
}

static void frame_end(struct wlr_buffer *b) { (void)b; }

static const struct wlr_buffer_impl frame_impl = {
	.destroy = frame_destroy,
	.begin_data_ptr_access = frame_begin,
	.end_data_ptr_access = frame_end,
};

/* Takes a copy of img's pixels. */
static struct pw_frame *frame_create(const struct pw_image *img)
{
	struct pw_frame *f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	size_t n = (size_t)img->width * img->height;
	f->pixels = malloc(n * 4);
	if (!f->pixels) {
		free(f);
		return NULL;
	}
	memcpy(f->pixels, img->pixels, n * 4);
	f->stride = img->width * 4;
	wlr_buffer_init(&f->base, &frame_impl, img->width, img->height);
	return f;
}

static void free_frames(struct pw_cursor *c)
{
	for (int i = 0; i < c->nframes; i++)
		if (c->frames[i])
			wlr_buffer_drop(&c->frames[i]->base);
	c->nframes = 0;
}

static bool add_frame(struct pw_cursor *c, const struct pw_image *img)
{
	struct pw_frame *f = frame_create(img);
	if (!f)
		return false;
	c->frames[c->nframes++] = f;
	return true;
}

static bool load_builtin(struct pw_cursor *c)
{
	const struct pw_config *cfg = c->server->config;
	struct pw_image fr[PW_BUILTIN_FRAMES];
	memset(fr, 0, sizeof(fr));
	if (!pw_cursor_builtin_frames(cfg->cursor_fill, cfg->cursor_outline, fr))
		return false;
	bool ok = true;
	for (int i = 0; i < PW_BUILTIN_FRAMES && ok; i++)
		ok = add_frame(c, &fr[i]);
	c->hot_x = PW_BUILTIN_SIZE / 2;
	c->hot_y = PW_BUILTIN_SIZE / 2;
	for (int i = 0; i < PW_BUILTIN_FRAMES; i++)
		pw_image_free(&fr[i]);
	if (!ok)
		free_frames(c);
	return ok;
}

static bool load_strip(struct pw_cursor *c, const char *path)
{
	char err[160] = "";
	struct pw_image strip = {0};
	if (!pw_pam_read(path, &strip, err, sizeof(err))) {
		pw_log(WLR_ERROR, "cursor: cannot read %s: %s", path, err);
		return false;
	}
	if (strip.height > 4 * PW_CURSOR_MAX_SIZE ||
	    strip.width > PW_MAX_FRAMES * strip.height ||
	    strip.width % strip.height != 0) {
		pw_log(WLR_ERROR, "cursor: %s: bad strip geometry %dx%d (need "
			"square frames, <= %d frames, height <= %d)", path,
			strip.width, strip.height, PW_MAX_FRAMES, 4 * PW_CURSOR_MAX_SIZE);
		pw_image_free(&strip);
		return false;
	}
	int n = pw_strip_count(&strip, 0);
	if (n < 1) {
		pw_log(WLR_ERROR, "cursor: %s: no frames", path);
		pw_image_free(&strip);
		return false;
	}
	if (n > PW_MAX_FRAMES)
		n = PW_MAX_FRAMES;
	int fw = strip.height;
	bool ok = true;
	for (int i = 0; i < n && ok; i++) {
		struct pw_image raw = {0}, fit = {0};
		if (!pw_strip_frame(&strip, 0, i, &raw)) {
			ok = false;
			break;
		}
		char why[160] = "";
		bool compliant = pw_cursorfit_check(&raw, why, sizeof(why));
		if (compliant) {
			ok = add_frame(c, &raw);
		} else {
			struct pw_cursorfit_opts o = { .max_size = PW_CURSOR_MAX_SIZE };
			struct pw_cursorfit_report r;
			int hx = raw.width / 2, hy = raw.height / 2;
			if (pw_cursorfit_fit(&raw, &fit, &hx, &hy, &o, &r)) {
				pw_log(WLR_ERROR, "cursor: warning: %s frame %d non-compliant (%s); "
					"fixed: alpha=%d colours=%d (merged %d) remapped=%d%s", path, i,
					why, r.clipped_alpha, r.input_colours, r.merged_colours,
					r.remapped_pixels, r.cropped ? " cropped" : "");
				ok = add_frame(c, &fit);
			} else {
				ok = false;
			}
		}
		pw_image_free(&raw);
		pw_image_free(&fit);
	}
	/* hotspot = centre of the (possibly cropped) frame */
	if (ok) {
		int w = fw > PW_CURSOR_MAX_SIZE ? PW_CURSOR_MAX_SIZE : fw;
		int h = strip.height > PW_CURSOR_MAX_SIZE ? PW_CURSOR_MAX_SIZE : strip.height;
		c->hot_x = w / 2;
		c->hot_y = h / 2;
	} else {
		free_frames(c);
	}
	pw_image_free(&strip);
	return ok;
}

static int timer_cb(void *data)
{
	struct pw_cursor *c = data;
	if (!c->own.running)
		return 0;
	c->cur = (c->cur + 1) % c->nframes;
	wlr_cursor_set_buffer(c->server->cursor, &c->frames[c->cur]->base,
		c->hot_x, c->hot_y, 1.0f);
	int ms = c->server->config->cursor_frame_ms;
	wl_event_source_timer_update(c->timer, ms > 0 ? ms : 83);
	return 0;
}

static void anim_show(struct pw_server *server, int x, int y)
{
	struct pw_cursor *c = server->hold_cursor;
	if ((int)server->cursor->x != x || (int)server->cursor->y != y)
		wlr_cursor_warp_closest(server->cursor, NULL, x, y);
	c->cur = 0;
	wlr_cursor_set_buffer(server->cursor, &c->frames[0]->base,
		c->hot_x, c->hot_y, 1.0f);
	int ms = server->config->cursor_frame_ms;
	wl_event_source_timer_update(c->timer, ms > 0 ? ms : 83);
}

static void anim_hide(struct pw_server *server)
{
	struct pw_cursor *c = server->hold_cursor;
	wl_event_source_timer_update(c->timer, 0);
	wlr_cursor_unset_image(server->cursor);
}

/* Only the client holding the pointer focus may change the cursor, as for
 * wl_pointer.set_cursor; wlroots passes the request on without checking. */
static bool shape_request_allowed(struct pw_server *server,
	const struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev)
{
	if (!ev->seat_client)
		return false;
	switch (ev->device_type) {
	case WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER:
		return ev->seat_client == server->seat->pointer_state.focused_client;
	case WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_TABLET_TOOL:
		return false; /* no tablet support yet, so no such device exists */
	}
	return false;
}

static void shape_handle_request(struct wl_listener *l, void *data)
{
	(void)l;
	const struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	struct pw_server *server = shape.server;
	struct pw_cursor *c = server->hold_cursor;
	if (!c || !shape_request_allowed(server, ev))
		return;

	switch (pw_owner_client_shape(&c->own, pw_cursor_shape_look(ev->shape))) {
	case PW_SHAPE_ACT_START:
		anim_show(server, (int)server->cursor->x, (int)server->cursor->y);
		break;
	case PW_SHAPE_ACT_STOP:
		anim_hide(server);
		break;
	case PW_SHAPE_ACT_NONE:
		break;
	}
}

/* A busy cursor belongs to the client that asked for it. */
static void shape_handle_focus_change(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
	struct pw_cursor *c = shape.server->hold_cursor;
	if (c && pw_owner_focus_change(&c->own))
		anim_hide(shape.server);
}

static void shape_init(struct pw_server *server)
{
	if (shape.added)
		return;
	struct wlr_cursor_shape_manager_v1 *mgr =
		wlr_cursor_shape_manager_v1_create(server->display, 1);
	if (!mgr) {
		pw_log(WLR_ERROR, "cursor: cannot create the cursor-shape manager");
		return;
	}
	shape.server = server;
	shape.request_set_shape.notify = shape_handle_request;
	wl_signal_add(&mgr->events.request_set_shape, &shape.request_set_shape);
	shape.focus_change.notify = shape_handle_focus_change;
	wl_signal_add(&server->seat->pointer_state.events.focus_change, &shape.focus_change);
	shape.added = true;
}

void pw_cursor_init(struct pw_server *server)
{
	shape_init(server);
	if (server->hold_cursor)
		return;
	struct pw_cursor *c = calloc(1, sizeof(*c));
	if (!c) {
		pw_log(WLR_ERROR, "cursor: out of memory");
		return;
	}
	c->server = server;
	const char *path = server->config->hold_animation;
	bool ok = false;
	if (path && *path && strcmp(path, "builtin") != 0) {
		ok = load_strip(c, path);
		if (!ok)
			pw_log(WLR_ERROR, "cursor: falling back to builtin animation");
	}
	if (!ok)
		ok = load_builtin(c);
	if (!ok) {
		pw_log(WLR_ERROR, "cursor: no animation available");
		free(c);
		return;
	}
	c->timer = wl_event_loop_add_timer(server->event_loop, timer_cb, c);
	if (!c->timer) {
		free_frames(c);
		free(c);
		return;
	}
	server->hold_cursor = c;
}

void pw_cursor_hold_start(struct pw_server *server, int x, int y)
{
	struct pw_cursor *c = server->hold_cursor;
	if (!c || !pw_owner_touch_start(&c->own))
		return;
	anim_show(server, x, y);
}

void pw_cursor_hold_stop(struct pw_server *server)
{
	struct pw_cursor *c = server->hold_cursor;
	if (!c || !pw_owner_stop(&c->own))
		return;
	anim_hide(server);
}

void pw_cursor_finish(struct pw_server *server)
{
	/* wlroots asserts at display destroy that nobody listens any more. */
	if (shape.added) {
		wl_list_remove(&shape.request_set_shape.link);
		wl_list_remove(&shape.focus_change.link);
		shape.added = false;
	}
	struct pw_cursor *c = server->hold_cursor;
	if (!c)
		return;
	pw_cursor_hold_stop(server);
	wl_event_source_remove(c->timer);
	free_frames(c);
	free(c);
	server->hold_cursor = NULL;
}
