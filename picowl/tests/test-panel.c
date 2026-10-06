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
#include "panel-crisp.h"
#include "panel-draw.h"
#include "panel-gfx.h"
#include "panel-logic.h"
#include "panel-sys.h"
#include "subpixel.h"

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

/* What the 11 px Liberation Sans Bold gives, near enough. */
static const struct pl_metrics M15 = { 33, 27, 24, false };

/* The default bar, the row under it and the surface with both. */
#define BAR PL_HEIGHT_DEFAULT
#define ROW 36
#define SURF (BAR + ROW)

static void test_layout(void)
{
	struct pl_layout l, o;

	CHECK_EQ(pl_clamp_height(10), PL_HEIGHT_MIN, "height clamps up to 18");
	CHECK_EQ(PL_HEIGHT_MIN, 18, "the smallest bar is 18");
	CHECK_EQ(pl_clamp_height(500), PL_HEIGHT_MAX, "height clamps down to 80");
	CHECK_EQ(PL_HEIGHT_MAX, 80, "the largest bar is 80");
	CHECK_EQ(pl_clamp_height(28), 28, "height in range");
	CHECK_EQ(PL_HEIGHT_DEFAULT, 18, "the bar is 18 px by default");
	CHECK_EQ(pl_clamp_alpha(-5), 0, "alpha clamps up");
	CHECK_EQ(pl_clamp_alpha(900), 255, "alpha clamps down");
	CHECK_EQ(pl_default_font_px(PL_HEIGHT_DEFAULT), 11, "11 px text on the default bar");
	CHECK(pl_default_font_px(80) > 11 && pl_default_font_px(80) <= 40, "bigger bar, bigger text");
	CHECK(pl_default_font_px(PL_HEIGHT_MIN) >= 8, "the smallest bar still gets text");

	CHECK_EQ(pl_row_height(BAR), ROW, "the row is 36 px under the default bar");
	CHECK_EQ(pl_row_height(80), 88, "a tall bar gets a taller row");
	CHECK_EQ(pl_surface_height(BAR, false), BAR, "closed: the bar");
	CHECK_EQ(pl_surface_height(BAR, true), SURF, "open: the bar and the row");
	CHECK_EQ(pl_surface_height(5, true), 18 + 36, "the height is clamped first");

	pl_layout_compute(&l, 240, BAR, false, false, &M15);
	pl_layout_compute(&o, 240, BAR, true, false, &M15);
	CHECK_EQ(l.h, BAR, "closed surface is the bar");
	CHECK_EQ(o.h, SURF, "open surface is the bar and the row");
	CHECK(!l.row_shown && o.row_shown, "row_shown follows the argument");
	CHECK_EQ(o.row.y, BAR, "the row is right under the bar");
	CHECK_EQ(o.row.h, ROW, "row height");
	CHECK_EQ(o.row.w, 240, "the row is as wide as the surface");
	CHECK_EQ(l.bar_h, BAR, "bar height");
	CHECK(memcmp(l.button, o.button, sizeof(l.button)) == 0,
		"opening the row does not move the buttons");
	CHECK(memcmp(&l.clock, &o.clock, sizeof(l.clock)) == 0 &&
		memcmp(&l.battery, &o.battery, sizeof(l.battery)) == 0,
		"nor the clock and the battery");

	struct pl_rect bar = { 0, 0, 240, BAR };
	CHECK_EQ(l.clock.x, 0, "the clock rectangle starts at the edge");
	CHECK(l.clock.x + l.clock.w >= PL_MARGIN + M15.clock_w, "the clock has room for its text");
	CHECK_EQ(l.clock.h, BAR - 1, "drawing rectangles stop above the line");
	CHECK(inside(l.clock, bar) && inside(l.battery, bar), "clock and battery are in the bar");
	CHECK_EQ(l.battery.x + l.battery.w, 240, "the battery rectangle reaches the right edge");
	CHECK(l.battery.w >= PL_MARGIN + l.bat_icon_w + PL_GAP + M15.bat_text_w,
		"the battery has room for the icon and '100%'");
	CHECK(l.bat_icon_w >= 16 && l.bat_icon_h * 2 <= l.bat_icon_w + 1 && l.bat_icon_h <= 12, "the battery icon is wide and fits the slim bar");
	CHECK(l.bar_icon >= 12 && l.bar_icon <= 14, "the button icons are about 12 px");
	for (int i = 0; i < PL_SLIDERS; i++) {
		CHECK(l.button[i].w >= 36, "a button is at least 36 px wide");
		CHECK_EQ(l.button[i].h, BAR, "a button is the whole bar height");
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
	struct pl_rect in = pl_input_rect(&l, 240, BAR);
	CHECK(in.x == 0 && in.y == 0 && in.w == 240 && in.h == BAR, "closed: the input region is the bar");
	in = pl_input_rect(&o, 240, SURF);
	CHECK(in.x == 0 && in.y == 0 && in.w == 240 && in.h == SURF, "open: the bar and the row");
	in = pl_input_rect(&o, 240, 300);
	CHECK_EQ(in.h, SURF, "a larger surface does not take touches below the row");
	in = pl_input_rect(&l, 240, SURF);
	CHECK_EQ(in.h, BAR, "a row on its way out does not take touches");
	in = pl_input_rect(&o, 200, 40);
	CHECK(in.w == 200 && in.h == 40, "a smaller surface limits it");

	struct pl_rect op[2];
	CHECK_EQ(pl_opaque_rects(&l, 255, 224, op), 1, "closed, opaque bar: one rectangle");
	CHECK(op[0].h == BAR && op[0].w == 240, "that is the bar");
	CHECK_EQ(pl_opaque_rects(&o, 255, 224, op), 1, "open, translucent row: only the bar is opaque");
	CHECK_EQ(op[0].h, BAR, "the bar");
	CHECK_EQ(pl_opaque_rects(&o, 255, 255, op), 2, "open, opaque row: the bar and the row");
	CHECK(op[1].y == BAR && op[1].h == ROW, "the second is the row");
	CHECK_EQ(pl_opaque_rects(&o, 200, 255, op), 1, "translucent bar, opaque row: the row");
	CHECK_EQ(op[0].y, BAR, "the row");
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
			struct pl_metrics m = { 30 + bars[j], 30 + bars[j] / 2, 24 + bars[j] / 3, false };
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
	pl_layout_compute(&l, 240, BAR, true, false, &M15);
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

	pl_layout_compute(&t, 240, BAR, true, false, &M15);
	pl_layout_compute(&b, 240, BAR, true, true, &M15);
	pl_layout_compute(&bc, 240, BAR, false, true, &M15);
	CHECK(b.bottom && !t.bottom, "the bottom flag");
	CHECK_EQ(b.h, SURF, "the same surface");
	CHECK_EQ(b.row.y, 0, "the row is at the top of the surface");
	CHECK_EQ(b.row.h, ROW, "and is as high as at the top");
	CHECK_EQ(b.bar_y, ROW, "the bar is at the bottom of it");
	CHECK_EQ(b.button[0].y, ROW, "its buttons too");
	CHECK_EQ(b.button[0].y + b.button[0].h, b.h, "and end at the edge of the screen");
	CHECK_EQ(bc.bar_y, 0, "closed, the bar is the surface");
	CHECK_EQ(bc.button[0].y, 0, "closed, a button starts at the top");
	CHECK_EQ(b.bar_line_y, ROW, "the bar's line is on its top, the side of the windows");
	CHECK_EQ(t.bar_line_y, BAR - 1, "at the top of the screen it is at the bottom of the bar");
	CHECK_EQ(bc.bar_line_y, 0, "a closed bottom bar has it on top, too");
	CHECK_EQ(b.row_line_y, 0, "the row's line is on its top edge");
	CHECK_EQ(t.row_line_y, SURF - 1, "at the top of the screen on its bottom edge");
	CHECK(b.clock.y == ROW + 1 && b.clock.h == BAR - 1, "the clock rectangle starts under the line");
	CHECK(b.row_in.y == 1 && b.row_in.h == ROW - 1, "the row without its line");
	CHECK_EQ(b.slider.cell.y, 0, "the touch area is the whole row");
	CHECK_EQ(b.slider.cell.h, ROW, "of its height");
	CHECK(b.hl[0].y >= b.clock.y && b.hl[0].y + b.hl[0].h <= b.clock.y + b.clock.h, "the highlight is inside the bar");
	struct pl_rect th = pl_slider_thumb(&b, 50);
	CHECK(th.y >= 1 && th.y + th.h <= ROW - 1, "the thumb is in the row, not on its line");
	struct pl_rect in = pl_input_rect(&b, 240, SURF);
	CHECK(in.y == 0 && in.h == SURF, "the input region is the whole surface of bar and row");
	struct pl_rect op[2];
	CHECK_EQ(pl_opaque_rects(&b, 255, 224, op), 1, "opaque: the bar");
	CHECK(op[0].y == ROW && op[0].h == BAR, "at the bottom");
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
	const bool both[PL_BUTTONS] = { true, true, true, true };
	const bool no_vol[PL_BUTTONS] = { true, false, true, true };
	struct pl_touch_out o;

	pl_layout_compute(&l, 240, BAR, true, false, &M15);
	pl_layout_compute(&c, 240, BAR, false, false, &M15);
	const struct pl_rect *b0 = &l.button[0], *b1 = &l.button[1];
	struct pl_hit h;

	/* Hit testing. */
	h = pl_hit_test(&l, b0->x + b0->w / 2, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_BACKLIGHT, "hit: the backlight button");
	h = pl_hit_test(&l, b1->x + b1->w / 2, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_VOLUME, "hit: the volume button");
	h = pl_hit_test(&l, b0->x, 0);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_BACKLIGHT, "hit: the corner of a button");
	h = pl_hit_test(&l, b0->x + b0->w - 1, BAR - 1);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_BACKLIGHT,
		"hit: the last pixel of a button, on the bar's line");
	h = pl_hit_test(&l, b0->x - 1, 10);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit just left of a button");
	h = pl_hit_test(&l, b1->x + b1->w, 10);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit just right of the volume button");
	h = pl_hit_test(&l, b0->x + b0->w, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_SLIDER_VOLUME, "the buttons are side by side");
	h = pl_hit_test(&l, 10, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_BTN_CLOCK, "hit: the clock is a button");
	h = pl_hit_test(&l, 232, 10);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_BTN_BATTERY, "hit: the battery is a button");
	h = pl_hit_test(&l, 3, 30);
	CHECK_EQ(h.kind, PL_HIT_NONE, "no hit in the row outside the touch area");
	int mx = l.slider.track.x + l.slider.track.w / 2;
	h = pl_hit_test(&l, mx, BAR);
	CHECK_EQ(h.kind, PL_HIT_TRACK, "hit: the track area, top of the row");
	h = pl_hit_test(&l, mx, SURF - 1);
	CHECK_EQ(h.kind, PL_HIT_TRACK, "hit: the track area, bottom of the row");
	h = pl_hit_test(&l, mx, SURF);
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
	pl_touch_press(&t, &l, PL_SLIDER_VOLUME, both, l.slider.cell.x + l.slider.cell.w - 1, SURF - 1, &o);
	CHECK(o.slider == PL_SLIDER_VOLUME && o.value == 100, "the margin right of the track, bottom row");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_VOLUME, both, mx, BAR, &o);
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

/* ---- the clock and the battery as buttons, the text rows ---- */

static int fake_width(void *ctx, int face, const char *s)
{
	/* A face is 10 px per character at 0, 8 at 1, 6 at 2 and 4 at 3. */
	(void)ctx;
	return (int)strlen(s) * (10 - 2 * face);
}

static void test_text_buttons(void)
{
	struct pl_layout l, o;
	struct pl_touch t = { .slider = PL_SLIDER_NONE };
	const bool all[PL_BUTTONS] = { true, true, true, true };
	const bool no_vol[PL_BUTTONS] = { true, false, true, true };
	struct pl_touch_out out;
	struct pl_rect bar = { 0, 0, 240, BAR };

	pl_layout_compute(&l, 240, BAR, false, false, &M15);
	pl_layout_compute(&o, 240, BAR, true, false, &M15);
	const struct pl_rect *cl = &l.button[PL_BTN_CLOCK], *bt = &l.button[PL_BTN_BATTERY];

	CHECK_EQ(PL_BUTTONS, 4, "four buttons: backlight, volume, clock, battery");
	CHECK(cl->w >= 36 && cl->h == BAR && cl->y == 0, "the clock button is 36 px wide and the whole bar high");
	CHECK(bt->w >= 36 && bt->h == BAR && bt->y == 0, "so is the battery button");
	CHECK(cl->x == 0 && cl->w >= l.clock.w, "the clock button covers the clock");
	CHECK(bt->x <= l.battery.x && bt->x + bt->w == 240, "the battery button covers the battery to the edge");
	CHECK(inside(*cl, bar) && inside(*bt, bar), "both are in the bar");
	CHECK(!overlap(*cl, l.button[0]) && !overlap(*cl, l.button[1]), "the clock button touches no slider button");
	CHECK(!overlap(*bt, l.button[0]) && !overlap(*bt, l.button[1]), "nor does the battery button");
	CHECK(l.button[1].x + l.button[1].w <= bt->x, "the volume button ends where the battery begins");
	CHECK(inside(l.hl[PL_BTN_CLOCK], *cl) && inside(l.hl[PL_BTN_BATTERY], *bt),
		"the highlights are inside their buttons");
	CHECK(inside(l.hl[PL_BTN_CLOCK], l.clock) && inside(l.hl[PL_BTN_BATTERY], l.battery),
		"and inside the rectangles that are redrawn");
	CHECK(l.hl[PL_BTN_CLOCK].x + l.hl[PL_BTN_CLOCK].w >= PL_MARGIN + M15.clock_w + PL_HL_PAD,
		"the clock highlight has room around the text");
	CHECK(l.hl[PL_BTN_CLOCK].x <= PL_MARGIN - PL_HL_PAD, "on the left too");
	CHECK(l.hl[PL_BTN_BATTERY].x <= l.battery.x + l.battery.w - PL_MARGIN - M15.bat_text_w - PL_GAP -
		l.bat_icon_w - PL_HL_PAD, "the battery highlight starts before the icon");
	CHECK(l.hl[PL_BTN_BATTERY].x + l.hl[PL_BTN_BATTERY].w <= 240 - PL_HL_PAD + 0 &&
		l.hl[PL_BTN_BATTERY].x + l.hl[PL_BTN_BATTERY].w >= 240 - PL_MARGIN + PL_HL_PAD,
		"and ends after the text, short of the edge");
	CHECK(memcmp(l.button, o.button, sizeof(l.button)) == 0 && memcmp(l.hl, o.hl, sizeof(l.hl)) == 0,
		"opening the row moves none of the four");
	CHECK(o.text.x == PL_MARGIN && o.text.w == 240 - 2 * PL_MARGIN && inside(o.text, o.row_in),
		"the line of a text row is the row less its margins");

	/* Every width and bar height: a button is at least 36 px, and the clock
	 * button is never under another one. */
	static const int widths[] = { 240, 320, 480, 640 };
	static const int bars[] = { 18, 24, 40 };
	for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++)
		for (size_t j = 0; j < sizeof(bars) / sizeof(bars[0]); j++) {
			struct pl_layout w;
			pl_layout_compute(&w, widths[i], bars[j], false, false, &M15);
			CHECK(w.button[PL_BTN_CLOCK].w >= 36 && w.button[PL_BTN_BATTERY].w >= 36 &&
				w.button[PL_BTN_BATTERY].x + w.button[PL_BTN_BATTERY].w == widths[i],
				"clock and battery buttons keep 36 px at every size");
			CHECK_EQ(w.button[PL_BTN_CLOCK].h, bars[j], "and the whole bar height");
		}

	/* A clock narrower than a stylus still gets a 36 px button. */
	struct pl_metrics thin = { 5, 5, 5, false };
	struct pl_layout tl;
	pl_layout_compute(&tl, 240, BAR, false, false, &thin);
	CHECK(tl.clock.w < 36 && tl.button[PL_BTN_CLOCK].w == 36, "the clock button is widened to 36 px");

	/* Hits and taps. */
	struct pl_hit h = pl_hit_test(&l, 3, 0);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_BTN_CLOCK, "the corner of the clock button");
	h = pl_hit_test(&l, cl->w - 1, BAR - 1);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_BTN_CLOCK, "its last pixel");
	h = pl_hit_test(&l, cl->w, 5);
	CHECK(h.kind != PL_HIT_BUTTON || h.slider != PL_BTN_CLOCK, "right of it is not the clock");
	h = pl_hit_test(&l, 239, BAR - 1);
	CHECK(h.kind == PL_HIT_BUTTON && h.slider == PL_BTN_BATTERY, "the last pixel of the battery button");
	h = pl_hit_test(&l, bt->x - 1, 5);
	CHECK(h.kind != PL_HIT_BUTTON || h.slider != PL_BTN_BATTERY, "left of it is not the battery");

	pl_touch_press(&t, &l, PL_SLIDER_NONE, all, 10, 5, &out);
	CHECK(out.tap == PL_BTN_CLOCK && out.slider == PL_SLIDER_NONE, "a press on the clock taps it and sets nothing");
	CHECK_EQ(pl_touch_release(&t), PL_SLIDER_NONE, "and the release drags nothing");
	pl_touch_press(&t, &l, PL_SLIDER_NONE, all, 230, 5, &out);
	CHECK(out.tap == PL_BTN_BATTERY && out.slider == PL_SLIDER_NONE, "so does the battery");
	pl_touch_release(&t);
	pl_touch_press(&t, &l, PL_SLIDER_NONE, no_vol, 10, 5, &out);
	CHECK_EQ(out.tap, PL_BTN_CLOCK, "a missing mixer does not disable the clock");
	pl_touch_release(&t);

	/* In a text row nothing is a slider: a press does not drag, wherever it is,
	 * and the pointer moving over it does nothing either. */
	int mx = o.slider.track.x + o.slider.track.w / 2;
	for (int open = PL_BTN_CLOCK; open <= PL_BTN_BATTERY; open++) {
		pl_touch_press(&t, &o, open, all, mx, 45, &out);
		CHECK(out.slider == PL_SLIDER_NONE && out.tap == PL_SLIDER_NONE,
			"a press in the middle of a text row sets nothing");
		pl_touch_motion(&t, &o, mx + 20, &out);
		CHECK_EQ(out.slider, PL_SLIDER_NONE, "nor does moving in it");
		pl_touch_release(&t);
		pl_touch_press(&t, &o, open, all, o.slider.cell.x, 45, &out);
		CHECK_EQ(out.slider, PL_SLIDER_NONE, "nor does the edge of what would be the track");
		pl_touch_release(&t);
		pl_touch_press(&t, &o, open, all, 70, 10, &out);
		CHECK_EQ(out.tap, PL_SLIDER_NONE, "a press in the bar away from the buttons does nothing");
		pl_touch_release(&t);
	}
	pl_touch_press(&t, &o, PL_BTN_CLOCK, all, 10, 5, &out);
	CHECK_EQ(out.tap, PL_BTN_CLOCK, "the open clock button is tapped again, to close it");
	pl_touch_release(&t);

	/* Which row a button opens, and the shared open, switch and close. */
	CHECK_EQ(pl_row_kind_of(PL_SLIDER_BACKLIGHT), PL_ROW_SLIDER, "the sun opens a slider");
	CHECK_EQ(pl_row_kind_of(PL_SLIDER_VOLUME), PL_ROW_SLIDER, "the speaker too");
	CHECK_EQ(pl_row_kind_of(PL_BTN_CLOCK), PL_ROW_DATE, "the clock opens the date");
	CHECK_EQ(pl_row_kind_of(PL_BTN_BATTERY), PL_ROW_ESTIMATE, "the battery opens the estimate");
	CHECK_EQ(pl_row_kind_of(PL_SLIDER_NONE), PL_ROW_NONE, "none opens nothing");
	CHECK_EQ(pl_row_kind_of(PL_BUTTONS), PL_ROW_NONE, "and neither does an id that does not exist");
	CHECK_EQ(pl_row_kind_of(-7), PL_ROW_NONE, "or a negative one");

	struct pl_popup p = { .open = PL_SLIDER_NONE };
	CHECK_EQ(pl_popup_tap(&p, PL_BTN_CLOCK, 1000), PL_BTN_CLOCK, "a tap on the clock opens its row");
	CHECK_EQ(pl_popup_wait_ms(&p, 1000, false), 3000, "which closes by itself after 3 s as well");
	CHECK_EQ(pl_popup_tap(&p, PL_BTN_BATTERY, 1500), PL_BTN_BATTERY, "the battery switches it");
	CHECK_EQ(pl_popup_tap(&p, PL_SLIDER_VOLUME, 1600), PL_SLIDER_VOLUME, "a slider switches it again");
	CHECK_EQ(pl_popup_tap(&p, PL_BTN_CLOCK, 1700), PL_BTN_CLOCK, "and back to the clock");
	CHECK_EQ(pl_popup_tap(&p, PL_BTN_CLOCK, 1800), PL_SLIDER_NONE, "the same button closes");
	pl_popup_tap(&p, PL_BTN_BATTERY, 2000);
	pl_popup_touch(&p, 4000);
	CHECK(!pl_popup_expire(&p, 6999, false), "a touch in a text row keeps it open");
	CHECK(pl_popup_expire(&p, 7000, false), "and it closes 3 s after that");

	/* The date, from a table: no locale in it. */
	char buf[64];
	pl_date_text(buf, sizeof(buf), 2026, 9, 5, 1);
	CHECK_STR(buf, "Monday 5 October 2026", "the date row's example");
	pl_date_text(buf, sizeof(buf), 2023, 0, 1, 0);
	CHECK_STR(buf, "Sunday 1 January 2023", "Sunday, January: the first of each table");
	pl_date_text(buf, sizeof(buf), 2026, 11, 31, 6);
	CHECK_STR(buf, "Saturday 31 December 2026", "Saturday, December: the last of each");
	pl_date_text(buf, sizeof(buf), 2024, 1, 29, 4);
	CHECK_STR(buf, "Thursday 29 February 2024", "a leap day");
	pl_date_text(buf, sizeof(buf), 2026, 8, 30, 3);
	CHECK_STR(buf, "Wednesday 30 September 2026", "the longest names together");
	CHECK(strlen(buf) < 40, "and it fits the buffer the header promises");
	pl_date_text(buf, sizeof(buf), 2026, 9, 5, 2);
	CHECK_STR(buf, "Tuesday 5 October 2026", "a weekday is not mistaken for another");
	pl_date_text(buf, sizeof(buf), 2026, 9, 5, 3);
	CHECK_STR(buf, "Wednesday 5 October 2026", "Wednesday");
	pl_date_text(buf, sizeof(buf), 2026, 9, 5, 5);
	CHECK_STR(buf, "Friday 5 October 2026", "Friday");
	pl_date_text(buf, sizeof(buf), 2026, 2, 7, 1);
	CHECK_STR(buf, "Monday 7 March 2026", "March");
	pl_date_text(buf, sizeof(buf), 2026, 4, 7, 1);
	CHECK_STR(buf, "Monday 7 May 2026", "May");
	pl_date_text(buf, sizeof(buf), -5, 99, 0, -3);
	CHECK_STR(buf, "Sunday 1 December 1", "garbage is clamped, not printed");
	pl_date_text(buf, sizeof(buf), 99999, -1, 99, 70);
	CHECK_STR(buf, "Saturday 31 January 9999", "the other way");
	pl_date_text(buf, 10, 2026, 8, 30, 3);
	CHECK(strlen(buf) == 9, "a small buffer is cut, not overrun");
	pl_date_text(buf, 0, 2026, 8, 30, 3);

	/* Fitting: faces from the largest, candidates from the most informative. */
	const char *const one[] = { "Monday 5 October 2026" };	/* 21 characters */
	int face = -1;
	CHECK_EQ(pl_fit_choose(one, 1, 4, 300, fake_width, NULL, &face), 0, "it fits: the one candidate");
	CHECK_EQ(face, 0, "in the largest face");
	pl_fit_choose(one, 1, 4, 200, fake_width, NULL, &face);
	CHECK_EQ(face, 1, "too wide for the largest: the next one");
	pl_fit_choose(one, 1, 4, 21 * 6, fake_width, NULL, &face);
	CHECK_EQ(face, 2, "exactly the width is a fit");
	pl_fit_choose(one, 1, 4, 21 * 6 - 1, fake_width, NULL, &face);
	CHECK_EQ(face, 3, "a pixel less is not");
	int c = pl_fit_choose(one, 1, 4, 10, fake_width, NULL, &face);
	CHECK(c == 0 && face == 3, "nothing fits: the last candidate in the smallest face");
	const char *const many[] = { "73%  About 3 h 20 min left", "About 3 h 20 min left", "3 h 20 min left" };
	c = pl_fit_choose(many, 3, 4, 26 * 10, fake_width, NULL, &face);
	CHECK(c == 0 && face == 0, "the most informative candidate in the largest face");
	c = pl_fit_choose(many, 3, 4, 21 * 10, fake_width, NULL, &face);
	CHECK(c == 1 && face == 0, "dropping information before shrinking the face");
	c = pl_fit_choose(many, 3, 4, 15 * 10, fake_width, NULL, &face);
	CHECK(c == 2 && face == 0, "down to the shortest in the largest face");
	c = pl_fit_choose(many, 3, 4, 15 * 10 - 1, fake_width, NULL, &face);
	CHECK(c == 2 && face == 1, "and only then a smaller face, where the longest that fits wins again");
	c = pl_fit_choose(many, 3, 4, 26 * 8, fake_width, NULL, &face);
	CHECK(c == 2 && face == 0, "never a smaller face while a candidate fits the larger");
	c = pl_fit_choose(many, 0, 4, 100, fake_width, NULL, &face);
	CHECK(c == 0 && face == 3, "no candidates: no crash");
	c = pl_fit_choose(many, 3, 0, 100, fake_width, NULL, &face);
	CHECK(c == 2 && face == 0, "no faces: no crash");
}


