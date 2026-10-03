/*
 * power.c - power-supply aware dimming glue. Owns the backlight, the
 * power-supply uevent source, the fallback poll timer and the dim/blank
 * timer. The decisions live in dim.c (pure); this file executes them.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "picowl.h"
#include "power.h"
#include "backlight.h"
#include "powersupply.h"
#include "dim.h"

struct pw_power {
	struct pw_server *server;
	struct pw_backlight *bl;        /* NULL: no usable backlight */
	bool bl_disabled;               /* write failed or on/off device */
	struct pw_dim dim;
	enum pw_power_profile profile;
	struct pw_ps_uevent uev;
	struct wl_event_source *uev_src;
	struct wl_event_source *dim_timer;
	struct wl_event_source *poll_timer;
};

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool bl_usable(const struct pw_power *p)
{
	return p->bl && !p->bl_disabled;
}

static int64_t dim_ms_for(const struct pw_power *p, enum pw_power_profile prof)
{
	if (!bl_usable(p))
		return 0;
	return (int64_t)p->server->config->power[prof].dim_after_s * 1000;
}

static int64_t blank_ms_for(const struct pw_power *p, enum pw_power_profile prof)
{
	return (int64_t)p->server->config->power[prof].blank_after_s * 1000;
}

/* Brightness the user should see right now (user level, LOW-capped). */
static int normal_level(const struct pw_power *p)
{
	int level = pw_backlight_get_user(p->bl);
	if (p->profile == PW_PROFILE_LOW) {
		int cap = pw_backlight_scale(pw_backlight_get_max(p->bl),
			p->server->config->low_max_brightness_pct);
		if (level > cap)
			level = cap;
	}
	return level;
}

static void write_level(struct pw_power *p, int level)
{
	if (!bl_usable(p))
		return;
	if (pw_backlight_set(p->bl, level) < 0) {
		if (errno == EACCES || errno == EPERM)
			pw_log(WLR_ERROR, "backlight %s not writable, dimming disabled",
				pw_backlight_name(p->bl));
		else
			pw_log(WLR_ERROR, "backlight %s write failed (%s), dimming disabled",
				pw_backlight_name(p->bl), strerror(errno));
		p->bl_disabled = true;
		pw_dim_set_timeouts(&p->dim, 0, blank_ms_for(p, p->profile), now_ms());
	}
}

/* Bring the backlight in line with the current state (not while blanked). */
static void apply_level(struct pw_power *p)
{
	if (!bl_usable(p) || p->dim.state == PW_DIM_BLANKED)
		return;
	int level = normal_level(p);
	if (p->dim.state == PW_DIM_DIMMED)
		level = pw_backlight_scale_level(level, p->server->config->dim_level);
	write_level(p, level);
}

static void rearm(struct pw_power *p)
{
	if (!p->dim_timer)
		return;
	int64_t dl = pw_dim_next_deadline(&p->dim);
	if (dl < 0) {
		wl_event_source_timer_update(p->dim_timer, 0);
		return;
	}
	int64_t delay = dl - now_ms();
	if (delay < 1)
		delay = 1;
	if (delay > 0x7fffffff)
		delay = 0x7fffffff;
	wl_event_source_timer_update(p->dim_timer, (int)delay);
}

static void run_actions(struct pw_power *p, unsigned act)
{
	if (act & PW_DIM_ACT_BLANK)
		pw_output_blank(p->server, true);
	if (act & PW_DIM_ACT_UNBLANK)
		pw_output_blank(p->server, false);
	if (act & (PW_DIM_ACT_DIM | PW_DIM_ACT_UNDIM | PW_DIM_ACT_UNBLANK))
		apply_level(p);
}

static int dim_timer_cb(void *data)
{
	struct pw_power *p = data;
	run_actions(p, pw_dim_tick(&p->dim, now_ms()));
	rearm(p);
	return 0;
}

static void profile_changed(struct pw_power *p, enum pw_power_profile np)
{
	if (np == p->profile)
		return;
	pw_log(WLR_INFO, "power profile %d -> %d", p->profile, np);
	p->profile = np;
	unsigned act = pw_dim_set_timeouts(&p->dim, dim_ms_for(p, np),
		blank_ms_for(p, np), now_ms());
	run_actions(p, act);
	apply_level(p); /* LOW cap on/off, or re-dim of the capped level */
	rearm(p);
}

static void reread_profile(struct pw_power *p)
{
	profile_changed(p, pw_ps_read_profile(NULL, p->server->config->low_capacity, NULL));
}

static int uevent_cb(int fd, uint32_t mask, void *data)
{
	struct pw_power *p = data;
	(void)fd; (void)mask;
	if (pw_ps_uevent_drain(&p->uev))
		reread_profile(p);
	return 0;
}

