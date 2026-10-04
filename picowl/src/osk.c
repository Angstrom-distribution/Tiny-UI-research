/*
 * osk.c - start, signal, restart and stop the on-screen keyboard.
 *
 * pw_spawn double-forks, so the pid of what it starts is lost. The keyboard is
 * forked once instead and stays picowl's child: that gives picowl its pid to
 * signal, and its exit to notice. Only that pid is ever reaped here, so the
 * SIGCHLD source cannot steal the status of anything else. The restart policy
 * is oskstate.c; this file only does the fork, the kill, the waitpid and the
 * timer, and none of it waits on the keyboard while the compositor runs.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "picowl.h"

static struct {
	struct pw_server *server;
	struct pw_oskstate st;
	pid_t pid;                    /* 0 = not running */
	struct wl_event_source *sigchld;
	struct wl_event_source *timer;
	char *exec_cmd;               /* "exec <cmd>" */
} O;

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void osk_exited(void)
{
	int delay = pw_oskstate_exited(&O.st, now_ms());

	if (delay >= 0) {
		pw_log(WLR_INFO, "osk: restarting in %d ms", delay);
		wl_event_source_timer_update(O.timer, delay);
	} else if (O.st.phase == PW_OSK_GAVE_UP) {
		pw_log(WLR_ERROR, "osk: '%s' exited %d times in a row within %d s, "
			"giving up; an osk show or toggle key starts it again",
			O.server->config->osk_cmd, PW_OSK_QUICK_LIMIT, PW_OSK_QUICK_MS / 1000);
	}
}

static void osk_start(void)
{
	wl_event_source_timer_update(O.timer, 0);
	pid_t pid = fork();
	if (pid < 0) {
		pw_log(WLR_ERROR, "osk: fork: %s", strerror(errno));
		pw_oskstate_started(&O.st, now_ms());
		osk_exited();
		return;
	}
	if (pid == 0) {
		/* The compositor blocks SIGCHLD for its signalfd and handles TERM
		 * and INT; the keyboard must not inherit that. It also must not
		 * outlive a compositor that was killed. */
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		execl("/bin/sh", "sh", "-c", O.exec_cmd, (char *)NULL);
		_exit(127);
	}
	O.pid = pid;
	pw_oskstate_started(&O.st, now_ms());
	pw_log(WLR_INFO, "osk: started pid %d: %s", (int)pid, O.server->config->osk_cmd);
}

static int handle_timer(void *data)
{
	(void)data;
	if (!O.pid && O.st.phase == PW_OSK_BACKOFF)
		osk_start();
	return 0;
}

static int handle_sigchld(int signo, void *data)
{
	(void)signo; (void)data;
	int status;

	if (!O.pid || waitpid(O.pid, &status, WNOHANG) != O.pid)
		return 0;
	if (WIFSIGNALED(status))
		pw_log(WLR_INFO, "osk: pid %d killed by signal %d", (int)O.pid, WTERMSIG(status));
	else
		pw_log(WLR_INFO, "osk: pid %d exited with status %d", (int)O.pid, WEXITSTATUS(status));
	O.pid = 0;
	osk_exited();
	return 0;
}

void pw_osk_init(struct pw_server *server)
{
	const char *cmd = server->config ? server->config->osk_cmd : NULL;

	memset(&O, 0, sizeof(O));
	if (!cmd || !*cmd)
		return;
	/* exec, so that the shell is replaced and the pid is the keyboard's. */
	size_t n = strlen(cmd) + sizeof("exec ");
	O.exec_cmd = malloc(n);
	if (!O.exec_cmd)
		return;
	snprintf(O.exec_cmd, n, "exec %s", cmd);
	O.server = server;
	pw_oskstate_init(&O.st, server->config->osk_restart);
	O.sigchld = wl_event_loop_add_signal(server->event_loop, SIGCHLD, handle_sigchld, NULL);
	O.timer = wl_event_loop_add_timer(server->event_loop, handle_timer, NULL);
	if (!O.sigchld || !O.timer) {
		pw_log(WLR_ERROR, "osk: cannot create the event sources, disabled");
		pw_osk_finish(server);
		return;
	}
	osk_start();
}

void pw_osk_finish(struct pw_server *server)
{
	(void)server;
	if (!O.server)
		return;
	pw_oskstate_stop(&O.st);
	if (O.timer)
		wl_event_source_remove(O.timer);
	if (O.sigchld)
		wl_event_source_remove(O.sigchld);
	if (O.pid > 0) {
		/* The compositor is going away, so a short wait is fine here;
		 * a keyboard that ignores SIGTERM is killed. */
		kill(O.pid, SIGTERM);
		for (int i = 0; i < 50; i++) {
			if (waitpid(O.pid, NULL, WNOHANG) == O.pid) {
				O.pid = 0;
				break;
			}
			nanosleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
		}
		if (O.pid > 0) {
			pw_log(WLR_ERROR, "osk: pid %d ignored SIGTERM, killing it", (int)O.pid);
			kill(O.pid, SIGKILL);
			waitpid(O.pid, NULL, 0);
		}
	}
	free(O.exec_cmd);
	memset(&O, 0, sizeof(O));
}

void pw_osk_action(struct pw_server *server, enum pw_osk_op op)
{
	(void)server;
	if (!O.server) {
		pw_log(WLR_INFO, "osk: no [osk] cmd configured, ignoring the osk action");
		return;
	}
	switch (pw_oskstate_request(&O.st, op)) {
	case PW_OSK_DO_START:
		osk_start();
		break;
	case PW_OSK_DO_SIGNAL_SHOW:
		if (kill(O.pid, SIGUSR2) < 0)
			pw_log(WLR_ERROR, "osk: kill: %s", strerror(errno));
		break;
	case PW_OSK_DO_SIGNAL_HIDE:
		if (kill(O.pid, SIGUSR1) < 0)
			pw_log(WLR_ERROR, "osk: kill: %s", strerror(errno));
		break;
	case PW_OSK_DO_SIGNAL_TOGGLE:
		/* A real-time signal, so its number is only known at run time. */
		if (kill(O.pid, SIGRTMIN) < 0)
			pw_log(WLR_ERROR, "osk: kill: %s", strerror(errno));
		break;
	case PW_OSK_DO_NOTHING:
		break;
	}
}
