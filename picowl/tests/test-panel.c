/* test-panel.c - unit tests for the wayland-free parts of picowl-panel:
 * value mapping, text, layout, touch, font, drawing and sysfs access. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "panel-draw.h"
#include "panel-logic.h"
#include "panel-sys.h"

static int test_count, fail_count;

#define CHECK(cond, msg) do { \
	test_count++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
		fail_count++; \
	} \
} while (0)

#define CHECK_EQ(got, want, msg) do { \
	long long g_ = (long long)(got), w_ = (long long)(want); \
	test_count++; \
	if (g_ != w_) { \
		fprintf(stderr, "FAIL: %s (got %lld, want %lld, line %d)\n", msg, g_, w_, __LINE__); \
		fail_count++; \
	} \
} while (0)

#define CHECK_STR(got, want, msg) do { \
	test_count++; \
	if (strcmp((got), (want)) != 0) { \
		fprintf(stderr, "FAIL: %s (got '%s', want '%s', line %d)\n", msg, got, want, __LINE__); \
		fail_count++; \
	} \
} while (0)

/* ---- value mapping ---- */

static void test_mapping(void)
{
	CHECK_EQ(pl_bl_pct_from_raw(600, 1023), 59, "600 of 1023 is 59 percent");
	CHECK_EQ(pl_bl_pct_from_raw(0, 1023), 0, "raw 0 is 0 percent");
	CHECK_EQ(pl_bl_pct_from_raw(1023, 1023), 100, "max is 100 percent");
	CHECK_EQ(pl_bl_pct_from_raw(5000, 1023), 100, "above max is 100 percent");
	CHECK_EQ(pl_bl_pct_from_raw(-4, 1023), 0, "negative raw is 0 percent");
	CHECK_EQ(pl_bl_pct_from_raw(10, 0), 0, "no max is 0 percent");

	CHECK_EQ(pl_bl_raw_from_pct(100, 1023), 1023, "100 percent is max");
	CHECK_EQ(pl_bl_raw_from_pct(50, 1023), 512, "50 percent of 1023");
	CHECK_EQ(pl_bl_raw_from_pct(20, 1023), 205, "20 percent of 1023");
	CHECK_EQ(pl_bl_raw_from_pct(0, 1023), 51, "0 percent is the 5 percent floor");
	CHECK_EQ(pl_bl_raw_from_pct(4, 1023), 51, "4 percent is the 5 percent floor");
	CHECK_EQ(pl_bl_raw_from_pct(5, 1023), 51, "5 percent is 51");
	CHECK_EQ(pl_bl_raw_from_pct(-20, 255), 13, "negative percent is the floor");
	CHECK_EQ(pl_bl_raw_from_pct(250, 255), 255, "above 100 percent is max");
	CHECK_EQ(pl_bl_raw_from_pct(0, 10), 1, "the floor of a coarse device is 1, never 0");
	CHECK_EQ(pl_bl_raw_from_pct(0, 1), 1, "an on/off device gets 1");
	CHECK_EQ(pl_bl_raw_from_pct(50, 0), 0, "no max gives 0");
	CHECK_EQ(pl_bl_clamp_pct(0), PL_BL_FLOOR_PCT, "percent clamps to the floor");
	CHECK_EQ(pl_bl_clamp_pct(300), 100, "percent clamps to 100");
	CHECK_EQ(pl_bl_clamp_pct(42), 42, "percent in range is kept");

	CHECK_EQ(pl_vol_raw_from_pct(80, 0, 40), 32, "80 percent of 0..40");
	CHECK_EQ(pl_vol_pct_from_raw(32, 0, 40), 80, "32 of 0..40 is 80 percent");
	CHECK_EQ(pl_vol_raw_from_pct(62, 0, 40), 25, "62 percent of 0..40 rounds up to 25");
	CHECK_EQ(pl_vol_raw_from_pct(1, 0, 40), 0, "1 percent of 0..40 rounds down to 0");
	CHECK_EQ(pl_vol_raw_from_pct(13, 0, 40), 5, "13 percent of 0..40 rounds to 5");
	CHECK_EQ(pl_vol_raw_from_pct(0, 0, 40), 0, "0 percent is the minimum");
	CHECK_EQ(pl_vol_raw_from_pct(100, 0, 40), 40, "100 percent is the maximum");
	CHECK_EQ(pl_vol_raw_from_pct(150, 0, 40), 40, "above 100 percent is the maximum");
	CHECK_EQ(pl_vol_raw_from_pct(-5, 0, 40), 0, "below 0 percent is the minimum");
	CHECK_EQ(pl_vol_raw_from_pct(50, 5, 65), 35, "50 percent of 5..65");
	CHECK_EQ(pl_vol_pct_from_raw(35, 5, 65), 50, "35 of 5..65 is 50 percent");
	CHECK_EQ(pl_vol_pct_from_raw(2, 5, 65), 0, "below the range is 0 percent");
	CHECK_EQ(pl_vol_pct_from_raw(99, 5, 65), 100, "above the range is 100 percent");
	CHECK_EQ(pl_vol_raw_from_pct(50, -6000, 0), -3000, "a negative range (dB steps)");
	CHECK_EQ(pl_vol_pct_from_raw(7, 7, 7), 0, "an empty range is 0 percent");
	CHECK_EQ(pl_vol_raw_from_pct(50, 7, 7), 7, "an empty range gives the minimum");
	int exact = 1;
	for (int p = 0; p <= 100; p++)
		if (pl_vol_pct_from_raw(pl_vol_raw_from_pct(p, 0, 255), 0, 255) != p)
			exact = 0;
	CHECK(exact, "percent survives the round trip through a 0..255 range");

	char b[16];
	pl_clock_text(b, sizeof(b), 7, 5);
	CHECK_STR(b, "07:05", "clock pads with zeros");
	pl_clock_text(b, sizeof(b), 23, 59);
	CHECK_STR(b, "23:59", "clock is 24 hour");
	pl_clock_text(b, sizeof(b), 0, 0);
	CHECK_STR(b, "00:00", "midnight");
	pl_clock_text(b, sizeof(b), 24, 60);
	CHECK_STR(b, "23:59", "out of range clock is clamped");
	pl_clock_text(b, sizeof(b), -1, -1);
	CHECK_STR(b, "00:00", "negative clock is clamped");
	pl_clock_text(b, 4, 12, 34);
	CHECK_STR(b, "12:", "a short buffer is cut, not overrun");

	pl_battery_text(b, sizeof(b), PL_BAT_DISCHARGING, 73);
	CHECK_STR(b, "73%", "battery percent");
	pl_battery_text(b, sizeof(b), PL_BAT_CHARGING, 5);
	CHECK_STR(b, "5%", "charging battery percent");
	pl_battery_text(b, sizeof(b), PL_BAT_FULL, 100);
	CHECK_STR(b, "100%", "full battery");
	pl_battery_text(b, sizeof(b), PL_BAT_DISCHARGING, 130);
	CHECK_STR(b, "100%", "battery percent is clamped");
	pl_battery_text(b, sizeof(b), PL_BAT_AC, -1);
	CHECK_STR(b, "AC", "mains only");
	pl_battery_text(b, sizeof(b), PL_BAT_NONE, -1);
	CHECK_STR(b, "--", "nothing");
}