/* ---- the battery estimate ---- */

#define SEC30 30000

static struct pl_batt_raw raw_of(enum pl_bat_status st, int pct, int charger)
{
	struct pl_batt_raw r;

	pl_batt_raw_clear(&r);
	r.st = st;
	r.pct = pct;
	r.charger = charger;
	return r;
}

/* n samples of r, 30 s apart, starting at t. Returns the time of the next. */
static int64_t feed(struct pl_est *e, const struct pl_batt_raw *r, int n, int64_t t)
{
	for (int i = 0; i < n; i++, t += SEC30)
		pl_est_add(e, r, t);
	return t;
}

static void est_text_is(const struct pl_estimate *e, int pct, int variant, const char *want,
	const char *msg)
{
	char buf[48];

	pl_est_text(buf, sizeof(buf), e, pct, variant);
	CHECK_STR(buf, want, msg);
}

static void test_estimator(void)
{
	struct pl_batt_raw r;
	struct pl_est e;
	struct pl_estimate o;

	/* The constants are named and the ones the wording depends on are what
	 * the design says. */
	CHECK_EQ(PL_EST_IDLE_UA, 10000, "idle floor 10 mA");
	CHECK_EQ(PL_EST_MAX_UA, 3000000, "ceiling 3 A");
	CHECK_EQ(PL_EST_WARM, 4, "four samples to warm up");
	CHECK_EQ(PL_EST_EMA_K, 8, "weight 1/8");
	CHECK_EQ(PL_EST_SKIP, 2, "two samples skipped after a change of direction");
	CHECK_EQ(PL_EST_GAP_MS, 600000, "a gap of 600 s starts again");

	pl_batt_raw_clear(&r);
	CHECK(r.st == PL_BAT_NONE && r.pct == -1 && r.charger == -1 && !r.not_charging, "clear: nothing");
	CHECK(r.charge_now == PL_ABSENT && r.charge_full == PL_ABSENT && r.charge_full_design == PL_ABSENT &&
		r.charge_empty == PL_ABSENT && r.energy_now == PL_ABSENT && r.energy_full == PL_ABSENT &&
		r.energy_full_design == PL_ABSENT && r.current_now == PL_ABSENT &&
		r.current_avg == PL_ABSENT && r.power_now == PL_ABSENT &&
		r.time_to_empty_now == PL_ABSENT, "clear: every attribute absent");

	/* ---- direction ---- */
	r = raw_of(PL_BAT_NONE, -1, -1);
	CHECK_EQ(pl_batt_dir(&r, PL_ABSENT), PL_DIR_NONE, "no battery: no direction");
	r.st = PL_BAT_AC;
	CHECK_EQ(pl_batt_dir(&r, 500000), PL_DIR_NONE, "AC only: none");
	r = raw_of(PL_BAT_FULL, 100, 1);
	CHECK_EQ(pl_batt_dir(&r, 500000), PL_DIR_FULL, "status Full is full, whatever the current");
	r = raw_of(PL_BAT_DISCHARGING, 50, 1);
	r.not_charging = true;
	CHECK_EQ(pl_batt_dir(&r, -500000), PL_DIR_NOTCHARGING, "status Not charging is that");
	r = raw_of(PL_BAT_CHARGING, 50, 1);
	r.not_charging = true;
	CHECK_EQ(pl_batt_dir(&r, 500000), PL_DIR_NOTCHARGING, "Not charging wins over a charging current");
	r = raw_of(PL_BAT_DISCHARGING, 50, 0);
	r.not_charging = true;
	CHECK_EQ(pl_batt_dir(&r, PL_ABSENT), PL_DIR_NOTCHARGING, "and over an offline charger");
	r = raw_of(PL_BAT_CHARGING, 50, 0);
	CHECK_EQ(pl_batt_dir(&r, 500000), PL_DIR_DISCHARGING, "no charger online: discharging, whatever status says");
	r = raw_of(PL_BAT_DISCHARGING, 50, 1);
	CHECK_EQ(pl_batt_dir(&r, 10001), PL_DIR_CHARGING, "charger online and above +10 mA: charging");
	CHECK_EQ(pl_batt_dir(&r, 10000), PL_DIR_NOTCHARGING, "exactly +10 mA is not above it: status decides");
	CHECK_EQ(pl_batt_dir(&r, -5001), PL_DIR_NOTCHARGING, "charger online and below -5 mA: not charging");
	CHECK_EQ(pl_batt_dir(&r, 0), PL_DIR_NOTCHARGING, "in between with status Discharging: not charging");
	r.st = PL_BAT_CHARGING;
	CHECK_EQ(pl_batt_dir(&r, 0), PL_DIR_CHARGING, "in between with status Charging: charging");
	CHECK_EQ(pl_batt_dir(&r, -5000), PL_DIR_CHARGING, "exactly -5 mA is in between");
	CHECK_EQ(pl_batt_dir(&r, -6000), PL_DIR_NOTCHARGING, "the current wins over a lagging status");
	CHECK_EQ(pl_batt_dir(&r, PL_ABSENT), PL_DIR_CHARGING, "no current: the status");
	r.st = PL_BAT_DISCHARGING;
	CHECK_EQ(pl_batt_dir(&r, PL_ABSENT), PL_DIR_NOTCHARGING, "online, no current, not charging: not charging");
	r.charger = -1;
	CHECK_EQ(pl_batt_dir(&r, 900000), PL_DIR_DISCHARGING, "no charger supply at all: the status");
	r.st = PL_BAT_CHARGING;
	CHECK_EQ(pl_batt_dir(&r, -900000), PL_DIR_CHARGING, "and the sign of the current means nothing");

	/* ---- the filter ---- */
	pl_est_init(&e);
	r = raw_of(PL_BAT_DISCHARGING, 50, 0);
	r.current_now = -100000;
	pl_est_add(&e, &r, 0);
	CHECK(e.n == 1 && e.ema == -100000 && e.dir == PL_DIR_DISCHARGING && e.kind == 1,
		"the first sample is the average");
	r.current_now = -200000;
	pl_est_add(&e, &r, SEC30);
	CHECK(e.n == 2 && e.ema == -150000, "the second: a running mean, signed");
	r.current_now = -300000;
	pl_est_add(&e, &r, 2 * SEC30);
	CHECK_EQ(e.ema, -200000, "the third: still the mean");
	pl_est_init(&e);
	r.current_now = -100000;
	feed(&e, &r, 8, 0);
	CHECK(e.n == 8 && e.ema == -100000, "eight of the same");
	r.current_now = -900000;
	pl_est_add(&e, &r, 8 * SEC30);
	CHECK_EQ(e.ema, -200000, "the ninth moves it by 1/8 of the difference");
	pl_est_add(&e, &r, 9 * SEC30);
	CHECK_EQ(e.ema, -200000 + (-900000 + 200000) / 8, "and so on, with weight 1/8 and no more");
	pl_est_init(&e);
	r.current_now = -99999;
	pl_est_add(&e, &r, 0);
	r.current_now = -100002;
	pl_est_add(&e, &r, SEC30);
	CHECK_EQ(e.ema, -99999 + (-3) / 2, "signed division truncates toward zero");

	/* current_avg is preferred, then current_now, then power_now. */
	pl_est_init(&e);
	r = raw_of(PL_BAT_DISCHARGING, 50, 0);
	r.current_now = -100000;
	r.current_avg = -300000;
	r.power_now = 1000000;
	pl_est_add(&e, &r, 0);
	CHECK(e.ema == -300000 && e.kind == 1, "current_avg first");
	pl_est_init(&e);
	r.current_avg = PL_ABSENT;
	pl_est_add(&e, &r, 0);
	CHECK(e.ema == -100000 && e.kind == 1, "then current_now");
	pl_est_init(&e);
	r.current_now = PL_ABSENT;
	pl_est_add(&e, &r, 0);
	CHECK(e.ema == 1000000 && e.kind == 2, "then power_now");
	pl_est_init(&e);
	r.power_now = PL_ABSENT;
	pl_est_add(&e, &r, 0);
	CHECK(e.n == 0 && e.kind == 0, "no rate attribute: nothing to filter");
	pl_est_add(&e, &r, SEC30);
	CHECK(e.n == 0, "and still nothing");

	/* Idle and glitch samples are not counted and do not move the average. */
	pl_est_init(&e);
	r = raw_of(PL_BAT_DISCHARGING, 50, 0);
	r.current_now = -200000;
	pl_est_add(&e, &r, 0);
	r.current_now = -9999;
	pl_est_add(&e, &r, SEC30);
	CHECK(e.n == 1 && e.ema == -200000, "9.999 mA is idle");
	r.current_now = 9999;
	pl_est_add(&e, &r, 2 * SEC30);
	CHECK(e.n == 1, "so is a positive one");
	r.current_now = 0;
	pl_est_add(&e, &r, 3 * SEC30);
	CHECK(e.n == 1, "so is zero");
	r.current_now = -10000;
	pl_est_add(&e, &r, 4 * SEC30);
	CHECK(e.n == 2, "exactly 10 mA counts");
	r.current_now = -3000001;
	pl_est_add(&e, &r, 5 * SEC30);
	CHECK(e.n == 2, "above 3 A is a glitch");
	r.current_now = -3000000;
	pl_est_add(&e, &r, 6 * SEC30);
	CHECK(e.n == 3, "exactly 3 A counts");
	pl_est_init(&e);
	r.current_now = PL_ABSENT;
	r.power_now = 39999;
	pl_est_add(&e, &r, 0);
	CHECK(e.n == 0, "a power below 40 mW is idle");
	r.power_now = 40000;
	pl_est_add(&e, &r, SEC30);
	CHECK(e.n == 1, "40 mW counts");
	r.power_now = 15000001;
	pl_est_add(&e, &r, 2 * SEC30);
	CHECK(e.n == 1, "a power above 15 W is a glitch");

	/* A change of direction starts again and ignores the next two samples. */
	pl_est_init(&e);
	r = raw_of(PL_BAT_DISCHARGING, 50, 0);
	r.current_now = -300000;
	int64_t t = feed(&e, &r, 3, 0);
	CHECK(e.n == 3 && e.dir == PL_DIR_DISCHARGING, "three samples discharging");
	r = raw_of(PL_BAT_CHARGING, 50, 1);
	r.current_now = 500000;
	pl_est_add(&e, &r, t);
	CHECK(e.n == 1 && e.ema == 500000 && e.dir == PL_DIR_CHARGING && e.skip == 2,
		"plugged in: a new run that starts with this sample");
	r.current_now = 900000;
	pl_est_add(&e, &r, t + SEC30);
	pl_est_add(&e, &r, t + 2 * SEC30);
	CHECK(e.n == 1 && e.ema == 500000 && e.skip == 0, "the next two are ignored");
	pl_est_add(&e, &r, t + 3 * SEC30);
	CHECK(e.n == 2 && e.ema == 700000, "the third counts");
	r.current_now = 5000;
	pl_est_add(&e, &r, t + 4 * SEC30);
	CHECK(e.n == 2, "an idle sample is no change of direction");
	/* Not charging and full leave the run alone. */
	r = raw_of(PL_BAT_FULL, 100, 1);
	r.current_now = 500000;
	pl_est_add(&e, &r, t + 5 * SEC30);
	CHECK(e.n == 2 && e.dir == PL_DIR_CHARGING, "a full battery adds nothing");
	r = raw_of(PL_BAT_CHARGING, 50, 1);
	r.current_now = -100000;
	pl_est_add(&e, &r, t + 6 * SEC30);
	CHECK(e.n == 2, "nor does a battery that is not charging");

	/* A gap starts again; exactly 600 s does not. */
	pl_est_init(&e);
	r = raw_of(PL_BAT_DISCHARGING, 50, 0);
	r.current_now = -300000;
	pl_est_add(&e, &r, 1000);
	pl_est_add(&e, &r, 1000 + 600000);
	CHECK_EQ(e.n, 2, "600 s apart is one run");
	pl_est_add(&e, &r, 1000 + 600000 + 600001);
	CHECK_EQ(e.n, 1, "600.001 s apart is not");
	pl_est_add(&e, &r, 10);
	CHECK_EQ(e.n, 1, "a clock that went back starts again too");
	r.current_now = -1000;
	pl_est_add(&e, &r, 10 + 700000);
	CHECK_EQ(e.n, 0, "an idle sample after a gap leaves nothing");
	/* A battery that changes the rate attribute it has. */
	pl_est_init(&e);
	r = raw_of(PL_BAT_DISCHARGING, 50, 0);
	r.current_now = -300000;
	feed(&e, &r, 3, 0);
	r.current_now = PL_ABSENT;
	r.power_now = 2000000;
	pl_est_add(&e, &r, 3 * SEC30);
	CHECK(e.n == 1 && e.kind == 2 && e.ema == 2000000, "from a current to a power: starts again");
	/* No battery adds nothing. */
	r = raw_of(PL_BAT_NONE, -1, -1);
	pl_est_add(&e, &r, 4 * SEC30);
	r = raw_of(PL_BAT_AC, -1, 1);
	r.current_now = -300000;
	pl_est_add(&e, &r, 5 * SEC30);
	CHECK_EQ(e.n, 1, "AC and none add nothing");

	/* ---- the estimate ---- */
	r = raw_of(PL_BAT_NONE, -1, -1);
	pl_est_init(&e);
	CHECK_EQ(pl_est_compute(&e, &r, 0).kind, PL_EST_NONE, "no battery: --");
	r = raw_of(PL_BAT_AC, -1, 1);
	CHECK_EQ(pl_est_compute(&e, &r, 0).kind, PL_EST_AC, "mains and no battery: AC");

	/* The DS2760: charge in uAh with a reserve below charge_empty, current in
	 * uA negative while discharging, no current_avg, a time_to_empty_now. */
	struct pl_batt_raw ds = raw_of(PL_BAT_DISCHARGING, 94, 0);
	ds.charge_now = 950000;
	ds.charge_empty = 50000;
	ds.charge_full = 1000000;
	ds.charge_full_design = 1000000;
	ds.current_now = -600000;
	ds.time_to_empty_now = 5400;
	pl_est_init(&e);
	t = feed(&e, &ds, 1, 0);
	o = pl_est_compute(&e, &ds, 0);
	CHECK(o.kind == PL_EST_LEFT && o.hint && o.minutes == 90, "cold start: the kernel's seconds as a hint, 5400 s are 90 min");
	feed(&e, &ds, 2, t);
	o = pl_est_compute(&e, &ds, 0);
	CHECK(o.kind == PL_EST_LEFT && o.hint, "three samples: still the hint");
	t = feed(&e, &ds, 1, t + 2 * SEC30);
	o = pl_est_compute(&e, &ds, 0);
	CHECK(o.kind == PL_EST_LEFT && !o.hint && o.minutes == 90,
		"four samples: our own number, (950000 - 50000) uAh at 600 mA is 90 min");
	ds.charge_empty = PL_ABSENT;
	o = pl_est_compute(&e, &ds, 0);
	CHECK(o.kind == PL_EST_LEFT && o.minutes == 95, "without charge_empty nothing is subtracted: 95 min");
	ds.charge_empty = 0;
	CHECK_EQ(pl_est_compute(&e, &ds, 0).minutes, 95, "charge_empty 0 is the same");
	ds.charge_empty = 960000;
	o = pl_est_compute(&e, &ds, 0);
	CHECK(o.kind == PL_EST_ESTIMATING, "charge_now below charge_empty: nothing usable left, never 0 min");
	ds.charge_empty = 50000;
	ds.time_to_empty_now = PL_ABSENT;
	ds.charge_now = PL_ABSENT;
	o = pl_est_compute(&e, &ds, 0);
	CHECK(o.kind == PL_EST_LEFT && o.minutes == 94 * 1000000 / 100 * 60 / 600000,
		"no charge_now: capacity times charge_full (94 percent of 1000000 uAh is 94 min)");
	ds.charge_full = PL_ABSENT;
	CHECK_EQ(pl_est_compute(&e, &ds, 0).minutes, 94, "then charge_full_design");
	ds.charge_full_design = PL_ABSENT;
	CHECK_EQ(pl_est_compute(&e, &ds, 0).kind, PL_EST_ESTIMATING, "no charge at all and no --battery-mah: unknown");
	o = pl_est_compute(&e, &ds, 1000);
	CHECK(o.kind == PL_EST_LEFT && o.minutes == 94, "--battery-mah 1000 is the last resort: 94 percent of 1000 mAh");
	o = pl_est_compute(&e, &ds, 2000);
	CHECK_EQ(o.minutes, 188, "and it scales");
	ds.pct = -1;
	CHECK_EQ(pl_est_compute(&e, &ds, 2000).kind, PL_EST_ESTIMATING, "no capacity either: unknown");
	ds.pct = 94;
	ds.charge_now = 950000;
	ds.time_to_empty_now = PL_ABSENT;
	o = pl_est_compute(&e, &ds, 0);
	CHECK(o.kind == PL_EST_LEFT && !o.hint && o.minutes == 90, "back to charge_now: no hint without time_to_empty_now");

	/* Hints: seconds, positive, under 24 h and at least a minute. */
	struct pl_est cold;
	pl_est_init(&cold);
	ds.time_to_empty_now = 0;
	CHECK_EQ(pl_est_compute(&cold, &ds, 0).kind, PL_EST_ESTIMATING, "time_to_empty_now 0 is unknown");
	ds.time_to_empty_now = 59;
	CHECK_EQ(pl_est_compute(&cold, &ds, 0).kind, PL_EST_ESTIMATING, "under a minute is not shown");
	ds.time_to_empty_now = 60;
	o = pl_est_compute(&cold, &ds, 0);
	CHECK(o.kind == PL_EST_LEFT && o.hint && o.minutes == 1, "a minute is");
	ds.time_to_empty_now = 86399;
	CHECK_EQ(pl_est_compute(&cold, &ds, 0).minutes, 1439, "just under 24 h is");
	ds.time_to_empty_now = 86400;
	CHECK_EQ(pl_est_compute(&cold, &ds, 0).kind, PL_EST_ESTIMATING, "24 h is not");
	ds.time_to_empty_now = 5400;
	ds.st = PL_BAT_CHARGING;
	ds.charger = 1;
	ds.current_now = 400000;
	ds.pct = 50;
	CHECK_EQ(pl_est_compute(&cold, &ds, 0).kind, PL_EST_ESTIMATING, "no hint while charging: it is a time to empty");
	ds.pct = 94;
	ds.st = PL_BAT_DISCHARGING;
	ds.charger = 0;
	ds.current_now = -600000;
	ds.time_to_empty_now = PL_ABSENT;

	/* The rate decides: more current, less time. */
	struct pl_est fast;
	pl_est_init(&fast);
	ds.current_now = -1200000;
	feed(&fast, &ds, 4, 0);
	CHECK_EQ(pl_est_compute(&fast, &ds, 0).minutes, 45, "at 1.2 A: 900000 uAh is 45 min");
	ds.current_now = -600000;
	/* Idle: no current to speak of, so no number. */
	struct pl_est idle;
	pl_est_init(&idle);
	ds.current_now = -5000;
	feed(&idle, &ds, 6, 0);
	CHECK_EQ(pl_est_compute(&idle, &ds, 0).kind, PL_EST_ESTIMATING, "idle: estimating, not a fake number");
	ds.current_now = PL_ABSENT;
	pl_est_init(&idle);
	feed(&idle, &ds, 6, 0);
	CHECK_EQ(pl_est_compute(&idle, &ds, 0).kind, PL_EST_ESTIMATING, "no current attribute: estimating");
	ds.time_to_empty_now = 5400;
	o = pl_est_compute(&idle, &ds, 0);
	CHECK(o.kind == PL_EST_LEFT && o.hint, "with the kernel's time as the only source it stays a hint");
	ds.time_to_empty_now = PL_ABSENT;
	ds.current_now = -600000;

	/* A run that is not the direction the battery has now. */
	struct pl_batt_raw chg = ds;
	chg.st = PL_BAT_CHARGING;
	chg.charger = 1;
	chg.current_now = 400000;
	chg.pct = 50;
	chg.charge_now = 500000;
	chg.charge_full = 1000000;
	chg.charge_full_design = 1000000;
	struct pl_batt_raw chg2 = chg;
	chg2.charger = -1;
	pl_est_init(&e);
	feed(&e, &ds, 5, 0);
	CHECK_EQ(pl_est_compute(&e, &chg2, 0).kind, PL_EST_ESTIMATING, "a discharging run says nothing about charging");

	/* Charging: time to full from charge_full - charge_now. */
	struct pl_est ce;
	pl_est_init(&ce);
	t = feed(&ce, &chg, 3, 0);
	CHECK_EQ(pl_est_compute(&ce, &chg, 0).kind, PL_EST_ESTIMATING, "charging: three samples are not enough");
	feed(&ce, &chg, 1, t);
	o = pl_est_compute(&ce, &chg, 0);
	CHECK(o.kind == PL_EST_TOFULL && o.minutes == 75, "500000 uAh to go at 400 mA is 75 min");
	chg.pct = 89;
	CHECK_EQ(pl_est_compute(&ce, &chg, 0).kind, PL_EST_TOFULL, "89 percent still has a time");
	chg.pct = 90;
	CHECK_EQ(pl_est_compute(&ce, &chg, 0).kind, PL_EST_CHARGING, "90 percent: just Charging, the taper makes the time optimistic");
	chg.pct = 100;
	CHECK_EQ(pl_est_compute(&ce, &chg, 0).kind, PL_EST_CHARGING, "100 percent and still charging: Charging");
	chg.pct = 50;
	chg.charge_now = PL_ABSENT;
	CHECK_EQ(pl_est_compute(&ce, &chg, 0).minutes, 75, "no charge_now: the percentage of charge_full");
	chg.charge_full = PL_ABSENT;
	CHECK_EQ(pl_est_compute(&ce, &chg, 0).minutes, 75, "then charge_full_design");
	chg.charge_full_design = PL_ABSENT;
	CHECK_EQ(pl_est_compute(&ce, &chg, 0).kind, PL_EST_ESTIMATING, "no full: unknown");
	CHECK_EQ(pl_est_compute(&ce, &chg, 1000).minutes, 75, "--battery-mah 1000: half of it to go, 75 min");
	chg.charge_now = 1500000;
	chg.charge_full = 1000000;
	o = pl_est_compute(&ce, &chg, 0);
	CHECK(o.kind == PL_EST_ESTIMATING, "charge_now past charge_full at 50 percent: nothing to go, never 0 min");
	chg.charge_now = 500000;
	chg.charge_full = 1000000;
	chg.current_now = 400000;
	/* The charger is on and the battery gives current: not charging. */
	struct pl_batt_raw nc = chg;
	nc.current_now = -300000;
	struct pl_est ne;
	pl_est_init(&ne);
	feed(&ne, &nc, 5, 0);
	CHECK_EQ(pl_est_compute(&ne, &nc, 0).kind, PL_EST_NOTCHARGING, "on the charger and giving current: Not charging, no time");
	nc.current_now = 400000;
	nc.not_charging = true;
	CHECK_EQ(pl_est_compute(&ne, &nc, 0).kind, PL_EST_NOTCHARGING, "status Not charging");
	nc.not_charging = false;
	nc.st = PL_BAT_FULL;
	pl_est_init(&ne);
	CHECK_EQ(pl_est_compute(&ne, &nc, 0).kind, PL_EST_FULL, "status Full is Fully charged without any sample");

	/* A generic battery: current_avg and energy. */
	struct pl_batt_raw gen = raw_of(PL_BAT_DISCHARGING, 60, 0);
	struct pl_est ge;
	pl_est_init(&ge);
	gen.current_avg = -300000;
	gen.current_now = -900000;
	gen.charge_now = 900000;
	feed(&ge, &gen, 4, 0);
	CHECK_EQ(pl_est_compute(&ge, &gen, 0).minutes, 180, "current_avg is the rate: 900000 uAh at 300 mA is 180 min");
	gen.current_avg = PL_ABSENT;
	gen.current_now = PL_ABSENT;
	gen.power_now = 2000000;
	gen.charge_now = PL_ABSENT;
	gen.energy_now = 6000000;
	pl_est_init(&ge);
	feed(&ge, &gen, 4, 0);
	CHECK_EQ(pl_est_compute(&ge, &gen, 0).minutes, 180, "power_now and energy_now: 6 Wh at 2 W is 180 min");
	gen.energy_now = PL_ABSENT;
	gen.energy_full = 10000000;
	CHECK_EQ(pl_est_compute(&ge, &gen, 0).minutes, 180, "no energy_now: 60 percent of energy_full");
	gen.energy_full = PL_ABSENT;
	gen.energy_full_design = 10000000;
	CHECK_EQ(pl_est_compute(&ge, &gen, 0).minutes, 180, "then energy_full_design");
	gen.energy_full_design = PL_ABSENT;
	gen.charge_full = 1000000;
	CHECK_EQ(pl_est_compute(&ge, &gen, 1000).kind, PL_EST_ESTIMATING,
		"charge cannot be divided by a power: unknown, not a wrong number");

	/* Over 24 h; under a minute. */
	struct pl_batt_raw big = raw_of(PL_BAT_DISCHARGING, 90, 0);
	struct pl_est be;
	pl_est_init(&be);
	big.current_now = -10000;
	big.charge_now = 3000000;
	feed(&be, &big, 4, 0);
	CHECK_EQ(pl_est_compute(&be, &big, 0).kind, PL_EST_OVER_LEFT, "3000 mAh at 10 mA is over 24 h");
	big.charge_now = 240000;
	CHECK_EQ(pl_est_compute(&be, &big, 0).minutes, 1440, "exactly 24 h is still a time");
	big.charge_now = 240001;
	CHECK_EQ(pl_est_compute(&be, &big, 0).minutes, 1440, "and a minute's fraction does not tip it");
	big.charge_now = 241000;
	CHECK_EQ(pl_est_compute(&be, &big, 0).kind, PL_EST_OVER_LEFT, "above it does");
	big.charge_now = 100;
	CHECK_EQ(pl_est_compute(&be, &big, 0).kind, PL_EST_ESTIMATING, "under a minute is never shown as 0");
	big.charge_now = 10000;
	CHECK_EQ(pl_est_compute(&be, &big, 0).minutes, 60, "10 mA for 10 mAh is an hour");
	struct pl_batt_raw bigc = big;
	bigc.st = PL_BAT_CHARGING;
	bigc.charger = 1;
	bigc.current_now = 10000;
	bigc.pct = 10;
	bigc.charge_now = 100000;
	bigc.charge_full = 3000000;
	pl_est_init(&be);
	feed(&be, &bigc, 4, 0);
	CHECK_EQ(pl_est_compute(&be, &bigc, 0).kind, PL_EST_OVER_FULL, "a charge that takes more than 24 h");

	/* Garbage: nothing crashes or prints nonsense. */
	struct pl_batt_raw junk = raw_of(PL_BAT_DISCHARGING, 500, 7);
	junk.charge_now = INT64_MAX / 100;
	junk.current_now = -3000000;
	junk.charge_full = 5;
	pl_est_init(&e);
	feed(&e, &junk, 5, 0);
	o = pl_est_compute(&e, &junk, 0);
	CHECK(o.kind == PL_EST_OVER_LEFT || o.kind == PL_EST_LEFT || o.kind == PL_EST_ESTIMATING,
		"absurd charge: some answer");
	est_text_is(&o, junk.pct, 0, "100%  > 24 h left", "a capacity of 500 is shown as 100 and a huge time as > 24 h");
	junk.pct = -50;
	junk.charge_now = -5;
	o = pl_est_compute(&e, &junk, 0);
	CHECK(o.kind != PL_EST_LEFT || o.minutes >= 1, "negative values: no negative or zero time");
	junk.charge_now = PL_ABSENT;
	o = pl_est_compute(&e, &junk, 100000);
	CHECK(o.kind == PL_EST_ESTIMATING, "a negative capacity is not a percentage");
	junk.current_now = 3000001;
	pl_est_init(&e);
	feed(&e, &junk, 5, 0);
	CHECK_EQ(e.n, 0, "a 3.000001 A sample of either sign is dropped");

	/* Every subset of the attributes gives some answer. */
	int bad = 0;
	for (unsigned mask = 0; mask < (1u << 12); mask++) {
		struct pl_batt_raw m = raw_of(mask & 1 ? PL_BAT_CHARGING : PL_BAT_DISCHARGING, mask & 2 ? 70 : -1,
			(int)((mask >> 2) % 3) - 1);
		int64_t *f[] = { &m.charge_now, &m.charge_full, &m.charge_full_design, &m.charge_empty,
			&m.energy_now, &m.current_now, &m.current_avg, &m.power_now, &m.time_to_empty_now };
		static const int64_t val[] = { 800000, 1000000, 1100000, 50000, 5000000, -500000, -400000,
			1500000, 3600 };
		for (int i = 0; i < 9; i++)
			if ((mask >> 3) >> i & 1)
				*f[i] = val[i];
		struct pl_est me;
		pl_est_init(&me);
		feed(&me, &m, 6, 0);
		struct pl_estimate mo = pl_est_compute(&me, &m, mask & 4 ? 1000 : 0);
		char tb[48];
		for (int v = 0; v < PL_EST_VARIANTS; v++) {
			pl_est_text(tb, sizeof(tb), &mo, m.pct, v);
			if (!tb[0])
				bad++;
		}
		if ((mo.kind == PL_EST_LEFT || mo.kind == PL_EST_TOFULL) && mo.minutes < 1)
			bad++;
	}
	CHECK_EQ(bad, 0, "every subset of the attributes: a sensible answer");

	/* ---- rounding and hysteresis ---- */
	CHECK_EQ(pl_est_round_min(-4), 0, "rounding: negative is 0");
	CHECK_EQ(pl_est_round_min(0), 0, "0");
	CHECK_EQ(pl_est_round_min(1), 1, "to the minute below 10: 1");
	CHECK_EQ(pl_est_round_min(9), 9, "9");
	CHECK_EQ(pl_est_round_min(10), 10, "10");
	CHECK_EQ(pl_est_round_min(12), 10, "to 5 min from 10: 12 is 10");
	CHECK_EQ(pl_est_round_min(13), 15, "13 is 15");
	CHECK_EQ(pl_est_round_min(59), 60, "59 is 60");
	CHECK_EQ(pl_est_round_min(90), 90, "90");
	CHECK_EQ(pl_est_round_min(297), 295, "297 is 295");
	CHECK_EQ(pl_est_round_min(298), 300, "298 is 300");
	CHECK_EQ(pl_est_round_min(299), 300, "299 is 300");
	CHECK_EQ(pl_est_round_min(300), 300, "300 (5 h)");
	CHECK_EQ(pl_est_round_min(307), 300, "to 15 min from 5 h: 307 is 300");
	CHECK_EQ(pl_est_round_min(308), 315, "308 is 315");
	CHECK_EQ(pl_est_round_min(1440), 1440, "1440");
	CHECK_EQ(pl_est_hysteresis(0, 77), 77, "nothing shown yet: the new value");
	CHECK_EQ(pl_est_hysteresis(100, 112), 100, "100 min shown: 12 away stays (limit 12)");
	CHECK_EQ(pl_est_hysteresis(100, 113), 113, "13 away moves");
	CHECK_EQ(pl_est_hysteresis(100, 88), 100, "12 below stays");
	CHECK_EQ(pl_est_hysteresis(100, 87), 87, "13 below moves");
	CHECK_EQ(pl_est_hysteresis(20, 23), 20, "20 min: limit is 3, 3 away stays");
	CHECK_EQ(pl_est_hysteresis(20, 24), 24, "4 away moves");
	CHECK_EQ(pl_est_hysteresis(10, 13), 10, "10 min: 3 away stays");
	CHECK_EQ(pl_est_hysteresis(10, 14), 14, "4 away moves");
	CHECK_EQ(pl_est_hysteresis(9, 10), 9, "under 10 min: 1 away stays");
	CHECK_EQ(pl_est_hysteresis(9, 11), 11, "2 away moves");
	CHECK_EQ(pl_est_hysteresis(5, 4), 5, "downwards too");
	CHECK_EQ(pl_est_hysteresis(5, 3), 3, "2 below moves");

	/* ---- the text ---- */
	struct pl_estimate l90 = { PL_EST_LEFT, 90, false };
	est_text_is(&l90, 73, 0, "73%  1 h 30 min left", "text: discharging with the percentage");
	est_text_is(&l90, 73, 1, "1 h 30 min left", "without it");
	est_text_is(&l90, 73, 2, "73%  1 h 30 min", "shortened");
	est_text_is(&l90, 73, 3, "1 h 30 min", "both");
	est_text_is(&l90, -1, 0, "1 h 30 min left", "no percentage to show");
	struct pl_estimate e45 = { PL_EST_LEFT, 45, false };
	est_text_is(&e45, 73, 0, "73%  45 min left", "under an hour: minutes only");
	struct pl_estimate e185 = { PL_EST_LEFT, 185, false };
	est_text_is(&e185, 5, 0, "5%  3 h 05 min left", "the minutes are zero-padded");
	struct pl_estimate e180 = { PL_EST_LEFT, 180, false };
	est_text_is(&e180, 5, 0, "5%  3 h 00 min left", "even when they are 00");
	struct pl_estimate e301 = { PL_EST_LEFT, 301, false };
	est_text_is(&e301, 5, 0, "5%  5 h 00 min left", "rounded to 15 min above 5 h");
	struct pl_estimate e59 = { PL_EST_LEFT, 59, false };
	est_text_is(&e59, 5, 0, "5%  1 h 00 min left", "59 min rounds up to an hour and is shown as one");
	struct pl_estimate e5 = { PL_EST_LEFT, 5, false };
	est_text_is(&e5, 50, 0, "50%  5 min left", "5 min");
	struct pl_estimate e1 = { PL_EST_LEFT, 1, false };
	est_text_is(&e1, 50, 0, "50%  1 min left", "1 min is the least");
	struct pl_estimate hint = { PL_EST_LEFT, 80, true };
	est_text_is(&hint, 73, 0, "73%  ~1 h 20 min left", "a hint has a tilde");
	est_text_is(&hint, 73, 3, "~1 h 20 min", "also shortened");
	struct pl_estimate tf = { PL_EST_TOFULL, 75, false };
	est_text_is(&tf, 47, 0, "47%  1 h 15 min to full", "charging");
	est_text_is(&tf, 47, 3, "1 h 15 min", "charging, shortened");
	struct pl_estimate tf20 = { PL_EST_TOFULL, 20, false };
	est_text_is(&tf20, 47, 1, "20 min to full", "20 min to full");
	struct pl_estimate ch = { PL_EST_CHARGING, 0, false };
	est_text_is(&ch, 95, 0, "95%  Charging", "charging above the taper");
	struct pl_estimate fu = { PL_EST_FULL, 0, false };
	est_text_is(&fu, 100, 0, "100%  Fully charged", "full");
	struct pl_estimate nc2 = { PL_EST_NOTCHARGING, 0, false };
	est_text_is(&nc2, 80, 0, "80%  Not charging", "not charging");
	struct pl_estimate es = { PL_EST_ESTIMATING, 0, false };
	est_text_is(&es, 73, 0, "73%  Estimating...", "estimating");
	est_text_is(&es, 73, 3, "Estimating...", "estimating, shortest");
	struct pl_estimate no = { PL_EST_NONE, 0, false };
	est_text_is(&no, -1, 0, "--", "no data");
	est_text_is(&no, 50, 0, "--", "no data has no percentage");
	struct pl_estimate ac = { PL_EST_AC, 0, false };
	est_text_is(&ac, -1, 0, "On AC power", "mains");
	est_text_is(&ac, 50, 0, "On AC power", "mains has no percentage");
	struct pl_estimate ovl = { PL_EST_OVER_LEFT, 0, false };
	est_text_is(&ovl, 73, 0, "73%  > 24 h left", "over a day");
	est_text_is(&ovl, 73, 3, "> 24 h", "shortened");
	struct pl_estimate ovf = { PL_EST_OVER_FULL, 0, false };
	est_text_is(&ovf, 10, 0, "10%  > 24 h to full", "over a day to full");
	est_text_is(&es, 250, 0, "100%  Estimating...", "a percentage is clamped to 100");
	est_text_is(&es, -9, 0, "Estimating...", "a negative one is not shown");
	struct pl_estimate neg = { PL_EST_LEFT, -30, false };
	est_text_is(&neg, 50, 3, "0 min", "garbage minutes print as 0, not a negative time");
	char tiny[6];
	pl_est_text(tiny, sizeof(tiny), &l90, 73, 0);
	CHECK_EQ(strlen(tiny), 5, "a small buffer is cut, not overrun");
	for (int k = PL_EST_NONE; k <= PL_EST_OVER_FULL; k++) {
		struct pl_estimate any = { k, 125, k % 2 };
		for (int v = 0; v < PL_EST_VARIANTS; v++) {
			char b[48];
			pl_est_text(b, sizeof(b), &any, 55, v);
			CHECK(b[0] && strlen(b) < 40, "every kind and variant has a line that fits the buffer");
		}
	}
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

#define NOFONT_PATH "/nonexistent/none.ttf"

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
	/* The text rows: every capital, the comma and the full stop. */
	int caps_ok = 1, caps_distinct = 1;
	for (char a = 'A'; a <= 'Z'; a++) {
		const uint8_t *g = pl_font_glyph(a);
		if (!g)
			caps_ok = 0;
		for (char b = a + 1; g && b <= 'Z'; b++)
			if (pl_font_glyph(b) && !memcmp(g, pl_font_glyph(b), 7))
				caps_distinct = 0;
	}
	CHECK(caps_ok, "the bitmap font has every capital");
	CHECK(caps_distinct, "and they all differ");
	CHECK(pl_font_glyph(',') && pl_font_glyph('.'), "the bitmap font has a comma and a full stop");
	CHECK(pl_font_glyph('>') && pl_font_glyph('~'), "and the > of the over-24-h estimate and the ~ of the warm-up hint");
	CHECK(memcmp(pl_font_glyph('>'), pl_font_glyph('~'), 7), "which differ");
	CHECK(memcmp(pl_font_glyph(','), pl_font_glyph('.'), 7), "which differ");
	{
		int px[PL_ROW_FACES];
		pl_row_face_px(11, px);
		CHECK_EQ(px[0], 14, "text rows: 4/3 of the 11 px bar text");
		CHECK(px[1] < px[0] && px[2] < px[1] && px[3] < px[2], "the steps get smaller");
		CHECK(px[3] >= 8, "and stay legible");
		pl_row_face_px(48, px);
		CHECK_EQ(px[0], 26, "a huge bar text is capped to fit the row");
		pl_row_face_px(1, px);
		CHECK(px[0] == 8 && px[1] == 8 && px[3] == 8, "a tiny one is floored at 8");
		for (int i = 1; i < PL_ROW_FACES; i++)
			CHECK(px[i] <= px[i - 1], "steps never grow");
	}
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
		/* The text rows: letters exist, and each step is narrower. */
		const char *line = "Wednesday 30 September 2026";
		int prev = 100000;
		for (int fc = PL_FACE_ROW; fc < PL_FACES; fc++) {
			int w = pl_font_text_w(&f, fc, line);
			CHECK(w > 0 && w < prev, "a text row face is narrower than the one before");
			prev = w;
		}
		CHECK(pl_font_text_w(&f, PL_FACE_ROW, "g") > 0 && pl_font_text_w(&f, PL_FACE_ROW, ",.") > 0,
			"lower case letters and punctuation have advances");
		CHECK(pl_font_text_w(&f, PL_FACE_ROW, "ab") ==
			pl_font_text_w(&f, PL_FACE_ROW, "a") + pl_font_text_w(&f, PL_FACE_ROW, "b"),
			"text width is the sum of the advances");
		CHECK(f.face[PL_FACE_ROW].g[40].m.a != NULL, "a lower case glyph has a coverage mask");
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
	/* Bitmap rows: the scale steps down from 2 to 1, lower case is drawn. */
	pl_font_load(&f, NOFONT_PATH, 11, 10);
	CHECK(pl_font_text_w(&f, PL_FACE_ROW, "Monday") > pl_font_text_w(&f, PL_FACES - 1, "Monday"),
		"bitmap text rows shrink from scale 2 to 1");
	pl_font_free(&f);
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
	pl_layout_compute(&e->l, 240, BAR, row, false, &m);
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
	CHECK_EQ(px(&e.b, 80, BAR - 1), pl_pixel(c, PL_COL_LINE, bar_alpha), "the bar's line, 1 px, at the bottom");
	CHECK_EQ(px(&e.b, 80, BAR - 2), pl_pixel(c, PL_COL_BG, bar_alpha), "and the bar above it");
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
	/* The icon is 12 px at the default bar: the solid centre of the speaker is
	 * about 10 pixels, the rest is anti-aliased. */
	CHECK(count_color(&e.b, b1, PL_COL_FG) > 8, "the other icon is in the text colour");
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
	CHECK_EQ(cl.b.c.h, BAR, "the closed canvas is the bar");
	CHECK_EQ(count_color(&cl.b, cl.l.hl[0], PL_COL_HL), 0, "nothing is highlighted");
	CHECK_EQ(px(&cl.b, 80, 2), pl_pixel(&cl.b.c, PL_COL_BG, bar_alpha), "bar background");
	CHECK(count_color(&cl.b, pl_button_rect(&cl.l, 0), PL_COL_FG) > 15, "idle icons are in the text colour");
	env_free(&cl);

	/* A widget redraw touches its own rectangle only. */
	const uint32_t sent = pl_pixel(c, 0x123456, 255);
	struct pl_rect whole = { 0, 0, 240, l->h };
	pl_fill(c, whole, 0x123456, 255);
	pl_render_clock(c, l, &e.a, &e.st, false);
	CHECK_EQ(count_value(&e.b, l->battery, sent), l->battery.w * l->battery.h, "clock redraw leaves the battery alone");
	CHECK_EQ(count_value(&e.b, b0, sent), b0.w * b0.h, "clock redraw leaves the buttons alone");
	CHECK_EQ(count_value(&e.b, l->row, sent), l->row.w * l->row.h, "clock redraw leaves the row alone");
	CHECK_EQ(count_value(&e.b, l->clock, sent), 0, "the clock redraw clears its background");
	pl_fill(c, whole, 0x123456, 255);
	pl_render_battery(c, l, &e.a, &e.st, false);
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
	CHECK_EQ(count_value(&e.b, (struct pl_rect){ 0, 0, 240, BAR }, sent), 240 * BAR, "and the bar");
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
	pl_layout_compute(&e.l, 240, BAR, true, true, &m);
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
	CHECK_EQ(px(&e.b, 80, SURF - 1), pl_pixel(c, PL_COL_BG, 255), "down to the edge of the screen");
	CHECK_EQ(px(&e.b, 80, 0), pl_pixel(c, PL_COL_ROW_LINE, 224), "the row's line is its top edge");
	CHECK_EQ(px(&e.b, 3, 1), pl_pixel(c, PL_COL_ROW, 224), "the row under it");
	CHECK_EQ(px(&e.b, 3, 35), pl_pixel(c, PL_COL_ROW, 224), "to the bar");
	CHECK(count_color(&e.b, pl_button_rect(l, 1), PL_COL_ACCENT) > 6, "the open button is highlighted in the bar");
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
	pl_render_battery(c, l, &e.a, &e.st, false);
	int full = count_color(&e.b, l->battery, PL_COL_BAT_FILL);
	e.st.bat_pct = 40;
	pl_render_battery(c, l, &e.a, &e.st, false);
	int mid = count_color(&e.b, l->battery, PL_COL_BAT_FILL);
	e.st.bat_pct = 20;
	pl_render_battery(c, l, &e.a, &e.st, false);
	int low = count_color(&e.b, l->battery, PL_COL_BAT_FILL);
	CHECK(full > mid && mid > low && low > 0, "the fill is proportional to the capacity");
	CHECK(full > 30, "a full battery has a good amount of fill");
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_LOW), 0, "20 percent is not red");
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_EMPTY) > 5, "the empty part has a ground of its own");
	e.st.bat_pct = 15;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_LOW) > 0, "15 percent is red");
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_FILL), 0, "and not grey");
	e.st.bat_pct = 10;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_LOW) > 0, "a nearly empty battery is red");
	e.st.bat_pct = 0;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_LOW), 0, "an empty battery has no fill");
	CHECK(count_color(&e.b, l->battery, PL_COL_FG) > 20, "but an outline and the text");
	e.st.bat_pct = 73;
	e.st.bat = PL_BAT_DISCHARGING;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BOLT), 0, "no charging marker while discharging");
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_CHARGING), 0, "and no green");
	e.st.bat = PL_BAT_CHARGING;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK(mask_sum(&e.a.bolt) > 255 * 4, "the bolt has substance");
	{
		struct canvas_buf was;
		canvas_init(&was, 240, l->h, PL_FMT_XRGB8888);
		struct pl_state d = e.st;
		d.bat = PL_BAT_DISCHARGING;
		pl_render_battery(&was.c, l, &e.a, &d, false);
		int diff = 0;
		for (int y = 0; y < l->battery.h; y++)
			for (int x = l->battery.x; x < l->battery.x + l->battery.w; x++)
				diff += px(&was, x, y) != px(&e.b, x, y);
		CHECK(diff > 6, "charging differs from discharging by more than the colour");
		free(was.mem);
	}
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_CHARGING) > 10, "and a green fill");
	e.st.bat_pct = 10;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK(count_color(&e.b, l->battery, PL_COL_BAT_LOW) > 0, "a nearly empty battery that charges is red");
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_CHARGING), 0, "and not green");
	e.st.bat = PL_BAT_FULL;
	e.st.bat_pct = 100;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BOLT), 0, "full is not charging");
	e.st.bat = PL_BAT_AC;
	e.st.bat_pct = -1;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK_EQ(count_color(&e.b, l->battery, PL_COL_BAT_EMPTY) + count_color(&e.b, l->battery, PL_COL_BAT_FILL), 0,
		"AC has no battery icon");
	CHECK(count_color(&e.b, l->battery, PL_COL_FG) > 10, "AC is written");
	e.st.bat = PL_BAT_NONE;
	pl_render_battery(c, l, &e.a, &e.st, false);
	CHECK(count_color(&e.b, l->battery, PL_COL_FG) > 5, "-- is written");
	CHECK(guards_intact(&e.b), "the battery stays in the buffer");

	/* The edges are on pixel boundaries, not smeared over two pixels: a 1 px
	 * outline, the fill one pixel inside it, a nub of whole pixels. */
	const struct pl_mask *ol = &e.a.bat_outline, *in = &e.a.bat_inner;
	int my = ol->h / 2, w = ol->w;
	CHECK_EQ(ol->a[(size_t)my * w + 0], 255, "the outline's left edge is a whole pixel");
	CHECK_EQ(ol->a[(size_t)my * w + 1], 0, "and one pixel wide");
	CHECK_EQ(ol->a[(size_t)my * w + (w - 3)], 255, "the right edge of the body is whole too");
	CHECK_EQ(ol->a[(size_t)my * w + (w - 1)], 255, "the nub is solid");
	CHECK_EQ(ol->a[(size_t)0 * w + (w - 1)], 0, "and does not reach the top");
	int nub_top = 0;
	while (nub_top < ol->h && ol->a[(size_t)nub_top * w + (w - 1)] == 0)
		nub_top++;
	CHECK_EQ(ol->a[(size_t)nub_top * w + (w - 1)], 255, "the nub starts on a pixel edge");
	int nub_bot = ol->h - 1;
	while (nub_bot > 0 && ol->a[(size_t)nub_bot * w + (w - 1)] == 0)
		nub_bot--;
	CHECK_EQ(nub_top, ol->h - 1 - nub_bot, "and is centred on the body");
	int first = 0;
	while (first < in->w && in->a[(size_t)my * in->w + first] == 0)
		first++;
	CHECK_EQ(in->a[(size_t)my * in->w + first], 255, "the fill starts on a pixel edge");
	CHECK_EQ(first, 2, "one pixel inside the outline, after a gap of one");

	/* The speaker's body and the sun's rays: the first pixel of the middle
	 * row is solid. */
	for (int v = 1; v < 3; v++)
		for (int i = 0; i < 2; i++) {
			const struct pl_mask *sp = &e.a.speaker[v][i];
			int x = 0, row = sp->h / 2;
			while (x < sp->w && sp->a[(size_t)row * sp->w + x] == 0)
				x++;
			CHECK_EQ(sp->a[(size_t)row * sp->w + x], 255, "the speaker's body starts on a pixel edge");
		}


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
	CHECK(e.a.sun[0].w >= 12 && e.a.sun[0].w <= 14, "the button icons are about 12 px");
	CHECK(e.a.sun[1].w >= 20, "the row icons are large");
	/* Disabled sliders are grey, not white. */
	st.vol_pct = -1;
	pl_render_button(c, l, &e.a, &st, PL_SLIDER_NONE, PL_SLIDER_VOLUME);
	CHECK_EQ(count_color(&e.b, pl_button_rect(l, 1), PL_COL_FG), 0, "a disabled button is not in the text colour");
	/* Few pixels of a 12 px icon are fully covered. */
	CHECK(count_color(&e.b, pl_button_rect(l, 1), PL_COL_DISABLED) > 3, "it is grey");
	CHECK(count_not(&e.b, pl_button_rect(l, 1), pl_pixel(c, PL_COL_BG, 255)) > 20, "and drawn");
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
		pl_render_clock(&e.b.c, l, &e.a, &e.st, false);
		pl_render_battery(&e.b.c, l, &e.a, &e.st, false);
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
	pl_font_draw(&b.c, &f, 0, -30, -3, 20, "12:34 %", 0xffffff, PL_SUB_NONE);
	pl_font_draw(&b.c, &f, 0, 30, 10, 20, "88", 0xffffff, PL_SUB_NONE);
	CHECK(guards_intact(&b), "text clips to the canvas");
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 40, 20 }, 0x000000, 255);
	pl_font_draw(&b.c, &f, 0, 4, 0, 20, "1", 0xffffff, PL_SUB_NONE);
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

