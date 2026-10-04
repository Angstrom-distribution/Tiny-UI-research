/*
 * panel-logic.h - the pure part of picowl-panel: value mapping, text, the
 * layout rectangles, the touch state machine and the 5x7 font. No wayland, no
 * alsa, no sysfs, so the unit test can run it anywhere.
 *
 * The panel is two rows of row_h pixels (the surface height / 2), width w:
 *   row 1: clock on the left half, battery on the right half;
 *   row 2: backlight slider on the left half, volume slider on the right half.
 */
#ifndef PICOWL_PANEL_LOGIC_H
#define PICOWL_PANEL_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PL_HEIGHT_DEFAULT 56
#define PL_HEIGHT_MIN 40
#define PL_HEIGHT_MAX 120
#define PL_SCALE_MAX 4

/* The backlight slider never goes below this, so the screen cannot be turned
 * black by accident. */
#define PL_BL_FLOOR_PCT 5

/* Writes to the system during a drag are at most this far apart. */
#define PL_APPLY_INTERVAL_MS 50

enum pl_slider_id {
	PL_SLIDER_NONE = -1,
	PL_SLIDER_BACKLIGHT = 0,
	PL_SLIDER_VOLUME = 1,
	PL_SLIDERS = 2,
};

enum pl_bat_status {
	PL_BAT_NONE,		/* no battery and no supply online: "--" */
	PL_BAT_DISCHARGING,
	PL_BAT_CHARGING,
	PL_BAT_FULL,
	PL_BAT_AC,		/* no battery, a mains or USB supply is online: "AC" */
};

struct pl_rect {
	int x, y, w, h;
};

/* What is shown. -1 in bl_pct and vol_pct: not available, drawn greyed out. */
struct pl_state {
	int hour, min;
	enum pl_bat_status bat;
	int bat_pct;		/* 0..100, only with a battery status */
	int bl_pct;
	int vol_pct;
};

struct pl_slider {
	struct pl_rect cell;	/* the whole touch target */
	struct pl_rect icon;
	struct pl_rect track;	/* the bar including the thumb travel */
	int thumb_w;
	int thumb_h;
};

struct pl_layout {
	int w, h;
	int row_h;
	int scale;
	int text_scale;		/* integer factor of the 5x7 font */
	struct pl_rect clock;
	struct pl_rect battery;
	struct pl_slider slider[PL_SLIDERS];
};

/* ---- value mapping ---- */

/* Backlight percent <-> raw. The percent is rounded to nearest and clamped to
 * 0..100; the raw value for a percent below the floor is the floor's, and at
 * least 1, so it never writes 0. max <= 0 gives 0. */
int pl_bl_pct_from_raw(int raw, int max);
int pl_bl_raw_from_pct(int pct, int max);
int pl_bl_clamp_pct(int pct);

/* Mixer volume percent <-> the element's raw range [min, max]. */
long pl_vol_raw_from_pct(int pct, long min, long max);
int pl_vol_pct_from_raw(long raw, long min, long max);

/* "HH:MM", 24 h. buf needs 6 bytes. Out-of-range input is clamped. */
void pl_clock_text(char *buf, size_t len, int hour, int min);

/* "NN%" with a battery, "AC" or "--" otherwise. */
void pl_battery_text(char *buf, size_t len, enum pl_bat_status st, int pct);

/* ---- layout ---- */

int pl_clamp_height(int h);

/* Lay the panel out for a surface of w x h and a UI scale 1..PL_SCALE_MAX. */
void pl_layout_compute(struct pl_layout *l, int w, int h, int scale);

/* Thumb rectangle of a slider at pct (0..100). */
struct pl_rect pl_slider_thumb(const struct pl_slider *s, int pct);

/* Value 0..100 for a pointer at x: the thumb is centred on x, clamped to the
 * travel of the bar. */
int pl_slider_value(const struct pl_slider *s, int x);

/* Slider whose cell contains the point, or PL_SLIDER_NONE. */
int pl_slider_at(const struct pl_layout *l, int x, int y);

/* ---- touch ---- */

/* A press inside a slider cell, right of its icon, starts a drag of that
 * slider; motion keeps setting its value, release ends it. A press anywhere
 * else (the icon included), or on a disabled slider, starts nothing and motion is ignored until the release. */
struct pl_touch {
	bool down;
	int slider;		/* dragged slider, PL_SLIDER_NONE if none */
};

/* Each returns the slider the event changes and sets *value, or
 * PL_SLIDER_NONE. The value of the backlight slider is already floored. */
int pl_touch_press(struct pl_touch *t, const struct pl_layout *l,
	const bool enabled[PL_SLIDERS], int x, int y, int *value);
int pl_touch_motion(struct pl_touch *t, const struct pl_layout *l, int x, int *value);
/* Returns the slider whose drag ended, or PL_SLIDER_NONE. */
int pl_touch_release(struct pl_touch *t);

/* Milliseconds until a write is allowed, 0 if now: last_ms is the time of the
 * previous write (any epoch), or a negative number for none yet. */
int pl_apply_wait_ms(int64_t now_ms, int64_t last_ms);

/* ---- font ---- */

/* 7 rows of a 5 pixel wide glyph, bit 4 of each byte is the leftmost pixel.
 * NULL for a character the font does not have. Space is an empty glyph. */
const uint8_t *pl_font_glyph(char c);

/* Width in pixels of s at an integer scale (glyphs are 5 wide, 1 apart). */
int pl_text_width(const char *s, int scale);

#endif