/* ---- layout ---- */

static bool inside(struct pl_rect in, struct pl_rect out)
{
	return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w &&
		in.y + in.h <= out.y + out.h;
}

static void test_layout(void)
{
	struct pl_layout l;

	CHECK_EQ(pl_clamp_height(10), PL_HEIGHT_MIN, "height clamps up");
	CHECK_EQ(pl_clamp_height(500), PL_HEIGHT_MAX, "height clamps down");
	CHECK_EQ(pl_clamp_height(56), 56, "height in range");

	pl_layout_compute(&l, 240, 56, 1);
	CHECK_EQ(l.row_h, 28, "two rows of 28");
	CHECK_EQ(l.clock.x, 0, "clock at the left");
	CHECK_EQ(l.clock.w, 120, "clock half");
	CHECK_EQ(l.battery.x, 120, "battery at the right");
	CHECK_EQ(l.battery.w, 120, "battery half");
	CHECK_EQ(l.clock.h, 28, "row 1 height");
	CHECK_EQ(l.slider[0].cell.y, 28, "sliders are in row 2");
	CHECK_EQ(l.slider[0].cell.w, 120, "backlight cell is half the width");
	CHECK_EQ(l.slider[1].cell.x, 120, "volume cell at the right");
	CHECK_EQ(l.slider[1].cell.h, 28, "the touch target is the whole row cell");
	CHECK(l.slider[0].thumb_w >= 20, "thumb is at least 20 px wide");
	CHECK(l.slider[1].thumb_w >= 20, "second thumb is at least 20 px wide");
	CHECK_EQ(l.text_scale, 2, "text is drawn at twice the font size");
	for (int i = 0; i < PL_SLIDERS; i++) {
		CHECK(inside(l.slider[i].icon, l.slider[i].cell), "icon is in its cell");
		CHECK(inside(l.slider[i].track, l.slider[i].cell), "track is in its cell");
		CHECK(l.slider[i].icon.x + l.slider[i].icon.w <= l.slider[i].track.x,
			"icon and track do not overlap");
		CHECK(l.slider[i].thumb_h <= l.slider[i].cell.h, "thumb fits the row");
		CHECK(l.slider[i].track.w > l.slider[i].thumb_w, "the thumb has room to travel");
	}

	pl_layout_compute(&l, 241, 57, 1);
	CHECK_EQ(l.slider[0].cell.w + l.slider[1].cell.w, 241, "odd width: the cells cover it");
	CHECK_EQ(l.clock.w + l.battery.w, 241, "odd width: row 1 covers it");
	CHECK_EQ(l.row_h + l.slider[0].cell.h, 57, "odd height: the rows cover it");

	pl_layout_compute(&l, 320, 56, 1);
	CHECK_EQ(l.slider[1].cell.x, 160, "wider surface: volume cell moves");
	struct pl_layout l240;
	pl_layout_compute(&l240, 240, 56, 1);
	CHECK(l.slider[0].track.w > l240.slider[0].track.w, "wider surface: longer track");

	pl_layout_compute(&l, 240, 56, 2);
	CHECK_EQ(l.text_scale, 3, "scale 2 text is limited by the row height");
	CHECK(l.slider[0].thumb_w >= 40 || l.slider[0].thumb_w >= l.slider[0].track.w * 2 / 3 - 1,
		"scale 2 doubles the thumb");
	pl_layout_compute(&l, 240, 120, 1);
	CHECK_EQ(l.row_h, 60, "height 120 has rows of 60");
	CHECK(l.slider[0].icon.w <= 20, "icon is not larger than 20 at scale 1");
	pl_layout_compute(&l, 240, 56, 99);
	CHECK_EQ(l.scale, PL_SCALE_MAX, "scale is clamped");
	pl_layout_compute(&l, 1, 1, 1);
	CHECK(l.w >= 2 && l.h >= 2, "a degenerate surface does not divide by zero");

	/* Slider mapping. */
	pl_layout_compute(&l, 240, 56, 1);
	const struct pl_slider *s = &l.slider[0];
	CHECK_EQ(pl_slider_value(s, s->track.x - 50), 0, "left of the track is 0");
	CHECK_EQ(pl_slider_value(s, s->track.x), 0, "the start of the track is 0");
	CHECK_EQ(pl_slider_value(s, s->track.x + s->track.w + 50), 100, "right of the track is 100");
	CHECK_EQ(pl_slider_value(s, s->track.x + s->track.w - 1), 100, "the end of the track is 100");
	int mid = pl_slider_value(s, s->track.x + s->track.w / 2);
	CHECK(mid >= 49 && mid <= 51, "the middle of the track is 50");
	int mono = 1, close = 1, last = -1;
	for (int x = s->track.x; x < s->track.x + s->track.w; x++) {
		int v = pl_slider_value(s, x);
		if (v < last)
			mono = 0;
		last = v;
	}
	for (int p = 0; p <= 100; p++) {
		struct pl_rect th = pl_slider_thumb(s, p);
		int v = pl_slider_value(s, th.x + th.w / 2);
		if (v < p - 1 || v > p + 1)
			close = 0;
	}
	CHECK(mono, "value never decreases to the right");
	CHECK(close, "the thumb centre maps back to its value, give or take a pixel");
	struct pl_rect t0 = pl_slider_thumb(s, 0), t100 = pl_slider_thumb(s, 100);
	CHECK_EQ(t0.x, s->track.x, "thumb at 0 starts at the track");
	CHECK_EQ(t100.x + t100.w, s->track.x + s->track.w, "thumb at 100 ends at the track");
	CHECK_EQ(t0.w, s->thumb_w, "thumb width");
	CHECK(inside(t0, s->cell) && inside(t100, s->cell), "thumb stays inside the cell");
	struct pl_rect tn = pl_slider_thumb(s, -50), tp = pl_slider_thumb(s, 500);
	CHECK(tn.x == t0.x && tp.x == t100.x, "thumb percent is clamped");

	CHECK_EQ(pl_slider_at(&l, 10, 40), PL_SLIDER_BACKLIGHT, "hit: backlight icon area");
	CHECK_EQ(pl_slider_at(&l, 100, 29), PL_SLIDER_BACKLIGHT, "hit: backlight, top of the row");
	CHECK_EQ(pl_slider_at(&l, 119, 28), PL_SLIDER_BACKLIGHT, "hit: last pixel of the backlight cell");
	CHECK_EQ(pl_slider_at(&l, 120, 28), PL_SLIDER_VOLUME, "hit: first pixel of the volume cell");
	CHECK_EQ(pl_slider_at(&l, 239, 55), PL_SLIDER_VOLUME, "hit: last pixel");
	CHECK_EQ(pl_slider_at(&l, 10, 10), PL_SLIDER_NONE, "no hit in the clock row");
	CHECK_EQ(pl_slider_at(&l, 130, 27), PL_SLIDER_NONE, "no hit just above row 2");
	CHECK_EQ(pl_slider_at(&l, 240, 40), PL_SLIDER_NONE, "no hit right of the panel");
	CHECK_EQ(pl_slider_at(&l, -1, 40), PL_SLIDER_NONE, "no hit left of the panel");
	CHECK_EQ(pl_slider_at(&l, 10, 56), PL_SLIDER_NONE, "no hit below the panel");
}

