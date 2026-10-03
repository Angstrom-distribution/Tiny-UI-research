#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "zbquota.h"

uint32_t pw_zb_round(uint32_t bytes, uint32_t page)
{
	if (!page)
		return bytes;
	if (bytes > UINT32_MAX - (page - 1))
		return 0;
	return (bytes + page - 1) / page * page;
}

uint32_t pw_zb_frame_bytes(int32_t w, int32_t h, uint32_t page)
{
	if (w <= 0 || h <= 0 || (uint32_t)w > UINT32_MAX / 2 / (uint32_t)h)
		return 0;
	return pw_zb_round((uint32_t)w * (uint32_t)h * 2, page);
}

uint32_t pw_zb_pool_cap(int kb, int buffers, uint32_t frame)
{
	if (kb >= 0)
		return (uint32_t)kb * 1024;
	if (buffers <= 0)
		return 0;
	if (frame > UINT32_MAX / (uint32_t)buffers)
		return UINT32_MAX;
	return (uint32_t)buffers * frame;
}

enum pw_zb_verdict pw_zb_check(const struct pw_zb_req *r)
{
	if (r->count >= r->max_count)
		return PW_ZB_OVER_COUNT;
	if (r->pool_used > r->pool_cap || r->bytes > r->pool_cap - r->pool_used)
		return PW_ZB_OVER_POOL;
	if (r->total_cap &&
	    (r->total_used > r->total_cap || r->bytes > r->total_cap - r->total_used))
		return PW_ZB_OVER_TOTAL;
	return PW_ZB_OK;
}

void pw_zb_refund(uint32_t *used, uint32_t bytes)
{
	*used = *used > bytes ? *used - bytes : 0;
}

bool pw_zb_exe_match(pid_t pid, const char *exe)
{
	char link[32], buf[PATH_MAX];
	snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
	ssize_t n = readlink(link, buf, sizeof(buf) - 1);
	if (n <= 0 || !exe)
		return false;
	buf[n] = '\0';
	return strcmp(buf, exe) == 0;
}
