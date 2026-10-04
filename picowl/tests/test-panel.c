/* test-panel.c - unit tests for the wayland-free parts of picowl-panel:
 * value mapping, text, layout, touch, font, drawing and sysfs access. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "panel-draw.h"
#include "panel-gfx.h"
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
	pl_pct_text(b, sizeof(b), 60);
	CHECK_STR(b, "60%", "slider value");
	pl_pct_text(b, sizeof(b), 400);
	CHECK_STR(b, "100%", "slider value is clamped");
	pl_pct_text(b, sizeof(b), -3);
	CHECK_STR(b, "0%", "slider value is clamped below");
}

/* ---- layout ---- */

static bool inside(struct pl_rect in, struct pl_rect out)
{
	return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w &&
		in.y + in.h <= out.y + out.h;
}

static bool overlap(struct pl_rect a, struct pl_rect b)
{
	return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* What the 12 px Liberation Sans Bold gives, near enough. */
static const struct pl_metrics M15 = { 33, 27, 24 };

static void test_layout(void)
{
	struct pl_layout l, o;

	CHECK_EQ(pl_clamp_height(10), PL_HEIGHT_MIN, "height clamps up to 18");
	CHECK_EQ(PL_HEIGHT_MIN, 18, "the smallest bar is 18");
	CHECK_EQ(pl_clamp_height(500), PL_HEIGHT_MAX, "height clamps down to 80");
	CHECK_EQ(PL_HEIGHT_MAX, 80, "the largest bar is 80");
	CHECK_EQ(pl_clamp_height(28), 28, "height in range");
	CHECK_EQ(PL_HEIGHT_DEFAULT, 20, "the bar is 20 px by default");
	CHECK_EQ(pl_clamp_alpha(-5), 0, "alpha clamps up");
	CHECK_EQ(pl_clamp_alpha(900), 255, "alpha clamps down");
	CHECK_EQ(pl_default_font_px(20), 12, "12 px text on the 20 px bar");
	CHECK(pl_default_font_px(80) > 12 && pl_default_font_px(80) <= 40, "bigger bar, bigger text");
	CHECK(pl_default_font_px(18) < 12 && pl_default_font_px(18) >= 8, "smaller bar, smaller text");

	CHECK_EQ(pl_row_height(20), 36, "the row is 36 px under the 20 px bar");
	CHECK_EQ(pl_row_height(80), 88, "a tall bar gets a taller row");
	CHECK_EQ(pl_surface_height(20, false), 20, "closed: the bar");
	CHECK_EQ(pl_surface_height(20, true), 56, "open: the bar and the row");
	CHECK_EQ(pl_surface_height(5, true), 18 + 36, "the height is clamped first");

	pl_layout_compute(&l, 240, 20, false, false, &M15);
	pl_layout_compute(&o, 240, 20, true, false, &M15);
	CHECK_EQ(l.h, 20, "closed surface is the bar");
	CHECK_EQ(o.h, 56, "open surface is the bar and the row");
	CHECK(!l.row_shown && o.row_shown, "row_shown follows the argument");
	CHECK_EQ(o.row.y, 20, "the row is right under the bar");
	CHECK_EQ(o.row.h, 36, "row height");
	CHECK_EQ(o.row.w, 240, "the row is as wide as the surface");
	CHECK_EQ(l.bar_h, 20, "bar height");
	CHECK(memcmp(l.button, o.button, sizeof(l.button)) == 0,
		"opening the row does not move the buttons");
	CHECK(memcmp(&l.clock, &o.clock, sizeof(l.clock)) == 0 &&
		memcmp(&l.battery, &o.battery, sizeof(l.battery)) == 0,
		"nor the clock and the battery");

	struct pl_rect bar = { 0, 0, 240, 20 };
	CHECK_EQ(l.clock.x, 0, "the clock rectangle starts at the edge");
	CHECK(l.clock.x + l.clock.w >= PL_MARGIN + M15.clock_w, "the clock has room for its text");
	CHECK_EQ(l.clock.h, 19, "drawing rectangles stop above the line");
	CHECK(inside(l.clock, bar) && inside(l.battery, bar), "clock and battery are in the bar");
	CHECK_EQ(l.battery.x + l.battery.w, 240, "the battery rectangle reaches the right edge");
	CHECK(l.battery.w >= PL_MARGIN + l.bat_icon_w + PL_GAP + M15.bat_text_w,
		"the battery has room for the icon and '100%'");
	CHECK(l.bat_icon_w >= 16 && l.bat_icon_h * 2 <= l.bat_icon_w + 1 && l.bat_icon_h <= 12, "the battery icon is wide and fits the slim bar");
	CHECK(l.bar_icon >= 12 && l.bar_icon <= 16, "the button icons are about 14 px");
	for (int i = 0; i < PL_SLIDERS; i++) {
		CHECK(l.button[i].w >= 36, "a button is at least 36 px wide");
		CHECK_EQ(l.button[i].h, 20, "a button is the whole bar height");
		CHECK_EQ(l.button[i].y, 0, "a button starts at the top");
		CHECK(inside(l.button[i], bar), "a button is in the bar");
		CHECK(inside(l.hl[i], l.button[i]), "the highlight is inside its button");
		CHECK(l.hl[i].w >= l.bar_icon && l.hl[i].h >= l.bar_icon, "the highlight is larger than the icon");
		CHECK(!overlap(l.button[i], l.clock) && !overlap(l.button[i], l.battery),
			"a button touches neither clock nor battery");
	}
	CHECK(!overlap(l.button[0], l.button[1]), "the two buttons do not overlap");
	CHECK(l.button[0].x < l.button[1].x, "backlight is left of volume");
	CHECK(l.button[1].x + l.button[1].w <= l.battery.x, "buttons are left of the battery");

	/* The row. */
	CHECK(inside(o.row_icon, o.row), "the row icon is in the row");
	CHECK(o.row_icon.w >= 20, "the row icon is large");
	CHECK(o.slider.thumb_d >= 22, "the thumb is at least 22 px");
	CHECK(o.slider.thumb_d <= o.row_h - 4, "the thumb fits the row");
	CHECK(o.slider.track_h >= 4 && o.slider.track_h < o.slider.thumb_d, "a slim track under a big thumb");
	CHECK(o.slider.track.w > 2 * o.slider.thumb_d, "the thumb has room to travel");
	CHECK(inside(o.slider.track, o.row), "the track is in the row");
	CHECK(inside(o.slider.cell, o.row), "the touch area is in the row");
	CHECK(o.slider.cell.x >= o.row_icon.x + o.row_icon.w, "the touch area does not cover the icon");
	CHECK(o.slider.cell.x + o.slider.cell.w <= o.pct.x, "nor the value text");
	CHECK_EQ(o.slider.cell.h, o.row_h, "the touch area is the whole height of the row");
	CHECK(o.slider.cell.x <= o.slider.track.x - 4 && o.slider.cell.x + o.slider.cell.w >=
		o.slider.track.x + o.slider.track.w + 4, "the touch area is wider than the track");
	CHECK_EQ(o.pct.x + o.pct.w, 240, "the value text rectangle reaches the right edge");
	CHECK(o.pct.w >= M15.pct_w + PL_MARGIN, "and has room for '100%'");

	/* The input region is the bar and, if shown, the row; never more than the
	 * surface, and never the whole of a surface that is larger. */
	struct pl_rect in = pl_input_rect(&l, 240, 20);
	CHECK(in.x == 0 && in.y == 0 && in.w == 240 && in.h == 20, "closed: the input region is the bar");
	in = pl_input_rect(&o, 240, 56);
	CHECK(in.x == 0 && in.y == 0 && in.w == 240 && in.h == 56, "open: the bar and the row");
	in = pl_input_rect(&o, 240, 300);
	CHECK_EQ(in.h, 56, "a larger surface does not take touches below the row");
	in = pl_input_rect(&l, 240, 56);
	CHECK_EQ(in.h, 20, "a row on its way out does not take touches");
	in = pl_input_rect(&o, 200, 40);
	CHECK(in.w == 200 && in.h == 40, "a smaller surface limits it");

	struct pl_rect op[2];
	CHECK_EQ(pl_opaque_rects(&l, 255, 224, op), 1, "closed, opaque bar: one rectangle");
	CHECK(op[0].h == 20 && op[0].w == 240, "that is the bar");
	CHECK_EQ(pl_opaque_rects(&o, 255, 224, op), 1, "open, translucent row: only the bar is opaque");
	CHECK_EQ(op[0].h, 20, "the bar");
	CHECK_EQ(pl_opaque_rects(&o, 255, 255, op), 2, "open, opaque row: the bar and the row");
	CHECK(op[1].y == 20 && op[1].h == 36, "the second is the row");
	CHECK_EQ(pl_opaque_rects(&o, 200, 255, op), 1, "translucent bar, opaque row: the row");
	CHECK_EQ(op[0].y, 20, "the row");
	CHECK_EQ(pl_opaque_rects(&o, 200, 200, op), 0, "all translucent: nothing is opaque");
	CHECK_EQ(pl_opaque_rects(&l, 254, 255, op), 0, "a bar at 254 is not opaque");

	CHECK_EQ(pl_pick_format(255, 255, true, true), PL_FMT_RGB565, "opaque: RGB565 where offered");
	CHECK_EQ(pl_pick_format(255, 255, false, false), PL_FMT_XRGB8888, "opaque without RGB565: XRGB8888");
	CHECK_EQ(pl_pick_format(255, 224, true, true), PL_FMT_ARGB8888, "open translucent row: ARGB8888");
	CHECK_EQ(pl_pick_format(255, 224, false, true), PL_FMT_RGB565,
		"closed with an opaque bar: the translucent row is not on the surface, RGB565");
	CHECK_EQ(pl_pick_format(250, 255, false, true), PL_FMT_ARGB8888, "translucent bar: ARGB8888");
	CHECK_EQ(pl_pick_format(255, 224, true, false), PL_FMT_ARGB8888, "ARGB8888 whatever is offered");

	/* Other sizes keep everything inside. */
	static const int widths[] = { 240, 320, 480, 640, 176 };
	static const int bars[] = { 18, 20, 40, 80 };
	for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++)
		for (size_t j = 0; j < sizeof(bars) / sizeof(bars[0]); j++) {
			struct pl_metrics m = { 30 + bars[j], 30 + bars[j] / 2, 24 + bars[j] / 3 };
			pl_layout_compute(&l, widths[i], bars[j], true, false, &m);
			struct pl_rect all = { 0, 0, l.w, l.h };
			char msg[96];
			snprintf(msg, sizeof(msg), "w=%d bar=%d: everything is inside", widths[i], bars[j]);
			bool ok = inside(l.clock, all) && inside(l.battery, all) && inside(l.row, all) &&
				inside(l.slider.cell, all) && inside(l.pct, all);
			for (int s = 0; s < PL_SLIDERS; s++)
				ok = ok && inside(l.button[s], all) && inside(l.hl[s], l.button[s]);
			CHECK(ok || (widths[i] < 320 && bars[j] >= 40) || widths[i] < 200, msg);
			CHECK_EQ(l.h, bars[j] + pl_row_height(bars[j]), "the surface is the bar and the row");
			CHECK(l.button[0].w >= 36 && l.button[0].h >= bars[j], "buttons keep their size");
			CHECK(l.button[0].x >= l.clock.x + l.clock.w || widths[i] < 320 || bars[j] >= 40,
				"buttons do not run into the clock");
		}
	pl_layout_compute(&l, 1, 1, true, false, &M15);
	CHECK(l.w >= 2 && l.h >= 18, "a degenerate surface does not divide by zero");

	/* Slider mapping. */
	pl_layout_compute(&l, 240, 20, true, false, &M15);
	const struct pl_slider *s = &l.slider;
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
		struct pl_rect th = pl_slider_thumb(&l, p);
		int v = pl_slider_value(s, th.x + th.w / 2);
		if (v < p - 1 || v > p + 1)
			close = 0;
	}
	CHECK(mono, "value never decreases to the right");
	CHECK(close, "the thumb centre maps back to its value, give or take a pixel");
	struct pl_rect t0 = pl_slider_thumb(&l, 0), t100 = pl_slider_thumb(&l, 100);
	CHECK_EQ(t0.x, s->track.x, "thumb at 0 starts at the track");
	CHECK_EQ(t100.x + t100.w, s->track.x + s->track.w, "thumb at 100 ends at the track");
	CHECK(t0.w == s->thumb_d && t0.h == s->thumb_d, "the thumb is a circle's box");
	CHECK(inside(t0, l.row) && inside(t100, l.row), "thumb stays inside the row");
	CHECK_EQ(t0.y + t0.h / 2, l.row.y + (l.row_h - 1) / 2, "the thumb is centred in the row");
	struct pl_rect tn = pl_slider_thumb(&l, -50), tp = pl_slider_thumb(&l, 500);
	CHECK(tn.x == t0.x && tp.x == t100.x, "thumb percent is clamped");
}