/* ---- touch ---- */

static void test_touch(void)
{
	struct pl_layout l;
	struct pl_touch t = { .slider = PL_SLIDER_NONE };
	const bool both[PL_SLIDERS] = { true, true };
	const bool no_vol[PL_SLIDERS] = { true, false };
	int v = -1;

	pl_layout_compute(&l, 240, 56, 1);
	const struct pl_slider *bl = &l.slider[0], *vol = &l.slider[1];

	/* A drag across the backlight cell, out of it, and the release. */
	int x = bl->track.x + bl->track.w / 2;
	CHECK_EQ(pl_touch_press(&t, &l, both, x, 40, &v), PL_SLIDER_BACKLIGHT, "press starts a drag");
	CHECK(v >= 49 && v <= 51, "press sets the value from x");
	CHECK_EQ(pl_touch_motion(&t, &l, bl->track.x + bl->track.w - 1, &v), PL_SLIDER_BACKLIGHT,
		"motion keeps updating");
	CHECK_EQ(v, 100, "motion to the end is 100");
	CHECK_EQ(pl_touch_motion(&t, &l, vol->track.x + 20, &v), PL_SLIDER_BACKLIGHT,
		"a drag that leaves the cell stays on its slider");
	CHECK_EQ(v, 100, "and is clamped");
	CHECK_EQ(pl_touch_motion(&t, &l, 0, &v), PL_SLIDER_BACKLIGHT, "left of the cell");
	CHECK_EQ(v, PL_BL_FLOOR_PCT, "the backlight never goes below the floor");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_BACKLIGHT, "release ends the drag of the slider");
	CHECK_EQ(pl_touch_motion(&t, &l, x, &v), PL_SLIDER_NONE, "motion after the release is ignored");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_NONE, "a second release changes nothing");

	/* The track and its margins up to the cell edges are the target, not
	 * only the thin bar. */
	CHECK_EQ(pl_touch_press(&t, &l, both, x, bl->cell.y, &v), PL_SLIDER_BACKLIGHT,
		"press at the top edge of the cell");
	pl_touch_release(&t);
	CHECK_EQ(pl_touch_press(&t, &l, both, x, bl->cell.y + bl->cell.h - 1, &v),
		PL_SLIDER_BACKLIGHT, "press at the bottom edge of the cell");
	pl_touch_release(&t);
	CHECK_EQ(pl_touch_press(&t, &l, both, bl->track.x + bl->track.w - 1, 40, &v),
		PL_SLIDER_BACKLIGHT, "press at the right end of the track");
	pl_touch_release(&t);
	CHECK_EQ(pl_touch_press(&t, &l, both, bl->track.x, 40, &v), PL_SLIDER_BACKLIGHT,
		"press at the left end of the track");
	CHECK_EQ(v, PL_BL_FLOOR_PCT, "the left end is the floor");
	pl_touch_release(&t);

	/* The icon is inert: no value, no drag, also not when the stylus then
	 * moves onto the track. */
	CHECK_EQ(pl_touch_press(&t, &l, both, bl->icon.x + 2, bl->icon.y + 2, &v), PL_SLIDER_NONE,
		"press on the sun does nothing");
	CHECK_EQ(pl_touch_motion(&t, &l, x, &v), PL_SLIDER_NONE, "and starts no drag");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_NONE, "release of the ignored press");
	CHECK_EQ(pl_touch_press(&t, &l, both, bl->cell.x, 40, &v), PL_SLIDER_NONE,
		"press at the left edge of the cell (icon strip)");
	pl_touch_release(&t);
	CHECK_EQ(pl_touch_press(&t, &l, both, bl->track.x - 1, 40, &v), PL_SLIDER_NONE,
		"press just left of the track");
	pl_touch_release(&t);
	CHECK_EQ(pl_touch_press(&t, &l, both, vol->icon.x + 2, vol->icon.y + 2, &v), PL_SLIDER_NONE,
		"press on the speaker does nothing");
	pl_touch_release(&t);

	/* Volume goes down to 0 from the left end of its track. */
	CHECK_EQ(pl_touch_press(&t, &l, both, vol->track.x, 40, &v), PL_SLIDER_VOLUME,
		"press at the left end of the volume track");
	CHECK_EQ(v, 0, "volume reaches 0");
	pl_touch_release(&t);

	/* A drag that starts outside a slider is ignored all the way. */
	CHECK_EQ(pl_touch_press(&t, &l, both, 60, 10, &v), PL_SLIDER_NONE, "press in the clock row");
	CHECK_EQ(pl_touch_motion(&t, &l, bl->track.x + 5, &v), PL_SLIDER_NONE,
		"a drag from outside does nothing when it enters a slider");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_NONE, "release of an ignored drag");
	CHECK_EQ(pl_touch_press(&t, &l, both, 300, 40, &v), PL_SLIDER_NONE, "press off the panel");
	pl_touch_release(&t);

	/* A disabled slider takes no touch. */
	CHECK_EQ(pl_touch_press(&t, &l, no_vol, vol->track.x + 10, 40, &v), PL_SLIDER_NONE,
		"press on a disabled slider");
	CHECK_EQ(pl_touch_motion(&t, &l, bl->track.x + 5, &v), PL_SLIDER_NONE,
		"and its drag does not move another slider");
	pl_touch_release(&t);
	CHECK_EQ(pl_touch_press(&t, &l, no_vol, x, 40, &v), PL_SLIDER_BACKLIGHT,
		"the enabled slider still works");
	pl_touch_release(&t);

	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_NONE, "release without a press");

	CHECK_EQ(pl_apply_wait_ms(100, -1), 0, "the first write is not delayed");
	CHECK_EQ(pl_apply_wait_ms(100, 100), PL_APPLY_INTERVAL_MS, "a write right after one waits 50 ms");
	CHECK_EQ(pl_apply_wait_ms(120, 100), 30, "20 ms later 30 ms remain");
	CHECK_EQ(pl_apply_wait_ms(149, 100), 1, "49 ms later 1 ms remains");
	CHECK_EQ(pl_apply_wait_ms(150, 100), 0, "after 50 ms it is due");
	CHECK_EQ(pl_apply_wait_ms(1000, 100), 0, "long after it is due");
	CHECK_EQ(pl_apply_wait_ms(90, 100), 0, "a clock that went back does not block");
}

