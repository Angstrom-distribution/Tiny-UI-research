/*
 * lease.h - DRM lease: offer the output to a KMS client (wp_drm_lease_v1) and
 * take it back when the lease ends. State lives in server->lease. All calls
 * are harmless while leasing is unavailable (headless and nested backends,
 * [lease] enable = false). The grant policy is in leasepolicy.h.
 */
#ifndef PICOWL_LEASE_H
#define PICOWL_LEASE_H

#include <stdbool.h>

struct pw_server;
struct pw_output;
struct pw_view;
struct wlr_surface;

/* Create the wp_drm_lease_device_v1 global. Called by pw_server_init() after
 * pw_output_init() and before the backend starts, so that the first output
 * can be offered. Never fails the server. */
void pw_lease_init(struct pw_server *server);

/* Offer a DRM output for lease. Called by output.c after it adopted the
 * output, which is also after every lease. NULL-safe. */
void pw_lease_offer(struct pw_output *output);

/* True while a client holds the lease. picowl then has no outputs. */
bool pw_lease_active(struct pw_server *server);

/* End the active lease (the lessee gets `finished`); picowl takes the output
 * back before this returns, unless the session is inactive. No-op without a
 * lease. */
void pw_lease_revoke(struct pw_server *server, const char *reason);

/* Surface which receives touch while leased (the lessee's toplevel), or NULL
 * when not leased. */
struct wlr_surface *pw_lease_touch_target(struct pw_server *server);

/* While leased, only the lessee's view may take the focus. */
bool pw_lease_blocks_focus(struct pw_server *server, const struct pw_view *view);

/* view is unmapping: the lease ends with its lessee. view.c calls it. */
void pw_lease_view_gone(struct pw_server *server, struct pw_view *view);

/* Revoke any lease and remove the listeners. Called by pw_server_finish()
 * before the backend is destroyed. NULL-safe, idempotent. */
void pw_lease_finish(struct pw_server *server);

#endif