/* At the bottom edge the row opens above the bar: the bar stays where it is. */
static void test_layout_bottom(void)
{
	struct pl_layout t, b, bc;

	pl_layout_compute(&t, 240, 20, true, false, &M15);
	pl_layout_compute(&b, 240, 20, true, true, &M15);
	pl_layout_compute(&bc, 240, 20, false, true, &M15);
	CHECK(b.bottom && !t.bottom, "the bottom flag");
	CHECK_EQ(b.h, 56, "the same surface");
	CHECK_EQ(b.row.y, 0, "the row is at the top of the surface");
	CHECK_EQ(b.row.h, 36, "and is as high as at the top");
	CHECK_EQ(b.bar_y, 36, "the bar is at the bottom of it");
	CHECK_EQ(b.button[0].y, 36, "its buttons too");
	CHECK_EQ(b.button[0].y + b.button[0].h, b.h, "and end at the edge of the screen");
	CHECK_EQ(bc.bar_y, 0, "closed, the bar is the surface");
	CHECK_EQ(bc.button[0].y, 0, "closed, a button starts at the top");
	CHECK_EQ(b.bar_line_y, 36, "the bar's line is on its top, the side of the windows");
	CHECK_EQ(t.bar_line_y, 19, "at the top of the screen it is at the bottom of the bar");
	CHECK_EQ(bc.bar_line_y, 0, "a closed bottom bar has it on top, too");
	CHECK_EQ(b.row_line_y, 0, "the row's line is on its top edge");
	CHECK_EQ(t.row_line_y, 55, "at the top of the screen on its bottom edge");
	CHECK(b.clock.y == 37 && b.clock.h == 19, "the clock rectangle starts under the line");
	CHECK(b.row_in.y == 1 && b.row_in.h == 35, "the row without its line");
	CHECK_EQ(b.slider.cell.y, 0, "the touch area is the whole row");
	CHECK_EQ(b.slider.cell.h, 36, "of its height");
	CHECK(b.hl[0].y >= b.clock.y && b.hl[0].y + b.hl[0].h <= b.clock.y + b.clock.h, "the highlight is inside the bar");
	struct pl_rect th = pl_slider_thumb(&b, 50);
	CHECK(th.y >= 1 && th.y + th.h <= 35, "the thumb is in the row, not on its line");
	struct pl_rect in = pl_input_rect(&b, 240, 56);
	CHECK(in.y == 0 && in.h == 56, "the input region is the whole surface of bar and row");
	struct pl_rect op[2];
	CHECK_EQ(pl_opaque_rects(&b, 255, 224, op), 1, "opaque: the bar");
	CHECK(op[0].y == 36 && op[0].h == 20, "at the bottom");
	struct pl_hit h = pl_hit_test(&b, b.button[0].x + 5, 40);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_BACKLIGHT, "hit: a button at the bottom");
	h = pl_hit_test(&b, b.button[0].x + 5, 10);
	CHECK_EQ(h.kind, PL_HIT_TRACK, "hit: the row above the bar");
	h = pl_hit_test(&b, 3, 10);
	CHECK_EQ(h.kind, PL_HIT_NONE, "hit: the row's icon is inert");
	h = pl_hit_test(&bc, bc.button[1].x + 5, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_VOLUME, "hit: closed, the bar");
}

/* ---- hit testing and touch ---- */