/* ---- font ---- */

static void test_font(void)
{
	const char *need = "0123456789:%-ACD ";
	for (const char *c = need; *c; c++) {
		const uint8_t *g = pl_font_glyph(*c);
		char msg[64];
		snprintf(msg, sizeof(msg), "font has '%c'", *c);
		CHECK(g != NULL, msg);
		if (!g)
			continue;
		int ok = 1;
		for (int i = 0; i < 7; i++)
			if (g[i] > 0x1f)
				ok = 0;
		CHECK(ok, "glyph rows use 5 bits");
	}
	CHECK(pl_font_glyph('z') == NULL, "no glyph for z");
	int distinct = 1;
	for (char a = '0'; a <= '9'; a++)
		for (char b = a + 1; b <= '9'; b++)
			if (!memcmp(pl_font_glyph(a), pl_font_glyph(b), 7))
				distinct = 0;
	CHECK(distinct, "all digits differ");
	const uint8_t one[7] = { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e };
	CHECK(!memcmp(pl_font_glyph('1'), one, 7), "the 1 looks like a 1");
	const uint8_t space[7] = { 0 };
	CHECK(!memcmp(pl_font_glyph(' '), space, 7), "space is empty");
	CHECK_EQ(pl_text_width("12:34", 2), 58, "text width at scale 2");
	CHECK_EQ(pl_text_width("12:34", 1), 29, "text width at scale 1");
	CHECK_EQ(pl_text_width("", 3), 0, "empty text has no width");
}

