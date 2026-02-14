/*
 * uck_exec.c (userspace) - Execute commands across the UCK cluster
 *
 * Usage:
 *   uck_exec <command>              Run on least-loaded node
 *   uck_exec -n <node> <command>    Run on specific node
 *   uck_exec -p <N> <command>       Run N copies across cluster (parallel)
 *   uck_exec jobs                   Show job status
 *   uck_exec wait <job_id>          Wait for job to complete
 *
 * Examples:
 *   uck_exec "stress --cpu 2 --timeout 30"
 *   uck_exec -p 4 "dd if=/dev/zero of=/dev/null bs=1M count=1000"
 *   uck_exec jobs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <time.h>

#include "uck.h"

static int open_uck(void)
{
	int fd = open("/dev/uck", O_RDWR);
	if (fd < 0) {
		perror("open /dev/uck");
		fprintf(stderr, "Is the uck kernel module loaded?\n");
	}
	return fd;
}

static const char *state_str(int state)
{
	switch (state) {
	case UCK_JOB_PENDING: return "pending";
	case UCK_JOB_RUNNING: return "running";
	case UCK_JOB_DONE:    return "done";
	case UCK_JOB_FAILED:  return "failed";
	default:              return "unknown";
	}
}

static int cmd_exec(int fd, const char *command, int dest_node)
{
	struct uck_remote_exec_req req;

	memset(&req, 0, sizeof(req));
	req.dest_node = dest_node;  /* 0 = auto */
	strncpy(req.command, command, sizeof(req.command) - 1);

	if (ioctl(fd, UCK_IOC_REMOTE_EXEC, &req) < 0) {
		perror("ioctl UCK_IOC_REMOTE_EXEC");
		return 1;
	}

	printf("Job %u submitted\n", req.dest_node);  /* job_id returned here */
	return 0;
}

static int cmd_parallel(int fd, const char *command, int copies)
{
	int i;
	printf("Submitting %d parallel jobs: %s\n", copies, command);

	for (i = 0; i < copies; i++) {
		struct uck_remote_exec_req req;
		memset(&req, 0, sizeof(req));
		req.dest_node = 0;  /* auto-select */
		strncpy(req.command, command, sizeof(req.command) - 1);

		if (ioctl(fd, UCK_IOC_REMOTE_EXEC, &req) < 0) {
			perror("ioctl UCK_IOC_REMOTE_EXEC");
			return 1;
		}
		printf("  Job %u submitted\n", req.dest_node);
	}
	return 0;
}

static int cmd_jobs(int fd)
{
	struct uck_job_query query;
	unsigned int i;

	memset(&query, 0, sizeof(query));
	query.job_id = 0;  /* list all */

	if (ioctl(fd, UCK_IOC_GET_JOBS, &query) < 0) {
		perror("ioctl UCK_IOC_GET_JOBS");
		return 1;
	}

	if (query.count == 0) {
		printf("No jobs.\n");
		return 0;
	}

	printf("%-6s %-6s %-8s %-6s %s\n",
	       "JOB", "NODE", "STATE", "EXIT", "COMMAND");
	printf("%-6s %-6s %-8s %-6s %s\n",
	       "---", "----", "-----", "----", "-------");

	for (i = 0; i < query.count; i++) {
		struct uck_job_info *j = &query.jobs[i];
		printf("%-6u %-6u %-8s %-6d %s\n",
		       j->job_id, j->node_id,
		       state_str(j->state),
		       j->exit_code, j->command);
	}
	return 0;
}

static int cmd_wait(int fd, unsigned int job_id)
{
	printf("Waiting for job %u...\n", job_id);

	while (1) {
		struct uck_job_query query;
		memset(&query, 0, sizeof(query));
		query.job_id = job_id;

		if (ioctl(fd, UCK_IOC_GET_JOBS, &query) < 0) {
			perror("ioctl UCK_IOC_GET_JOBS");
			return 1;
		}

		if (query.count == 0) {
			fprintf(stderr, "Job %u not found\n", job_id);
			return 1;
		}

		if (query.jobs[0].state == UCK_JOB_DONE) {
			printf("Job %u completed (exit code %d)\n",
			       job_id, query.jobs[0].exit_code);
			return query.jobs[0].exit_code;
		}
		if (query.jobs[0].state == UCK_JOB_FAILED) {
			printf("Job %u failed (exit code %d)\n",
			       job_id, query.jobs[0].exit_code);
			return query.jobs[0].exit_code;
		}

		usleep(500000);  /* 500ms poll */
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] <command>\n"
		"       %s jobs              Show all jobs\n"
		"       %s wait <job_id>     Wait for job completion\n"
		"\n"
		"Options:\n"
		"  -n <node>    Run on specific node (0=auto, default)\n"
		"  -p <N>       Run N copies in parallel across cluster\n"
		"\n"
		"Examples:\n"
		"  %s \"echo hello\"           Run on least-loaded node\n"
		"  %s -n 2 \"ls /\"            Run on node 2\n"
		"  %s -p 4 \"stress --cpu 1\"  4 parallel stress jobs\n",
		prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
	int fd, opt;
	int dest_node = 0;
	int parallel = 0;

	static struct option long_opts[] = {
		{"node",     required_argument, 0, 'n'},
		{"parallel", required_argument, 0, 'p'},
		{"help",     no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "n:p:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'n':
			dest_node = atoi(optarg);
			break;
		case 'p':
			parallel = atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return 1;
	}

	/* Handle subcommands */
	if (strcmp(argv[optind], "jobs") == 0) {
		fd = open_uck();
		if (fd < 0) return 1;
		int ret = cmd_jobs(fd);
		close(fd);
		return ret;
	}

	if (strcmp(argv[optind], "wait") == 0) {
		if (optind + 1 >= argc) {
			fprintf(stderr, "Usage: %s wait <job_id>\n", argv[0]);
			return 1;
		}
		fd = open_uck();
		if (fd < 0) return 1;
		int ret = cmd_wait(fd, atoi(argv[optind + 1]));
		close(fd);
		return ret;
	}

	/* Build command string from remaining args */
	char command[512] = {0};
	int i;
	for (i = optind; i < argc; i++) {
		if (i > optind)
			strncat(command, " ", sizeof(command) - strlen(command) - 1);
		strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
	}

	fd = open_uck();
	if (fd < 0) return 1;

	int ret;
	if (parallel > 0)
		ret = cmd_parallel(fd, command, parallel);
	else
		ret = cmd_exec(fd, command, dest_node);

	close(fd);
	return ret;
}