static void test_touch(void)
{
	struct pl_layout l, c;
	struct pl_touch t = { .slider = PL_SLIDER_NONE };
	const bool both[PL_SLIDERS] = { true, true };
	const bool no_vol[PL_SLIDERS] = { true, false };
	struct pl_touch_out o;

	pl_layout_compute(&l, 240, 20, true, false, &M15);
	pl_layout_compute(&c, 240, 20, false, false, &M15);
	const struct pl_rect *b0 = &l.button[0], *b1 = &l.button[1];
	struct pl_hit h;

	/* Hit testing. */
	h = pl_hit_test(&l, b0->x + b0->w / 2, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_BACKLIGHT, "hit: the backlight button");
	h = pl_hit_test(&l, b1->x + b1->w / 2, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_VOLUME, "hit: the volume button");
	h = pl_hit_test(&l, b0->x, 0);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_BACKLIGHT, "hit: the corner of a button");
	h = pl_hit_test(&l, b0->x + b0->w - 1, 19);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_BACKLIGHT,
		"hit: the last pixel of a button, on the bar's line");
	h = pl_hit_test(&l, b0->x - 1, 10);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit just left of a button");
	h = pl_hit_test(&l, b1->x + b1->w, 10);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit just right of the volume button");
	h = pl_hit_test(&l, b0->x + b0->w, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_VOLUME, "the buttons are side by side");
	h = pl_hit_test(&l, 10, 10);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit on the clock");
	h = pl_hit_test(&l, 232, 10);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit on the battery");
	h = pl_hit_test(&l, 3, 30);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit in the row outside the touch area");
	int mx = l.slider.track.x + l.slider.track.w / 2;
	h = pl_hit_test(&l, mx, 20);
	CHECK_EQ(h.kind, PL_HIT_TRACK, "hit: the track area, top of the row");
	h = pl_hit_test(&l, mx, 55);
	CHECK_EQ(h.kind, PL_HIT_TRACK, "hit: the track area, bottom of the row");
	h = pl_hit_test(&l, mx, 56);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit below the row");
	h = pl_hit_test(&l, l.slider.cell.x, 40);
	CHECK_EQ(h.kind, PL_HIT_TRACK, "hit: the left edge of the touch area");
	h = pl_hit_test(&l, l.slider.cell.x - 1, 40);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit left of the touch area");
	h = pl_hit_test(&l, l.row_icon.x + 3, l.row_icon.y + 3);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit on the row's icon");
	h = pl_hit_test(&l, l.pct.x + 3, 40);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit on the value text");
	h = pl_hit_test(&l, 240, 40);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit right of the surface");
	h = pl_hit_test(&l, -1, 10);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit left of the surface");
	h = pl_hit_test(&c, mx, 40);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no row, no track hit");
	h = pl_hit_test(&c, b0->x + 3, 5);
	CHECK_EQ(h.kind, PL_HIT_BUTTON, "buttons work without the row");

	/* Tapping a button never sets a value. */
	pl_touch_press(&t, &c, PL_SLIDER_NONE, both, b0->x + 5, 10, &o);
	CHECK(o.tap == PL_SLIDER_BACKLIGHT && o.slider == PL_SLIDER_NONE, "press on the sun taps it");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_NONE, "release of a tap drags nothing");
	pl_touch_press(&t, &l, PL_SLIDER_BACKLIGHT, both, b0->x + 5, 10, &o);
	CHECK(o.tap == PL_SLIDER_BACKLIGHT && o.slider == PL_SLIDER_NONE,
		"press on the open sun taps it, it sets no value");
	pl_touch_motion(&t, &l, mx, &o);
	CHECK(o.tap == PL_SLIDER_NONE && o.slider == PL_SLIDER_NONE, "and starts no drag when moved onto the track");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_BACKLIGHT, both, b1->x + 5, 10, &o);
	CHECK(o.tap == PL_SLIDER_VOLUME && o.slider == PL_SLIDER_NONE, "the other button taps the other slider");
	pl_touch_release(&t);
	pl_touch_press(&t, &c, PL_SLIDER_NONE, no_vol, b1->x + 5, 10, &o);
	CHECK(o.tap == PL_SLIDER_NONE && o.slider == PL_SLIDER_NONE, "a disabled button takes no tap");
	pl_touch_release(&t);

	/* A drag in the row: the value of the open slider, from x. */
	pl_touch_press(&t, &l, PL_SLIDER_BACKLIGHT, both, mx, 45, &o);
	CHECK(o.slider == PL_SLIDER_BACKLIGHT && o.tap == PL_SLIDER_NONE, "press on the track starts a drag");
	CHECK(o.value >= 49 && o.value <= 51, "press sets the value from x");
	pl_touch_motion(&t, &l, l.slider.track.x + l.slider.track.w - 1, &o);
	CHECK(o.slider == PL_SLIDER_BACKLIGHT && o.value == 100, "motion to the end is 100");
	pl_touch_motion(&t, &l, 239, &o);
	CHECK(o.slider == PL_SLIDER_BACKLIGHT && o.value == 100, "a drag that leaves the track stays on the slider, clamped");
	pl_touch_motion(&t, &l, 0, &o);
	CHECK(o.slider == PL_SLIDER_BACKLIGHT && o.value == PL_BL_FLOOR_PCT, "the backlight never goes below the floor");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_BACKLIGHT, "release ends the drag of the slider");
	pl_touch_motion(&t, &l, mx, &o);
	CHECK_EQ(o.slider, PL_SLIDER_NONE, "motion after the release is ignored");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_NONE, "a second release changes nothing");

	pl_touch_press(&t, &l, PL_SLIDER_VOLUME, both, l.slider.track.x, 45, &o);
	CHECK(o.slider == PL_SLIDER_VOLUME && o.value == 0, "volume reaches 0 at the left end");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_VOLUME, both, l.slider.cell.x, 45, &o);
	CHECK(o.slider == PL_SLIDER_VOLUME && o.value == 0, "the margin left of the track counts");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_BACKLIGHT, both, l.slider.cell.x, 45, &o);
	CHECK(o.slider == PL_SLIDER_BACKLIGHT && o.value == PL_BL_FLOOR_PCT, "the floor at the left edge");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_VOLUME, both, l.slider.cell.x + l.slider.cell.w - 1, 55, &o);
	CHECK(o.slider == PL_SLIDER_VOLUME && o.value == 100, "the margin right of the track, bottom row");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_VOLUME, both, mx, 20, &o);
	CHECK(o.slider == PL_SLIDER_VOLUME, "top edge of the row");
	pl_touch_release(&t);

	/* Nothing sets a value without an open row, or from outside the area. */
	pl_touch_press(&t, &l, PL_SLIDER_NONE, both, mx, 45, &o);
	CHECK(o.slider == PL_SLIDER_NONE && o.tap == PL_SLIDER_NONE, "a press on the track of a closing row does nothing");
	pl_touch_release(&t);
	pl_touch_press(&t, &c, PL_SLIDER_BACKLIGHT, both, mx, 45, &o);
	CHECK(o.slider == PL_SLIDER_NONE, "no row on the surface, no value");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_BACKLIGHT, both, 60, 10, &o);
	pl_touch_motion(&t, &l, mx, &o);
	CHECK_EQ(o.slider, PL_SLIDER_NONE, "a drag from the clock does nothing when it enters the track");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_NONE, "release of an ignored drag");
	pl_touch_press(&t, &l, PL_SLIDER_BACKLIGHT, both, l.row_icon.x + 3, l.row_icon.y + 3, &o);
	CHECK(o.slider == PL_SLIDER_NONE && o.tap == PL_SLIDER_NONE, "the row's icon is inert");
	pl_touch_motion(&t, &l, mx, &o);
	CHECK_EQ(o.slider, PL_SLIDER_NONE, "and so is a drag from it");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_BACKLIGHT, both, l.pct.x + 5, 45, &o);
	CHECK(o.slider == PL_SLIDER_NONE, "the value text is inert");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_BACKLIGHT, both, 300, 45, &o);
	CHECK(o.slider == PL_SLIDER_NONE && o.tap == PL_SLIDER_NONE, "a press off the surface");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_VOLUME, no_vol, mx, 45, &o);
	CHECK_EQ(o.slider, PL_SLIDER_NONE, "a disabled slider takes no drag");
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

/* ---- the pop-out row ---- */