/* ---- the crisp style ---- */

/* The pixel font the shared crisp checks run with: they run once per font. */
static enum pl_crisp_font g_cfont = PL_CRISP_FIXED;

static void crisp_env_init(struct env *e, bool row, int bar_h)
{
	struct pl_metrics m;

	pl_assets_init_crisp(&e->a, bar_h, 255, 255, g_cfont);
	pl_assets_metrics(&e->a, &m);
	pl_layout_compute(&e->l, 240, bar_h, row, false, &m);
	CHECK(pl_assets_prepare(&e->a, &e->l), "the crisp style has nothing to build");
	canvas_init(&e->b, 240, e->l.h, PL_FMT_XRGB8888);
	e->st = (struct pl_state){ .hour = 20, .min = 22, .bat = PL_BAT_DISCHARGING,
		.bat_pct = 73, .bl_pct = 60, .vol_pct = 80, .year = 2026, .mon = 9, .mday = 5,
		.wday = 1 };
}

/* A ground that no panel colour is, so that a pixel the panel did not write
 * shows up as a colour of its own. */
static void patterned(struct canvas_buf *b)
{
	for (int y = 0; y < b->c.h; y++)
		for (int x = 0; x < b->c.w; x++)
			((uint32_t *)(b->c.data + (size_t)y * b->c.stride))[x] =
				(x ^ y) & 1 ? 0x123457 : 0xabcdee;
}