static int poll_cb(void *data)
{
	struct pw_power *p = data;
	reread_profile(p);
	wl_event_source_timer_update(p->poll_timer, p->server->config->poll_s * 1000);
	return 0;
}

void pw_power_init(struct pw_server *server)
{
	struct pw_config *c = server->config;
	struct pw_power *p = calloc(1, sizeof(*p));
	if (!p) {
		pw_log(WLR_ERROR, "power: out of memory");
		return;
	}
	p->server = server;
	server->power = p;

	p->bl = pw_backlight_open(NULL, c->backlight);
	if (p->bl && pw_backlight_get_max(p->bl) <= 1) {
		pw_log(WLR_INFO, "backlight %s is on/off only, dimming skipped",
			pw_backlight_name(p->bl));
		p->bl_disabled = true;
	}
	if (!p->bl)
		pw_log(WLR_DEBUG, "power: no backlight, dimming inactive");

	p->profile = pw_ps_read_profile(NULL, c->low_capacity, NULL);
	pw_dim_init(&p->dim, dim_ms_for(p, p->profile), blank_ms_for(p, p->profile));
	pw_dim_activity(&p->dim, now_ms());

	p->dim_timer = wl_event_loop_add_timer(server->event_loop, dim_timer_cb, p);
	if (!p->dim_timer)
		pw_log(WLR_ERROR, "power: cannot create idle timer");

	p->uev.fd = -1;
	if (pw_ps_uevent_open(&p->uev) == 0) {
		p->uev_src = wl_event_loop_add_fd(server->event_loop,
			pw_ps_uevent_fd(&p->uev), WL_EVENT_READABLE, uevent_cb, p);
		if (!p->uev_src)
			pw_log(WLR_ERROR, "power: cannot watch uevent socket");
	} else {
		pw_log(WLR_DEBUG, "power: no uevent socket (%s), polling only",
			strerror(errno));
	}
	if (c->poll_s > 0) {
		p->poll_timer = wl_event_loop_add_timer(server->event_loop, poll_cb, p);
		if (p->poll_timer)
			wl_event_source_timer_update(p->poll_timer, c->poll_s * 1000);
	}

	apply_level(p); /* LOW cap at startup */
	rearm(p);
	pw_log(WLR_INFO, "power: profile %d, backlight %s, dim %lld ms, blank %lld ms",
		p->profile, p->bl ? pw_backlight_name(p->bl) : "none",
		(long long)p->dim.dim_ms, (long long)p->dim.blank_ms);
}

/* The output power protocol may change server->blanked behind our back. */
static void sync_blanked(struct pw_power *p)
{
	bool sb = p->server->blanked;
	if (sb && p->dim.state != PW_DIM_BLANKED)
		pw_dim_force_blank(&p->dim);
	else if (!sb && p->dim.state == PW_DIM_BLANKED) {
		pw_dim_force_unblank(&p->dim, now_ms());
		apply_level(p);
	}
}

bool pw_power_activity(struct pw_server *server)
{
	struct pw_power *p = server->power;
	if (!p)
		return false;
	sync_blanked(p);
	unsigned act = pw_dim_activity(&p->dim, now_ms());
	run_actions(p, act);
	rearm(p);
	return act & PW_DIM_ACT_SWALLOW_INPUT;
}

void pw_power_set_blanked(struct pw_server *server, bool blanked)
{
	struct pw_power *p = server->power;
	if (!p) {
		pw_output_blank(server, blanked);
		return;
	}
	unsigned act = blanked ? pw_dim_force_blank(&p->dim)
		: pw_dim_force_unblank(&p->dim, now_ms());
	/* Make sure the outputs match even if the machine was already there. */
	if (!act)
		pw_output_blank(server, blanked);
	run_actions(p, act);
	rearm(p);
}

void pw_power_sync_blanked(struct pw_server *server)
{
	struct pw_power *p = server->power;
	if (!p)
		return;
	sync_blanked(p);
	rearm(p);
}

void pw_power_finish(struct pw_server *server)
{
	struct pw_power *p = server->power;
	if (!p)
		return;
	server->power = NULL;
	if (p->dim_timer)
		wl_event_source_remove(p->dim_timer);
	if (p->poll_timer)
		wl_event_source_remove(p->poll_timer);
	if (p->uev_src)
		wl_event_source_remove(p->uev_src);
	pw_ps_uevent_close(&p->uev);
	if (p->bl) {
		if (!p->bl_disabled)
			pw_backlight_set(p->bl, pw_backlight_get_user(p->bl));
		pw_backlight_close(p->bl);
	}
	free(p);
}
