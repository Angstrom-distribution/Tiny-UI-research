/* test-zbquota.c - unit tests for the picowl_buffer_v1 limits. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include "../src/zbquota.h"

#define PAGE 4096u
#define VGA (480 * 640 * 2u)
#define QVGA (240 * 320 * 2u)

static void test_frame_bytes(void)
{
	assert(pw_zb_frame_bytes(240, 320, PAGE) == 155648);
	assert(pw_zb_frame_bytes(480, 640, PAGE) == 614400);
	assert(pw_zb_frame_bytes(1, 1, PAGE) == 4096);
	assert(pw_zb_frame_bytes(0, 10, PAGE) == 0);
	assert(pw_zb_frame_bytes(10, 0, PAGE) == 0);
	assert(pw_zb_frame_bytes(-1, 10, PAGE) == 0);
	assert(pw_zb_frame_bytes(10, -5, PAGE) == 0);
	assert(pw_zb_frame_bytes(65535, 65535, PAGE) == 0);
	/* around the 32-bit limit */
	assert(pw_zb_frame_bytes(INT32_MAX, 1, 1) == 4294967294u);
	assert(pw_zb_frame_bytes(INT32_MAX, 1, PAGE) == 0);
	assert(pw_zb_frame_bytes(0x7fffffff, 2, 1) == 0);
	assert(pw_zb_frame_bytes(32768, 32767, 1) == 2147418112u);
	assert(pw_zb_frame_bytes(32768, 32768, 1) == 2147483648u);
	assert(pw_zb_frame_bytes(65536, 32768, 1) == 0);
	assert(pw_zb_frame_bytes(65535, 32768, 4096) == 0xffff0000u); /* rounds, no overflow */
	/* page 0 means no rounding */
	assert(pw_zb_frame_bytes(3, 3, 0) == 18);
	assert(pw_zb_round(1, PAGE) == PAGE && pw_zb_round(PAGE, PAGE) == PAGE);
	assert(pw_zb_round(0, PAGE) == 0);
	assert(pw_zb_round(UINT32_MAX, PAGE) == 0);
	printf("ok frame bytes\n");
}

static void test_pool_cap(void)
{
	assert(pw_zb_pool_cap(2048, 0, 0) == 2u * 1024 * 1024);
	assert(pw_zb_pool_cap(0, 7, 1000) == 0);
	assert(pw_zb_pool_cap(-1, 7, 614400) == 4300800);
	assert(pw_zb_pool_cap(-1, 7, 0) == 0);
	assert(pw_zb_pool_cap(-1, 0, 4096) == 0);
	assert(pw_zb_pool_cap(-1, 32, UINT32_MAX / 2) == UINT32_MAX);
	assert(pw_zb_pool_cap(65536, 0, 0) == 64u * 1024 * 1024);
	printf("ok pool cap\n");
}

static void test_check(void)
{
	struct pw_zb_req r = {
		.bytes = 100, .count = 0, .max_count = 3,
		.pool_used = 0, .pool_cap = 1000, .total_used = 0, .total_cap = 0,
	};
	assert(pw_zb_check(&r) == PW_ZB_OK);

	/* count wins over pool and ceiling */
	r.count = 3;
	r.pool_cap = 0;
	r.total_cap = 1;
	assert(pw_zb_check(&r) == PW_ZB_OVER_COUNT);
	/* pool wins over ceiling */
	r.count = 2;
	assert(pw_zb_check(&r) == PW_ZB_OVER_POOL);
	r.pool_cap = 1000;
	assert(pw_zb_check(&r) == PW_ZB_OVER_TOTAL);

	/* exact fit is fine, one byte over is not */
	r.total_cap = 0;
	r.pool_used = 900;
	assert(pw_zb_check(&r) == PW_ZB_OK);
	r.pool_used = 901;
	assert(pw_zb_check(&r) == PW_ZB_OVER_POOL);
	r.pool_used = 0;
	r.total_cap = 500;
	r.total_used = 400;
	assert(pw_zb_check(&r) == PW_ZB_OK);
	r.total_used = 401;
	assert(pw_zb_check(&r) == PW_ZB_OVER_TOTAL);

	/* total_cap 0 is no ceiling, however much is in use */
	r.total_cap = 0;
	r.total_used = UINT32_MAX;
	assert(pw_zb_check(&r) == PW_ZB_OK);

	/* a pool or total above its cap (output shrank) rejects, no underflow */
	r.total_used = 0;
	r.pool_used = 2000;
	assert(pw_zb_check(&r) == PW_ZB_OVER_POOL);
	r.pool_used = 0;
	r.total_cap = 100;
	r.total_used = 5000;
	assert(pw_zb_check(&r) == PW_ZB_OVER_TOTAL);

	/* a zero count limit allows nothing, a zero pool takes only empty buffers */
	r.total_cap = 0;
	r.max_count = 0;
	assert(pw_zb_check(&r) == PW_ZB_OVER_COUNT);
	printf("ok check\n");
}