#define MAX_COLORS 64

/* The distinct colours (0xRRGGBB) of the canvas; stops counting at the size of
 * out, and returns how many there are (more than that if it overflowed). */
static int colours(const struct canvas_buf *b, uint32_t *out, int max)
{
	int n = 0;

	for (int y = 0; y < b->c.h; y++)
		for (int x = 0; x < b->c.w; x++) {
			uint32_t v = px(b, x, y) & 0xffffff;
			int i = 0;

			while (i < n && out[i] != v)
				i++;
			if (i < n)
				continue;
			if (n < max)
				out[n] = v;
			n++;
		}
	return n;
}

/* The canvas has exactly the colours of want (n of them, in any order). */
static void palette_is(const struct canvas_buf *b, const uint32_t *want, int n, const char *what)
{
	uint32_t got[MAX_COLORS];
	int ng = colours(b, got, MAX_COLORS);
	char msg[160];
	int missing = 0, extra = 0;

	for (int i = 0; i < n; i++) {
		int f = 0;
		for (int j = 0; j < ng && j < MAX_COLORS; j++)
			f |= got[j] == want[i];
		if (!f) {
			missing++;
			fprintf(stderr, "  %s: missing #%06x\n", what, want[i]);
		}
	}
	for (int j = 0; j < ng && j < MAX_COLORS; j++) {
		int f = 0;
		for (int i = 0; i < n; i++)
			f |= got[j] == want[i];
		if (!f) {
			extra++;
			fprintf(stderr, "  %s: foreign colour #%06x\n", what, got[j]);
		}
	}
	snprintf(msg, sizeof(msg), "%s: exactly the %d colours of the palette (%d found)", what, n, ng);
	CHECK(missing == 0 && extra == 0 && ng == n, msg);
	printf("test-panel: crisp %s: %d distinct colours\n", what, ng);
}

static void crisp_scene(const char *what, int open, bool row, struct pl_state st,
	const uint32_t *want, int n)
{
	struct env e;

	crisp_env_init(&e, row, BAR);
	patterned(&e.b);
	e.st = st;
	pl_render_all(&e.b.c, &e.l, &e.a, &e.st, open);
	CHECK(guards_intact(&e.b), "crisp drawing stays inside the buffer");
	palette_is(&e.b, want, n, what);
	env_free(&e);
}

static void test_crisp_palette(void)
{
	struct env e0;
	crisp_env_init(&e0, false, BAR);
	struct pl_state base = e0.st;
	env_free(&e0);

	/* The bar: ground, its line, text, the icons and the battery's outline in
	 * one colour, and the battery's empty and filled parts. */
	crisp_scene("closed bar", PL_SLIDER_NONE, false, base,
		(uint32_t[]){ PL_COL_BG, PL_COL_LINE, PL_COL_FG, PL_COL_BAT_EMPTY, PL_COL_BAT_FILL }, 5);

	/* The backlight row: the highlight and the accent of the open button, the
	 * row's ground and line, track, thumb and its ring, and the green of a
	 * charging battery with the white of its bolt (the thumb's colour too). */
	struct pl_state chg = base;
	chg.bat = PL_BAT_CHARGING;
	crisp_scene("backlight row", PL_SLIDER_BACKLIGHT, true, chg,
		(uint32_t[]){ PL_COL_BG, PL_COL_LINE, PL_COL_FG, PL_COL_BAT_EMPTY, PL_COL_BAT_CHARGING,
			PL_COL_HL, PL_COL_ACCENT, PL_COL_ROW, PL_COL_ROW_LINE, PL_COL_TRACK,
			PL_COL_THUMB, PL_COL_THUMB_RING }, 12);
	CHECK_EQ(PL_COL_BOLT, PL_COL_THUMB, "the bolt and the thumb share their white");

	struct pl_state low = base;
	low.bat_pct = 10;
	crisp_scene("volume row", PL_SLIDER_VOLUME, true, low,
		(uint32_t[]){ PL_COL_BG, PL_COL_LINE, PL_COL_FG, PL_COL_BAT_EMPTY, PL_COL_BAT_LOW,
			PL_COL_HL, PL_COL_ACCENT, PL_COL_ROW, PL_COL_ROW_LINE, PL_COL_TRACK, PL_COL_THUMB,
			PL_COL_THUMB_RING }, 12);

	crisp_scene("date row", PL_BTN_CLOCK, true, base,
		(uint32_t[]){ PL_COL_BG, PL_COL_LINE, PL_COL_FG, PL_COL_BAT_EMPTY, PL_COL_BAT_FILL,
			PL_COL_HL, PL_COL_ROW, PL_COL_ROW_LINE }, 8);

	struct pl_state est = low;
	est.bat = PL_BAT_CHARGING;
	est.est = (struct pl_estimate){ PL_EST_TOFULL, 95, false };
	crisp_scene("battery row", PL_BTN_BATTERY, true, est,
		(uint32_t[]){ PL_COL_BG, PL_COL_LINE, PL_COL_FG, PL_COL_BAT_EMPTY, PL_COL_BAT_LOW,
			PL_COL_BOLT, PL_COL_HL, PL_COL_ROW, PL_COL_ROW_LINE }, 9);

	struct pl_state dis = base;
	dis.bl_pct = dis.vol_pct = -1;
	dis.bat_pct = 100;
	crisp_scene("disabled buttons", PL_SLIDER_NONE, false, dis,
		(uint32_t[]){ PL_COL_BG, PL_COL_LINE, PL_COL_FG, PL_COL_DISABLED, PL_COL_BAT_FILL }, 5);

	/* The same scene in the smooth style has intermediate colours all over. */
	struct env e;
	env_init(&e, PL_FMT_XRGB8888, 255, 255, true, NOFONT);
	patterned(&e.b);
	e.st.bat = PL_BAT_CHARGING;
	pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
	uint32_t got[MAX_COLORS];
	int ns = colours(&e.b, got, MAX_COLORS);
	CHECK(ns > 25, "the smooth style has many intermediate colours");
	printf("test-panel: smooth backlight row: %d distinct colours (the crisp one has 12)\n", ns);
	env_free(&e);
}

/* The colours are flat in every pixel format, not only in XRGB8888: a premultiplied
 * ARGB canvas and RGB565 hold one value per colour too. */
static void test_crisp_formats(void)
{
	static const enum pl_fmt fmts[] = { PL_FMT_RGB565, PL_FMT_ARGB8888 };

	for (int k = 0; k < 2; k++) {
		struct env e;
		struct pl_metrics m;

		pl_assets_init_crisp(&e.a, BAR, 255, 255, g_cfont);
		pl_assets_metrics(&e.a, &m);
		pl_layout_compute(&e.l, 240, BAR, true, false, &m);
		canvas_init(&e.b, 240, e.l.h, fmts[k]);
		e.st = (struct pl_state){ .hour = 20, .min = 22, .bat = PL_BAT_CHARGING, .bat_pct = 73,
			.bl_pct = 60, .vol_pct = 80 };
		pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
		uint32_t seen[MAX_COLORS];
		int n = 0, bad = 0;
		static const uint32_t pal[] = { PL_COL_BG, PL_COL_LINE, PL_COL_FG, PL_COL_BAT_EMPTY,
			PL_COL_BAT_CHARGING, PL_COL_BOLT, PL_COL_HL, PL_COL_ACCENT, PL_COL_ROW,
			PL_COL_ROW_LINE, PL_COL_TRACK, PL_COL_THUMB, PL_COL_THUMB_RING };
		for (int y = 0; y < e.l.h; y++)
			for (int x = 0; x < 240; x++) {
				uint32_t v = px(&e.b, x, y);
				int i = 0, ok = 0;

				for (size_t q = 0; q < sizeof(pal) / sizeof(pal[0]); q++)
					ok |= v == pl_pixel(&e.b.c, pal[q], 255);
				bad += !ok;
				while (i < n && seen[i] != v)
					i++;
				if (i == n && n < MAX_COLORS)
					seen[n++] = v;
			}
		CHECK_EQ(bad, 0, fmts[k] == PL_FMT_RGB565 ? "RGB565: every pixel is a palette colour" :
			"ARGB8888: every pixel is a palette colour");
		CHECK(n <= 12, "and no more distinct values than the palette has");
		env_free(&e);
	}
}