static void test_popup(void)
{
	struct pl_popup p = { .open = PL_SLIDER_NONE };

	CHECK_EQ(PL_POPUP_MS, 3000, "the row closes after 3 s");
	CHECK_EQ(pl_popup_wait_ms(&p, 1000, false), -1, "closed: no deadline, no timer");
	CHECK(!pl_popup_expire(&p, 99999, false), "a closed row does not expire");

	CHECK_EQ(pl_popup_tap(&p, PL_SLIDER_BACKLIGHT, 1000), PL_SLIDER_BACKLIGHT, "a tap opens the row");
	CHECK_EQ(pl_popup_wait_ms(&p, 1000, false), 3000, "3 s to go right away");
	CHECK_EQ(pl_popup_wait_ms(&p, 2000, false), 2000, "2 s to go after 1 s");
	CHECK_EQ(pl_popup_wait_ms(&p, 3999, false), 1, "1 ms to go");
	CHECK(!pl_popup_expire(&p, 3999, false), "not yet at 2999 ms");
	CHECK_EQ(p.open, PL_SLIDER_BACKLIGHT, "still open");
	CHECK_EQ(pl_popup_wait_ms(&p, 4000, false), 0, "due at exactly 3 s");
	CHECK(pl_popup_expire(&p, 4000, false), "it closes at 3 s");
	CHECK_EQ(p.open, PL_SLIDER_NONE, "closed");
	CHECK_EQ(pl_popup_wait_ms(&p, 4001, false), -1, "and has no deadline");

	/* A touch moves the deadline. */
	pl_popup_tap(&p, PL_SLIDER_VOLUME, 10000);
	pl_popup_touch(&p, 12500);
	CHECK_EQ(pl_popup_wait_ms(&p, 12500, false), 3000, "a touch resets the 3 s");
	CHECK(!pl_popup_expire(&p, 13000, false), "so 3 s after the first touch it is still open");
	CHECK_EQ(pl_popup_wait_ms(&p, 13000, false), 2500, "2.5 s are left");
	CHECK(pl_popup_expire(&p, 15500, false), "it closes 3 s after the last touch");

	/* The same button closes it, the other one switches. */
	CHECK_EQ(pl_popup_tap(&p, PL_SLIDER_BACKLIGHT, 20000), PL_SLIDER_BACKLIGHT, "open");
	CHECK_EQ(pl_popup_tap(&p, PL_SLIDER_VOLUME, 20100), PL_SLIDER_VOLUME, "the other button switches the row");
	CHECK_EQ(pl_popup_wait_ms(&p, 20100, false), 3000, "a tap counts as a touch");
	CHECK_EQ(pl_popup_tap(&p, PL_SLIDER_VOLUME, 20200), PL_SLIDER_NONE, "the same button closes it");
	CHECK_EQ(pl_popup_wait_ms(&p, 20200, false), -1, "and there is nothing to wait for");
	CHECK_EQ(pl_popup_tap(&p, PL_SLIDER_VOLUME, 20300), PL_SLIDER_VOLUME, "and opens it again");

	/* A stylus that is down keeps it open. */
	CHECK_EQ(pl_popup_wait_ms(&p, 90000, true), 3000, "touching: the full time, whatever the deadline");
	CHECK(!pl_popup_expire(&p, 90000, true), "touching: it does not expire");
	CHECK_EQ(p.open, PL_SLIDER_VOLUME, "still open");
	CHECK(pl_popup_expire(&p, 90000, false), "released: the old deadline has passed");

	/* A clock that went back. */
	pl_popup_tap(&p, PL_SLIDER_BACKLIGHT, 50000);
	CHECK(pl_popup_wait_ms(&p, 10, false) <= PL_POPUP_MS, "never longer than 3 s, even if time went back");
}

/* ---- blending ---- */

static void test_blend(void)
{
	CHECK_EQ(pl_premul(0xffffff, 255), 0xffffffffu, "opaque white");
	CHECK_EQ(pl_premul(0x102030, 255), 0xff102030u, "opaque is unchanged");
	CHECK_EQ(pl_premul(0xffffff, 0), 0x00000000u, "transparent is all zero");
	CHECK_EQ(pl_premul(0xff8000, 128), 0x80804000u, "128 of 255 is premultiplied: ff->80, 80->40");
	CHECK_EQ(pl_premul(0x1c1f24, 224), 0xe0191b20u, "the translucent row ground");
	CHECK_EQ(pl_premul(0xffffff, 300), 0xffffffffu, "alpha is clamped");

	CHECK_EQ(pl_over(0xff000000u, 0xffffff, 255), 0xffffffffu, "opaque source replaces");
	CHECK_EQ(pl_over(0xff123456u, 0xffffff, 0), 0xff123456u, "no coverage leaves the destination");
	CHECK_EQ(pl_over(0x00000000u, 0xff8000, 128), pl_premul(0xff8000, 128),
		"over transparent: the source, premultiplied by its coverage");
	uint32_t half = pl_over(0xff000000u, 0xffffff, 128);
	int g = (int)((half >> 8) & 0xff);
	CHECK_EQ(half >> 24, 0xff, "over an opaque pixel the result is opaque");
	CHECK(g >= 184 && g <= 192, "half of white over black is 188, as blending in linear light gives");
	CHECK_EQ(half & 0xff, (half >> 16) & 0xff, "grey stays grey");
	uint32_t half2 = pl_over(0xffffffffu, 0x000000, 128);
	int g2 = (int)((half2 >> 8) & 0xff);
	CHECK(g2 >= 184 && g2 <= 192, "and so is half of black over white");

	int mono = 1, prev = -1;
	for (int a = 0; a <= 255; a++) {
		int v = (int)((pl_over(0xff202020u, 0xe8eaed, a) >> 8) & 0xff);
		if (v < prev)
			mono = 0;
		prev = v;
	}
	CHECK(mono, "more coverage is never darker for light on dark");
	CHECK_EQ(prev, 0xea, "full coverage is the source");

	/* Over a translucent pixel the alpha adds up: a + da * (1 - a). */
	uint32_t row = pl_premul(0x252930, 224);
	uint32_t o = pl_over(row, 0xe8eaed, 128);
	int want = 128 + 224 * (255 - 128) / 255;
	CHECK(abs((int)(o >> 24) - want) <= 1, "alpha of a blend over a translucent pixel");
	CHECK(((o >> 16) & 0xff) <= (o >> 24) && ((o >> 8) & 0xff) <= (o >> 24) &&
		(o & 0xff) <= (o >> 24), "the result is premultiplied");
	uint32_t full = pl_over(row, 0xe8eaed, 255);
	CHECK_EQ(full, 0xffe8eaedu, "an opaque pixel over a translucent one is opaque");

	/* The premultiplied invariant for any input, and no overflow. */
	int ok = 1;
	for (int da = 0; da <= 255; da += 15)
		for (int c = 0; c <= 255; c += 17)
			for (int a = 0; a <= 255; a += 5) {
				uint32_t dst = pl_premul(((uint32_t)c << 16) | ((uint32_t)(255 - c) << 8) | 0x40, da);
				uint32_t r = pl_over(dst, 0x80ff20, a);
				int ra = (int)(r >> 24);
				if (((r >> 16) & 0xff) > (uint32_t)ra || ((r >> 8) & 0xff) > (uint32_t)ra ||
						(r & 0xff) > (uint32_t)ra)
					ok = 0;
				if (ra < da && a > 0)
					ok = 0;
			}
	CHECK(ok, "colour never exceeds alpha, and a blend never makes a pixel more transparent");

	/* How a canvas stores it. */
	struct pl_canvas c = { NULL, 1, 1, 4, PL_FMT_ARGB8888 };
	CHECK_EQ(pl_pixel(&c, 0x252930, 224), pl_premul(0x252930, 224), "ARGB8888 holds the premultiplied colour");
	CHECK_EQ(pl_pixel(&c, 0x3c8ce6, 255), 0xff3c8ce6u, "opaque ARGB8888");
	c.fmt = PL_FMT_XRGB8888;
	CHECK_EQ(pl_pixel(&c, 0x3c8ce6, 100), 0x3c8ce6u, "XRGB8888 ignores the alpha");
	c.fmt = PL_FMT_RGB565;
	CHECK_EQ(pl_pixel(&c, 0x3c8ce6, 100), 0x3c7c, "RGB565 of the blue, rounded");
	CHECK_EQ(pl_pixel(&c, 0xffffff, 255), 0xffff, "RGB565 white");
	CHECK_EQ(pl_pixel(&c, 0x000000, 255), 0x0000, "RGB565 black");
}

/* ---- masks ---- */

struct sd_ctx {
	int kind;
};

static float sd_test(const void *ctx, float x, float y)
{
	const struct sd_ctx *c = ctx;
	static const float tri[3][2] = { { 2, 2 }, { 18, 2 }, { 2, 18 } };

	switch (c->kind) {
	case 0: return pl_sd_circle(x, y, 10, 10, 8);
	case 1: return pl_sd_rrect(x, y, 2, 4, 18, 16, 5);
	case 2: return pl_sd_capsule(x, y, 4, 10, 16, 10, 2);
	case 3: return pl_sd_poly(x, y, tri, 3);
	default: return pl_sd_arc(x, y, 4, 10, 8, 2, 40);
	}
}

static int mask_sum(const struct pl_mask *m)
{
	int n = 0;

	for (int i = 0; i < m->w * m->h; i++)
		n += m->a[i];
	return n;
}

