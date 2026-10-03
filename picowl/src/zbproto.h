/* zbproto.h - the picowl_buffer_manager_v1 bind events, with version gating. */
#ifndef PW_ZBPROTO_H
#define PW_ZBPROTO_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

/* Highest picowl_buffer_manager_v1 version picowl implements. Keep it in
 * step with the XML: wl_global_create fails if the XML is older. */
#define PW_ZB_MGR_VERSION 2

/* Sends format(RGB565), copy_type and, if the client bound version 2 or
 * later, caching (a wire value of enum pw_caching). libwayland does not
 * check event versions on the server side, so the gate is here. */
void pw_zbproto_send_bind(struct wl_resource *mgr, bool copy_type,
	uint32_t caching);

#endif
