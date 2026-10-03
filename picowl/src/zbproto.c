#include "zbproto.h"

#include <drm_fourcc.h>
#include "picowl-buffer-v1-protocol.h"

void pw_zbproto_send_bind(struct wl_resource *mgr, bool copy_type,
	uint32_t caching)
{
	picowl_buffer_manager_v1_send_format(mgr, DRM_FORMAT_RGB565);
	picowl_buffer_manager_v1_send_copy_type(mgr, copy_type ? 1 : 0);
	if (wl_resource_get_version(mgr) >=
	    PICOWL_BUFFER_MANAGER_V1_CACHING_SINCE_VERSION)
		picowl_buffer_manager_v1_send_caching(mgr, caching);
}