/* The panel's own wordings, every one the panel can print. */
static void test_crisp_wordings(void)
{
	struct pl_font f;
	char buf[64];

	pl_font_load_crisp(&f, BAR, PL_CRISP_FIXED);
	CHECK(f.crisp && !f.ttf, "a crisp font is neither TrueType nor the 5x7 fallback");
	/* Sizes by role. */
	CHECK_EQ(pl_font_face_px(&f, PL_FACE_BAR), 13, "the bar text is a 13 px pixel font");
	CHECK_EQ(pl_font_face_px(&f, PL_FACE_SMALL), 13, "so is the value in the slider row");
	CHECK_EQ(pl_font_face_px(&f, PL_FACE_ROW), 20, "the text rows start at 20 px");
	CHECK_EQ(pl_font_face_px(&f, PL_FACE_ROW + 1), 15, "then 15");
	CHECK_EQ(pl_font_face_px(&f, PL_FACE_ROW + 2), 14, "then 14");
	CHECK_EQ(pl_font_face_px(&f, PL_FACE_ROW + 3), 10, "and 10");
	for (int i = 0; i < PL_FACES; i++)
		CHECK_EQ(f.pix[i].scale, 1, "every face of the default bar is at scale 1");
	CHECK_STR(f.pix[PL_FACE_BAR].f->name, "7x13B", "the bar font is 7x13 bold");
	CHECK_STR(f.pix[PL_FACE_ROW].f->name, "10x20", "the largest row font is 10x20");
	/* The digits are 9 rows high in the 17 rows of the bar: 4 above, 4 below. */
	CHECK_EQ(f.face[PL_FACE_BAR].digit_h, 9, "the bar's digits are 9 px high");
	CHECK_EQ((BAR - 1 - f.face[PL_FACE_BAR].digit_h) % 2, 0, "which centres them on whole pixels");
	CHECK_EQ(f.face[PL_FACE_ROW].digit_h, 13, "the big row's digits are 13 px high");
	CHECK_EQ((35 - f.face[PL_FACE_ROW].digit_h) % 2, 0, "which centre in the row's 35 px");
	/* Fixed pitch: a width is the number of characters times the cell. */
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_BAR, "88:88"), 35, "the clock is 35 px wide");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_BAR, "100%"), 28, "100% is 28 px");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_ROW, "Monday"), 60, "six characters of 10 px");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_ROW, ""), 0, "nothing is nothing");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_BAR, "a\x01\x7f""b"), 14, "characters the font lacks have no width");

	int missing = 0, total = 0;
	for (int wd = 0; wd < 7; wd++)
		for (int mon = 0; mon < 12; mon++) {
			pl_date_text(buf, sizeof(buf), 2026, mon, 28 + wd % 4, wd);
			for (int face = PL_FACE_ROW; face < PL_FACES; face++)
				for (const char *c = buf; *c; c++) {
					const struct pl_pixfont *pf = f.pix[face].f;
					const uint8_t *g = pl_pixfont_glyph(pf, (unsigned char)*c);
					int ink = 0;

					for (int r = 0; g && r < pf->h; r++)
						for (int x = 0; x < pf->w; x++)
							ink += pl_pixfont_bit(pf, g, r, x);
					total++;
					if (!g || (*c != ' ' && !ink))
						missing++;
				}
		}
	CHECK_EQ(missing, 0, "every character of every date renders in every row face");
	CHECK(total > 5000, "(and they were all looked at)");

	static const struct pl_estimate kinds[] = {
		{ PL_EST_NONE, 0, false }, { PL_EST_AC, 0, false }, { PL_EST_FULL, 0, false },
		{ PL_EST_NOTCHARGING, 0, false }, { PL_EST_CHARGING, 0, false },
		{ PL_EST_ESTIMATING, 0, false }, { PL_EST_LEFT, 5, false }, { PL_EST_LEFT, 90, false },
		{ PL_EST_LEFT, 80, true }, { PL_EST_TOFULL, 75, false }, { PL_EST_OVER_LEFT, 0, false },
		{ PL_EST_OVER_FULL, 0, false },
	};
	missing = 0;
	for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
		for (int v = 0; v < PL_EST_VARIANTS; v++) {
			pl_est_text(buf, sizeof(buf), &kinds[k], v ? -1 : 94, v);
			for (int face = PL_FACE_ROW; face < PL_FACES; face++)
				for (const char *c = buf; *c; c++)
					missing += pl_pixfont_glyph(f.pix[face].f, (unsigned char)*c) == NULL;
		}
	CHECK_EQ(missing, 0, "every character of every battery sentence renders (> and ~ included)");
	for (const char *c = "0123456789:%-,. ><~ACDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"; *c; c++)
		for (int face = 0; face < PL_FACES; face++)
			CHECK(pl_pixfont_glyph(f.pix[face].f, (unsigned char)*c) != NULL, "every panel character has a glyph");

	/* Glyph data as the generator made it, from the BDF files. */
	{
		const uint8_t *one = pl_pixfont_glyph(&pl_pixfont_7x13B, '1');
		static const uint8_t want[13] = { 0x00, 0x00, 0x30, 0x70, 0xb0, 0x30, 0x30, 0x30, 0x30,
			0x30, 0xfc, 0x00, 0x00 };
		CHECK(!memcmp(one, want, 13), "the 1 of 7x13B is what the BDF says");
		const uint8_t *zero = pl_pixfont_glyph(&pl_pixfont_7x13B, '0');
		static const uint8_t z[13] = { 0x00, 0x00, 0x30, 0x48, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x48,
			0x30, 0x00, 0x00 };
		CHECK(!memcmp(zero, z, 13), "and the 0");
		/* A cell of two bytes per row: the M of 10x20 (rows 3 to 15 of 20). */
		const uint8_t *m = pl_pixfont_glyph(&pl_pixfont_10x20, 'M');
		CHECK_EQ(m[3 * 2], 0x61, "the first ink row of the 10x20 M, high byte");
		CHECK_EQ(m[3 * 2 + 1], 0x80, "and low byte");
		CHECK_EQ(m[7 * 2], 0x7f, "its crossbar");
		CHECK_EQ(pl_pixfont_bit(&pl_pixfont_10x20, m, 7, 1), 1, "starts at x 1");
		CHECK_EQ(pl_pixfont_bit(&pl_pixfont_10x20, m, 7, 0), 0, "and not before");
		CHECK_EQ(pl_pixfont_bit(&pl_pixfont_10x20, m, 7, 8), 1, "ends at x 8");
		CHECK_EQ(pl_pixfont_bit(&pl_pixfont_10x20, m, 7, 9), 0, "with the pitch on the right");
		CHECK(pl_pixfont_glyph(&pl_pixfont_7x13B, 0x1f) == NULL && pl_pixfont_glyph(&pl_pixfont_7x13B, 0x7f) == NULL,
			"only printable ASCII is in the font");
	}
	/* Characters are distinct in every font, but for the 0 and the O of 7x14B,
	 * which the font draws alike. */
	int same = 0;
	for (int face = 0; face < PL_FACES; face++) {
		const struct pl_pixfont *pf = f.pix[face].f;
		for (int a = PL_PIX_FIRST + 1; a <= PL_PIX_LAST; a++)
			for (int b = a + 1; b <= PL_PIX_LAST; b++)
				if (!(a == '0' && b == 'O' && pf == &pl_pixfont_7x14B))
					same += !memcmp(pl_pixfont_glyph(pf, a), pl_pixfont_glyph(pf, b), (size_t)pf->h * pf->bpr);
	}
	CHECK_EQ(same, 0, "no two characters of a pixel font look the same");
	pl_font_free(&f);

	/* Integer scales only: a taller bar scales the font by a whole number. */
	pl_font_load_crisp(&f, 38, PL_CRISP_FIXED);
	CHECK_EQ(f.pix[PL_FACE_BAR].scale, 2, "a 38 px bar has the bar font twice as big");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_BAR, "88:88"), 70, "and twice as wide");
	CHECK_EQ(pl_font_face_px(&f, PL_FACE_BAR), 26, "and tall");
	CHECK_EQ(f.pix[PL_FACE_ROW].scale, 1, "its 46 px row is still the normal size");
	pl_font_free(&f);
	pl_font_load_crisp(&f, 64, PL_CRISP_FIXED);
	CHECK_EQ(f.pix[PL_FACE_BAR].scale, 3, "a 64 px bar: scale 3");
	CHECK_EQ(f.pix[PL_FACE_ROW].scale, 2, "and a 72 px row: scale 2");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_ROW, "Monday"), 120, "Monday in 10x20 at scale 2");
	pl_font_free(&f);
	pl_font_load_crisp(&f, 80, PL_CRISP_FIXED);
	CHECK_EQ(f.pix[PL_FACE_BAR].scale, 4, "an 80 px bar: scale 4");
	pl_font_free(&f);
	pl_font_load_crisp(&f, 18, PL_CRISP_FIXED);
	CHECK(f.pix[PL_FACE_BAR].scale == 1 && f.pix[PL_FACE_ROW].scale == 1, "scales never fall below 1");
	pl_font_free(&f);
}

/* The text-fit logic picks among the pixel sizes like it does among the
 * TrueType ones: the first face (largest) in which a candidate fits. */
static void test_crisp_fit(void)
{
	struct env e;
	struct pl_rowtext t;

	crisp_env_init(&e, true, BAR);
	CHECK_EQ(e.l.text.w, 224, "the row's text is 224 px wide");
	/* Monday 5 October 2026 is 21 characters: 210 px at 10 px each. */
	e.st.wday = 1; e.st.mday = 5; e.st.mon = 9;
	pl_row_text(&e.l, &e.a, &e.st, PL_BTN_CLOCK, &t);
	CHECK_STR(t.text, "Monday 5 October 2026", "the date");
	CHECK_EQ(t.face, PL_FACE_ROW, "fits the largest font");
	CHECK_EQ(t.w, 210, "at 10 px a character");
	/* Wednesday 30 September 2026: 27 characters, 270 and 243 are too wide, 7x14
	 * bold makes 189. */
	e.st.wday = 3; e.st.mday = 30; e.st.mon = 8;
	pl_row_text(&e.l, &e.a, &e.st, PL_BTN_CLOCK, &t);
	CHECK_STR(t.text, "Wednesday 30 September 2026", "the longest date");
	CHECK_EQ(t.face, PL_FACE_ROW + 2, "needs the third size");
	CHECK_EQ(t.w, 189, "which is 7 px a character");
	/* The estimate loses words before it loses size. */
	e.st.est = (struct pl_estimate){ PL_EST_LEFT, 90, false };
	e.st.bat_pct = 94;
	pl_row_text(&e.l, &e.a, &e.st, PL_BTN_BATTERY, &t);
	CHECK_STR(t.text, "94%  1 h 30 min left", "the estimate with the percentage");
	CHECK_EQ(t.face, PL_FACE_ROW, "in the largest font: 200 px of 224");
	e.st.est = (struct pl_estimate){ PL_EST_LEFT, 80, true };
	pl_row_text(&e.l, &e.a, &e.st, PL_BTN_BATTERY, &t);
	CHECK_STR(t.text, "94%  ~1 h 20 min left", "with the hint's tilde");
	CHECK_EQ(t.face, PL_FACE_ROW, "21 characters, 210 px: still the largest");
	e.st.est = (struct pl_estimate){ PL_EST_TOFULL, 75, false };
	pl_row_text(&e.l, &e.a, &e.st, PL_BTN_BATTERY, &t);
	CHECK_STR(t.text, "1 h 15 min to full", "to full is longer: the percentage goes first");
	CHECK_EQ(t.face, PL_FACE_ROW, "and the size stays the largest");
	CHECK_EQ(t.w, 180, "18 characters of 10 px");
	/* A narrow output: the text shrinks through the sizes and is never wider than the room. */
	env_free(&e);
	for (int w = 120; w <= 240; w += 20) {
		struct pl_metrics m;
		struct pl_layout l;
		struct pl_assets a;
		struct pl_state st = { .est = { PL_EST_TOFULL, 75, false }, .bat_pct = 94,
			.year = 2026, .mon = 8, .mday = 30, .wday = 3 };

		pl_assets_init_crisp(&a, BAR, 255, 255, g_cfont);
		pl_assets_metrics(&a, &m);
		pl_layout_compute(&l, w, BAR, true, false, &m);
		pl_row_text(&l, &a, &st, PL_BTN_CLOCK, &t);
		CHECK(t.w <= l.text.w || t.face == PL_FACES - 1, "a date fits or has run out of sizes");
		pl_row_text(&l, &a, &st, PL_BTN_BATTERY, &t);
		CHECK(t.w <= l.text.w || t.face == PL_FACES - 1, "an estimate fits or has run out of sizes");
		pl_assets_free(&a);
	}
}

static void test_crisp_layout(void)
{
	/* Only the thumb and the track change, to odd sizes, which is what gives a
	 * disc a centre pixel and a bar a centre row. */
	for (int bar = 18; bar <= 80; bar += 7) {
		struct pl_metrics m = { 33, 27, 24, false }, mc = { 33, 27, 24, true };
		struct pl_layout a, b;

		pl_layout_compute(&a, 240, bar, true, false, &m);
		pl_layout_compute(&b, 240, bar, true, false, &mc);
		CHECK(b.slider.thumb_d % 2 == 1 && b.slider.track_h % 2 == 1, "crisp: odd thumb and track");
		CHECK(b.slider.thumb_d <= a.slider.thumb_d && a.slider.thumb_d - b.slider.thumb_d <= 1,
			"the thumb is at most a pixel smaller");
		CHECK(b.slider.track_h <= a.slider.track_h && a.slider.track_h - b.slider.track_h <= 1,
			"and so is the track");
		int td = b.slider.thumb_d, th = b.slider.track_h;
		b.slider.thumb_d = a.slider.thumb_d;
		b.slider.track_h = a.slider.track_h;
		CHECK(!memcmp(&a, &b, sizeof(a)), "nothing else of the layout changes: touch targets, rows, highlights");
		/* The disc and the bar share their centre, on a pixel of both. */
		b.slider.thumb_d = td;
		b.slider.track_h = th;
		struct pl_rect t = pl_slider_thumb(&b, 40);
		int ty = b.row_in.y + (b.row_h - 1 - th) / 2;
		if ((b.row_h - 1) % 2 == 1)
			CHECK_EQ(t.y + td / 2, ty + th / 2, "the thumb's centre row is the track's");
	}
}

static void test_crisp_icons(void)
{
	const struct pl_bitmap *all[] = { &pl_ico_sun, &pl_ico_speaker[0], &pl_ico_speaker[1],
		&pl_ico_speaker[2] };
	int ink[4];

	for (int i = 0; i < 4; i++) {
		const struct pl_bitmap *b = all[i];
		int n = 0, ok = 1;

		CHECK(b->w == PL_ICON_GRID && b->h == PL_ICON_GRID, "icons are 12x12");
		for (int r = 0; r < b->h; r++) {
			ok &= (int)strlen(b->rows[r]) == b->w;
			for (int x = 0; x < b->w; x++)
				n += b->rows[r][x] == '#';
		}
		CHECK(ok, "every row of an icon is as wide as the icon");
		ink[i] = n;
		/* Top to bottom symmetric. */
		int sym = 1;
		for (int r = 0; r < b->h; r++)
			sym &= !strcmp(b->rows[r], b->rows[b->h - 1 - r]);
		CHECK(sym, "an icon is mirror symmetric top to bottom");
	}
	int lr = 1;
	for (int r = 0; r < 12; r++)
		for (int x = 0; x < 6; x++)
			lr &= pl_ico_sun.rows[r][x] == pl_ico_sun.rows[r][11 - x];
	CHECK(lr, "the sun is symmetric left to right too");
	CHECK_STR(pl_ico_sun.rows[0], ".....##.....", "the sun's top ray is two pixels wide");
	CHECK_STR(pl_ico_sun.rows[5], "##.######.##", "its middle row: rays and the disc");
	CHECK(ink[3] > ink[2] && ink[2] > 0 && ink[1] > 0, "two waves have more ink than one");
	CHECK(pl_ico_speaker[1].rows[5][0] == '.' && pl_ico_speaker[1].rows[5][1] == '#', "the speaker's box starts at x 1");
	/* The speaker is in the same place in all three variants. */
	int same = 1;
	for (int v = 1; v < 3; v++)
		for (int r = 0; r < 12; r++)
			for (int x = 0; x < 7; x++)
				same &= pl_ico_speaker[v].rows[r][x] == pl_ico_speaker[0].rows[r][x];
	CHECK(same, "the speaker does not move between muted, one wave and two");

	/* Drawn: exactly its set pixels, in one colour, at any whole scale. */
	for (int sc = 1; sc <= 3; sc++) {
		struct canvas_buf b;

		canvas_init(&b, 12 * sc + 6, 12 * sc + 6, PL_FMT_XRGB8888);
		pl_fill(&b.c, (struct pl_rect){ 0, 0, b.c.w, b.c.h }, PL_COL_BG, 255);
		pl_crisp_bitmap(&b.c, &pl_ico_sun, sc, 3, 3, PL_COL_FG);
		CHECK_EQ(count_color(&b, (struct pl_rect){ 0, 0, b.c.w, b.c.h }, PL_COL_FG), ink[0] * sc * sc,
			"a scaled icon has the pixels of the bitmap times scale squared");
		CHECK_EQ(count_not(&b, (struct pl_rect){ 0, 0, b.c.w, b.c.h }, pl_pixel(&b.c, PL_COL_BG, 255)),
			ink[0] * sc * sc, "and nothing else");
		/* Top ray of the sun, pixel column 5 and 6, row 0. */
		CHECK_EQ(px(&b, 3 + 5 * sc, 3), pl_pixel(&b.c, PL_COL_FG, 255), "its first set pixel is at the grid's");
		CHECK_EQ(px(&b, 3 + 5 * sc - 1, 3), pl_pixel(&b.c, PL_COL_BG, 255), "and the one before it is not set");
		CHECK(guards_intact(&b), "icons stay in the buffer");
		free(b.mem);
	}
	CHECK_EQ(pl_crisp_icon_scale(12), 1, "12 px box: scale 1");
	CHECK_EQ(pl_crisp_icon_scale(16), 1, "16 px box: scale 1");
	CHECK_EQ(pl_crisp_icon_scale(24), 2, "24 px box: scale 2");
	CHECK_EQ(pl_crisp_icon_scale(28), 2, "28 px box: scale 2");
	CHECK_EQ(pl_crisp_icon_scale(5), 1, "never less than 1");

	/* In the bar and in the row. */
	struct env e;
	crisp_env_init(&e, true, BAR);
	pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
	struct pl_rect ri = e.l.row_icon;
	CHECK_EQ(ri.w, 24, "the row icon box is 24 px");
	CHECK_EQ(count_color(&e.b, ri, PL_COL_FG), ink[0] * 4, "the row's sun is the bitmap at scale 2");
	struct pl_rect b0 = pl_button_rect(&e.l, 0);
	CHECK_EQ(count_color(&e.b, b0, PL_COL_ACCENT), ink[0], "the open button's sun is the bitmap, in the accent colour");
	e.st.vol_pct = 0;
	pl_render_button(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT, PL_SLIDER_VOLUME);
	CHECK_EQ(count_color(&e.b, pl_button_rect(&e.l, 1), PL_COL_FG), ink[1], "muted speaker");
	e.st.vol_pct = 30;
	pl_render_button(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT, PL_SLIDER_VOLUME);
	CHECK_EQ(count_color(&e.b, pl_button_rect(&e.l, 1), PL_COL_FG), ink[2], "one wave");
	e.st.vol_pct = 80;
	pl_render_button(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT, PL_SLIDER_VOLUME);
	CHECK_EQ(count_color(&e.b, pl_button_rect(&e.l, 1), PL_COL_FG), ink[3], "two waves");
	env_free(&e);
}

static void test_crisp_shapes(void)
{
	struct canvas_buf b;
	const struct pl_rect all = { 0, 0, 40, 30 };

	canvas_init(&b, 40, 30, PL_FMT_XRGB8888);

	/* The battery: a 1 px outline whose rows and columns are fully set, a nub,
	 * a gap, and a fill of whole columns. */
	pl_fill(&b.c, all, PL_COL_BG, 255);
	pl_crisp_battery(&b.c, 4, 3, 16, 8, 50, PL_COL_BAT_FILL, false);
	uint32_t fg = pl_pixel(&b.c, PL_COL_FG, 255), bg = pl_pixel(&b.c, PL_COL_BG, 255);
	for (int x = 1; x <= 12; x++) {
		CHECK_EQ(px(&b, 4 + x, 3), fg, "the outline's top row is set");
		CHECK_EQ(px(&b, 4 + x, 3 + 7), fg, "and the bottom row");
	}
	for (int y = 1; y <= 6; y++) {
		CHECK_EQ(px(&b, 4, 3 + y), fg, "the left column is set");
		CHECK_EQ(px(&b, 4 + 13, 3 + y), fg, "and the right one");
	}
	CHECK_EQ(px(&b, 4, 3), bg, "the corners are cut");
	CHECK_EQ(px(&b, 4 + 13, 3), bg, "all four");
	CHECK_EQ(px(&b, 4, 3 + 7), bg, "of them");
	CHECK_EQ(px(&b, 4 + 13, 3 + 7), bg, "yes");
	for (int y = 2; y <= 5; y++)
		CHECK_EQ(px(&b, 4 + 14, 3 + y), fg, "the nub is 4 rows");
	CHECK_EQ(px(&b, 4 + 14, 3 + 1), bg, "starting at row 2");
	CHECK_EQ(px(&b, 4 + 14, 3 + 6), bg, "and symmetric");
	CHECK_EQ(px(&b, 4 + 15, 3 + 3), fg, "two columns wide");
	CHECK_EQ(px(&b, 4 + 16, 3 + 3), bg, "no more than that");
	CHECK_EQ(px(&b, 4 + 1, 3 + 3), bg, "the gap inside the outline is the ground, 1 px");
	CHECK_EQ(px(&b, 4 + 2, 3 + 1), bg, "above the fill too");
	int fillc = 0, empty = 0;
	for (int x = 2; x <= 11; x++) {
		uint32_t top = px(&b, 4 + x, 3 + 2);
		for (int y = 2; y <= 5; y++)
			CHECK_EQ(px(&b, 4 + x, 3 + y), top, "a column of the interior is one colour");
		fillc += top == pl_pixel(&b.c, PL_COL_BAT_FILL, 255);
		empty += top == pl_pixel(&b.c, PL_COL_BAT_EMPTY, 255);
	}
	CHECK_EQ(fillc, 5, "50 percent of 10 columns is 5 whole columns of fill");
	CHECK_EQ(empty, 5, "and 5 of the empty colour");
	CHECK_EQ(pl_crisp_battery_fill(10, 0), 0, "0 percent has no fill");
	CHECK_EQ(pl_crisp_battery_fill(10, 1), 1, "1 percent shows a pixel");
	CHECK_EQ(pl_crisp_battery_fill(10, 100), 10, "100 percent fills it");
	CHECK_EQ(pl_crisp_battery_fill(10, 73), 7, "73 percent is 7 columns");
	CHECK_EQ(pl_crisp_battery_fill(10, 95), 10, "95 percent is 10 columns by rounding");
	CHECK_EQ(pl_crisp_battery_fill(10, 14), 1, "14 percent is one");
	CHECK(guards_intact(&b), "the battery stays in the buffer");
	/* The bolt over a charging battery stays inside the body's interior rows. */
	pl_fill(&b.c, all, PL_COL_BG, 255);
	pl_crisp_battery(&b.c, 4, 3, 16, 8, 50, PL_COL_BAT_CHARGING, true);
	CHECK_EQ(count_color(&b, all, PL_COL_BOLT), 13, "the bolt is drawn with its 13 pixels");
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 14; x++)
			if (px(&b, 4 + x, 3 + y) == pl_pixel(&b.c, PL_COL_BOLT, 255))
				CHECK(x >= 1 && x <= 12 && y >= 1 && y <= 6, "the bolt stays inside the outline");

	/* A pill: the corner pixels are missing, nothing else. */
	pl_fill(&b.c, all, PL_COL_BG, 255);
	pl_crisp_pill(&b.c, (struct pl_rect){ 3, 4, 20, 15 }, PL_COL_HL, 255, 100);
	CHECK_EQ(count_color(&b, all, PL_COL_HL), 20 * 15 - 4, "a highlight is its rectangle without the 4 corner pixels");
	CHECK_EQ(px(&b, 3, 4), bg, "top left");
	CHECK_EQ(px(&b, 22, 4), bg, "top right");
	CHECK_EQ(px(&b, 3, 18), bg, "bottom left");
	CHECK_EQ(px(&b, 22, 18), bg, "bottom right");
	CHECK_EQ(px(&b, 4, 4), pl_pixel(&b.c, PL_COL_HL, 255), "next to the corner is set");
	CHECK_EQ(px(&b, 3, 5), pl_pixel(&b.c, PL_COL_HL, 255), "and below it");
	/* Cut short at x_end: the end is straight. */
	pl_fill(&b.c, all, PL_COL_BG, 255);
	pl_crisp_pill(&b.c, (struct pl_rect){ 3, 4, 20, 5 }, PL_COL_ACCENT, 255, 13);
	CHECK_EQ(count_color(&b, all, PL_COL_ACCENT), 10 * 5 - 2, "a fill that stops short has one rounded end only");
	CHECK_EQ(px(&b, 12, 4), pl_pixel(&b.c, PL_COL_ACCENT, 255), "the straight end has its corners");
	CHECK_EQ(px(&b, 13, 4), bg, "and ends at x_end");
	pl_fill(&b.c, all, PL_COL_BG, 255);
	pl_crisp_pill(&b.c, (struct pl_rect){ 3, 4, 20, 5 }, PL_COL_ACCENT, 255, 2);
	CHECK_EQ(count_color(&b, all, PL_COL_ACCENT), 0, "nothing at or past an x_end left of the shape");
	CHECK(guards_intact(&b), "pills stay in the buffer");
	free(b.mem);

	/* The thumb: odd diameter, a centre pixel, 1 px ring, symmetric. */
	for (int d = 15; d <= 25; d += 2) {
		struct canvas_buf t;
		int r = d / 2;

		canvas_init(&t, d + 4, d + 4, PL_FMT_XRGB8888);
		pl_fill(&t.c, (struct pl_rect){ 0, 0, d + 4, d + 4 }, PL_COL_ROW, 255);
		pl_crisp_thumb(&t.c, 2, 2, d, PL_COL_THUMB_RING, PL_COL_THUMB);
		uint32_t ring = pl_pixel(&t.c, PL_COL_THUMB_RING, 255), fill = pl_pixel(&t.c, PL_COL_THUMB, 255);
		CHECK_EQ(px(&t, 2 + r, 2 + r), fill, "the centre pixel is white");
		CHECK_EQ(px(&t, 2 + r, 2), ring, "the ring on top");
		CHECK_EQ(px(&t, 2 + r, 2 + d - 1), ring, "at the bottom");
		CHECK_EQ(px(&t, 2, 2 + r), ring, "at the left");
		CHECK_EQ(px(&t, 2 + d - 1, 2 + r), ring, "and at the right");
		CHECK_EQ(px(&t, 2 + r, 3), fill, "white right inside the ring");
		int sym = 1, rings = 0, fills = 0;
		for (int y = 0; y < d; y++)
			for (int x = 0; x < d; x++) {
				uint32_t v = px(&t, 2 + x, 2 + y);
				sym &= v == px(&t, 2 + d - 1 - x, 2 + y) && v == px(&t, 2 + x, 2 + d - 1 - y) &&
					v == px(&t, 2 + y, 2 + x);
				rings += v == ring;
				fills += v == fill;
			}
		CHECK(sym, "the disc is symmetric left-right, top-bottom and on the diagonal");
		/* The area is that of the circle, to a few percent. */
		double area = 3.14159265 * (d / 2.0) * (d / 2.0);
		CHECK(rings + fills > area * 0.93 && rings + fills < area * 1.10, "the disc is round enough: its area is a circle's");
		CHECK(rings > 0 && fills > 0, "a ring and a fill");
		CHECK_EQ(count_not(&t, (struct pl_rect){ 0, 0, d + 4, d + 4 }, pl_pixel(&t.c, PL_COL_ROW, 255)),
			rings + fills, "and nothing but those two colours is drawn");
		/* The ring is one pixel thick on the axes. */
		CHECK_EQ(px(&t, 2 + 1, 2 + r), fill, "1 px thick at the left");
		CHECK(guards_intact(&t), "the thumb stays in the buffer");
		free(t.mem);
	}
	CHECK_EQ(pl_crisp_disc_hw(10, 0), 10, "the middle row of a 21 px disc is 21 wide");
	CHECK_EQ(pl_crisp_disc_hw(10, 10), 3, "the top row is 7 wide");
	CHECK_EQ(pl_crisp_disc_hw(10, 11), -1, "nothing past it");
	CHECK_EQ(pl_crisp_disc_hw(10, -4), pl_crisp_disc_hw(10, 4), "symmetric");
}

