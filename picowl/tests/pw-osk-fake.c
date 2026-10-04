/*
 * pw-osk-fake - stands in for the on-screen keyboard in tests/osk-e2e.sh.
 *
 * usage: pw-osk-fake LOG [--exit-after MS]
 *
 * Blocks the three signals wvkbd handles and reads them from a signalfd, like
 * wvkbd does, and appends one line per event to LOG: "start PID", "hide"
 * (SIGUSR1), "show" (SIGUSR2), "toggle" (SIGRTMIN), "term" (SIGTERM while its
 * parent is alive, or "term-orphaned" after the parent died; then it exits 0). With --exit-after it exits with status 1 that many milliseconds
 * after it started, which is a crash as far as the supervisor can tell.
 * Each line is written with a single write(), so the test can read the file
 * while this runs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

static int logfd;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char buf[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	buf[n++] = '\n';
	if (write(logfd, buf, n) < 0)
		_exit(2);
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	int64_t exit_after = -1;

	if (argc < 2) {
		fprintf(stderr, "usage: pw-osk-fake LOG [--exit-after MS]\n");
		return 2;
	}
	if (argc == 4 && !strcmp(argv[2], "--exit-after"))
		exit_after = atoll(argv[3]);

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGRTMIN);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_BLOCK, &set, NULL);
	int sfd = signalfd(-1, &set, 0);
	logfd = open(argv[1], O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (sfd < 0 || logfd < 0)
		return 2;

	pid_t parent = getppid();
	int64_t start = now_ms();
	say("start %d", (int)getpid());
	for (;;) {
		int timeout = -1;
		if (exit_after >= 0) {
			int64_t left = start + exit_after - now_ms();
			if (left <= 0)
				return 1;
			timeout = (int)left;
		}
		struct pollfd p = { .fd = sfd, .events = POLLIN };
		if (poll(&p, 1, timeout) < 0 && errno != EINTR)
			return 2;
		struct signalfd_siginfo si;
		if (p.revents & POLLIN && read(sfd, &si, sizeof(si)) == sizeof(si)) {
			if ((int)si.ssi_signo == SIGUSR1)
				say("hide");
			else if ((int)si.ssi_signo == SIGUSR2)
				say("show");
			else if ((int)si.ssi_signo == SIGRTMIN)
				say("toggle");
			else if ((int)si.ssi_signo == SIGTERM) {
				/* The kernel's parent-death signal also lands here, but
				 * then the parent is gone: only a supervisor that
				 * stops the keyboard itself gets plain "term". */
				say(getppid() == parent ? "term" : "term-orphaned");
				return 0;
			}
		}
	}
}
