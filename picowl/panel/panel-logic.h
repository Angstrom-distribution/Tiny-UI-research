/*
 * panel-logic.h - the pure part of picowl-panel: value mapping, text, the
 * layout rectangles, hit testing, the touch and pop-out state machines and the
 * 5x7 fallback font. No wayland, no alsa, no sysfs, so the unit test can run
 * it anywhere.
 *
 * The surface is a slim bar of bar_h pixels (clock on the left; on the right a
 * backlight button, a volume button and the battery). Each of the four is a
 * button: while a row is open the surface is bar_h + row_h pixels high and the
 * row sits below the bar. The backlight and the volume open a slider row, the
 * clock a row with the date and the battery a row with the time left. Only the
 * bar is the panel's exclusive zone, so the row covers windows instead of
 * moving them.
 */
#ifndef PICOWL_PANEL_LOGIC_H
#define PICOWL_PANEL_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PL_HEIGHT_DEFAULT 18
#define PL_HEIGHT_MIN 18
#define PL_HEIGHT_MAX 80

/* The backlight slider never goes below this, so the screen cannot be turned
 * black by accident. */
#define PL_BL_FLOOR_PCT 5

/* Writes to the system during a drag are at most this far apart. */
#define PL_APPLY_INTERVAL_MS 50

/* The slider row closes by itself this long after the last touch. */
#define PL_POPUP_MS 3000

/* Margins of the panel and the gap between its parts. */
#define PL_MARGIN 8
#define PL_GAP 4

/* The highlight of an open clock or battery reaches this far past the text,
 * and the battery's rectangle starts this far before its icon so that the
 * highlight fits inside it. */
#define PL_HL_PAD 4
#define PL_BAT_PAD 6

#define PL_ALPHA_BAR_DEFAULT 255
#define PL_ALPHA_POPUP_DEFAULT 224

enum pl_slider_id {
	PL_SLIDER_NONE = -1,
	PL_SLIDER_BACKLIGHT = 0,
	PL_SLIDER_VOLUME = 1,
	PL_SLIDERS = 2,
};

/* The buttons of the bar: the sliders' ids first, so that an id says which row
 * is open (PL_SLIDER_NONE: none). */
enum pl_button_id {
	PL_BTN_CLOCK = PL_SLIDERS,
	PL_BTN_BATTERY,
	PL_BUTTONS,
};

/* What a row shows. */
enum pl_row_kind {
	PL_ROW_NONE,
	PL_ROW_SLIDER,		/* a slider and its value */
	PL_ROW_DATE,		/* weekday and date, one line of text */
	PL_ROW_ESTIMATE,	/* the battery's time estimate, one line of text */
};

enum pl_bat_status {
	PL_BAT_NONE,		/* no battery and no supply online: "--" */
	PL_BAT_DISCHARGING,
	PL_BAT_CHARGING,
	PL_BAT_FULL,
	PL_BAT_AC,		/* no battery, a mains or USB supply is online: "AC" */
};

/* Pixel format of the surface's buffer. ARGB8888 is premultiplied. */
enum pl_fmt {
	PL_FMT_RGB565,
	PL_FMT_XRGB8888,
	PL_FMT_ARGB8888,
};

struct pl_rect {
	int x, y, w, h;
};

/* ---- the battery's time estimate ---- */

/* Everything below is integer arithmetic (the h2200 has no FPU) and the
 * constants are a synthesis from how the kernel's power_supply attributes are
 * documented to behave and from what other programs do; none of them has been
 * tuned on a real battery. They are named so that they are easy to change. */

/* One sample of the battery per 30 s (the panel's poll); a sample whose current
 * is below the idle floor says nothing about the rate and is skipped, one above
 * the ceiling is a glitch and is dropped. In uA, or in uW for a battery that
 * only has power_now. */
#define PL_EST_IDLE_UA 10000
#define PL_EST_MAX_UA 3000000
#define PL_EST_IDLE_UW 40000
#define PL_EST_MAX_UW 15000000
/* A number is shown after this many valid samples (2 min); until then the
 * filter is a running mean of what it has, and the row says it is estimating. */
#define PL_EST_WARM 4
/* The filter is a running mean for the first samples and then an exponential
 * moving average with weight 1/PL_EST_EMA_K. */
#define PL_EST_EMA_K 8
/* After a change between charging and discharging the filter starts again and
 * ignores this many samples: the current settles after a plug event. */
#define PL_EST_SKIP 2
/* Samples further apart than this are not one run (the panel was stopped, the
 * clock jumped): the filter starts again. */
