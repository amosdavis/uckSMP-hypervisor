/*
 * uckctl.c - UCK Cluster control tool
 *
 * Usage:
 *   uckctl status       Show cluster status (nodes, CPUs, memory)
 *   uckctl migrate PID NODE   Migrate process PID to NODE
 *   uckctl nodes        Show per-node details
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

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

static int cmd_status(void)
{
	int fd;
	struct uck_cluster_info info;

	fd = open_uck();
	if (fd < 0)
		return 1;

	if (ioctl(fd, UCK_IOC_GET_CLUSTER, &info) < 0) {
		perror("ioctl UCK_IOC_GET_CLUSTER");
		close(fd);
		return 1;
	}

	printf("UCK Cluster Status\n");
	printf("==================\n");
	printf("Active Nodes:    %u\n", info.num_nodes);
	printf("Total CPUs:      %u\n", info.total_cpus);
	printf("Total Memory:    %llu MB\n",
	       (unsigned long long)info.total_mem / (1024 * 1024));
	printf("Free Memory:     %llu MB\n",
	       (unsigned long long)info.total_free_mem / (1024 * 1024));
	printf("Running Tasks:   %u\n", info.total_running);
	printf("\nPer-node details: cat /proc/uck/nodes\n");

	close(fd);
	return 0;
}

static int cmd_migrate(int argc, char *argv[])
{
	int fd;
	struct uck_migrate_req req;

	if (argc < 4) {
		fprintf(stderr, "Usage: uckctl migrate <pid> <dest_node>\n");
		return 1;
	}

	req.pid = atoi(argv[2]);
	req.dest_node = atoi(argv[3]);

	if (req.pid == 0 || req.dest_node == 0) {
		fprintf(stderr, "Invalid PID or node ID\n");
		return 1;
	}

	fd = open_uck();
	if (fd < 0)
		return 1;

	printf("Migrating PID %u to node %u...\n", req.pid, req.dest_node);

	if (ioctl(fd, UCK_IOC_MIGRATE_PROC, &req) < 0) {
		perror("ioctl UCK_IOC_MIGRATE_PROC");
		close(fd);
		return 1;
	}

	printf("Migration initiated successfully.\n");
	close(fd);
	return 0;
}

static int cmd_nodes(void)
{
	FILE *f;
	char buf[1024];

	f = fopen("/proc/uck/nodes", "r");
	if (!f) {
		perror("open /proc/uck/nodes");
		fprintf(stderr, "Is the uck kernel module loaded?\n");
		return 1;
	}

	while (fgets(buf, sizeof(buf), f))
		fputs(buf, stdout);

	fclose(f);
	return 0;
}

static int cmd_jobs(void)
{
	FILE *f;
	char buf[1024];

	f = fopen("/proc/uck/jobs", "r");
	if (!f) {
		perror("open /proc/uck/jobs");
		fprintf(stderr, "Is the uck kernel module loaded?\n");
		return 1;
	}

	while (fgets(buf, sizeof(buf), f))
		fputs(buf, stdout);

	fclose(f);
	return 0;
}

static int cmd_exec(int argc, char *argv[])
{
	int fd;
	struct uck_remote_exec_req req;
	int i;

	if (argc < 3) {
		fprintf(stderr, "Usage: uckctl exec <command...>\n");
		return 1;
	}

	memset(&req, 0, sizeof(req));
	req.dest_node = 0;  /* auto-select */

	/* Concatenate remaining args into command */
	for (i = 2; i < argc; i++) {
		if (i > 2)
			strncat(req.command, " ",
				sizeof(req.command) - strlen(req.command) - 1);
		strncat(req.command, argv[i],
			sizeof(req.command) - strlen(req.command) - 1);
	}

	fd = open_uck();
	if (fd < 0)
		return 1;

	if (ioctl(fd, UCK_IOC_REMOTE_EXEC, &req) < 0) {
		perror("ioctl UCK_IOC_REMOTE_EXEC");
		close(fd);
		return 1;
	}

	printf("Job %u submitted to cluster.\n", req.dest_node);
	close(fd);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: uckctl <command> [args]\n"
		"\n"
		"Commands:\n"
		"  status                Show cluster summary\n"
		"  nodes                 Show per-node details\n"
		"  jobs                  Show cluster jobs\n"
		"  exec <command...>     Execute command on cluster\n"
		"  migrate <pid> <node>  Migrate process to node\n");
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		usage();
		return 1;
	}

	if (strcmp(argv[1], "status") == 0)
		return cmd_status();
	else if (strcmp(argv[1], "nodes") == 0)
		return cmd_nodes();
	else if (strcmp(argv[1], "jobs") == 0)
		return cmd_jobs();
	else if (strcmp(argv[1], "exec") == 0)
		return cmd_exec(argc, argv);
	else if (strcmp(argv[1], "migrate") == 0)
		return cmd_migrate(argc, argv);
	else {
		fprintf(stderr, "Unknown command: %s\n", argv[1]);
		usage();
		return 1;
	}
}
