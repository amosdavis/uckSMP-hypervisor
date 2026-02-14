// SPDX-License-Identifier: GPL-2.0
/*
 * uck_netd.c - Userspace networking proxy daemon
 *
 * Moves TCP/TLS network handling out of the kernel module to reduce
 * the Trusted Computing Base (TCB).  Communicates with the kernel
 * module via /dev/uck ioctls and relays inter-node messages over
 * authenticated TLS connections.
 *
 * This is a design stub — full TLS integration requires linking
 * against OpenSSL or GnuTLS in userspace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>

#define UCK_NETD_PORT      9999
#define UCK_NETD_MAX_PEERS 16
#define UCK_NETD_BUF_SIZE  (4096 + 256)  /* page + header */

static volatile int running = 1;

struct peer {
	int      fd;
	uint32_t node_id;
	struct   sockaddr_in addr;
	int      authenticated;
};

static struct peer peers[UCK_NETD_MAX_PEERS];
static int uck_fd = -1;  /* /dev/uck file descriptor */

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

static int uck_netd_listen(int port)
{
	int fd, opt = 1;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port        = htons(port);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return -1;
	}
	if (listen(fd, UCK_NETD_MAX_PEERS) < 0) {
		perror("listen");
		close(fd);
		return -1;
	}
	return fd;
}

static void uck_netd_relay(int peer_fd)
{
	char buf[UCK_NETD_BUF_SIZE];
	ssize_t n;

	n = recv(peer_fd, buf, sizeof(buf), 0);
	if (n <= 0)
		return;

	/*
	 * In a full implementation, this would:
	 * 1. Decrypt the TLS payload
	 * 2. Verify the HMAC
	 * 3. Relay to /dev/uck via ioctl
	 *
	 * For now, this is a structural stub showing the architecture.
	 */
	if (uck_fd >= 0) {
		/* Forward message to kernel module */
		if (write(uck_fd, buf, n) < 0)
			perror("write to /dev/uck");
	}
}

int main(int argc, char *argv[])
{
	int listen_fd, nfds;
	struct pollfd fds[UCK_NETD_MAX_PEERS + 1];

	(void)argc;
	(void)argv;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	printf("uck_netd: starting userspace networking proxy\n");

	/* Open kernel module device */
	uck_fd = open("/dev/uck", O_RDWR);
	if (uck_fd < 0) {
		perror("open /dev/uck");
		fprintf(stderr, "uck_netd: continuing without kernel module\n");
	}

	listen_fd = uck_netd_listen(UCK_NETD_PORT);
	if (listen_fd < 0) {
		fprintf(stderr, "uck_netd: failed to listen on port %d\n",
			UCK_NETD_PORT);
		return 1;
	}

	printf("uck_netd: listening on port %d\n", UCK_NETD_PORT);
	memset(peers, 0, sizeof(peers));
	memset(fds, 0, sizeof(fds));

	fds[0].fd     = listen_fd;
	fds[0].events = POLLIN;
	nfds          = 1;

	while (running) {
		int ret = poll(fds, nfds, 1000);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		/* Accept new connections */
		if (fds[0].revents & POLLIN) {
			struct sockaddr_in peer_addr;
			socklen_t peer_len = sizeof(peer_addr);
			int peer_fd = accept(listen_fd,
					     (struct sockaddr *)&peer_addr,
					     &peer_len);
			if (peer_fd >= 0 && nfds < UCK_NETD_MAX_PEERS + 1) {
				fds[nfds].fd     = peer_fd;
				fds[nfds].events = POLLIN;
				nfds++;
				printf("uck_netd: accepted connection from "
				       "%s:%d\n",
				       inet_ntoa(peer_addr.sin_addr),
				       ntohs(peer_addr.sin_port));
			}
		}

		/* Handle peer data */
		for (int i = 1; i < nfds; i++) {
			if (fds[i].revents & POLLIN)
				uck_netd_relay(fds[i].fd);
			if (fds[i].revents & (POLLHUP | POLLERR)) {
				close(fds[i].fd);
				fds[i] = fds[nfds - 1];
				nfds--;
				i--;
			}
		}
	}

	printf("uck_netd: shutting down\n");
	for (int i = 1; i < nfds; i++)
		close(fds[i].fd);
	close(listen_fd);
	if (uck_fd >= 0)
		close(uck_fd);
	return 0;
}
