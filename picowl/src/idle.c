/* idle.c - idle notifier (ext-idle-notify-v1); dim/blank timing is in power.c. */
#include "picowl.h"
#include "power.h"

void pw_idle_init(struct pw_server *server)
{
	server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
	if (!server->idle_notifier)
		pw_log(WLR_ERROR, "cannot create idle notifier");
}

/* Tell ext-idle-notify clients about user activity. */
void pw_idle_notify(struct pw_server *server)
{
	if (server->idle_notifier && server->seat)
		wlr_idle_notifier_v1_notify_activity(server->idle_notifier,
			server->seat);
}

/* Notify idle clients and drive the dim/blank state machine (power.c). */
void pw_idle_activity(struct pw_server *server)
{
	pw_idle_notify(server);
	(void)pw_power_activity(server);
}
