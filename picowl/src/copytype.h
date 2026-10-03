/* copytype.h - which DRM drivers copy damage to device memory, and how their buffers are mapped. */
#ifndef PW_COPYTYPE_H
#define PW_COPYTYPE_H

#include <stdbool.h>

enum pw_copy_override { PW_COPY_AUTO, PW_COPY_YES, PW_COPY_NO };

bool pw_copytype_parse(const char *s, enum pw_copy_override *out);
bool pw_copytype_driver(const char *drm_driver_name);
bool pw_copytype_resolve(const char *driver, enum pw_copy_override ov);

/* CPU mapping of the dmabufs picowl_buffer_manager_v1 hands out. */
enum pw_caching { PW_CACHING_WRITE_COMBINED = 0, PW_CACHING_CACHEABLE = 1 }; /* wire values */
enum pw_caching_override { PW_CACHING_OV_AUTO, PW_CACHING_OV_CACHEABLE, PW_CACHING_OV_WC };

bool pw_caching_parse(const char *s, enum pw_caching_override *out);
enum pw_caching pw_caching_driver(const char *drm_driver_name);
enum pw_caching pw_caching_resolve(const char *driver, enum pw_caching_override ov);
const char *pw_caching_name(enum pw_caching c);  /* "cacheable" | "write_combined" */

#endif