static int mask_partial(const struct pl_mask *m)
{
	int n = 0;

	for (int i = 0; i < m->w * m->h; i++)
		n += m->a[i] > 0 && m->a[i] < 255;
	return n;
}

static void test_masks(void)
{
	struct sd_ctx k = { 0 };
	struct pl_mask m = pl_mask_from_sdf(20, 20, sd_test, &k);

	CHECK(m.a != NULL, "a mask is allocated");
	CHECK_EQ(m.a[10 * 20 + 10], 255, "the middle of a disc is covered");
	CHECK_EQ(m.a[0], 0, "the corner is not");
	double area = mask_sum(&m) / 255.0;
	CHECK(fabs(area - 3.14159 * 64) < 3.0, "the coverage adds up to the area of the disc");
	CHECK(mask_partial(&m) >= 20, "the edge is anti-aliased, not a hard step");
	int sym = 1;
	for (int y = 0; y < 20; y++)
		for (int x = 0; x < 20; x++)
			if (abs(m.a[y * 20 + x] - m.a[(19 - y) * 20 + (19 - x)]) > 1)
				sym = 0;
	CHECK(sym, "a disc in the middle of its box is symmetric");
	pl_mask_free(&m);
	CHECK(m.a == NULL, "freed");
	m = pl_mask_from_sdf(0, 5, sd_test, &k);
	CHECK(m.a == NULL, "an empty mask is NULL");

	k.kind = 1;
	m = pl_mask_from_sdf(20, 20, sd_test, &k);
	CHECK_EQ(m.a[10 * 20 + 10], 255, "rounded rectangle: inside");
	CHECK_EQ(m.a[4 * 20 + 2], 0, "rounded rectangle: the corner is cut");
	CHECK_EQ(m.a[10 * 20 + 2], 255, "rounded rectangle: the straight edge is solid");
	CHECK_EQ(m.a[10 * 20 + 1], 0, "rounded rectangle: outside");
	pl_mask_free(&m);

	k.kind = 2;
	m = pl_mask_from_sdf(20, 20, sd_test, &k);
	CHECK_EQ(m.a[10 * 20 + 10], 255, "capsule: on the line");
	CHECK(m.a[8 * 20 + 10] == 255 && m.a[11 * 20 + 10] == 255 && m.a[7 * 20 + 10] == 0 && m.a[12 * 20 + 10] == 0, "capsule: 4 px thick");
	CHECK(m.a[10 * 20 + 3] > 0, "capsule: round end reaches past the end point");
	pl_mask_free(&m);

	k.kind = 3;
	m = pl_mask_from_sdf(20, 20, sd_test, &k);
	CHECK_EQ(m.a[5 * 20 + 5], 255, "triangle: inside");
	CHECK_EQ(m.a[15 * 20 + 15], 0, "triangle: outside the hypotenuse");
	CHECK(fabs(mask_sum(&m) / 255.0 - 128.0) < 4.0, "triangle: the area is 16*16/2");
	pl_mask_free(&m);
	/* The same triangle the other way round. */
	const float rev[3][2] = { { 2, 18 }, { 18, 2 }, { 2, 2 } };
	CHECK(pl_sd_poly(5, 5, rev, 3) < 0 && pl_sd_poly(15, 15, rev, 3) > 0,
		"a polygon in either winding order");

	k.kind = 4;
	m = pl_mask_from_sdf(20, 20, sd_test, &k);
	CHECK(m.a[10 * 20 + 12] > 240, "arc: on the ring in front of its centre");
	CHECK_EQ(m.a[10 * 20 + 3], 0, "arc: behind the centre nothing");
	CHECK_EQ(m.a[0 * 20 + 6], 0, "arc: outside the wedge nothing");
	CHECK(m.a[8 * 20 + 12] > 0, "arc: inside the wedge");
	pl_mask_free(&m);
}

/* ---- the font ---- */

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

	struct pl_font f;
	CHECK(!pl_font_load(&f, "/nonexistent/font.ttf", 15, 12), "a missing font file is no font");
	CHECK(!f.ttf, "and the bitmap font is used");
	CHECK_EQ(pl_font_text_w(&f, 0, "12:34"), 58, "bitmap text is scale 2 at 15 px");
	CHECK_EQ(pl_font_text_w(&f, 0, ""), 0, "empty text");
	pl_font_free(&f);

	/* A file that is not a font. */
	char path[100];
	snprintf(path, sizeof(path), "/tmp/picowl-panel-font.%d", getpid());
	FILE *fp = fopen(path, "w");
	if (fp) {
		for (int i = 0; i < 400; i++)
			fputs("this is not a TrueType font, only text\n", fp);
		fclose(fp);
		CHECK(!pl_font_load(&f, path, 15, 12), "a file that is not a font is rejected");
		CHECK(!f.ttf, "and the bitmap font is used for it too");
		pl_font_free(&f);
		snprintf(path + 0, sizeof(path), "/tmp/picowl-panel-font.%d", getpid());
		unlink(path);
	}
	CHECK(!pl_font_load(&f, "/tmp", 15, 12), "a directory is not a font");
	pl_font_free(&f);

	/* The real thing, where a default font is installed. */
	if (pl_font_load(&f, NULL, 15, 12)) {
		printf("test-panel: font %s\n", f.path);
		CHECK(f.ttf, "a default font file was found");
		CHECK(pl_font_text_w(&f, 0, "88:88") > 30, "the clock text is wider than 30 px");
		CHECK(pl_font_text_w(&f, 1, "100%") < pl_font_text_w(&f, 0, "100%"), "the small face is smaller");
		CHECK(f.face[0].digit_h >= 9 && f.face[0].digit_h <= 13, "15 px digits are about 11 px high");
		int partial = mask_partial(&f.face[0].g[0].m);
		CHECK(partial > 10, "glyphs have anti-aliased edges");
		CHECK(pl_font_text_w(&f, 0, "11") == 2 * f.face[0].g[1].adv, "the advance is per glyph");
		CHECK(pl_font_text_w(&f, 0, "1:") == f.face[0].g[1].adv + f.face[0].g[10].adv,
			"the colon has an advance too");
		/* The same text twice at another size. */
		struct pl_font g;
		if (pl_font_load(&g, f.path, 22, 18)) {
			CHECK(pl_font_text_w(&g, 0, "88:88") > pl_font_text_w(&f, 0, "88:88"), "a larger size is wider");
			pl_font_free(&g);
		}
		pl_font_free(&f);
	} else {
		printf("test-panel: no default font file, the real font checks are skipped\n");
	}
	CHECK(!pl_font_load(&f, "/dev/null", 15, 12), "an empty file is not a font");
}

/* ---- drawing ---- */

#define GUARD 64

struct canvas_buf {
	uint8_t *mem;
	struct pl_canvas c;
	size_t size;
};

static void canvas_init(struct canvas_buf *b, int w, int h, enum pl_fmt fmt)
{
	int bpp = fmt == PL_FMT_RGB565 ? 2 : 4;
	b->size = (size_t)w * h * bpp;
	b->mem = malloc(b->size + 2 * GUARD);
	memset(b->mem, 0xaa, b->size + 2 * GUARD);
	b->c = (struct pl_canvas){ b->mem + GUARD, w, h, w * bpp, fmt };
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
	return b->c.fmt == PL_FMT_RGB565 ? ((const uint16_t *)row)[x] : ((const uint32_t *)row)[x];
}

static int count_value(const struct canvas_buf *b, struct pl_rect r, uint32_t v)
{
	int n = 0;

	for (int y = r.y; y < r.y + r.h; y++)
		for (int x = r.x; x < r.x + r.w; x++)
			if (px(b, x, y) == v)
				n++;
	return n;
}

static int count_color(const struct canvas_buf *b, struct pl_rect r, uint32_t rgb)
{
	return count_value(b, r, pl_pixel(&b->c, rgb, 255));
}

static int count_not(const struct canvas_buf *b, struct pl_rect r, uint32_t v)
{
	return r.w * r.h - count_value(b, r, v);
}

struct env {
	struct pl_assets a;
	struct pl_layout l;
	struct canvas_buf b;
	struct pl_state st;
};

static void env_init(struct env *e, enum pl_fmt fmt, int bar_alpha, int popup_alpha,
	bool row, const char *font)
{
	struct pl_metrics m;

	pl_assets_init(&e->a, font, 12, bar_alpha, popup_alpha);
	pl_assets_metrics(&e->a, &m);
	pl_layout_compute(&e->l, 240, 20, row, false, &m);
	CHECK(pl_assets_prepare(&e->a, &e->l), "the masks are built");
	canvas_init(&e->b, 240, e->l.h, fmt);
	e->st = (struct pl_state){ .hour = 12, .min = 34, .bat = PL_BAT_DISCHARGING,
		.bat_pct = 73, .bl_pct = 60, .vol_pct = 80 };
}