/* ---- drawing ---- */

#define GUARD 64

struct canvas_buf {
	uint8_t *mem;
	struct pl_canvas c;
	size_t size;
};

static void canvas_init(struct canvas_buf *b, int w, int h, bool rgb565)
{
	int bpp = rgb565 ? 2 : 4;
	b->size = (size_t)w * h * bpp;
	b->mem = malloc(b->size + 2 * GUARD);
	memset(b->mem, 0xaa, b->size + 2 * GUARD);
	b->c = (struct pl_canvas){ b->mem + GUARD, w, h, w * bpp, rgb565 };
}

static bool guards_intact(const struct canvas_buf *b)
{
	for (int i = 0; i < GUARD; i++)
		if (b->mem[i] != 0xaa || b->mem[GUARD + b->size + i] != 0xaa)
			return false;
	return true;
}

static uint32_t px(const struct canvas_buf *b, int x, int y)
{
	const uint8_t *row = b->c.data + (size_t)y * b->c.stride;
	return b->c.rgb565 ? ((const uint16_t *)row)[x] : ((const uint32_t *)row)[x];
}

static int count_color(const struct canvas_buf *b, struct pl_rect r, uint32_t rgb)
{
	uint32_t want = pl_pixel(&b->c, rgb);
	int n = 0;

	for (int y = r.y; y < r.y + r.h; y++)
		for (int x = r.x; x < r.x + r.w; x++)
			if (px(b, x, y) == want)
				n++;
	return n;
}

