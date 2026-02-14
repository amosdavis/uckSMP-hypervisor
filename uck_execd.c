// SPDX-License-Identifier: GPL-2.0
/*
 * uck_execd.c - Userspace job execution daemon
 *
 * Handles remote command execution with proper privilege dropping.
 * Receives job requests from the kernel module via /dev/uck ioctl,
 * drops to the requesting user's UID/GID, and exec's the command.
 *
 * This moves job execution out of the kernel TCB and enables
 * proper privilege separation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#include <syslog.h>

#include "uck.h"

#define UCK_EXECD_MAX_JOBS 64

static volatile int running = 1;
static int uck_fd = -1;

struct uck_job {
	uint32_t job_id;
	uint32_t src_node;
	uid_t    uid;
	gid_t    gid;
	char     command[512];
	pid_t    pid;
	int      active;
};

static struct uck_job jobs[UCK_EXECD_MAX_JOBS];

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

static void sigchld_handler(int sig)
{
	int status;
	pid_t pid;
	int i;

	(void)sig;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (i = 0; i < UCK_EXECD_MAX_JOBS; i++) {
			if (jobs[i].active && jobs[i].pid == pid) {
				syslog(LOG_INFO,
				       "uck_execd: job %u (pid %d) exited "
				       "with status %d",
				       jobs[i].job_id, pid,
				       WEXITSTATUS(status));
				jobs[i].active = 0;
				break;
			}
		}
	}
}

static int drop_privileges(uid_t uid, gid_t gid)
{
	if (uid == 0)
		return 0;  /* Already root, no drop needed */

	if (setgid(gid) < 0) {
		syslog(LOG_ERR, "uck_execd: setgid(%u) failed: %s",
		       gid, strerror(errno));
		return -1;
	}
	if (initgroups("nobody", gid) < 0) {
		/* Non-fatal: supplementary groups */
		syslog(LOG_WARNING,
		       "uck_execd: initgroups failed: %s",
		       strerror(errno));
	}
	if (setuid(uid) < 0) {
		syslog(LOG_ERR, "uck_execd: setuid(%u) failed: %s",
		       uid, strerror(errno));
		return -1;
	}
	return 0;
}

static int execute_job(struct uck_job *job)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		syslog(LOG_ERR, "uck_execd: fork failed: %s",
		       strerror(errno));
		return -1;
	}

	if (pid == 0) {
		/* Child: drop privileges and exec */
		if (drop_privileges(job->uid, job->gid) < 0)
			_exit(126);

		/* Close all FDs except stdio */
		for (int fd = 3; fd < 1024; fd++)
			close(fd);

		execl("/bin/sh", "sh", "-c", job->command, (char *)NULL);
		syslog(LOG_ERR, "uck_execd: exec failed: %s",
		       strerror(errno));
		_exit(127);
	}

	job->pid    = pid;
	job->active = 1;
	syslog(LOG_INFO,
	       "uck_execd: started job %u (pid %d) as uid=%u gid=%u: %s",
	       job->job_id, pid, job->uid, job->gid, job->command);
	return 0;
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	openlog("uck_execd", LOG_PID | LOG_CONS, LOG_DAEMON);
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGCHLD, sigchld_handler);

	syslog(LOG_INFO, "uck_execd: starting execution daemon");

	uck_fd = open("/dev/uck", O_RDWR);
	if (uck_fd < 0) {
		syslog(LOG_ERR, "uck_execd: open /dev/uck: %s",
		       strerror(errno));
		fprintf(stderr,
			"uck_execd: cannot open /dev/uck: %s\n",
			strerror(errno));
		return 1;
	}

	memset(jobs, 0, sizeof(jobs));

	syslog(LOG_INFO,
	       "uck_execd: ready, polling for job requests");

	/*
	 * Main loop: poll /dev/uck for incoming job requests.
	 * In a full implementation this would use an ioctl to
	 * dequeue pending jobs from the kernel module.
	 */
	while (running) {
		/*
		 * Placeholder: in production, this reads job requests
		 * from the kernel module via blocking ioctl or read().
		 * Each request contains job_id, uid, gid, command.
		 */
		usleep(100000);  /* 100ms poll interval */
	}

	syslog(LOG_INFO, "uck_execd: shutting down");

	/* Wait for active jobs */
	for (int i = 0; i < UCK_EXECD_MAX_JOBS; i++) {
		if (jobs[i].active) {
			syslog(LOG_INFO,
			       "uck_execd: waiting for job %u (pid %d)",
			       jobs[i].job_id, jobs[i].pid);
			waitpid(jobs[i].pid, NULL, 0);
		}
	}

	close(uck_fd);
	closelog();
	return 0;
}