static void test_crisp_row_geometry(void)
{
	/* The slider on the screen: the track is a bar of odd height with its fill
	 * ending at the thumb's centre column, and the thumb is centred on the track. */
	struct env e;

	crisp_env_init(&e, true, BAR);
	pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
	const struct pl_slider *s = &e.l.slider;
	struct pl_rect th = pl_slider_thumb(&e.l, 60);
	int ty = e.l.row_in.y + (e.l.row_h - 1 - s->track_h) / 2;

	CHECK_EQ(s->thumb_d, 21, "the thumb is 21 px");
	CHECK_EQ(s->track_h, 5, "the track is 5 px");
	CHECK_EQ(th.y + th.h / 2, ty + s->track_h / 2, "the thumb's centre pixel is on the track's centre row");
	/* The track's rows: 5 high, its first and last row without the end pixels. */
	uint32_t tr = pl_pixel(&e.b.c, PL_COL_TRACK, 255);
	int xe = s->track.x + s->track.w - 1;
	CHECK_EQ(px(&e.b, xe, ty), pl_pixel(&e.b.c, PL_COL_ROW, 255), "the track's end has a cut corner");
	CHECK_EQ(px(&e.b, xe - 1, ty), tr, "after one pixel");
	CHECK_EQ(px(&e.b, xe, ty + 2), tr, "the middle row reaches the end");
	CHECK_EQ(px(&e.b, xe, ty + 4), pl_pixel(&e.b.c, PL_COL_ROW, 255), "the bottom corner is cut too");
	CHECK_EQ(px(&e.b, xe, ty - 1), pl_pixel(&e.b.c, PL_COL_ROW, 255), "and nothing is above the track");
	CHECK_EQ(px(&e.b, xe, ty + 5), pl_pixel(&e.b.c, PL_COL_ROW, 255), "or below");
	int cx = th.x + th.w / 2;
	uint32_t acc = pl_pixel(&e.b.c, PL_COL_ACCENT, 255);
	CHECK_EQ(px(&e.b, th.x - 1, ty + 2), acc, "the fill runs to the thumb");
	CHECK_EQ(px(&e.b, th.x + th.w, ty + 2), tr, "and the rest is the track");
	CHECK_EQ(px(&e.b, s->track.x, ty), pl_pixel(&e.b.c, PL_COL_ROW, 255), "the fill's left end is cut as well");
	CHECK_EQ(px(&e.b, cx, th.y + th.h / 2), pl_pixel(&e.b.c, PL_COL_THUMB, 255), "the thumb's centre pixel is white");
	/* The highlight of the open button has cut corners. */
	const struct pl_rect *h = &e.l.hl[0];
	CHECK_EQ(px(&e.b, h->x, h->y), pl_pixel(&e.b.c, PL_COL_BG, 255), "the highlight's corner is cut");
	CHECK_EQ(px(&e.b, h->x + 1, h->y), pl_pixel(&e.b.c, PL_COL_HL, 255), "and the pixel next to it is set");
	CHECK_EQ(px(&e.b, h->x + h->w - 1, h->y + h->h - 1), pl_pixel(&e.b.c, PL_COL_BG, 255), "at the other end too");
	env_free(&e);
}

/* The digits of a 17 px bar are centred: 4 rows above, 4 below. */
static void test_crisp_centering(void)
{
	struct env e;

	crisp_env_init(&e, false, BAR);
	pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_NONE);
	uint32_t bg = pl_pixel(&e.b.c, PL_COL_BG, 255);
	int top = -1, bottom = -1;

	/* The first digit: the colon is a pixel row lower than the digits in this font. */
	for (int y = 0; y < e.l.clock.h; y++)
		if (count_not(&e.b, (struct pl_rect){ PL_MARGIN, y, 7, 1 }, bg) > 0) {
			if (top < 0)
				top = y;
			bottom = y;
		}
	CHECK_EQ(top, 4, "the clock's digits start 4 rows below the top");
	CHECK_EQ(e.l.clock.h - 1 - bottom, 4, "and end 4 rows above the line");
	/* The first digit's left ink pixel is at the margin. */
	int left = -1;
	for (int x = 0; x < e.l.clock.w && left < 0; x++)
		if (count_not(&e.b, (struct pl_rect){ x, 0, 1, e.l.clock.h }, bg) > 0)
			left = x;
	CHECK_EQ(left, PL_MARGIN, "the text starts at the margin");
	/* The text rows: centred in the row. */
	crisp_env_init(&e, true, BAR);
	pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_BTN_CLOCK);
	uint32_t row = pl_pixel(&e.b.c, PL_COL_ROW, 255);
	int t2 = -1, b2 = -1;

	for (int y = e.l.row_in.y; y < e.l.row_in.y + e.l.row_in.h; y++)
		if (count_not(&e.b, (struct pl_rect){ 0, y, 240, 1 }, row) > 0) {
			if (t2 < 0)
				t2 = y;
			b2 = y;
		}
	/* "Monday 5 October 2026" has ascenders and a descender (y): the digits and
	 * capitals are centred, the y hangs below. */
	int space_above = t2 - e.l.row_in.y;
	CHECK(space_above >= 8 && space_above <= 12, "the date is about centred in the row");
	CHECK(b2 < e.l.row_in.y + e.l.row_in.h - 4, "and clear of the row's line");
	env_free(&e);
}

/* Redrawing in the crisp style allocates nothing. */
static void test_crisp_no_alloc(void)
{
#ifdef __GLIBC__
	struct env e;

	crisp_env_init(&e, true, BAR);
	pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
	struct mallinfo2 m0 = mallinfo2();
	for (int i = 0; i < 300; i++) {
		e.st.bl_pct = i % 101;
		e.st.min = i % 60;
		pl_render_all(&e.b.c, &e.l, &e.a, &e.st, i % 4 - 1);
		pl_render_row(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_VOLUME);
		pl_render_row_value(&e.b.c, &e.l, &e.a, &e.st, PL_SLIDER_BACKLIGHT);
		pl_render_battery(&e.b.c, &e.l, &e.a, &e.st, true);
		pl_assets_prepare(&e.a, &e.l);
	}
	struct mallinfo2 m1 = mallinfo2();
	CHECK_EQ((long long)m1.uordblks, (long long)m0.uordblks, "redrawing in the crisp style allocates nothing");
	env_free(&e);
#endif
}

/* The characters of the bar (the clock, the percentage, AC and --) and of every
 * date and battery sentence have a glyph with an advance in every face that
 * text can be set in, and ink unless it is a space. */
static int proportional_missing_glyphs(const struct pl_font *f)
{
	char buf[64];
	int missing = 0;

	for (int wd = 0; wd < 7; wd++)
		for (int mon = 0; mon < 12; mon++)
			for (int md = 1; md <= 31; md++) {
				pl_date_text(buf, sizeof(buf), 2026, mon, md, wd);
				for (int face = PL_FACE_ROW; face < PL_FACES; face++)
					for (const char *c = buf; *c; c++) {
						const struct pl_pixfont *pf = f->pix[face].f;
						const uint8_t *g = pl_pixfont_glyph(pf, (unsigned char)*c);
						int ink = 0;

						for (int r = 0; g && r < pf->h; r++)
							for (int x = 0; x < pf->w; x++)
								ink += pl_pixfont_bit(pf, g, r, x);
						missing += !g || (*c != ' ' && !ink) || !pl_pixfont_adv(pf, *c);
					}
			}
	static const struct pl_estimate kinds[] = {
		{ PL_EST_NONE, 0, false }, { PL_EST_AC, 0, false }, { PL_EST_FULL, 0, false },
		{ PL_EST_NOTCHARGING, 0, false }, { PL_EST_CHARGING, 0, false },
		{ PL_EST_ESTIMATING, 0, false }, { PL_EST_LEFT, 5, false }, { PL_EST_LEFT, 90, false },
		{ PL_EST_LEFT, 80, true }, { PL_EST_TOFULL, 75, false }, { PL_EST_OVER_LEFT, 0, false },
		{ PL_EST_OVER_FULL, 0, false },
	};
	for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
		for (int v = 0; v < PL_EST_VARIANTS; v++) {
			pl_est_text(buf, sizeof(buf), &kinds[k], v ? -1 : 94, v);
			for (int face = PL_FACE_ROW; face < PL_FACES; face++)
				for (const char *c = buf; *c; c++)
					missing += pl_pixfont_glyph(f->pix[face].f, (unsigned char)*c) == NULL ||
						!pl_pixfont_adv(f->pix[face].f, (unsigned char)*c);
		}
	for (int face = 0; face < 2; face++)
		for (const char *c = "0123456789:%-AC"; *c; c++)
			missing += pl_pixfont_glyph(f->pix[face].f, (unsigned char)*c) == NULL ||
				!pl_pixfont_adv(f->pix[face].f, (unsigned char)*c);
	return missing;
}

/* The leftmost and rightmost x in the rows of band that is not the ground. */
static void ink_extent(const struct canvas_buf *b, struct pl_rect band, uint32_t ground, int *lo,
	int *hi)
{
	*lo = b->c.w;
	*hi = -1;
	for (int y = band.y; y < band.y + band.h; y++)
		for (int x = 0; x < b->c.w; x++)
			if (px(b, x, y) != ground) {
				if (x < *lo)
					*lo = x;
				if (x > *hi)
					*hi = x;
			}
}

/* DejaVu Sans, hinted and baked offline: proportional text through the same
 * fit and scale rules as the fixed fonts. */
static void test_crisp_dejavu(void)
{
	struct pl_font f;

	pl_font_load_crisp(&f, BAR, PL_CRISP_DEJAVU);
	CHECK(f.crisp && !f.ttf, "a crisp DejaVu font is neither TrueType nor the 5x7 fallback");
	CHECK_STR(f.pix[PL_FACE_BAR].f->name, "DejaVuSans Bold 11", "the bar is DejaVu Sans Bold 11");
	CHECK(f.pix[PL_FACE_SMALL].f == f.pix[PL_FACE_BAR].f, "and so is the value in the slider row");
	CHECK_STR(f.pix[PL_FACE_ROW].f->name, "DejaVuSans Bold 12", "the rows start at Bold 12");
	CHECK_STR(f.pix[PL_FACE_ROW + 1].f->name, "DejaVuSans Bold 11", "then 11");
	CHECK_STR(f.pix[PL_FACE_ROW + 2].f->name, "DejaVuSans Bold 10", "then 10");
	CHECK_STR(f.pix[PL_FACE_ROW + 3].f->name, "DejaVuSans Bold 9", "and 9");
	for (int i = 0; i < PL_FACES; i++)
		CHECK_EQ(f.pix[i].scale, 1, "every face of the default bar is at scale 1");
	CHECK_EQ(f.face[PL_FACE_BAR].digit_h, 8, "the bar's digits are 8 px high");
	CHECK_EQ(f.face[PL_FACE_BAR].digit_top, 8, "and sit on the baseline");
	CHECK_EQ(f.face[PL_FACE_ROW].digit_h, 9, "the 12 px digits are 9 px high");

	/* The glyph data is what the generator wrote: the 1 and the colon of the bold 11, and the
	 * advances that make the digits tabular. */
	{
		static const uint8_t one[24] = { 0, 0, 0x3c, 0, 0x0c, 0, 0x0c, 0, 0x0c, 0, 0x0c, 0, 0x0c, 0,
			0x0c, 0, 0x3f, 0, 0, 0, 0, 0, 0, 0 };
		static const uint8_t colon[24] = { 0, 0, 0, 0, 0, 0, 0x30, 0, 0x30, 0, 0, 0, 0, 0, 0x30, 0,
			0x30, 0, 0, 0, 0, 0, 0, 0 };
		const struct pl_pixfont *b = &pl_pixfont_dv_bold_11;

		CHECK_EQ(b->w, 14, "the bold 11 cell is 14 wide");
		CHECK_EQ(b->h, 12, "and 12 high");
		CHECK_EQ(b->ox, 1, "with the pen one column in");
		CHECK(!memcmp(pl_pixfont_glyph(b, '1'), one, sizeof(one)), "the 1 of the bold 11 is as generated");
		CHECK(!memcmp(pl_pixfont_glyph(b, ':'), colon, sizeof(colon)), "and the colon");
		CHECK_EQ(pl_pixfont_adv(b, '0'), 8, "digits advance 8");
		CHECK_EQ(pl_pixfont_adv(b, ':'), 4, "the colon 4");
		CHECK_EQ(pl_pixfont_adv(b, '%'), 11, "the percent sign 11");
		CHECK_EQ(pl_pixfont_adv(b, 0x1f), 0, "a character the font lacks does not advance");
		CHECK(pl_pixfont_glyph(b, 0x7f) == NULL, "only printable ASCII is in the font");
		CHECK(pl_pixfont_adv(&pl_pixfont_10x20, 'M') == 10 && pl_pixfont_adv(&pl_pixfont_10x20, 'i') == 10,
			"a fixed font advances by its cell");
	}
	for (int face = 0; face < PL_FACES; face++) {
		const struct pl_pixfont *pf = f.pix[face].f;

		for (int d = '1'; d <= '9'; d++)
			CHECK_EQ(pl_pixfont_adv(pf, d), pl_pixfont_adv(pf, '0'), "digits are tabular, the clock does not jitter");
		CHECK(pf->w <= 16 && pf->w > pf->ox, "a cell fits its 16 bit rows");
		for (int c = PL_PIX_FIRST; c <= PL_PIX_LAST; c++)
			CHECK(pl_pixfont_adv(pf, c) > 0 && pl_pixfont_adv(pf, c) <= pf->w, "an advance is 1 to the cell width");
	}

	/* Proportional: widths follow the glyphs. */
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_BAR, "88:88"), 36, "the clock is 36 px wide");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_BAR, "100%"), 35, "100% is 35 px");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_BAR, "20:22"), 36, "and every time is as wide as every other");
	CHECK(pl_font_text_w(&f, PL_FACE_ROW, "iiiiii") < pl_font_text_w(&f, PL_FACE_ROW, "MMMMMM"), "i is narrower than M");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_ROW, "Monday"), 56, "Monday in Bold 12");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_ROW, ""), 0, "nothing is nothing");
	CHECK_EQ(proportional_missing_glyphs(&f), 0, "every character of every date, battery sentence and bar text renders");

	/* The characters of the panel's text are all different from each other. */
	for (int face = 0; face < PL_FACES; face++) {
		const struct pl_pixfont *pf = f.pix[face].f;

		for (const char *c = "0123456789ACDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"; *c; c++)
			for (const char *d = c + 1; *d; d++)
				CHECK(memcmp(pl_pixfont_glyph(pf, (unsigned char)*c), pl_pixfont_glyph(pf, (unsigned char)*d),
					(size_t)pf->h * pf->bpr), "no two characters of the text look the same");
	}
	pl_font_free(&f);

	/* Whole-number scales, widths scale with them. */
	pl_font_load_crisp(&f, 38, PL_CRISP_DEJAVU);
	CHECK_EQ(f.pix[PL_FACE_BAR].scale, 2, "a 38 px bar has the bar font twice as big");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_BAR, "88:88"), 72, "and twice as wide");
	CHECK_EQ(pl_font_face_px(&f, PL_FACE_BAR), 24, "and tall");
	CHECK_EQ(f.pix[PL_FACE_ROW].scale, 1, "its row is still the normal size");
	pl_font_free(&f);
	pl_font_load_crisp(&f, 64, PL_CRISP_DEJAVU);
	CHECK_EQ(f.pix[PL_FACE_BAR].scale, 3, "a 64 px bar: scale 3");
	CHECK_EQ(f.pix[PL_FACE_ROW].scale, 2, "and the row: scale 2");
	CHECK_EQ(pl_font_text_w(&f, PL_FACE_ROW, "Monday"), 112, "Monday at scale 2");
	pl_font_free(&f);

	/* Layout: the same bar and rows, sized by the text. The x positions that depend on the
	 * text widths move; nothing else does, and nothing overlaps. */
	struct pl_metrics mf, mv;
	struct pl_assets af, av;
	struct pl_layout lf, lv;

	pl_assets_init_crisp(&af, BAR, 255, 255, PL_CRISP_FIXED);
	pl_assets_init_crisp(&av, BAR, 255, 255, PL_CRISP_DEJAVU);
	pl_assets_metrics(&af, &mf);
	pl_assets_metrics(&av, &mv);
	pl_layout_compute(&lf, 240, BAR, true, false, &mf);
	pl_layout_compute(&lv, 240, BAR, true, false, &mv);
	CHECK_EQ(lv.h, lf.h, "the surface is as high");
	CHECK_EQ(lv.row_h, lf.row_h, "the row is as high");
	CHECK(!memcmp(&lv.row, &lf.row, sizeof(lv.row)) && !memcmp(&lv.row_in, &lf.row_in, sizeof(lv.row_in)) &&
		!memcmp(&lv.text, &lf.text, sizeof(lv.text)) && !memcmp(&lv.row_icon, &lf.row_icon, sizeof(lv.row_icon)),
		"the row and its text area are where they were");
	CHECK_EQ(lv.slider.thumb_d, lf.slider.thumb_d, "the thumb is as big");
	CHECK_EQ(lv.slider.track_h, lf.slider.track_h, "and the track as high");
	for (int i = 0; i < PL_BUTTONS; i++) {
		CHECK(lv.button[i].h == lf.button[i].h && lv.button[i].y == lf.button[i].y,
			"a touch target is as high and in the same row");
		CHECK(lv.button[i].x >= 0 && lv.button[i].x + lv.button[i].w <= 240, "and inside the surface");
		if (i != PL_BTN_CLOCK && i != PL_BTN_BATTERY)
			CHECK_EQ(lv.button[i].w, lf.button[i].w, "an icon button is as wide");
		for (int j = i + 1; j < PL_BUTTONS; j++)
			CHECK(lv.button[i].x + lv.button[i].w <= lv.button[j].x ||
				lv.button[j].x + lv.button[j].w <= lv.button[i].x, "touch targets do not overlap");
	}
	printf("test-panel: crisp DejaVu bar: clock %d px (fixed %d), battery text %d px (fixed %d), battery button x=%d (fixed %d), backlight button x=%d (fixed %d)\n",
		mv.clock_w, mf.clock_w, mv.bat_text_w, mf.bat_text_w, lv.button[PL_BTN_BATTERY].x,
		lf.button[PL_BTN_BATTERY].x, lv.button[0].x, lf.button[0].x);
	pl_assets_free(&af);
	pl_assets_free(&av);

	/* Fit: each date and each estimate wording at the default width takes the first face and is
	 * not clipped by the row's text area; narrower outputs step down the sizes. */
	struct env e;
	int widest_date = 0, widest_est = 0, worst_lo = 1000, worst_hi = -1;

	g_cfont = PL_CRISP_DEJAVU;
	crisp_env_init(&e, true, BAR);
	CHECK_EQ(e.l.text.w, 224, "the row's text is 224 px wide");
	for (int wd = 0; wd < 7; wd++)
		for (int mon = 0; mon < 12; mon++)
			for (int md = 1; md <= 31; md += 3) {
				struct pl_rowtext t;
				int lo, hi;

				e.st.wday = wd; e.st.mon = mon; e.st.mday = md;
				pl_row_text(&e.l, &e.a, &e.st, PL_BTN_CLOCK, &t);
				CHECK_EQ(t.face, PL_FACE_ROW, "every date is set in the largest face");
				CHECK(t.w <= e.l.text.w, "and fits");
				if (t.w > widest_date)
					widest_date = t.w;
				patterned(&e.b);
				pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_BTN_CLOCK);
				CHECK(guards_intact(&e.b), "the text stays inside the buffer");
				ink_extent(&e.b, (struct pl_rect){ 0, e.l.row_in.y + 2, 240, e.l.row_in.h - 4 },
					pl_pixel(&e.b.c, PL_COL_ROW, 255), &lo, &hi);
				CHECK(lo >= e.l.text.x && hi < e.l.text.x + e.l.text.w,
					"no ink outside the text area: nothing is clipped at 240 px");
				if (lo < worst_lo)
					worst_lo = lo;
				if (hi > worst_hi)
					worst_hi = hi;
			}
	static const struct pl_estimate ests[] = {
		{ PL_EST_LEFT, 90, false }, { PL_EST_LEFT, 80, true }, { PL_EST_LEFT, 5, false },
		{ PL_EST_TOFULL, 75, false }, { PL_EST_OVER_LEFT, 0, false }, { PL_EST_OVER_FULL, 0, false },
		{ PL_EST_FULL, 0, false }, { PL_EST_NOTCHARGING, 0, false }, { PL_EST_CHARGING, 0, false },
		{ PL_EST_ESTIMATING, 0, false }, { PL_EST_AC, 0, false }, { PL_EST_NONE, 0, false },
	};
	for (size_t k = 0; k < sizeof(ests) / sizeof(ests[0]); k++)
		for (int pct = 0; pct <= 100; pct += 50) {
			struct pl_rowtext t;
			int lo, hi;

			e.st.est = ests[k];
			e.st.bat_pct = pct;
			pl_row_text(&e.l, &e.a, &e.st, PL_BTN_BATTERY, &t);
			CHECK_EQ(t.face, PL_FACE_ROW, "every battery sentence is set in the largest face");
			CHECK(t.w <= e.l.text.w, "and fits");
			if (t.w > widest_est)
				widest_est = t.w;
			patterned(&e.b);
			pl_render_all(&e.b.c, &e.l, &e.a, &e.st, PL_BTN_BATTERY);
			ink_extent(&e.b, (struct pl_rect){ 0, e.l.row_in.y + 2, 240, e.l.row_in.h - 4 },
				pl_pixel(&e.b.c, PL_COL_ROW, 255), &lo, &hi);
			CHECK(lo >= e.l.text.x && hi < e.l.text.x + e.l.text.w, "no battery text outside the text area");
		}
	printf("test-panel: crisp DejaVu: widest date %d px, widest battery sentence %d px, date ink from x=%d to x=%d of 240\n",
		widest_date, widest_est, worst_lo, worst_hi);
	env_free(&e);
	g_cfont = PL_CRISP_FIXED;
	for (int w = 100; w <= 240; w += 20) {
		struct pl_metrics m;
		struct pl_layout l;
		struct pl_assets a;
		struct pl_rowtext t;
		struct pl_state st = { .est = { PL_EST_TOFULL, 75, false }, .bat_pct = 94,
			.year = 2026, .mon = 8, .mday = 30, .wday = 3 };

		pl_assets_init_crisp(&a, BAR, 255, 255, PL_CRISP_DEJAVU);
		pl_assets_metrics(&a, &m);
		pl_layout_compute(&l, w, BAR, true, false, &m);
		pl_row_text(&l, &a, &st, PL_BTN_CLOCK, &t);
		CHECK(t.w <= l.text.w || t.face == PL_FACES - 1, "a date fits or has run out of sizes");
		if (w == 160)
			CHECK(t.face > PL_FACE_ROW, "a narrow output steps down a size");
		pl_row_text(&l, &a, &st, PL_BTN_BATTERY, &t);
		CHECK(t.w <= l.text.w || t.face == PL_FACES - 1, "an estimate fits or has run out of sizes");
		pl_assets_free(&a);
	}
}