static void test_draw_format(bool rgb565)
{
	struct canvas_buf b;
	struct pl_layout l;
	struct pl_state st = { .hour = 12, .min = 34, .bat = PL_BAT_DISCHARGING,
		.bat_pct = 73, .bl_pct = 50, .vol_pct = -1 };
	const char *fmt = rgb565 ? "565" : "8888";
	char msg[96];

	canvas_init(&b, 240, 56, rgb565);
	pl_layout_compute(&l, 240, 56, 1);
	pl_render_all(&b.c, &l, &st);
	CHECK(guards_intact(&b), "drawing stays inside the buffer");

	snprintf(msg, sizeof(msg), "%s: background", fmt);
	CHECK_EQ(px(&b, 239, 0), pl_pixel(&b.c, PL_COL_BG), msg);

	const struct pl_slider *s = &l.slider[0];
	struct pl_rect th = pl_slider_thumb(s, 50);
	int bar_y = s->cell.y + s->cell.h / 2;
	snprintf(msg, sizeof(msg), "%s: the filled part is the strong blue", fmt);
	CHECK_EQ(px(&b, s->track.x + 2, bar_y), pl_pixel(&b.c, PL_COL_FILL), msg);
	CHECK_EQ(px(&b, th.x - 2, bar_y), pl_pixel(&b.c, PL_COL_FILL), "the fill runs up to the thumb");
	snprintf(msg, sizeof(msg), "%s: the rest of the track", fmt);
	CHECK_EQ(px(&b, s->track.x + s->track.w - 2, bar_y), pl_pixel(&b.c, PL_COL_TRACK), msg);
	CHECK_EQ(px(&b, th.x, th.y + 2), pl_pixel(&b.c, PL_COL_THUMB_EDGE), "thumb edge, left");
	CHECK_EQ(px(&b, th.x + th.w - 1, th.y + 2), pl_pixel(&b.c, PL_COL_THUMB_EDGE), "thumb edge, right");
	CHECK_EQ(px(&b, th.x + 3, th.y + 3), pl_pixel(&b.c, PL_COL_THUMB), "thumb body");
	CHECK(th.w >= 20, "the drawn thumb is at least 20 px wide");
	CHECK(px(&b, th.x - 1, th.y + 2) != pl_pixel(&b.c, PL_COL_THUMB_EDGE), "no edge left of the thumb");
	CHECK(px(&b, th.x + th.w, th.y + 2) != pl_pixel(&b.c, PL_COL_THUMB_EDGE), "no edge right of the thumb");
	CHECK(count_color(&b, s->icon, PL_COL_FG) > 20, "the sun is drawn");

	/* The disabled volume slider: no blue, no thumb, grey icon. */
	const struct pl_slider *v = &l.slider[1];
	CHECK_EQ(count_color(&b, v->cell, PL_COL_FILL), 0, "disabled slider has no fill");
	CHECK_EQ(count_color(&b, v->cell, PL_COL_THUMB), 0, "disabled slider has no thumb");
	CHECK_EQ(count_color(&b, v->cell, PL_COL_FG), 0, "disabled slider is not drawn in the text colour");
	CHECK(count_color(&b, v->icon, PL_COL_DISABLED) > 20, "disabled speaker is grey");

	st.vol_pct = 80;
	pl_render_slider(&b.c, &l, PL_SLIDER_VOLUME, st.vol_pct);
	CHECK(count_color(&b, v->cell, PL_COL_FILL) > 0, "enabled volume has a fill");
	CHECK(count_color(&b, v->icon, PL_COL_FG) > 20, "the speaker is drawn");
	CHECK_EQ(count_color(&b, v->cell, PL_COL_DISABLED), 0, "no grey left on an enabled slider");
	int full = count_color(&b, v->cell, PL_COL_FILL);
	pl_render_slider(&b.c, &l, PL_SLIDER_VOLUME, 20);
	CHECK(count_color(&b, v->cell, PL_COL_FILL) < full, "less volume, less blue");
	CHECK(guards_intact(&b), "slider redraw stays inside the buffer");

	/* Battery: the icon and its colours. */
	st.bat_pct = 73;
	pl_render_battery(&b.c, &l, &st);
	CHECK(count_color(&b, l.battery, PL_COL_BAT_OK) > 0, "a good battery is green");
	CHECK_EQ(count_color(&b, l.battery, PL_COL_BOLT), 0, "no charging marker while discharging");
	st.bat = PL_BAT_CHARGING;
	pl_render_battery(&b.c, &l, &st);
	CHECK(count_color(&b, l.battery, PL_COL_BOLT) > 5, "charging marker");
	st.bat = PL_BAT_DISCHARGING;
	st.bat_pct = 10;
	pl_render_battery(&b.c, &l, &st);
	CHECK(count_color(&b, l.battery, PL_COL_BAT_CRIT) > 0, "a nearly empty battery is red");
	CHECK_EQ(count_color(&b, l.battery, PL_COL_BAT_OK), 0, "and not green");
	st.bat_pct = 25;
	pl_render_battery(&b.c, &l, &st);
	CHECK(count_color(&b, l.battery, PL_COL_BAT_LOW) > 0, "a low battery is amber");
	st.bat = PL_BAT_AC;
	st.bat_pct = -1;
	pl_render_battery(&b.c, &l, &st);
	CHECK_EQ(count_color(&b, l.battery, PL_COL_BAT_OK) + count_color(&b, l.battery, PL_COL_BAT_LOW) +
		count_color(&b, l.battery, PL_COL_BAT_CRIT), 0, "AC has no battery icon");
	CHECK(count_color(&b, l.battery, PL_COL_FG) > 10, "AC is written");
	st.bat = PL_BAT_NONE;
	pl_render_battery(&b.c, &l, &st);
	CHECK(count_color(&b, l.battery, PL_COL_FG) > 10, "-- is written");

	/* A widget redraw touches its own rectangle only. */
	struct pl_state st2 = st;
	st2.hour = 8;
	st2.min = 5;
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 240, 56 }, 0x123456);
	pl_render_clock(&b.c, &l, &st2);
	CHECK_EQ(count_color(&b, l.battery, 0x123456), l.battery.w * l.battery.h, "clock redraw leaves the battery alone");
	CHECK_EQ(count_color(&b, l.slider[0].cell, 0x123456), l.slider[0].cell.w * l.slider[0].cell.h,
		"clock redraw leaves the sliders alone");
	CHECK(count_color(&b, l.clock, PL_COL_FG) > 30, "the clock text is drawn");
	CHECK_EQ(count_color(&b, l.clock, 0x123456), 0, "the clock redraw clears its background");
	CHECK(guards_intact(&b), "widget redraws stay inside the buffer");
	free(b.mem);
}

