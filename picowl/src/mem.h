/* mem.h - malloc tuning and memory status logging. */
#ifndef PW_MEM_H
#define PW_MEM_H

struct pw_config;

void pw_mem_init(const struct pw_config *c); /* c may be NULL */
void pw_mem_trim(void);
void pw_mem_log_status(const char *tag);

#endif