#define PL_EST_GAP_MS 600000
/* With a charger online the battery is charging above this (uA, signed, > 0 is
 * into the battery) and not charging below the negative of the second. */
#define PL_EST_CHG_ON_UA 10000
#define PL_EST_NOTCHG_UA 5000
/* Above this percent the current-based time to full is optimistic (the
 * constant-voltage taper), so it is not shown. */
#define PL_EST_TAPER_PCT 90
/* The kernel's time_to_empty_now (seconds) is used as a hint, with a '~', until
 * the filter is warm, if it is in this range. */
#define PL_EST_HINT_MAX_S 86400
/* More than this is shown as '> 24 h'. */
#define PL_EST_OVER_MIN 1440
/* Rounding of the minutes shown: to the minute below RND1, to PL_EST_STEP1
 * below RND2, to PL_EST_STEP2 from there on. */
#define PL_EST_RND1_MIN 10
#define PL_EST_RND2_MIN 300
#define PL_EST_STEP1 5
#define PL_EST_STEP2 15
/* While discharging the minutes shown change only if the new value is more
 * than max(HYST_MIN, shown / HYST_DIV) away, and by more than 1 below
 * HYST_SMALL minutes. */
#define PL_EST_HYST_MIN 3
#define PL_EST_HYST_DIV 8
#define PL_EST_HYST_SMALL 10

/* An attribute that is not there (or is garbage). */
#define PL_ABSENT INT64_MIN

/* What the sysfs reader found; every attribute is PL_ABSENT if missing. Charge
 * is in uAh, energy in uWh, current in uA (signed, > 0 into the battery), power
 * in uW, time_to_empty_now in seconds. */
struct pl_batt_raw {
	enum pl_bat_status st;
	bool not_charging;	/* status says "Not charging" */
	int pct;		/* capacity, -1 if none */
	int charger;		/* a Mains or USB supply: 1 online, 0 offline, -1 none */
	int64_t charge_now, charge_full, charge_full_design, charge_empty;
	int64_t energy_now, energy_full, energy_full_design;
	int64_t current_now, current_avg, power_now;
	int64_t time_to_empty_now;
};

void pl_batt_raw_clear(struct pl_batt_raw *r);

enum pl_dir {
	PL_DIR_NONE,
	PL_DIR_DISCHARGING,
	PL_DIR_CHARGING,
	PL_DIR_NOTCHARGING,	/* on a charger, but not charging */
	PL_DIR_FULL,
};

/* The direction the battery is going: status Full is full, "Not charging" is
 * that; with no charger it is discharging; with one it is charging if the
 * current (smoothed, PL_ABSENT if unknown) is above +10 mA, not charging if
 * it is below -5 mA, and between, what the status says. smoothed is only
 * used if the rate is a current. */
enum pl_dir pl_batt_dir(const struct pl_batt_raw *r, int64_t smoothed);

/* The filter that smooths the rate over the samples. */
struct pl_est {
	int kind;		/* 0 none yet, 1 current (uA), 2 power (uW) */
	int64_t ema;		/* signed */
	int n;			/* valid samples since the last reset */
	int skip;		/* samples still to ignore */
	int64_t last_v;		/* the last sample that was not idle or a glitch */
	bool have_v;
	enum pl_dir dir;	/* of the run */
	int64_t last_ms;
	bool have_last;
};

void pl_est_init(struct pl_est *e);
/* One sample at now_ms (any epoch, monotonic). The rate is current_avg if the
 * battery has it, else current_now, else power_now. */
void pl_est_add(struct pl_est *e, const struct pl_batt_raw *r, int64_t now_ms);

enum pl_est_kind {
	PL_EST_NONE,		/* no battery data: "--" */
	PL_EST_AC,		/* mains and no battery */
	PL_EST_FULL,
	PL_EST_NOTCHARGING,
	PL_EST_CHARGING,	/* charging, no time (above the taper) */
	PL_EST_ESTIMATING,	/* warming up, idle or no rate */
	PL_EST_LEFT,		/* minutes left */
	PL_EST_TOFULL,		/* minutes to full */
	PL_EST_OVER_LEFT,	/* more than 24 h left */
	PL_EST_OVER_FULL,
};

struct pl_estimate {
	enum pl_est_kind kind;
	int minutes;		/* exact, for LEFT and TOFULL */
	bool hint;		/* from the kernel's time_to_empty_now: shown with '~' */
};

/* What to show for a battery with the filter e. battery_mah is the capacity
 * the user gave as a last resort for the charge (0: unknown). */