static void test_crisp(void)
{
	test_crisp_wordings();
	test_crisp_fit();
	test_crisp_layout();
	test_crisp_icons();
	test_crisp_shapes();
	test_crisp_centering();
	test_crisp_dejavu();
	/* What does not depend on the font runs once per font: the palette is the same exact set. */
	for (int k = 0; k < 2; k++) {
		g_cfont = k ? PL_CRISP_DEJAVU : PL_CRISP_FIXED;
		printf("test-panel: crisp palette checks with the %s font\n", k ? "DejaVu" : "fixed");
		test_crisp_row_geometry();
		test_crisp_palette();
		test_crisp_formats();
		test_crisp_no_alloc();
	}
	g_cfont = PL_CRISP_FIXED;
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

/* ---- subpixel text ---- */

static uint32_t rgb_of(const struct canvas_buf *b, int x, int y)
{
	uint32_t v = px(b, x, y);

	if (b->c.fmt != PL_FMT_RGB565)
		return v & 0xffffff;
	uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, bl = v & 31;
	return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((bl << 3) | (bl >> 2));
}

static void test_subpixel_modes(void)
{
	enum { N = 0, R90 = 1 };

	/* The panel's modes. */
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_AUTO, PW_SUBPIXEL_HORIZONTAL_RGB, N), PL_SUB_RGB, "auto: horizontal_rgb is RGB");
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_AUTO, PW_SUBPIXEL_HORIZONTAL_BGR, N), PL_SUB_BGR, "auto: horizontal_bgr is BGR");
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_AUTO, PW_SUBPIXEL_HORIZONTAL_RGB, R90), PL_SUB_NONE, "auto: rotated, grayscale");
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_AUTO, PW_SUBPIXEL_VERTICAL_RGB, N), PL_SUB_NONE, "auto: vertical, grayscale");
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_AUTO, PW_SUBPIXEL_UNKNOWN, N), PL_SUB_NONE, "auto: unknown, grayscale");
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_AUTO, PW_SUBPIXEL_VERTICAL_RGB, R90), PL_SUB_BGR, "auto: h3900 rotated for portrait");
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_RGB, PW_SUBPIXEL_UNKNOWN, N), PL_SUB_RGB, "forced rgb");
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_BGR, PW_SUBPIXEL_HORIZONTAL_RGB, N), PL_SUB_BGR, "forced bgr");
	CHECK_EQ(pl_subpixel_resolve(PL_SUBOPT_NONE, PW_SUBPIXEL_HORIZONTAL_RGB, N), PL_SUB_NONE, "forced none");
	CHECK_EQ(pl_text_sub(PL_SUB_RGB, 255), PL_SUB_RGB, "an opaque ground: subpixel");
	CHECK_EQ(pl_text_sub(PL_SUB_RGB, 254), PL_SUB_NONE, "a translucent ground: grayscale");
	CHECK_EQ(pl_text_sub(PL_SUB_BGR, 0), PL_SUB_NONE, "a clear ground: grayscale");
}

static void test_lcd_filter(void)
{
	int sum = 0;

	for (int i = 0; i < PL_LCD_TAPS; i++)
		sum += pl_lcd_weights[i];
	CHECK_EQ(sum, 256, "the filter weights add up to 256");
	/* Symmetric: a mirrored glyph gets a mirrored filter. */
	CHECK(pl_lcd_weights[0] == pl_lcd_weights[4] && pl_lcd_weights[1] == pl_lcd_weights[3],
		"and are symmetric");

	/* A stem one subpixel wide, at full coverage: in[2] is out[4], and the
	 * taps spread over out[2..6]. */
	uint8_t in[5] = { 0, 0, 255, 0, 0 }, out[9];
	pl_lcd_filter(in, 5, out);
	const uint8_t stem[9] = { 0, 0, 14, 61, 106, 61, 14, 0, 0 };
	CHECK(!memcmp(out, stem, 9), "a one subpixel stem spreads into 14 61 106 61 14");

	/* The total coverage is kept, up to rounding. */
	uint8_t blob[12] = { 0, 30, 255, 255, 200, 90, 0, 255, 0, 0, 120, 60 }, ob[16];
	int a = 0, b = 0;
	pl_lcd_filter(blob, 12, ob);
	for (int i = 0; i < 12; i++)
		a += blob[i];
	for (int i = 0; i < 16; i++)
		b += ob[i];
	CHECK(abs(a - b) <= 8, "the filter keeps the energy of a glyph row");
	/* A flat run stays flat in the middle: no ringing. */
	uint8_t flat[12], of[16];
	memset(flat, 255, sizeof(flat));
	pl_lcd_filter(flat, 12, of);
	CHECK(of[6] == 255 && of[5] == 255 && of[8] == 255, "the inside of a wide stem stays solid");
	CHECK(of[0] < 20 && of[1] < 100, "and the ends ramp");
}

static void test_lcd_blend(void)
{
	/* White on dark, the three coverages meant for R, G, B. */
	uint32_t dark = 0xff1c1f24;
	uint32_t o = pl_over_lcd(dark, 0xffffff, 255, 0, 0);
	CHECK_EQ((o >> 16) & 0xff, 0xff, "red coverage lights red");
	CHECK_EQ((o >> 8) & 0xff, 0x1f, "and leaves green");
	CHECK_EQ(o & 0xff, 0x24, "and blue");
	o = pl_over_lcd(dark, 0xffffff, 0, 0, 255);
	CHECK(((o >> 16) & 0xff) == 0x1c && (o & 0xff) == 0xff, "blue coverage lights blue only");
	o = pl_over_lcd(dark, 0xffffff, 255, 255, 255);
	CHECK_EQ(o & 0xffffff, 0xffffff, "full coverage is the colour");
	o = pl_over_lcd(dark, 0xffffff, 0, 0, 0);
	CHECK_EQ(o & 0xffffff, dark & 0xffffff, "no coverage is the ground");
	CHECK_EQ(o >> 24, 0xff, "and the result is opaque");
	/* Linear light: half coverage of white over black is well above 128. */
	o = pl_over_lcd(0xff000000, 0xffffff, 128, 128, 128);
	CHECK(((o >> 16) & 0xff) > 170 && ((o >> 16) & 0xff) < 200, "half coverage is blended in linear light");
	CHECK(((o >> 16) & 0xff) == ((o >> 8) & 0xff) && ((o >> 8) & 0xff) == (o & 0xff),
		"equal coverages: equal channels");

	/* pl_blit_lcd: one pixel whose leftmost subpixel is covered. */
	uint8_t cov[3] = { 255, 0, 0 };
	struct pl_mask m = { 3, 1, cov };
	struct canvas_buf b;
	canvas_init(&b, 4, 2, PL_FMT_XRGB8888);
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 4, 2 }, 0x1c1f24, 255);
	pl_blit_lcd(&b.c, &m, 1, 0, 0xffffff, PL_SUB_RGB);
	CHECK_EQ(rgb_of(&b, 1, 0), 0xff1f24, "RGB: the left subpixel is red");
	CHECK_EQ(rgb_of(&b, 0, 0), 0x1c1f24, "the neighbour is untouched");
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 4, 2 }, 0x1c1f24, 255);
	pl_blit_lcd(&b.c, &m, 1, 0, 0xffffff, PL_SUB_BGR);
	CHECK_EQ(rgb_of(&b, 1, 0), 0x1c1fff, "BGR: the left subpixel is blue");
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 4, 2 }, 0x1c1f24, 255);
	pl_blit_lcd(&b.c, &m, 1, 0, 0xffffff, PL_SUB_NONE);
	CHECK_EQ(rgb_of(&b, 1, 0), 0x1c1f24, "no order: nothing is drawn (the caller uses the gray path)");
	CHECK(guards_intact(&b), "the blit stays in the buffer");

	/* Clipped at both edges. */
	uint8_t wide[9] = { 255, 255, 255, 255, 255, 255, 255, 255, 255 };
	struct pl_mask wm = { 9, 1, wide };
	pl_blit_lcd(&b.c, &wm, -1, 1, 0xffffff, PL_SUB_RGB);
	CHECK_EQ(rgb_of(&b, 0, 1), 0xffffff, "clipped at the left");
	CHECK_EQ(rgb_of(&b, 1, 1), 0xffffff, "the pixels inside the canvas are drawn");
	CHECK_EQ(rgb_of(&b, 3, 1), 0x1c1f24, "and the one past the mask is not");
	CHECK(guards_intact(&b), "clipping keeps to the buffer");
	free(b.mem);

	/* Equal coverages are what the grayscale path draws. */
	uint8_t g3[3] = { 100, 100, 100 }, g1[1] = { 100 };
	struct pl_mask lm = { 3, 1, g3 }, gm = { 1, 1, g1 };
	struct canvas_buf x, y;
	canvas_init(&x, 2, 1, PL_FMT_XRGB8888);
	canvas_init(&y, 2, 1, PL_FMT_XRGB8888);
	pl_fill(&x.c, (struct pl_rect){ 0, 0, 2, 1 }, 0x1c1f24, 255);
	pl_fill(&y.c, (struct pl_rect){ 0, 0, 2, 1 }, 0x1c1f24, 255);
	pl_blit_lcd(&x.c, &lm, 0, 0, 0xe8eaed, PL_SUB_RGB);
	pl_blit(&y.c, &gm, 0, 0, 0xe8eaed, 255, NULL);
	CHECK_EQ(px(&x, 0, 0), px(&y, 0, 0), "R=G=B coverage equals the grayscale blend");
	free(x.mem);
	free(y.mem);

	/* RGB565: blended in 8 bits, then packed. */
	struct canvas_buf s;
	uint8_t c2[3] = { 200, 90, 10 };
	struct pl_mask m2 = { 3, 1, c2 };
	canvas_init(&s, 1, 1, PL_FMT_RGB565);
	pl_fill(&s.c, (struct pl_rect){ 0, 0, 1, 1 }, 0x1c1f24, 255);
	uint32_t before = rgb_of(&s, 0, 0);
	pl_blit_lcd(&s.c, &m2, 0, 0, 0xffffff, PL_SUB_RGB);
	uint32_t e8 = pl_over_lcd(0xff000000 | before, 0xffffff, 200, 90, 10);
	uint32_t want565 = ((((e8 >> 16) & 0xff) * 31 + 127) / 255) << 11 |
		((((e8 >> 8) & 0xff) * 63 + 127) / 255) << 5 | (((e8 & 0xff) * 31 + 127) / 255);
	CHECK_EQ(px(&s, 0, 0), want565, "565 is the packed 8 bit blend");
	free(s.mem);
}

static void test_lcd_text(void)
{
	struct pl_font probe;

	if (!pl_font_load(&probe, NULL, 12, 11)) {
		printf("test-panel: no default font, the subpixel text checks are skipped\n");
		return;
	}
	pl_font_free(&probe);
	struct env e;
	env_init(&e, PL_FMT_XRGB8888, 255, 255, true, NULL);
	const struct pl_font *f = &e.a.font;
	CHECK(f->face[0].g[0].lcd.a != NULL, "glyphs have a subpixel mask");
	CHECK(f->face[0].g[0].lcd.w % 3 == 0, "of whole pixels");

	/* The clock does not move when the minute changes: the digits are tabular
	 * and advances are whole pixels. */
	int w0 = pl_font_text_w(f, 0, "00:00");
	CHECK_EQ(pl_font_text_w(f, 0, "59:59"), w0, "clock text is as wide at 59:59");
	CHECK_EQ(pl_font_text_w(f, 0, "11:11"), w0, "and at 11:11");
	CHECK_EQ(pl_font_text_w(f, 0, "88:88"), w0, "and at 88:88");
	for (int d = 1; d <= 9; d++)
		CHECK_EQ(f->face[0].g[d].adv, f->face[0].g[0].adv, "every digit has the advance of the 0");

	/* The same string drawn twice looks the same: the pen is on a whole pixel,
	 * so the subpixel phase does not depend on where the text starts. */
	struct canvas_buf b;
	canvas_init(&b, 200, 20, PL_FMT_XRGB8888);
	pl_fill(&b.c, (struct pl_rect){ 0, 0, 200, 20 }, PL_COL_BG, 255);
	pl_font_draw(&b.c, f, 0, 10, 0, 20, "12:34", PL_COL_FG, PL_SUB_RGB);
	pl_font_draw(&b.c, f, 0, 110, 0, 20, "12:34", PL_COL_FG, PL_SUB_RGB);
	bool same = true;
	for (int y = 0; y < 20; y++)
		for (int x = 0; x < 80; x++)
			if (px(&b, 10 + x, y) != px(&b, 110 + x, y))
				same = false;
	CHECK(same, "equal strings are drawn equal wherever they start");
	/* The height of the digits is a whole number of pixels: the flat top of
	 * the 7 is a crisp row. */
	const struct pl_glyph *seven = &f->face[0].g[7];
	int top_max = 0;
	for (int x = 0; x < seven->m.w; x++)
		if (seven->m.a[x] > top_max)
			top_max = seven->m.a[x];
	CHECK(top_max >= 250, "the top bar of a 7 starts on a pixel edge");
	CHECK_EQ(pl_font_baseline(f, 0, 0, 20), (20 - f->face[0].digit_h) / 2 + f->face[0].digit_top,
		"the baseline is an integer by construction");

	/* Colour appears at the edges with a mode, and not without. */
	struct canvas_buf gcv;
	canvas_init(&gcv, 200, 20, PL_FMT_XRGB8888);
	pl_fill(&gcv.c, (struct pl_rect){ 0, 0, 200, 20 }, PL_COL_BG, 255);
	pl_font_draw(&gcv.c, f, 0, 10, 0, 20, "12:34", PL_COL_FG, PL_SUB_NONE);
	/* The ground is a little blue, so R-B of an untouched pixel is not 0; a
	 * grayscale pixel keeps the hue of the ground to the text colour line, a
	 * fringe leaves it. */
	int fringes_sub = 0, fringes_gray = 0;
	for (int y = 0; y < 20; y++)
		for (int x = 0; x < 80; x++) {
			uint32_t s = rgb_of(&b, 10 + x, y), g = rgb_of(&gcv, 10 + x, y);
			int ds = (int)((s >> 16) & 0xff) - (int)(s & 0xff);
			int dg = (int)((g >> 16) & 0xff) - (int)(g & 0xff);
			if (abs(ds) > 40)
				fringes_sub++;
			if (abs(dg) > 40)
				fringes_gray++;
		}
	CHECK(fringes_sub > 8, "subpixel text has colour at its edges");
	CHECK_EQ(fringes_gray, 0, "grayscale text has none");

	/* The order is the panel's: on the left edge of a light stem the first
	 * coverage is under the stripe nearest the stem, the rightmost of the
	 * pixel, which is blue on an RGB panel and red on a BGR one. */
	for (int order = 0; order < 2; order++) {
		struct canvas_buf one;
		canvas_init(&one, 40, 20, PL_FMT_XRGB8888);
		pl_fill(&one.c, (struct pl_rect){ 0, 0, 40, 20 }, PL_COL_BG, 255);
		pl_font_draw(&one.c, f, 0, 10, 0, 20, "1", PL_COL_FG, order ? PL_SUB_BGR : PL_SUB_RGB);
		int x = 0, row = 10, dr = 0, db = 0;
		while (x < 40 && rgb_of(&one, x, row) == PL_COL_BG)
			x++;
		if (x < 40) {
			dr = (int)((rgb_of(&one, x, row) >> 16) & 0xff) - (int)((PL_COL_BG >> 16) & 0xff);
			db = (int)(rgb_of(&one, x, row) & 0xff) - (int)(PL_COL_BG & 0xff);
		}
		CHECK(order ? dr > db : db > dr, order ? "BGR: the left edge of a stem is red" :
			"RGB: the left edge of a stem is blue");
		free(one.mem);
	}
	free(b.mem);
	free(gcv.mem);
	env_free(&e);

	/* The rule: a ground that is not opaque gets grayscale, whatever the mode. */
	struct env t, u;
	env_init(&t, PL_FMT_ARGB8888, 255, 224, true, NULL);
	env_init(&u, PL_FMT_ARGB8888, 255, 224, true, NULL);
	t.a.sub = PL_SUB_RGB;
	u.a.sub = PL_SUB_NONE;
	pl_render_all(&t.b.c, &t.l, &t.a, &t.st, PL_SLIDER_BACKLIGHT);
	pl_render_all(&u.b.c, &u.l, &u.a, &u.st, PL_SLIDER_BACKLIGHT);
	CHECK(!memcmp(t.b.c.data + (size_t)t.l.row.y * t.b.c.stride,
			u.b.c.data + (size_t)u.l.row.y * u.b.c.stride,
			(size_t)t.l.row_h * t.b.c.stride), "popup-alpha 224: the row text is grayscale in every mode");
	CHECK(memcmp(t.b.c.data, u.b.c.data, (size_t)t.l.bar_h * t.b.c.stride) != 0,
		"while the opaque bar next to it is drawn with subpixels");
	env_free(&t);
	env_free(&u);
	env_init(&t, PL_FMT_ARGB8888, 128, 255, false, NULL);
	env_init(&u, PL_FMT_ARGB8888, 128, 255, false, NULL);
	t.a.sub = PL_SUB_RGB;
	u.a.sub = PL_SUB_NONE;
	pl_render_all(&t.b.c, &t.l, &t.a, &t.st, PL_SLIDER_NONE);
	pl_render_all(&u.b.c, &u.l, &u.a, &u.st, PL_SLIDER_NONE);
	CHECK(!memcmp(t.b.c.data, u.b.c.data, (size_t)t.l.bar_h * t.b.c.stride),
		"bar-alpha 128: the bar text is grayscale");
	env_free(&t);
	env_free(&u);
	env_init(&t, PL_FMT_RGB565, 255, 255, true, NULL);
	env_init(&u, PL_FMT_RGB565, 255, 255, true, NULL);
	t.a.sub = PL_SUB_RGB;
	u.a.sub = PL_SUB_NONE;
	pl_render_all(&t.b.c, &t.l, &t.a, &t.st, PL_SLIDER_BACKLIGHT);
	pl_render_all(&u.b.c, &u.l, &u.a, &u.st, PL_SLIDER_BACKLIGHT);
	CHECK(memcmp(t.b.c.data + (size_t)t.l.row.y * t.b.c.stride,
			u.b.c.data + (size_t)u.l.row.y * u.b.c.stride,
			(size_t)t.l.row_h * t.b.c.stride) != 0,
		"popup-alpha 255: the value in the row gets subpixels too");
	CHECK(guards_intact(&t.b), "565 subpixel rendering stays in the buffer");
	env_free(&t);
	env_free(&u);
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


	/* The attributes the estimate uses, as a DS2760 has them: signed current,
	 * charge with a reserve, a time in seconds, and no current_avg. */
	snprintf(root, sizeof(root), "%s/ps3", base);
	struct pl_batt_raw raw;
	pl_battery_read_raw(root, &raw);
	CHECK(raw.st == PL_BAT_NONE && raw.charger == -1, "raw: no power_supply directory");
	supply(root, "ds2760-battery.0", "Battery\n", "capacity", "94\n", "Discharging\n");
	snprintf(dev, sizeof(dev), "%s/class/power_supply/ds2760-battery.0", root);
	write_file(dev, "current_now", "-600000\n");
	write_file(dev, "charge_now", "950000\n");
	write_file(dev, "charge_empty", "50000\n");
	write_file(dev, "charge_full", "1000000\n");
	write_file(dev, "charge_full_design", "1100000\n");
	write_file(dev, "time_to_empty_now", "5400\n");
	write_file(dev, "voltage_now", "3900000\n");
	pl_battery_read_raw(root, &raw);
	CHECK(raw.st == PL_BAT_DISCHARGING && raw.pct == 94 && raw.charger == -1 && !raw.not_charging,
		"raw: the battery and no charger supply");
	CHECK(raw.current_now == -600000 && raw.charge_now == 950000 && raw.charge_empty == 50000 &&
		raw.charge_full == 1000000 && raw.charge_full_design == 1100000 &&
		raw.time_to_empty_now == 5400, "raw: the attributes, the current signed");
	CHECK(raw.current_avg == PL_ABSENT && raw.energy_now == PL_ABSENT && raw.power_now == PL_ABSENT &&
		raw.energy_full == PL_ABSENT && raw.energy_full_design == PL_ABSENT,
		"raw: what the battery does not have is absent");
	supply(root, "ac", "Mains\n", "online", "0\n", NULL);
	pl_battery_read_raw(root, &raw);
	CHECK_EQ(raw.charger, 0, "raw: a charger that is offline");
	supply(root, "ac", "Mains\n", "online", "1\n", NULL);
	pl_battery_read_raw(root, &raw);
	CHECK_EQ(raw.charger, 1, "raw: online");
	supply(root, "usb", "USB\n", "online", "0\n", NULL);
	pl_battery_read_raw(root, &raw);
	CHECK_EQ(raw.charger, 1, "raw: one of two online is online");
	write_file(dev, "status", "Not charging\n");
	pl_battery_read_raw(root, &raw);
	CHECK(raw.not_charging && raw.st == PL_BAT_DISCHARGING, "raw: Not charging is its own flag");
	write_file(dev, "status", "Charging\n");
	write_file(dev, "current_now", "400000\n");
	write_file(dev, "current_avg", "350000\n");
	write_file(dev, "energy_now", "6000000\n");
	write_file(dev, "energy_full", "10000000\n");
	write_file(dev, "energy_full_design", "11000000\n");
	write_file(dev, "power_now", "2000000\n");
	pl_battery_read_raw(root, &raw);
	CHECK(raw.st == PL_BAT_CHARGING && raw.current_now == 400000 && raw.current_avg == 350000 &&
		raw.energy_now == 6000000 && raw.energy_full == 10000000 &&
		raw.energy_full_design == 11000000 && raw.power_now == 2000000, "raw: a generic battery's attributes");
	write_file(dev, "charge_now", "garbage\n");
	write_file(dev, "charge_full", "-5\n");
	write_file(dev, "charge_empty", "\n");
	write_file(dev, "current_now", "99999999999999999999\n");
	write_file(dev, "time_to_empty_now", "-1\n");
	pl_battery_read_raw(root, &raw);
	CHECK(raw.charge_now == PL_ABSENT && raw.charge_full == PL_ABSENT && raw.charge_empty == PL_ABSENT &&
		raw.current_now == PL_ABSENT && raw.time_to_empty_now == PL_ABSENT,
		"raw: garbage, negative charge and overflow are absent");
	pl_battery_read(root, &st, &pct);
	CHECK(st == PL_BAT_CHARGING && pct == 94, "pl_battery_read still gives status and percent");
	snprintf(root, sizeof(root), "%s/ps4", base);
	supply(root, "ac", "Mains\n", "online", "1\n", NULL);
	pl_battery_read_raw(root, &raw);
	CHECK(raw.st == PL_BAT_AC && raw.charger == 1 && raw.pct == -1, "raw: mains without a battery");

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
	if (system(cmd) != 0)
		exit(2);
}

