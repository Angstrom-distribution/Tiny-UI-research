#include "copytype.h"

#include <string.h>
#include <ctype.h>

/* Case-insensitive comparison */
static bool strcaseeq(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
		a++;
		b++;
	}
	return *a == *b;
}

bool pw_copytype_parse(const char *s, enum pw_copy_override *out)
{
	if (!s || !out)
		return false;
	if (strcaseeq(s, "auto"))
		*out = PW_COPY_AUTO;
	else if (strcaseeq(s, "yes") || strcaseeq(s, "true") ||
		 strcaseeq(s, "1") || strcaseeq(s, "on"))
		*out = PW_COPY_YES;
	else if (strcaseeq(s, "no") || strcaseeq(s, "false") ||
		 strcaseeq(s, "0") || strcaseeq(s, "off"))
		*out = PW_COPY_NO;
	else
		return false;
	return true;
}

bool pw_copytype_driver(const char *drm_driver_name)
{
	if (!drm_driver_name)
		return false;
	/* Copy-type drivers: media query (mq11xx), imageon (w100), sa1100-lcdc.
	 * These drivers copy damage rectangles from a persistent buffer to
	 * device memory during the commit. The buffer is shmem on mq11xx and
	 * w100, CMA on sa1100-lcdc (see pw_caching_driver). */
	if (strcaseeq(drm_driver_name, "mq11xx") ||
	    strcaseeq(drm_driver_name, "mediaq") ||
	    strcaseeq(drm_driver_name, "w100") ||
	    strcaseeq(drm_driver_name, "imageon") ||
	    strcaseeq(drm_driver_name, "sa1100-lcdc") ||
	    strcaseeq(drm_driver_name, "sa1100_lcdc") ||
	    strcaseeq(drm_driver_name, "sa11x0-lcdc") ||
	    strcaseeq(drm_driver_name, "sa1100"))
		return true;
	return false;
}

bool pw_copytype_resolve(const char *driver, enum pw_copy_override ov)
{
	if (ov == PW_COPY_YES) return true;
	if (ov == PW_COPY_NO) return false;
	return pw_copytype_driver(driver);
}

bool pw_caching_parse(const char *s, enum pw_caching_override *out)
{
	if (!s || !out)
		return false;
	if (strcaseeq(s, "auto"))
		*out = PW_CACHING_OV_AUTO;
	else if (strcaseeq(s, "cacheable"))
		*out = PW_CACHING_OV_CACHEABLE;
	else if (strcaseeq(s, "write_combined"))
		*out = PW_CACHING_OV_WC;
	else
		return false;
	return true;
}

enum pw_caching pw_caching_driver(const char *drm_driver_name)
{
	if (!drm_driver_name)
		return PW_CACHING_WRITE_COMBINED;
	/* shmem buffers: cacheable. Every copy-type driver gets an explicit
	 * entry (sa1100-lcdc is CMA); anything unknown is the safe value. */
	if (strcaseeq(drm_driver_name, "mq11xx") ||
	    strcaseeq(drm_driver_name, "mediaq") ||
	    strcaseeq(drm_driver_name, "w100") ||
	    strcaseeq(drm_driver_name, "imageon"))
		return PW_CACHING_CACHEABLE;
	return PW_CACHING_WRITE_COMBINED;
}

enum pw_caching pw_caching_resolve(const char *driver, enum pw_caching_override ov)
{
	if (ov == PW_CACHING_OV_CACHEABLE) return PW_CACHING_CACHEABLE;
	if (ov == PW_CACHING_OV_WC) return PW_CACHING_WRITE_COMBINED;
	return pw_caching_driver(driver);
}

const char *pw_caching_name(enum pw_caching c)
{
	return c == PW_CACHING_CACHEABLE ? "cacheable" : "write_combined";
}