struct pl_estimate pl_est_compute(const struct pl_est *e, const struct pl_batt_raw *r,
	int battery_mah);

/* The minutes as shown: to the minute below 10, to 5 min below 5 h, to 15 min
 * above. */
int pl_est_round_min(int minutes);

/* The minutes to show when fresh replaces shown (discharging): shown stays
 * unless fresh is further away than the hysteresis. */
int pl_est_hysteresis(int shown, int fresh);

/* Candidates for the text of the row, from the most to the least informative:
 * variant 0 has the percentage and the whole wording, 1 drops the percentage,
 * 2 shortens the wording ("1 h 30 min" for "1 h 30 min left") and 3 does both.
 * pct < 0 has no percentage. buf needs 48 bytes. */
#define PL_EST_VARIANTS 4
void pl_est_text(char *buf, size_t len, const struct pl_estimate *e, int pct, int variant);

/* What is shown. -1 in bl_pct and vol_pct: not available, drawn greyed out. */
struct pl_state {
	int hour, min;
	int year, mon, mday, wday;	/* local date: mon 0..11, wday 0 is Sunday */
	struct pl_estimate est;		/* what the battery row says */
	enum pl_bat_status bat;
	int bat_pct;		/* 0..100, only with a battery status */
	int bl_pct;
	int vol_pct;
};

/* Text widths the layout has to leave room for, in pixels. */
struct pl_metrics {
	int clock_w;		/* "88:88" */
	int bat_text_w;		/* "100%" */
	int pct_w;		/* "100%" in the slider row */
};

struct pl_slider {
	struct pl_rect cell;	/* where a press sets the value */
	struct pl_rect track;	/* the bar including the thumb travel */
	int thumb_d;		/* thumb diameter */
	int track_h;		/* thickness of the bar */
};

