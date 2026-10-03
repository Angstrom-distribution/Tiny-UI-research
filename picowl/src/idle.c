/* idle.c - idle notification and screen blanking timer. */
#include <stdlib.h>
#include "picowl.h"

/* Timer callback: blank the screen when idle timeout expires. */
static int idle_timer_callback(void *data)
{
	struct pw_server *server = data;
	if (server->config->idle_timeout_ms > 0)
		pw_output_blank(server, true);
	return 0;  /* Do not re-arm the timer; rearm happens on activity. */
}

void pw_idle_init(struct pw_server *server)
{
	/* Create idle notifier for clients using ext-idle-notify-v1 protocol. */
	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
	if (!server->idle_notifier) {
		pw_log(WLR_ERROR, "cannot create idle notifier");
		return;
	}

	/* Create timer if idle blanking is configured (idle_timeout_ms > 0).
	 * Timer is armed on first activity; initially disabled. */
	if (server->config->idle_timeout_ms > 0) {
		server->idle_timer = wl_event_loop_add_timer(
			server->event_loop, idle_timer_callback, server);
		if (!server->idle_timer) {
			pw_log(WLR_ERROR, "cannot create idle timer");
			return;
		}
		wl_event_source_timer_update(server->idle_timer,
			server->config->idle_timeout_ms);
		pw_log(WLR_INFO, "idle blanking configured: %d ms",
			server->config->idle_timeout_ms);
	}
}

void pw_idle_activity(struct pw_server *server)
{
	/* Notify clients of user activity via the idle notifier. */
	if (server->idle_notifier && server->seat) {
		wlr_idle_notifier_v1_notify_activity(server->idle_notifier,
			server->seat);
	}

	/* If screen is blanked, unblank it on activity. */
	if (server->blanked)
		pw_output_blank(server, false);

	/* Re-arm the idle timer if configured. */
	if (server->idle_timer && server->config->idle_timeout_ms > 0) {
		wl_event_source_timer_update(server->idle_timer,
			server->config->idle_timeout_ms);
	}
}