static void test_refund(void)
{
	uint32_t u = 300;
	pw_zb_refund(&u, 100);
	assert(u == 200);
	pw_zb_refund(&u, 200);
	assert(u == 0);
	pw_zb_refund(&u, 1);
	assert(u == 0);
	u = 5;
	pw_zb_refund(&u, UINT32_MAX);
	assert(u == 0);
	printf("ok refund\n");
}

/* The accounting create_buffer does, driven by a script: the player rule
 * (7 buffers on VGA) next to a client of the default pool. */
struct acct {
	uint32_t pool[2], total;
	unsigned count[2];       /* per client: 0 player, 1 other */
	uint32_t held[2][8];
};

static bool alloc(struct acct *a, int client, int pool, unsigned maxc,
	uint32_t pool_cap, uint32_t total_cap, uint32_t bytes)
{
	struct pw_zb_req r = {
		.bytes = bytes, .count = a->count[client], .max_count = maxc,
		.pool_used = a->pool[pool], .pool_cap = pool_cap,
		.total_used = a->total, .total_cap = total_cap,
	};
	if (pw_zb_check(&r) != PW_ZB_OK)
		return false;
	a->held[client][a->count[client]++] = bytes;
	a->pool[pool] += bytes;
	a->total += bytes;
	return true;
}

static void test_script(void)
{
	struct acct a;
	memset(&a, 0, sizeof(a));
	uint32_t frame = pw_zb_frame_bytes(480, 640, PAGE);
	uint32_t app_cap = pw_zb_pool_cap(-1, 7, frame);
	uint32_t def_cap = pw_zb_pool_cap(2048, 0, 0);
	assert(frame == VGA && app_cap == 7 * VGA);

	for (int i = 0; i < 7; i++)
		assert(alloc(&a, 0, 1, 7, app_cap, 0, frame));
	assert(!alloc(&a, 0, 1, 7, app_cap, 0, frame));   /* 8th: count and pool */
	assert(a.pool[1] == app_cap);

	/* the default pool is unaffected: 3 buffers of 600 KiB fit 2 MiB */
	assert(alloc(&a, 1, 0, 3, def_cap, 0, frame));
	assert(alloc(&a, 1, 0, 3, def_cap, 0, frame));
	assert(alloc(&a, 1, 0, 3, def_cap, 0, frame));
	assert(!alloc(&a, 1, 0, 3, def_cap, 0, frame));   /* count */
	assert(a.pool[0] == 3 * VGA);

	/* the same with a ceiling below the sum of the pools */
	struct acct b;
	memset(&b, 0, sizeof(b));
	for (int i = 0; i < 7; i++)
		assert(alloc(&b, 0, 1, 7, app_cap, 5000u * 1024, frame));
	assert(alloc(&b, 1, 0, 3, def_cap, 5000u * 1024, frame));   /* 8 * 600 < 5000 */
	assert(!alloc(&b, 1, 0, 3, def_cap, 5000u * 1024, frame));   /* ceiling */

	/* refunds, as buffer_resource_destroy does, return everything */
	for (int c = 0; c < 2; c++)
		for (unsigned i = 0; i < a.count[c]; i++) {
			pw_zb_refund(&a.pool[c ? 0 : 1], a.held[c][i]);
			pw_zb_refund(&a.total, a.held[c][i]);
		}
	assert(a.pool[0] == 0 && a.pool[1] == 0 && a.total == 0);
	/* and a destroy after the "app_id changed": refund to the original pool */
	a.count[0] = a.count[1] = 0;
	assert(alloc(&a, 0, 1, 7, app_cap, 0, QVGA));
	pw_zb_refund(&a.pool[1], QVGA);
	pw_zb_refund(&a.total, QVGA);
	assert(a.pool[0] == 0 && a.pool[1] == 0 && a.total == 0);
	printf("ok script\n");
}

static void test_exe(void)
{
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	assert(n > 0);
	self[n] = '\0';
	assert(pw_zb_exe_match(getpid(), self));
	assert(!pw_zb_exe_match(getpid(), "/nonexistent"));
	assert(!pw_zb_exe_match(getpid(), ""));
	assert(!pw_zb_exe_match(getpid(), NULL));
	/* a path which is only a prefix, or the deleted form, is no match */
	assert(!pw_zb_exe_match(getpid(), "/"));
	char deleted[PATH_MAX + 16];
	snprintf(deleted, sizeof(deleted), "%s (deleted)", self);
	assert(!pw_zb_exe_match(getpid(), deleted));
	assert(!pw_zb_exe_match(0x7fffffff, self));       /* no such process */
	assert(!pw_zb_exe_match(-1, self));
	printf("ok exe\n");
}

int main(void)
{
	test_frame_bytes();
	test_pool_cap();
	test_check();
	test_refund();
	test_script();
	test_exe();
	printf("All tests passed!\n");
	return 0;
}