struct pl_layout {
	int w, h;		/* the surface: w x (bar_h + row_h if row_shown) */
	int bar_h;
	int row_h;		/* height of the row, also when it is not shown */
	bool row_shown;
	bool bottom;		/* anchored at the bottom: the row is above the bar */
	int bar_y;		/* top of the bar in the surface */
	int bar_line_y;		/* the 1 px line of the bar, on the side of the windows */
	int row_line_y;		/* the same for the row */
	struct pl_rect clock;	/* drawing rectangles, they leave out the line */
	struct pl_rect battery;
	int bat_icon_w, bat_icon_h;
	struct pl_rect button[PL_BUTTONS];	/* touch targets, the whole bar height */
	struct pl_rect hl[PL_BUTTONS];		/* highlight of an open button */
	int bar_icon;				/* edge of the icon in a button */
	struct pl_rect row;			/* the slider row, valid if row_shown */
	struct pl_rect row_in;			/* the row without its line */
	struct pl_rect row_icon;
	struct pl_rect text;			/* the line of a text row: the row less its margins */
	struct pl_rect pct;			/* where the value text goes */
	struct pl_slider slider;
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

/* "NN%" for a slider value. */
void pl_pct_text(char *buf, size_t len, int pct);

/* "Monday 5 October 2026": the names are in a table of the panel's own, so the
 * line does not depend on the locale. mon is 0..11 and wday 0..6 from Sunday,
 * as in struct tm; a value out of range is clamped. buf needs 40 bytes. */
void pl_date_text(char *buf, size_t len, int year, int mon, int mday, int wday);

/* What a button opens: PL_ROW_NONE for an id that is none. */
enum pl_row_kind pl_row_kind_of(int open);

/* Width of s in pixels in the face (see panel-gfx.h). */
typedef int (*pl_width_fn)(void *ctx, int face, const char *s);

/* The text of a row is shrunk stepwise instead of clipped: it tries the faces
 * from 0 (the largest) on, and in each one the candidates from the most to the
 * least informative, and takes the first that is at most avail pixels wide.
 * Returns its index and sets *face. If none fits it is the last candidate in
 * the last face, which the caller should make the shortest. */
int pl_fit_choose(const char *const *cand, int ncand, int nfaces, int avail,
	pl_width_fn width, void *ctx, int *face);

/* ---- layout ---- */

int pl_clamp_height(int h);
int pl_clamp_alpha(int a);

/* Default font size for a bar height: 12 px on the 20 px bar. */
int pl_default_font_px(int bar_h);

/* Height of the row for a bar. */
int pl_row_height(int bar_h);

/* Height of the surface with or without the row. */
int pl_surface_height(int bar_h, bool row);

/* Lay out a surface of width w: the bar, and the row if row_shown. At the top
 * of the screen the row is under the bar, at the bottom (bottom) above it, so
 * that the bar stays at the edge of the screen when the surface grows. */
void pl_layout_compute(struct pl_layout *l, int w, int bar_h, bool row_shown,
	bool bottom, const struct pl_metrics *m);

/* The rectangle that takes touches: the bar and the row, never more than the
 * surface of sw x sh, which the compositor may have made larger. */
struct pl_rect pl_input_rect(const struct pl_layout *l, int sw, int sh);

/* Opaque parts of the surface, for wl_surface.set_opaque_region: the bar when
 * the bar is opaque, the row when it is shown and opaque. Returns how many
 * rectangles were written (at most 2). */
int pl_opaque_rects(const struct pl_layout *l, int bar_alpha, int popup_alpha,
	struct pl_rect out[2]);

/* Buffer format: RGB565 when nothing on the surface is translucent and the
 * compositor offers it, XRGB8888 when opaque without it, else ARGB8888. */
enum pl_fmt pl_pick_format(int bar_alpha, int popup_alpha, bool row_shown,
	bool have_rgb565);

/* Thumb rectangle of the slider at pct (0..100). */
struct pl_rect pl_slider_thumb(const struct pl_layout *l, int pct);

/* Value 0..100 for a pointer at x: the thumb is centred on x, clamped to the
 * travel of the bar. */
int pl_slider_value(const struct pl_slider *s, int x);

enum pl_hit_kind {
	PL_HIT_NONE,
	PL_HIT_BUTTON,		/* a button of the bar: slider says which */
	PL_HIT_TRACK,		/* the track area of the row */
};

struct pl_hit {
	enum pl_hit_kind kind;
	int slider;
};

struct pl_hit pl_hit_test(const struct pl_layout *l, int x, int y);

/* ---- touch ---- */

/* A press on a button taps it. A press in the track area of an open slider row
 * starts a drag of that slider; motion keeps setting its value, release ends
 * it. A press anywhere else (the icon and the value text of the row included,
 * and anything in a text row), or on a disabled button, starts nothing and
 * motion is ignored until the release. */
struct pl_touch {
	bool down;
	int slider;		/* dragged slider, PL_SLIDER_NONE if none */
};

struct pl_touch_out {
	int tap;		/* button that was tapped (a pl_button_id), or PL_SLIDER_NONE */
	int slider;		/* slider whose value changes, or PL_SLIDER_NONE */
	int value;		/* the new value; the backlight's is already floored */
};

/* open: the button whose row is shown, or PL_SLIDER_NONE. */
void pl_touch_press(struct pl_touch *t, const struct pl_layout *l, int open,
	const bool enabled[PL_BUTTONS], int x, int y, struct pl_touch_out *out);
void pl_touch_motion(struct pl_touch *t, const struct pl_layout *l, int x,
	struct pl_touch_out *out);
/* Returns the slider whose drag ended, or PL_SLIDER_NONE. */
int pl_touch_release(struct pl_touch *t);

/* Milliseconds until a write is allowed, 0 if now: last_ms is the time of the
 * previous write (any epoch), or a negative number for none yet. */
int pl_apply_wait_ms(int64_t now_ms, int64_t last_ms);

/* ---- the pop-out row ---- */

struct pl_popup {
	int open;		/* button whose row is shown, or PL_SLIDER_NONE */
	int64_t last_ms;	/* time of the last touch */
};

/* A tap on a button: the same one closes the row, another one switches it.
 * Returns the button now open. Counts as a touch. */
int pl_popup_tap(struct pl_popup *p, int button, int64_t now_ms);
void pl_popup_touch(struct pl_popup *p, int64_t now_ms);
/* Milliseconds until the row closes by itself, -1 if it is not open. A stylus
 * that is still down keeps it open, so the wait is then the full time. */
int pl_popup_wait_ms(const struct pl_popup *p, int64_t now_ms, bool touching);
/* Closes the row if its time is up; true if it did. */
bool pl_popup_expire(struct pl_popup *p, int64_t now_ms, bool touching);

/* ---- fallback font ---- */

/* 7 rows of a 5 pixel wide glyph, bit 4 of each byte is the leftmost pixel.
 * NULL for a character the font does not have. Space is an empty glyph. */
const uint8_t *pl_font_glyph(char c);

/* Width in pixels of s at an integer scale (glyphs are 5 wide, 1 apart). */
int pl_text_width(const char *s, int scale);

#endif
