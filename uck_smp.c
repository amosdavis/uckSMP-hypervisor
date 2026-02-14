/*
 * uck_smp.c - Run a program with UCK SMP distribution
 *
 * Registers the calling process for SMP mode, then exec's the target
 * program. Any fork()/clone() calls made by the target will be
 * transparently distributed across the UCK cluster.
 *
 * Usage:
 *   uck_smp <program> [args...]
 *
 * Examples:
 *   uck_smp make -j4           # distribute 4 compile jobs across cluster
 *   uck_smp ./my_parallel_app  # forks are distributed automatically
 *   uck_smp bash -c 'for i in 1 2 3 4; do ./work & done; wait'
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "uck.h"

int main(int argc, char *argv[])
{
	int fd;
	struct uck_smp_req req;

	if (argc < 2) {
		fprintf(stderr,
			"Usage: %s <program> [args...]\n\n"
			"Runs <program> with UCK SMP distribution enabled.\n"
			"Any fork/clone calls will be transparently distributed\n"
			"across the UCK cluster nodes.\n\n"
			"Examples:\n"
			"  %s make -j4\n"
			"  %s ./my_app\n"
			"  %s bash -c 'for i in 1 2 3 4; do ./work & done; wait'\n",
			argv[0], argv[0], argv[0], argv[0]);
		return 1;
	}

	fd = open("/dev/uck", O_RDWR);
	if (fd < 0) {
		perror("open /dev/uck");
		fprintf(stderr, "Is the uck kernel module loaded?\n");
		return 1;
	}

	/* Register this process for SMP distribution */
	memset(&req, 0, sizeof(req));
	req.pid = 0;  /* 0 = current process */

	if (ioctl(fd, UCK_IOC_ENABLE_SMP, &req) < 0) {
		perror("ioctl UCK_IOC_ENABLE_SMP");
		close(fd);
		return 1;
	}

	printf("uck_smp: SMP mode enabled (pid %d)\n", getpid());
	printf("uck_smp: fork/clone calls will be distributed across cluster\n");
	close(fd);

	/* Exec the target program */
	execvp(argv[1], &argv[1]);
	perror("execvp");
	return 1;
}