static void env_free(struct env *e)
{
	pl_assets_free(&e->a);
	free(e->b.mem);
}

static const char *const NOFONT = "/nonexistent/none.ttf";

static void test_render(enum pl_fmt fmt, int bar_alpha, int popup_alpha)
{
	struct env e;
	char msg[120];
	const char *fn = fmt == PL_FMT_RGB565 ? "565" : fmt == PL_FMT_XRGB8888 ? "8888" : "argb";

	env_init(&e, fmt, bar_alpha, popup_alpha, true, NOFONT);
	const struct pl_layout *l = &e.l;
	const struct pl_canvas *c = &e.b.c;
	pl_render_all(c, l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
	CHECK(guards_intact(&e.b), "drawing stays inside the buffer");

	snprintf(msg, sizeof(msg), "%s a%d/a%d: bar background", fn, bar_alpha, popup_alpha);
	CHECK_EQ(px(&e.b, 80, 2), pl_pixel(c, PL_COL_BG, bar_alpha), msg);
	CHECK_EQ(px(&e.b, 80, 19), pl_pixel(c, PL_COL_LINE, bar_alpha), "the bar's line, 1 px, at the bottom");
	CHECK_EQ(px(&e.b, 80, 18), pl_pixel(c, PL_COL_BG, bar_alpha), "and the bar above it");
	CHECK_EQ(px(&e.b, 3, l->row.y + 2), pl_pixel(c, PL_COL_ROW, popup_alpha), "row background");
	CHECK_EQ(px(&e.b, 3, l->row.y + l->row_h - 1), pl_pixel(c, PL_COL_ROW_LINE, popup_alpha),
		"the row's last line");
	CHECK_EQ(px(&e.b, 3, l->row.y + l->row_h - 2), pl_pixel(c, PL_COL_ROW, popup_alpha),
		"the row above its line");

	const struct pl_slider *s = &l->slider;
	struct pl_rect th = pl_slider_thumb(l, 60);
	int ty = l->row.y + (l->row_h - 1 - s->track_h) / 2 + s->track_h / 2;
	CHECK_EQ(px(&e.b, s->track.x + 8, ty), pl_pixel(c, PL_COL_ACCENT, 255), "the filled part is the accent");
	CHECK_EQ(px(&e.b, th.x - 3, ty), pl_pixel(c, PL_COL_ACCENT, 255), "the fill runs up to the thumb");
	CHECK_EQ(px(&e.b, s->track.x + s->track.w - 8, ty), pl_pixel(c, PL_COL_TRACK, 255), "the rest is dark grey");
	CHECK_EQ(px(&e.b, th.x + th.w / 2, th.y + th.h / 2), pl_pixel(c, PL_COL_THUMB, 255), "the thumb is white");
	CHECK(th.w >= 22, "the drawn thumb is at least 22 px");
	CHECK(px(&e.b, th.x + th.w / 2, th.y) != pl_pixel(c, PL_COL_THUMB, 255), "its edge is a ring, not white");
	CHECK(px(&e.b, th.x + th.w + 2, th.y + th.h / 2) != pl_pixel(c, PL_COL_THUMB, 255), "nothing right of it");
	CHECK(count_color(&e.b, l->pct, PL_COL_FG) > 10, "the value is written");
	struct pl_rect icon = { l->row_icon.x, l->row_icon.y, l->row_icon.w, l->row_icon.h };
	CHECK(count_color(&e.b, icon, PL_COL_FG) > 40, "the row's sun is drawn");
	CHECK(count_not(&e.b, icon, pl_pixel(c, PL_COL_ROW, popup_alpha)) > 100, "and it is large");

	/* The open button is highlighted, in the accent colour. */
	struct pl_rect b0 = pl_button_rect(l, 0), b1 = pl_button_rect(l, 1);
	CHECK_EQ(px(&e.b, l->hl[0].x + 4, l->hl[0].y + l->hl[0].h / 2), pl_pixel(c, PL_COL_HL, 255),
		"the open button has a highlight");
	CHECK_EQ(px(&e.b, l->hl[1].x + 4, l->hl[1].y + l->hl[1].h / 2), pl_pixel(c, PL_COL_BG, bar_alpha),
		"the other button has not");
	CHECK(count_color(&e.b, b0, PL_COL_ACCENT) > 15, "the open icon is drawn in the accent");
	CHECK_EQ(count_color(&e.b, b1, PL_COL_ACCENT), 0, "the other icon is not");
	CHECK(count_color(&e.b, b1, PL_COL_FG) > 15, "the other icon is in the text colour");
	CHECK(count_color(&e.b, l->clock, PL_COL_FG) > 30, "the clock text is drawn");
	CHECK_EQ(count_color(&e.b, l->clock, PL_COL_FG) + count_value(&e.b, l->clock,
		pl_pixel(c, PL_COL_BG, bar_alpha)), l->clock.w * l->clock.h, "the clock has two colours in the bitmap font");

	/* ARGB8888 holds premultiplied alpha. */
	if (fmt == PL_FMT_ARGB8888) {
		CHECK_EQ(px(&e.b, 80, 2) >> 24, (uint32_t)bar_alpha, "the bar's alpha");
		CHECK_EQ(px(&e.b, 3, l->row.y + 2) >> 24, (uint32_t)popup_alpha, "the row's alpha");
		CHECK_EQ(px(&e.b, 3, l->row.y + 2), pl_premul(PL_COL_ROW, popup_alpha), "premultiplied");
		CHECK_EQ(px(&e.b, th.x + th.w / 2, th.y + th.h / 2) >> 24, 255u, "the thumb is opaque");
		CHECK(count_color(&e.b, l->pct, PL_COL_FG) > 10, "text over the translucent row is opaque");
		struct pl_rect all = { 0, 0, 240, l->h };
		int bad = 0;
		for (int y = 0; y < all.h; y++)
			for (int x = 0; x < all.w; x++) {
				uint32_t v = px(&e.b, x, y);
				uint32_t a = v >> 24;
				if (((v >> 16) & 0xff) > a || ((v >> 8) & 0xff) > a || (v & 0xff) > a)
					bad++;
			}
		CHECK_EQ(bad, 0, "no pixel has a colour above its alpha");
	}

	/* Closed: no row on the surface. */
	struct env cl;
	env_init(&cl, fmt, bar_alpha, popup_alpha, false, NOFONT);
	pl_render_all(&cl.b.c, &cl.l, &cl.a, &cl.st, PL_SLIDER_NONE);
	CHECK(guards_intact(&cl.b), "a closed bar stays inside the buffer");
	CHECK_EQ(cl.b.c.h, 20, "the closed canvas is the bar");
	CHECK_EQ(count_color(&cl.b, cl.l.hl[0], PL_COL_HL), 0, "nothing is highlighted");
	CHECK_EQ(px(&cl.b, 80, 2), pl_pixel(&cl.b.c, PL_COL_BG, bar_alpha), "bar background");
	CHECK(count_color(&cl.b, pl_button_rect(&cl.l, 0), PL_COL_FG) > 15, "idle icons are in the text colour");
	env_free(&cl);

	/* A widget redraw touches its own rectangle only. */
	const uint32_t sent = pl_pixel(c, 0x123456, 255);
	struct pl_rect whole = { 0, 0, 240, l->h };
	pl_fill(c, whole, 0x123456, 255);
	pl_render_clock(c, l, &e.a, &e.st);
	CHECK_EQ(count_value(&e.b, l->battery, sent), l->battery.w * l->battery.h, "clock redraw leaves the battery alone");
	CHECK_EQ(count_value(&e.b, b0, sent), b0.w * b0.h, "clock redraw leaves the buttons alone");
	CHECK_EQ(count_value(&e.b, l->row, sent), l->row.w * l->row.h, "clock redraw leaves the row alone");
	CHECK_EQ(count_value(&e.b, l->clock, sent), 0, "the clock redraw clears its background");
	pl_fill(c, whole, 0x123456, 255);
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK_EQ(count_value(&e.b, l->clock, sent), l->clock.w * l->clock.h, "battery redraw leaves the clock alone");
	CHECK_EQ(count_value(&e.b, b1, sent), b1.w * b1.h, "battery redraw leaves the buttons alone");
	CHECK_EQ(count_value(&e.b, l->battery, sent), 0, "the battery redraw clears its background");
	pl_fill(c, whole, 0x123456, 255);
	pl_render_button(c, l, &e.a, &e.st, PL_SLIDER_BACKLIGHT, PL_SLIDER_VOLUME);
	CHECK_EQ(count_value(&e.b, b0, sent), b0.w * b0.h, "a button redraw leaves the other button alone");
	CHECK_EQ(count_value(&e.b, l->clock, sent), l->clock.w * l->clock.h, "a button redraw leaves the clock alone");
	CHECK_EQ(count_value(&e.b, b1, sent), 0, "a button redraw clears its background");
	pl_fill(c, whole, 0x123456, 255);
	pl_render_row_value(c, l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
	CHECK_EQ(count_value(&e.b, icon, sent), icon.w * icon.h, "a value redraw leaves the row's icon alone");
	CHECK_EQ(count_value(&e.b, (struct pl_rect){ 0, 0, 240, 20 }, sent), 240 * 20, "and the bar");
	struct pl_rect vr = pl_row_value_rect(l);
	CHECK_EQ(count_value(&e.b, vr, sent), 0, "and covers its whole rectangle");
	CHECK_EQ(count_value(&e.b, (struct pl_rect){ 0, l->row.y + l->row_h - 1, 240, 1 }, sent), 240,
		"and leaves the row's last line alone");
	CHECK(guards_intact(&e.b), "widget redraws stay inside the buffer");

	/* Less value, less accent. */
	pl_render_all(c, l, &e.a, &e.st, PL_SLIDER_VOLUME);
	struct pl_rect rowr = l->row;
	int a80 = count_color(&e.b, rowr, PL_COL_ACCENT);
	e.st.vol_pct = 20;
	pl_render_row_value(c, l, &e.a, &e.st, PL_SLIDER_VOLUME);
	CHECK(count_color(&e.b, rowr, PL_COL_ACCENT) < a80, "less volume, less blue");
	e.st.vol_pct = 0;
	pl_render_row_value(c, l, &e.a, &e.st, PL_SLIDER_VOLUME);
	th = pl_slider_thumb(l, 0);
	CHECK_EQ(px(&e.b, th.x + th.w / 2, th.y + th.h / 2), pl_pixel(c, PL_COL_THUMB, 255), "thumb at 0");
	env_free(&e);
}

static void test_render_bottom(void)
{
	struct env e;
	struct pl_metrics m;

	pl_assets_init(&e.a, NOFONT, 12, 255, 224);
	pl_assets_metrics(&e.a, &m);
	pl_layout_compute(&e.l, 240, 20, true, true, &m);
	CHECK(pl_assets_prepare(&e.a, &e.l), "the masks are built");
	canvas_init(&e.b, 240, e.l.h, PL_FMT_ARGB8888);
	e.st = (struct pl_state){ .hour = 12, .min = 34, .bat = PL_BAT_DISCHARGING,
		.bat_pct = 73, .bl_pct = 60, .vol_pct = 80 };
	const struct pl_layout *l = &e.l;
	const struct pl_canvas *c = &e.b.c;
	pl_render_all(c, l, &e.a, &e.st, PL_SLIDER_VOLUME);
	CHECK(guards_intact(&e.b), "a bottom bar stays inside the buffer");
	CHECK_EQ(px(&e.b, 80, l->bar_line_y), pl_pixel(c, PL_COL_LINE, 255), "the line is on top of the bar");
	CHECK_EQ(px(&e.b, 80, l->bar_line_y + 1), pl_pixel(c, PL_COL_BG, 255), "and the bar under it");
	CHECK_EQ(px(&e.b, 80, 55), pl_pixel(c, PL_COL_BG, 255), "down to the edge of the screen");
	CHECK_EQ(px(&e.b, 80, 0), pl_pixel(c, PL_COL_ROW_LINE, 224), "the row's line is its top edge");
	CHECK_EQ(px(&e.b, 3, 1), pl_pixel(c, PL_COL_ROW, 224), "the row under it");
	CHECK_EQ(px(&e.b, 3, 35), pl_pixel(c, PL_COL_ROW, 224), "to the bar");
	CHECK(count_color(&e.b, pl_button_rect(l, 1), PL_COL_ACCENT) > 10, "the open button is highlighted in the bar");
	struct pl_rect th = pl_slider_thumb(l, 80);
	CHECK_EQ(px(&e.b, th.x + th.w / 2, th.y + th.h / 2), pl_pixel(c, PL_COL_THUMB, 255), "the thumb is in the row");
	CHECK(th.y + th.h < 36, "above the bar");
	pl_assets_free(&e.a);
	free(e.b.mem);
}

static void test_battery_icon(void)
{
	struct env e;

	env_init(&e, PL_FMT_XRGB8888, 255, 255, false, NOFONT);
	const struct pl_layout *l = &e.l;
	const struct pl_canvas *c = &e.b.c;

	e.st.bat_pct = 100;
	pl_render_battery(c, l, &e.a, &e.st);
	int full = count_color(&e.b, l->battery, PL_COL_BAT_FILL);
	e.st.bat_pct = 40;
	pl_render_battery(c, l, &e.a, &e.st);
	int mid = count_color(&e.b, l->battery, PL_COL_BAT_FILL);
	e.st.bat_pct = 20;
	pl_render_battery(c, l, &e.a, &e.st);
	int low = count_color(&e.b, l->battery, PL_COL_BAT_FILL);
	CHECK(full > mid && mid > low && low > 0, "the fill is proportional to the capacity");
	CHECK(full > 30, "a full battery has a good amount of fill");
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_LOW), 0, "20 percent is not red");
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_EMPTY) > 5, "the empty part has a ground of its own");
	e.st.bat_pct = 15;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_LOW) > 0, "15 percent is red");
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_FILL), 0, "and not grey");
	e.st.bat_pct = 10;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_LOW) > 0, "a nearly empty battery is red");
	e.st.bat_pct = 0;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_LOW), 0, "an empty battery has no fill");
	CHECK(count_color(&e.b, l->battery, PL_COL_FG) > 20, "but an outline and the text");
	e.st.bat_pct = 73;
	e.st.bat = PL_BAT_DISCHARGING;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BOLT), 0, "no charging marker while discharging");
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_CHARGING), 0, "and no green");
	e.st.bat = PL_BAT_CHARGING;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK(mask_sum(&e.a.bolt) > 255 * 4, "the bolt has substance");
	{
		struct canvas_buf was;
		canvas_init(&was, 240, l->h, PL_FMT_XRGB8888);
		struct pl_state d = e.st;
		d.bat = PL_BAT_DISCHARGING;
		pl_render_battery(&was.c, l, &e.a, &d);
		int diff = 0;
		for (int y = 0; y < l->battery.h; y++)
			for (int x = l->battery.x; x < l->battery.x + l->battery.w; x++)
				diff += px(&was, x, y) != px(&e.b, x, y);
		CHECK(diff > 6, "charging differs from discharging by more than the colour");
		free(was.mem);
	}
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_CHARGING) > 10, "and a green fill");
	e.st.bat_pct = 10;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_LOW) > 0, "a nearly empty battery that charges is red");
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_CHARGING), 0, "and not green");
	e.st.bat = PL_BAT_FULL;
	e.st.bat_pct = 100;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BOLT), 0, "full is not charging");
	e.st.bat = PL_BAT_AC;
	e.st.bat_pct = -1;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_EMPTY) + count_color(&e.b, l->battery, PL_COL_BAT_FILL), 0,
		"AC has no battery icon");
	CHECK(count_color(&e.b, l->battery, PL_COL_FG) > 10, "AC is written");
	e.st.bat = PL_BAT_NONE;
	pl_render_battery(c, l, &e.a, &e.st);
	CHECK(count_color(&e.b, l->battery, PL_COL_FG) > 5, "-- is written");
	CHECK(guards_intact(&e.b), "the battery stays in the buffer");

	/* The speaker shows the level; muted has the slash. */
	struct pl_state st = e.st;
	int ink[3];
	int v[3] = { 0, 30, 80 };
	for (int i = 0; i < 3; i++) {
		st.vol_pct = v[i];
		pl_render_button(c, l, &e.a, &st, PL_SLIDER_NONE, PL_SLIDER_VOLUME);
		ink[i] = count_not(&e.b, pl_button_rect(l, 1), pl_pixel(c, PL_COL_BG, 255));
	}
	CHECK(ink[2] > ink[1], "two waves have more ink than one");
	CHECK_EQ(pl_speaker_variant(0), 0, "0 percent is muted");
	CHECK_EQ(pl_speaker_variant(1), 1, "1 percent: one wave");
	CHECK_EQ(pl_speaker_variant(50), 1, "50 percent: one wave");
	CHECK_EQ(pl_speaker_variant(51), 2, "51 percent: two waves");
	CHECK_EQ(pl_speaker_variant(100), 2, "100 percent: two waves");
	CHECK(memcmp(e.a.speaker[0][0].a, e.a.speaker[1][0].a, (size_t)e.a.speaker[0][0].w * e.a.speaker[0][0].h) != 0,
		"muted differs from one wave");
	CHECK(mask_partial(&e.a.sun[0]) >= 15 && mask_partial(&e.a.speaker[2][0]) >= 15,
		"the icons are anti-aliased");
	CHECK(mask_sum(&e.a.sun[0]) > 255 * 30, "the sun has substance");
	CHECK(e.a.sun[0].w >= 12 && e.a.sun[0].w <= 16, "the button icons are about 14 px");
	CHECK(e.a.sun[1].w >= 20, "the row icons are large");
	/* Disabled sliders are grey, not white. */
	st.vol_pct = -1;
	pl_render_button(c, l, &e.a, &st, PL_SLIDER_NONE, PL_SLIDER_VOLUME);
	CHECK_EQ(count_color(&e.b, pl_button_rect(l, 1), PL_COL_FG), 0, "a disabled button is not in the text colour");
	CHECK(count_color(&e.b, pl_button_rect(l, 1), PL_COL_DISABLED) > 15, "it is grey");
	env_free(&e);
}

