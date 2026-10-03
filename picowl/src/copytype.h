/* copytype.h - which DRM drivers copy damage to device memory. */
#ifndef PW_COPYTYPE_H
#define PW_COPYTYPE_H

#include <stdbool.h>

enum pw_copy_override { PW_COPY_AUTO, PW_COPY_YES, PW_COPY_NO };

bool pw_copytype_parse(const char *s, enum pw_copy_override *out);
bool pw_copytype_driver(const char *drm_driver_name);
bool pw_copytype_resolve(const char *driver, enum pw_copy_override ov);

#endif