static void test_draw(void)
{
	struct canvas_buf b;

	canvas_init(&b, 240, 56, false);
	CHECK_EQ(pl_pixel(&b.c, PL_COL_FILL), 0x3c8ce6, "XRGB8888 keeps the colour");
	b.c.rgb565 = true;
	CHECK_EQ(pl_pixel(&b.c, PL_COL_FILL), 0x3c7c, "RGB565 of the strong blue");
	CHECK_EQ(pl_pixel(&b.c, 0xffffff), 0xffff, "RGB565 white");
	CHECK_EQ(pl_pixel(&b.c, 0x000000), 0x0000, "RGB565 black");
	free(b.mem);

	canvas_init(&b, 40, 20, false);
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000);
	pl_fill(&b.c, (struct pl_rect){ -10, -10, 100, 100 }, 0x00ff00);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0x00ff00), 800, "fill clips to the canvas");
	pl_fill(&b.c, (struct pl_rect){ 35, 15, 20, 20 }, 0x0000ff);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0x0000ff), 25, "fill clips at the far corner");
	pl_fill(&b.c, (struct pl_rect){ 50, 50, 10, 10 }, 0xff0000);
	pl_fill(&b.c, (struct pl_rect){ 5, 5, -3, 4 }, 0xff0000);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0xff0000), 0, "empty and far rectangles draw nothing");
	pl_draw_text(&b.c, -30, -3, 3, "12:34 %", 0xffffff);
	pl_draw_text(&b.c, 30, 10, 3, "88", 0xffffff);
	CHECK(guards_intact(&b), "text clips to the canvas");

	/* The 1 at scale 3: its top pixel is column 2 of row 0. */
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000);
	pl_draw_text(&b.c, 4, 2, 3, "1", 0xffffff);
	CHECK_EQ(px(&b, 4 + 2 * 3, 2), 0xffffff, "glyph pixel, top");
	CHECK_EQ(px(&b, 4 + 2 * 3 + 2, 2 + 2), 0xffffff, "a pixel is scale x scale");
	CHECK_EQ(px(&b, 4 + 2 * 3 + 3, 2), 0, "no pixel right of the stem");
	CHECK_EQ(px(&b, 4, 2), 0, "no pixel in the empty corner");
	CHECK_EQ(px(&b, 4 + 1 * 3, 2 + 1 * 3), 0xffffff, "glyph pixel, second row (the serif)");
	free(b.mem);

	test_draw_format(false);
	test_draw_format(true);
}

/* ---- sysfs ---- */

static void write_file(const char *dir, const char *name, const char *text)
{
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(2);
	}
	fputs(text, f);
	fclose(f);
}