/* Redrawing is blending masks that exist: it must not allocate. */
static void test_no_alloc(void)
{
#ifdef __GLIBC__
	struct env e;

	env_init(&e, PL_FMT_ARGB8888, 255, 224, true, NULL);
	const struct pl_layout *l = &e.l;
	pl_render_all(&e.b.c, l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
	struct mallinfo2 m0 = mallinfo2();
	for (int i = 0; i < 300; i++) {
		e.st.bl_pct = i % 101;
		e.st.min = i % 60;
		pl_render_all(&e.b.c, l, &e.a, &e.st, i & 1);
		pl_render_clock(&e.b.c, l, &e.a, &e.st);
		pl_render_battery(&e.b.c, l, &e.a, &e.st);
		pl_render_button(&e.b.c, l, &e.a, &e.st, PL_SLIDER_BACKLIGHT, PL_SLIDER_BACKLIGHT);
		pl_render_row_value(&e.b.c, l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
		pl_render_row(&e.b.c, l, &e.a, &e.st, PL_SLIDER_VOLUME);
		pl_assets_prepare(&e.a, l);
	}
	struct mallinfo2 m1 = mallinfo2();
	CHECK_EQ((long long)m1.uordblks, (long long)m0.uordblks, "redrawing allocates nothing");
	CHECK_EQ((long long)m1.arena, (long long)m0.arena, "and does not grow the heap");
	env_free(&e);
#endif
}

static void test_render_ttf(void)
{
	struct env e;
	struct pl_font probe;

	if (!pl_font_load(&probe, NULL, 12, 11)) {
		printf("test-panel: no default font, the TrueType rendering checks are skipped\n");
		return;
	}
	pl_font_free(&probe);
	env_init(&e, PL_FMT_RGB565, 255, 255, true, NULL);
	CHECK(e.a.font.ttf, "the TrueType font is used");
	const struct pl_layout *l = &e.l;
	pl_render_all(&e.b.c, l, &e.a, &e.st, PL_SLIDER_VOLUME);
	CHECK(guards_intact(&e.b), "TrueType text stays inside the buffer");
	uint32_t bg = pl_pixel(&e.b.c, PL_COL_BG, 255), fg = pl_pixel(&e.b.c, PL_COL_FG, 255);
	int ink = count_not(&e.b, l->clock, bg);
	int solid = count_value(&e.b, l->clock, fg);
	CHECK(ink > 60, "the clock text is drawn");
	CHECK(ink - solid > 15, "its edges are anti-aliased: intermediate colours");
	/* Text is centred vertically in the bar. */
	int top = -1, bottom = -1;
	for (int y = 0; y < l->clock.h; y++)
		if (count_not(&e.b, (struct pl_rect){ 0, y, l->clock.w, 1 }, bg) > 0) {
			if (top < 0)
				top = y;
			bottom = y;
		}
	CHECK(top >= 2 && bottom <= l->clock.h - 2, "the digits do not touch the bar's edges");
	CHECK(abs(top - (l->clock.h - 1 - bottom)) <= 1, "and are centred");
	CHECK(count_not(&e.b, l->battery, bg) > 80, "the battery is drawn");
	env_free(&e);
}

static void test_draw(void)
{
	struct canvas_buf b;

	canvas_init(&b, 40, 20, PL_FMT_XRGB8888);
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000, 255);
	pl_fill(&b.c, (struct pl_rect){ -10, -10, 100, 100 }, 0x00ff00, 255);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0x00ff00), 800, "fill clips to the canvas");
	pl_fill(&b.c, (struct pl_rect){ 35, 15, 20, 20 }, 0x0000ff, 255);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0x0000ff), 25, "fill clips at the far corner");
	pl_fill(&b.c, (struct pl_rect){ 50, 50, 10, 10 }, 0xff0000, 255);
	pl_fill(&b.c, (struct pl_rect){ 5, 5, -3, 4 }, 0xff0000, 255);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0xff0000), 0, "empty and far rectangles draw nothing");
	CHECK(guards_intact(&b), "fills stay in the buffer");

	/* Blits clip too, and honour a clip rectangle. */
	struct pl_mask full = { 6, 6, malloc(36) };
	memset(full.a, 255, 36);
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000, 255);
	pl_blit(&b.c, &full, -3, -3, 0xffffff, 255, NULL);
	pl_blit(&b.c, &full, 37, 17, 0xffffff, 255, NULL);
	pl_blit(&b.c, &full, 100, 100, 0xffffff, 255, NULL);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0xffffff), 9 + 9, "blits clip to the canvas");
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000, 255);
	struct pl_rect clip = { 12, 4, 3, 100 };
	pl_blit(&b.c, &full, 10, 2, 0xffffff, 255, &clip);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0xffffff), 3 * 4, "a blit honours its clip");
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000, 255);
	pl_blit(&b.c, &full, 4, 4, 0xffffff, 0, NULL);
	CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, 40, 20 }, 0xffffff), 0, "alpha 0 draws nothing");
	pl_blit(&b.c, &full, 4, 4, 0xffffff, 128, NULL);
	uint32_t p = px(&b, 6, 6);
	CHECK(((p >> 8) & 0xff) >= 184 && ((p >> 8) & 0xff) <= 192, "half alpha is a half tone");
	free(full.a);
	CHECK(guards_intact(&b), "blits stay in the buffer");
	free(b.mem);

	/* The bitmap font clips. */
	struct pl_font f;
	pl_font_load(&f, NOFONT, 12, 11);
	canvas_init(&b, 40, 20, PL_FMT_RGB565);
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000, 255);
	pl_font_draw(&b.c, &f, 0, -30, -3, 20, "12:34 %", 0xffffff);
	pl_font_draw(&b.c, &f, 0, 30, 10, 20, "88", 0xffffff);
	CHECK(guards_intact(&b), "text clips to the canvas");
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000, 255);
	pl_font_draw(&b.c, &f, 0, 4, 0, 20, "1", 0xffffff);
	/* The 1 at scale 2, 14 px high in a band of 20: the top pixel is column 2 of row 0, at y 3. */
	CHECK_EQ(px(&b, 4 + 2 * 2, 3), 0xffff, "glyph pixel, top");
	CHECK_EQ(px(&b, 4 + 2 * 2 + 1, 3 + 1), 0xffff, "a pixel is scale x scale");
	CHECK_EQ(px(&b, 4, 3), 0, "no pixel in the empty corner");
	pl_font_free(&f);
	free(b.mem);

	test_render(PL_FMT_RGB565, 255, 255);
	test_render(PL_FMT_XRGB8888, 255, 255);
	test_render(PL_FMT_ARGB8888, 255, 224);
	test_render(PL_FMT_ARGB8888, 255, 255);
	test_render(PL_FMT_ARGB8888, 128, 128);
	test_render_bottom();
	test_battery_icon();
	test_no_alloc();
	test_render_ttf();
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
	test_layout_bottom();
	test_touch();
	test_popup();
	test_blend();
	test_masks();
	test_font();
	test_draw();
	test_sys();
	printf("test-panel: %d checks, %d failed\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