/* The strip: the panel on the short side of an output rotated by 90 or 270
 * degrees, drawn in its own orientation and turned by the buffer transform. */
/* The turn and the panel's layout an output has for the panel: the hint when it
 * says the display turns the output, else geometry. */
/* ---- the look: style and height from the density ---- */

static struct pl_look look_of(enum pl_styleopt st, int dpi, int height, int w, int h, int mw,
	int mh)
{
	struct pl_look_in in = { st, dpi, height, w, h, mw, mh };
	struct pl_look l;

	pl_look_resolve(&in, &l);
	return l;
}

static void test_look(void)
{
	struct pl_look l;

	/* The boards, as their kernels report them. */
	CHECK_EQ(pl_density_ppi(240, 320, 53, 71), 115, "h2200 53x71: 115 ppi");
	CHECK_EQ(pl_density_ppi(240, 320, 57, 77), 106, "h5xxx 57x77: 106 ppi");
	CHECK_EQ(pl_density_ppi(320, 240, 77, 57), 106, "h3900 77x57 in the landscape scan frame: 106");
	CHECK_EQ(pl_density_ppi(240, 320, 77, 57), 106, "the same panel, the mode in portrait");
	CHECK_EQ(pl_density_ppi(480, 640, 60, 80), 203, "hx4700 480x640 on 60x80: 203 ppi");
	CHECK_EQ(pl_density_ppi(240, 320, 60, 80), 102, "hx4700 240x320 on 60x80: 102 ppi");
	/* The diagonal does not care how the mode or the size is turned. */
	CHECK_EQ(pl_density_ppi(640, 480, 60, 80), 203, "mode swapped (hardware rotation): the same");
	CHECK_EQ(pl_density_ppi(640, 480, 80, 60), 203, "both swapped: the same");
	CHECK_EQ(pl_density_ppi(0, 320, 57, 77), 0, "no mode: unknown");
	CHECK_EQ(pl_density_ppi(240, 320, 0, 0), 0, "0x0 mm (old kernels): unknown");
	CHECK_EQ(pl_density_ppi(240, 320, 57, 0), 0, "a zero side: unknown");
	CHECK_EQ(pl_density_ppi(240, 320, -1, 77), 0, "a negative size: unknown");

	l = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 53, 71);
	CHECK(l.crisp && !l.vga && l.src == PL_DPI_REPORTED && l.height == 18, "h2200: crisp, 18");
	l = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 57, 77);
	CHECK(l.crisp && l.ppi == 106 && l.height == 18, "h5xxx: crisp, 18");
	l = look_of(PL_STYLE_AUTO, 0, 0, 320, 240, 77, 57);
	CHECK(l.crisp && l.ppi == 106 && l.height == 18, "h3900 in the scan frame: crisp, 18");
	l = look_of(PL_STYLE_AUTO, 0, 0, 480, 640, 60, 80);
	CHECK(!l.crisp && l.vga && l.ppi == 203 && l.src == PL_DPI_REPORTED, "hx4700 VGA: smooth");
	CHECK_EQ(l.height, 34, "hx4700 VGA: 18 * 203 / 110 = 33.2, 33, made even: 34");
	l = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 60, 80);
	CHECK(l.crisp && l.ppi == 102 && l.height == 18, "hx4700 QVGA mode on the same size: crisp, 18");
	l = look_of(PL_STYLE_AUTO, 0, 0, 640, 480, 60, 80);
	CHECK(!l.crisp && l.ppi == 203, "hx4700 with the mode reported swapped: still smooth");

	/* Without a size: the class of the mode. */
	l = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 0, 0);
	CHECK(l.crisp && l.src == PL_DPI_FALLBACK && l.ppi == 110 && !l.vga && l.height == 18,
		"0 mm, 240x320: the QVGA class");
	l = look_of(PL_STYLE_AUTO, 0, 0, 480, 640, 0, 0);
	CHECK(!l.crisp && l.src == PL_DPI_FALLBACK && l.ppi == 200 && l.vga && l.height == 34,
		"0 mm, 480x640: the VGA class");
	l = look_of(PL_STYLE_AUTO, 0, 0, 640, 480, 0, 0);
	CHECK(l.vga && l.src == PL_DPI_FALLBACK, "0 mm, 640x480: the VGA class, the long side counts");
	l = look_of(PL_STYLE_AUTO, 0, 0, 639, 400, 0, 0);
	CHECK(!l.vga, "0 mm, a long side of 639: QVGA");
	l = look_of(PL_STYLE_AUTO, 0, 0, 0, 0, 0, 0);
	CHECK(l.src == PL_DPI_FALLBACK && !l.vga && l.crisp, "nothing at all: the QVGA class");
	l = look_of(PL_STYLE_AUTO, 0, 0, 800, 600, 0, 0);
	CHECK(l.vga && !l.crisp, "0 mm, larger than VGA: the VGA class");

	/* The threshold: at 150 smooth, below crisp, decided on the rounded ppi
	 * that is logged. */
	l = look_of(PL_STYLE_AUTO, 150, 0, 240, 320, 0, 0);
	CHECK(!l.crisp && l.vga && l.ppi == 150, "--dpi 150: at the threshold, smooth");
	l = look_of(PL_STYLE_AUTO, 149, 0, 240, 320, 0, 0);
	CHECK(l.crisp && !l.vga && l.ppi == 149, "--dpi 149: crisp");
	CHECK_EQ(pl_density_ppi(240, 320, 40, 54), 151, "40x54 mm: 151 ppi");
	CHECK_EQ(pl_density_ppi(240, 320, 40, 55), 149, "40x55 mm: 149 ppi");
	l = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 40, 54);
	CHECK(!l.crisp && l.ppi == 151 && l.height == 26, "151 ppi reported: smooth, 18 * 151 / 110 = 24.7: 26");
	l = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 40, 55);
	CHECK(l.crisp && l.ppi == 149 && l.height == 18, "149 ppi reported: crisp");

	/* The override. */
	l = look_of(PL_STYLE_AUTO, 300, 0, 240, 320, 57, 77);
	CHECK(!l.crisp && l.src == PL_DPI_OVERRIDE && l.ppi == 300, "--dpi 300 over a QVGA output: smooth");
	l = look_of(PL_STYLE_AUTO, 100, 0, 480, 640, 60, 80);
	CHECK(l.crisp && l.src == PL_DPI_OVERRIDE && l.ppi == 100 && l.height == 18, "--dpi 100 over a VGA output: crisp");
	l = look_of(PL_STYLE_AUTO, 1, 0, 240, 320, 0, 0);
	CHECK_EQ(l.ppi, PL_DPI_MIN, "the override is held to its range (low)");
	l = look_of(PL_STYLE_AUTO, 99999, 0, 240, 320, 0, 0);
	CHECK_EQ(l.ppi, PL_DPI_MAX, "the override is held to its range (high)");

	/* The explicit styles win over the density. */
	l = look_of(PL_STYLE_SMOOTH, 0, 0, 240, 320, 57, 77);
	CHECK(!l.crisp && l.ppi == 106 && l.height == 18, "--style smooth on a QVGA panel: 18");
	l = look_of(PL_STYLE_CRISP, 0, 0, 480, 640, 60, 80);
	CHECK(l.crisp && l.ppi == 203 && l.height == 18, "--style crisp on a VGA panel: 18, not scaled");
	l = look_of(PL_STYLE_SMOOTH, 0, 0, 480, 640, 60, 80);
	CHECK(!l.crisp && l.height == 34, "--style smooth on a VGA panel: scaled");
	l = look_of(PL_STYLE_CRISP, 300, 0, 240, 320, 57, 77);
	CHECK(l.crisp && l.ppi == 300, "--style crisp with --dpi 300: crisp, the density is still known");

	/* The height: an explicit one wins in both classes, within the range. */
	l = look_of(PL_STYLE_AUTO, 0, 40, 480, 640, 60, 80);
	CHECK_EQ(l.height, 40, "--height 40 on a VGA panel");
	l = look_of(PL_STYLE_AUTO, 0, 40, 240, 320, 57, 77);
	CHECK_EQ(l.height, 40, "--height 40 on a QVGA panel (crisp, whole-number scales)");
	l = look_of(PL_STYLE_AUTO, 0, 500, 480, 640, 60, 80);
	CHECK_EQ(l.height, PL_HEIGHT_MAX, "--height too large: clamped");
	l = look_of(PL_STYLE_AUTO, 0, 3, 480, 640, 60, 80);
	CHECK_EQ(l.height, PL_HEIGHT_MIN, "--height too small: clamped");

	/* The height of the smooth style: 18 at 110, even, within the range. */
	CHECK_EQ(pl_height_for_ppi(110), 18, "110 ppi: 18");
	CHECK_EQ(pl_height_for_ppi(106), 18, "106 ppi: 17.3, 17, made even: 18");
	CHECK_EQ(pl_height_for_ppi(150), 26, "150 ppi: 24.5, 25, made even: 26");
	CHECK_EQ(pl_height_for_ppi(200), 34, "200 ppi: 32.7, 33, made even: 34");
	CHECK_EQ(pl_height_for_ppi(220), 36, "220 ppi: 36");
	CHECK_EQ(pl_height_for_ppi(20), PL_HEIGHT_MIN, "20 ppi: the least");
	CHECK_EQ(pl_height_for_ppi(1000), PL_HEIGHT_MAX, "1000 ppi: the most");
	for (int ppi = PL_DPI_MIN; ppi <= PL_DPI_MAX; ppi++) {
		int h = pl_height_for_ppi(ppi);

		if (h < PL_HEIGHT_MIN || h > PL_HEIGHT_MAX || (h & 1) || h < pl_height_for_ppi(ppi - 1 < PL_DPI_MIN ? PL_DPI_MIN : ppi - 1)) {
			CHECK(0, "the height is in range, even and does not shrink with the density");
			break;
		}
	}

	/* The re-evaluation: what a mode switch, a rotation or a hotplug does. */
	struct pl_look a = look_of(PL_STYLE_AUTO, 0, 0, 480, 640, 60, 80), b;
	b = look_of(PL_STYLE_AUTO, 0, 0, 640, 480, 60, 80);
	CHECK(pl_look_same(&a, &b), "a rotation (the mode turned) changes nothing");
	b = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 60, 80);
	CHECK(!pl_look_same(&a, &b) && b.crisp && b.height == 18, "the hx4700 switched to 240x320: crisp, 18");
	a = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 0, 0);
	b = look_of(PL_STYLE_AUTO, 0, 0, 240, 320, 57, 77);
	CHECK(!pl_look_same(&a, &b) && a.crisp == b.crisp && a.height == b.height,
		"the size arrived: the source changed, the look did not");
	CHECK_STR(pl_dpi_src_name(PL_DPI_REPORTED), "reported", "source name");
	CHECK_STR(pl_dpi_src_name(PL_DPI_FALLBACK), "fallback", "source name");
	CHECK_STR(pl_dpi_src_name(PL_DPI_OVERRIDE), "override", "source name");
}

static void test_rot_hint(void)
{
	struct pl_rot_hint none = { 0 };
	struct pl_rot_hint hw = { true, true, 1, 1 };	/* 90, horizontal_rgb */
	struct pl_rot_hint sw = { true, false, 1, 1 };
	int t, s;

	pl_rot_resolve(NULL, 0, 4, &t, &s);
	CHECK(t == 0 && s == 4, "no hint: geometry (old picowl, another compositor)");
	pl_rot_resolve(&none, 3, 2, &t, &s);
	CHECK(t == 3 && s == 2, "a hint that never came: geometry");
	pl_rot_resolve(&hw, 0, 4, &t, &s);
	CHECK(t == 1 && s == 1, "hardware: the hint's turn and the panel's own layout");
	pl_rot_resolve(&sw, 3, 2, &t, &s);
	CHECK(t == 3 && s == 2, "software: geometry is used, not the hint");

	/* What the panel does with it at each turn: a strip at 90 and 270 only. */
	for (int rot = 0; rot < 8; rot++) {
		struct pl_rot_hint h = { true, true, rot, 1 };

		pl_rot_resolve(&h, 0, 4, &t, &s);
		CHECK_EQ(pl_strip_for(t, false), pl_strip_for(rot, false),
			"under hardware rotation the strip follows the hint");
	}
	pl_rot_resolve(&hw, 0, 4, &t, &s);
	CHECK_EQ(pl_strip_for(t, true), PL_STRIP_NONE, "--edge top wins over the hint");
}

static void test_strip(void)
{
	CHECK_EQ(pl_strip_for(1, false), PL_STRIP_90, "90 is a strip");
	CHECK_EQ(pl_strip_for(3, false), PL_STRIP_270, "270 is a strip");
	for (int t = 0; t < 8; t++)
		if (t != 1 && t != 3)
			CHECK_EQ(pl_strip_for(t, false), PL_STRIP_NONE,
				"normal, 180 and the flipped transforms keep the bar on the top");
	CHECK_EQ(pl_strip_for(1, true), PL_STRIP_NONE, "--edge top at 90");
	CHECK_EQ(pl_strip_for(3, true), PL_STRIP_NONE, "--edge top at 270");
	CHECK_EQ(pl_strip_for(-1, false), PL_STRIP_NONE, "a bad transform is none");

	CHECK_EQ(pl_edge_for(PL_STRIP_NONE, false), PL_EDGE_TOP, "top edge");
	CHECK_EQ(pl_edge_for(PL_STRIP_NONE, true), PL_EDGE_BOTTOM, "bottom edge");
	CHECK_EQ(pl_edge_for(PL_STRIP_90, false), PL_EDGE_RIGHT, "physical top at 90");
	CHECK_EQ(pl_edge_for(PL_STRIP_270, false), PL_EDGE_LEFT, "physical top at 270");
	CHECK_EQ(pl_edge_for(PL_STRIP_90, true), PL_EDGE_LEFT, "physical bottom at 90");
	CHECK_EQ(pl_edge_for(PL_STRIP_270, true), PL_EDGE_RIGHT, "physical bottom at 270");

	int bw, bh;
	pl_strip_buffer_size(PL_STRIP_NONE, 240, 54, &bw, &bh);
	CHECK(bw == 240 && bh == 54, "no strip: the buffer is the surface");
	pl_strip_buffer_size(PL_STRIP_90, 54, 240, &bw, &bh);
	CHECK(bw == 240 && bh == 54, "a strip: the axes change places");
	pl_strip_buffer_size(PL_STRIP_270, 18, 240, &bw, &bh);
	CHECK(bw == 240 && bh == 18, "a strip of 270: the axes change places");

	/* Every pixel of the buffer is one pixel of the surface and back, and the
	 * rectangle of a pixel is the pixel's place, in both turns. */
	for (int strip = PL_STRIP_90; strip <= PL_STRIP_270; strip += 2) {
		bool all = true, in = true;
		for (int y = 0; y < 54; y++)
			for (int x = 0; x < 240; x++) {
				struct pl_rect r = pl_rect_to_surface(strip,
					(struct pl_rect){ x, y, 1, 1 }, 240, 54);
				int bx, by;
				pl_point_to_buffer(strip, r.x, r.y, 240, 54, &bx, &by);
				all = all && r.w == 1 && r.h == 1 && bx == x && by == y;
				in = in && r.x >= 0 && r.y >= 0 && r.x < 54 && r.y < 240;
			}
		CHECK(all, "a pixel of the buffer maps to one pixel of the surface and back");
		CHECK(in, "the pixels of the buffer land inside the surface");
	}

	/* The bar of the buffer is its first rows: the surface's columns next to
	 * the edge, with the row inward of them. */
	struct pl_rect bar = { 0, 0, 240, 18 }, row = { 0, 18, 240, 36 };
	struct pl_rect a = pl_rect_to_surface(PL_STRIP_90, bar, 240, 54);
	CHECK(a.x == 36 && a.y == 0 && a.w == 18 && a.h == 240, "90: the bar is the right 18 columns");
	a = pl_rect_to_surface(PL_STRIP_90, row, 240, 54);
	CHECK(a.x == 0 && a.y == 0 && a.w == 36 && a.h == 240, "90: the row is inward of the bar");
	a = pl_rect_to_surface(PL_STRIP_270, bar, 240, 54);
	CHECK(a.x == 0 && a.y == 0 && a.w == 18 && a.h == 240, "270: the bar is the left 18 columns");
	a = pl_rect_to_surface(PL_STRIP_270, row, 240, 54);
	CHECK(a.x == 18 && a.y == 0 && a.w == 36 && a.h == 240, "270: the row is inward of the bar");
	a = pl_rect_to_surface(PL_STRIP_NONE, row, 240, 54);
	CHECK(a.x == 0 && a.y == 18 && a.w == 240 && a.h == 36, "no strip: a rectangle is left as it is");

	/* The left end of the bar (the clock) is at the top of the strip at 90 and
	 * at the bottom at 270. */
	int bx, by;
	pl_point_to_buffer(PL_STRIP_90, 50, 0, 240, 54, &bx, &by);
	CHECK(bx == 0 && by == 3, "90: the top of the strip is the left of the bar");
	pl_point_to_buffer(PL_STRIP_270, 3, 239, 240, 54, &bx, &by);
	CHECK(bx == 0 && by == 3, "270: the bottom of the strip is the left of the bar");
	pl_point_to_buffer(PL_STRIP_NONE, 7, 9, 240, 54, &bx, &by);
	CHECK(bx == 7 && by == 9, "no strip: a point is left as it is");
}

int main(void)
{
	printf("test-panel: unit tests\n");
	test_mapping();
	test_layout();
	test_layout_bottom();
	test_strip();
	test_rot_hint();
	test_look();
	test_touch();
	test_popup();
	test_text_buttons();
	test_estimator();
	test_blend();
	test_masks();
	test_font();
	test_draw();
	test_crisp();
	test_subpixel_modes();
	test_lcd_filter();
	test_lcd_blend();
	test_lcd_text();
	test_sys();
	printf("test-panel: %d checks, %d failed\n", test_count, fail_count);
	return fail_count ? 1 : 0;
}
