/* zbquota.h - picowl_buffer_v1 allocation limits. Integers only, no wlroots. */
#ifndef PW_ZBQUOTA_H
#define PW_ZBQUOTA_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

enum pw_zb_verdict { PW_ZB_OK, PW_ZB_OVER_COUNT, PW_ZB_OVER_POOL, PW_ZB_OVER_TOTAL };

struct pw_zb_req {
	uint32_t bytes;                   /* page-rounded size of the new buffer */
	unsigned count, max_count;        /* buffers the client holds, its limit */
	uint32_t pool_used, pool_cap;
	uint32_t total_used, total_cap;   /* total_cap 0 = no ceiling */
};

/* bytes rounded up to a multiple of page; 0 on overflow. page 0 = no rounding. */
uint32_t pw_zb_round(uint32_t bytes, uint32_t page);
/* w*h*2 rounded up to page; 0 if w or h <= 0, or on overflow. */
uint32_t pw_zb_frame_bytes(int32_t w, int32_t h, uint32_t page);
/* Pool cap in bytes: kb * 1024 if kb >= 0, else buffers * frame (saturating). */
uint32_t pw_zb_pool_cap(int kb, int buffers, uint32_t frame);
/* Count, then pool, then ceiling; the first failure wins. A pool above its cap
 * (an output shrank) rejects without underflow. */
enum pw_zb_verdict pw_zb_check(const struct pw_zb_req *r);
/* *used -= bytes, clamped at 0. */
void pw_zb_refund(uint32_t *used, uint32_t bytes);
/* /proc/<pid>/exe is exactly exe. An unreadable link or a " (deleted)"
 * suffix does not match. */
bool pw_zb_exe_match(pid_t pid, const char *exe);

#endif