static void mkdirs(const char *path)
{
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

static void supply(const char *root, const char *name, const char *type, const char *key,
	const char *val, const char *status)
{
	char dir[600];
	snprintf(dir, sizeof(dir), "%s/class/power_supply/%s", root, name);
	mkdirs(dir);
	write_file(dir, "type", type);
	if (key)
		write_file(dir, key, val);
	if (status)
		write_file(dir, "status", status);
}

static void test_sys(void)
{
	char base[200], root[300], dev[400], cmd[300];
	struct pl_backlight bl;
	enum pl_bat_status st;
	int pct;

	snprintf(base, sizeof(base), "/tmp/picowl-panel-test.%d", getpid());
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
	if (system(cmd) != 0)
		exit(2);

	unsetenv("PICOWL_SYSFS_ROOT");
	CHECK_STR(pl_sysfs_root(), "/sys", "sysfs root defaults to /sys");
	setenv("PICOWL_SYSFS_ROOT", "", 1);
	CHECK_STR(pl_sysfs_root(), "/sys", "an empty PICOWL_SYSFS_ROOT is ignored");
	setenv("PICOWL_SYSFS_ROOT", "/fake", 1);
	CHECK_STR(pl_sysfs_root(), "/fake", "PICOWL_SYSFS_ROOT is honoured");

	/* Backlight. */
	snprintf(root, sizeof(root), "%s/bl", base);
	mkdirs(root);
	CHECK(!pl_backlight_find(&bl, root), "no class directory: no backlight");
	snprintf(dev, sizeof(dev), "%s/class/backlight/zz", root);
	mkdirs(dev);
	write_file(dev, "max_brightness", "100\n");
	write_file(dev, "brightness", "50\n");
	snprintf(dev, sizeof(dev), "%s/class/backlight/mm", root);
	mkdirs(dev);
	write_file(dev, "max_brightness", "0\n");
	write_file(dev, "brightness", "0\n");
	snprintf(dev, sizeof(dev), "%s/class/backlight/aa", root);
	mkdirs(dev);
	write_file(dev, "max_brightness", "255\n");
	write_file(dev, "brightness", "128\n");
	CHECK(pl_backlight_find(&bl, root), "backlight found");
	CHECK_EQ(bl.max, 255, "the first device by name, aa, is taken");
	CHECK_EQ(pl_backlight_read(&bl), 128, "brightness is read");
	CHECK(pl_backlight_write(&bl, 200), "write succeeds");
	CHECK_EQ(pl_backlight_read(&bl), 200, "and is read back");
	CHECK(pl_backlight_write(&bl, 0), "writing 0 succeeds as 1");
	CHECK_EQ(pl_backlight_read(&bl), 1, "the screen is never turned off");
	CHECK(pl_backlight_write(&bl, 9999), "writing too much succeeds as max");
	CHECK_EQ(pl_backlight_read(&bl), 255, "clamped to max");
	snprintf(dev, sizeof(dev), "%s/class/backlight/aa", root);
	write_file(dev, "max_brightness", "garbage\n");
	CHECK(pl_backlight_find(&bl, root) && bl.max == 100, "a device without a valid max is skipped");
	if (getuid() != 0) {
		char path[500];
		snprintf(path, sizeof(path), "%s/class/backlight/zz/brightness", root);
		chmod(path, 0444);
		CHECK(pl_backlight_find(&bl, root), "the read-only device is found");
		CHECK(!pl_backlight_write(&bl, 10), "a read-only brightness reports failure");
	}
	/* root NULL goes through PICOWL_SYSFS_ROOT. */
	setenv("PICOWL_SYSFS_ROOT", root, 1);
	CHECK(pl_backlight_find(&bl, NULL), "root NULL uses PICOWL_SYSFS_ROOT");

	/* Battery. */
	snprintf(root, sizeof(root), "%s/ps0", base);
	mkdirs(root);
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_NONE && pct == -1, "no power_supply directory: nothing");

	supply(root, "bat0", "Battery\n", "capacity", "73\n", "Discharging\n");
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_DISCHARGING && pct == 73, "battery 73 percent, discharging");
	supply(root, "bat0", "Battery\n", "capacity", "73\n", "Charging\n");
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_CHARGING && pct == 73, "battery charging");
	supply(root, "bat0", "Battery\n", "capacity", "100\n", "Full\n");
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_FULL && pct == 100, "battery full");
	supply(root, "bat0", "Battery\n", "capacity", "80\n", "Not charging\n");
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_DISCHARGING && pct == 80, "not charging counts as discharging");
	supply(root, "bat0", "Battery\n", "capacity", "250\n", "Unknown\n");
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_DISCHARGING && pct == 100, "capacity is clamped to 100");

	/* A mains supply does not hide the battery. */
	supply(root, "ac", "Mains\n", "online", "1\n", NULL);
	supply(root, "bat0", "Battery\n", "capacity", "42\n", "Charging\n");
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_CHARGING && pct == 42, "the battery wins over AC");

	/* An input device's battery is not the system's. */
	supply(root, "bat0", "Battery\n", "scope", "Device\n", NULL);
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_AC && pct == -1, "a device-scope battery is skipped, AC is online");

	/* No battery: AC online shows AC, offline shows nothing. */
	snprintf(root, sizeof(root), "%s/ps1", base);
	supply(root, "ac", "Mains\n", "online", "1\n", NULL);
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_AC && pct == -1, "mains online: AC");
	supply(root, "ac", "Mains\n", "online", "0\n", NULL);
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_NONE, "mains offline: nothing");
	supply(root, "usb", "USB_PD\n", "online", "1\n", NULL);
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_AC, "a USB supply online counts as AC");
	supply(root, "bat0", "Battery\n", NULL, NULL, "Charging\n");
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_AC && pct == -1, "a battery without a capacity is not a battery");
	supply(root, "wl0", "Wireless\n", "online", "1\n", NULL);
	snprintf(root, sizeof(root), "%s/ps2", base);
	supply(root, "wl0", "Wireless\n", "online", "1\n", NULL);
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_NONE, "other supply types are not AC");

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
	if (system(cmd) != 0)
		exit(2);
}

int main(void)
{
	printf("test-panel: unit tests\n");
	test_mapping();
	test_layout();
	test_touch();
	test_font();
	test_draw();
	test_sys();
	printf("test-panel: %d checks, %d failed\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
