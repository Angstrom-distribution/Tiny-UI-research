#define _GNU_SOURCE
#include <malloc.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

#include "mem.h"
#include "picowl.h"

void pw_mem_init(const struct pw_config *c)
{
	int arena_max = 1;
	int trim_threshold_kb = 256;
	int mmap_threshold_kb = 128;
	int top_pad_kb = 16;

	if (c) {
		arena_max = c->arena_max;
		trim_threshold_kb = c->trim_threshold_kb;
		mmap_threshold_kb = c->mmap_threshold_kb;
		top_pad_kb = c->top_pad_kb;
	}

	mallopt(M_ARENA_MAX, arena_max);
	mallopt(M_TRIM_THRESHOLD, trim_threshold_kb * 1024);
	mallopt(M_MMAP_THRESHOLD, mmap_threshold_kb * 1024);
	mallopt(M_TOP_PAD, top_pad_kb * 1024);

	pw_log(WLR_DEBUG, "malloc: M_ARENA_MAX=%d M_TRIM_THRESHOLD=%d*1024 M_MMAP_THRESHOLD=%d*1024 M_TOP_PAD=%d*1024",
		arena_max, trim_threshold_kb, mmap_threshold_kb, top_pad_kb);
}

void pw_mem_trim(void)
{
	malloc_trim(0);
}

void pw_mem_log_status(const char *tag)
{
	char buf[4096];
	int fd = open("/proc/self/status", O_RDONLY);
	if (fd < 0)
		return;

	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (n <= 0)
		return;

	buf[n] = '\0';

	/* Parse VmRSS and VmHWM. Simple line-by-line parser. */
	uint32_t vm_rss = 0, vm_hwm = 0;
	char *p = buf;
	while (*p) {
		/* Skip to line start */
		while (*p && *p != '\n')
			p++;
		if (*p == '\n')
			p++;

		/* Check if this is a VmRSS or VmHWM line */
		if (p[0] == 'V' && p[1] == 'm') {
			if ((p[2] == 'R' && p[3] == 'S' && p[4] == 'S') ||
			    (p[2] == 'H' && p[3] == 'W' && p[4] == 'M')) {
				/* Find the number after the colon */
				char *colon = p;
				while (*colon && *colon != ':')
					colon++;
				if (*colon == ':') {
					colon++;
					/* Skip spaces */
					while (*colon && isspace(*colon))
						colon++;
					/* Parse the number */
					uint32_t val = 0;
					while (*colon && isdigit(*colon)) {
						val = val * 10 + (*colon - '0');
						colon++;
					}
					if (p[2] == 'R')
						vm_rss = val;
					else
						vm_hwm = val;
				}
			}
		}
	}

	pw_log(WLR_INFO, "%s: VmRSS=%u kB VmHWM=%u kB", tag, vm_rss, vm_hwm);
}
