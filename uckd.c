/*
 * uckd.c - UCK userspace daemon
 *
 * Configures the kernel module via ioctls:
 *   1. Sets local node identity
 *   2. Registers remote nodes
 *   3. Creates/joins shared memory regions
 *
 * Usage:
 *   uckd --node-id 1 --ip 10.4.4.100 --port 9999 \
 *        --remote 2:10.4.4.101:9999 \
 *        --create-region 1:67108864
 *
 *   uckd --node-id 2 --ip 10.4.4.101 --port 9999 \
 *        --remote 1:10.4.4.100:9999 \
 *        --join-region 1:67108864:1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <sys/stat.h>
#include <errno.h>

#include "uck.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --node-id N          Local node ID\n"
		"  --ip ADDR            Local IP address\n"
		"  --port PORT          Local UCK port (default 9999)\n"
		"  --remote ID:IP:PORT  Add a remote node (repeatable)\n"
		"  --create-region ID:SIZE\n"
		"                       Create a shared region (owner=self)\n"
		"  --join-region ID:SIZE:OWNER\n"
		"                       Join a remote region\n"
		"  --daemon             Run in background\n",
		prog);
}

static int parse_remote(const char *s, struct uck_node_info *info)
{
	char ip_str[64];
	int port;
	if (sscanf(s, "%u:%63[^:]:%d", &info->node_id, ip_str, &port) != 3)
		return -1;
	info->ip_addr = inet_addr(ip_str);
	info->port = port;
	info->flags = 0;
	return 0;
}

int main(int argc, char *argv[])
{
	int fd, ret;
	int node_id = -1;
	char *ip_str = NULL;
	int port = 9999;
	int daemon_mode = 0;

	struct uck_node_info remotes[UCK_MAX_NODES];
	int num_remotes = 0;

	struct {
		struct uck_region_info info;
		int is_create; /* 1=create, 0=join */
	} regions[UCK_MAX_REGIONS];
	int num_regions = 0;

	static struct option long_opts[] = {
		{"node-id",       required_argument, 0, 'n'},
		{"ip",            required_argument, 0, 'i'},
		{"port",          required_argument, 0, 'p'},
		{"remote",        required_argument, 0, 'r'},
		{"create-region", required_argument, 0, 'c'},
		{"join-region",   required_argument, 0, 'j'},
		{"daemon",        no_argument,       0, 'd'},
		{"help",          no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "n:i:p:r:c:j:dh",
				  long_opts, NULL)) != -1) {
		switch (opt) {
		case 'n':
			node_id = atoi(optarg);
			break;
		case 'i':
			ip_str = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'r':
			if (num_remotes >= UCK_MAX_NODES) {
				fprintf(stderr, "Too many remotes\n");
				return 1;
			}
			if (parse_remote(optarg, &remotes[num_remotes]) < 0) {
				fprintf(stderr, "Bad remote: %s\n", optarg);
				return 1;
			}
			num_remotes++;
			break;
		case 'c': {
			unsigned long long rid, sz;
			if (sscanf(optarg, "%llu:%llu", &rid, &sz) != 2) {
				fprintf(stderr, "Bad region: %s\n", optarg);
				return 1;
			}
			regions[num_regions].info.region_id = rid;
			regions[num_regions].info.size = sz;
			regions[num_regions].info.owner_node = node_id;
			regions[num_regions].is_create = 1;
			num_regions++;
			break;
		}
		case 'j': {
			unsigned long long rid, sz;
			unsigned int owner;
			if (sscanf(optarg, "%llu:%llu:%u",
				   &rid, &sz, &owner) != 3) {
				fprintf(stderr, "Bad join: %s\n", optarg);
				return 1;
			}
			regions[num_regions].info.region_id = rid;
			regions[num_regions].info.size = sz;
			regions[num_regions].info.owner_node = owner;
			regions[num_regions].is_create = 0;
			num_regions++;
			break;
		}
		case 'd':
			daemon_mode = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (node_id < 0 || !ip_str) {
		fprintf(stderr, "Error: --node-id and --ip are required\n");
		usage(argv[0]);
		return 1;
	}

	/* Configuration validation */
	if (port == 0 || port > 65535) {
		fprintf(stderr, "uckd: invalid port %d\n", port);
		return 1;
	}
	if ((unsigned int)node_id >= UCK_MAX_NODES) {
		fprintf(stderr, "uckd: node_id %d exceeds max %u\n",
			node_id, UCK_MAX_NODES - 1);
		return 1;
	}
	if (num_remotes < 1) {
		fprintf(stderr, "uckd: need at least 1 remote node (got %d)\n",
			num_remotes);
		return 1;
	}

	/* Create /run/uck/ directory for secure state files */
	if (mkdir("/run/uck", 0700) < 0 && errno != EEXIST) {
		perror("uckd: failed to create /run/uck");
		return 1;
	}

	fd = open("/dev/uck", O_RDWR);
	if (fd < 0) {
		perror("open /dev/uck");
		fprintf(stderr, "Is the uck kernel module loaded?\n");
		return 1;
	}

	/* 1. Set local node */
	{
		struct uck_node_info local;
		memset(&local, 0, sizeof(local));
		local.node_id = node_id;
		local.ip_addr = inet_addr(ip_str);
		local.port = port;
		ret = ioctl(fd, UCK_IOC_SET_NODE, &local);
		if (ret < 0) {
			perror("ioctl UCK_IOC_SET_NODE");
			close(fd);
			return 1;
		}
		printf("uckd: local node set (id=%d ip=%s port=%d)\n",
		       node_id, ip_str, port);
	}

	/* 2. Add remote nodes */
	for (int i = 0; i < num_remotes; i++) {
		ret = ioctl(fd, UCK_IOC_ADD_NODE, &remotes[i]);
		if (ret < 0) {
			perror("ioctl UCK_IOC_ADD_NODE");
			close(fd);
			return 1;
		}
		printf("uckd: added remote node %u\n", remotes[i].node_id);
	}

	/* 3. Create/join regions */
	for (int i = 0; i < num_regions; i++) {
		if (regions[i].is_create) {
			/* Owner is self for created regions */
			regions[i].info.owner_node = node_id;
			ret = ioctl(fd, UCK_IOC_CREATE_REGION,
				    &regions[i].info);
			if (ret < 0) {
				perror("ioctl UCK_IOC_CREATE_REGION");
				close(fd);
				return 1;
			}
			printf("uckd: created region %llu (%llu bytes)\n",
			       (unsigned long long)regions[i].info.region_id,
			       (unsigned long long)regions[i].info.size);
		} else {
			ret = ioctl(fd, UCK_IOC_JOIN_REGION,
				    &regions[i].info);
			if (ret < 0) {
				perror("ioctl UCK_IOC_JOIN_REGION");
				close(fd);
				return 1;
			}
			printf("uckd: joined region %llu (owner=%u)\n",
			       (unsigned long long)regions[i].info.region_id,
			       regions[i].info.owner_node);
		}
	}

	printf("uckd: configuration complete\n");

	if (daemon_mode) {
		printf("uckd: running in background (fd kept open)\n");
		if (daemon(0, 0) < 0) {
			perror("daemon");
			close(fd);
			return 1;
		}
		/* Keep fd open so the module stays configured */
		pause();
	}

	/* Keep fd open - closing would be fine, module state persists */
	printf("uckd: done (module configured, fd open)\n");

	/* In non-daemon mode, just sleep forever to keep fd open */
	if (!daemon_mode) {
		printf("uckd: press Ctrl+C to stop\n");
		pause();
	}

	close(fd);
	return 0;
}
