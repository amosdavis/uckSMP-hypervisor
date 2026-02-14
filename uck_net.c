/*
 * uck_net.c - Network layer for UCK (TCP-based page transfer + migration)
 *
 * Handles:
 *   - Listening for incoming page requests from remote nodes
 *   - Fetching pages from remote nodes on fault
 *   - Connecting to remote nodes
 *   - Heartbeat exchange
 *   - Process migration messages
 */

#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/socket.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/delay.h>
#include <net/sock.h>
#include <linux/sockptr.h>

#include "uck_internal.h"

/* ---- Helpers (non-static for use by other modules) ---- */

int uck_sock_send(struct socket *sock, void *buf, int len)
{
	struct kvec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };
	int sent = 0, ret;

	while (sent < len) {
		iov.iov_base = (char *)buf + sent;
		iov.iov_len = len - sent;
		ret = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
		if (ret <= 0)
			return ret ? ret : -EIO;
		sent += ret;
	}
	return sent;
}

int uck_sock_recv(struct socket *sock, void *buf, int len)
{
	struct kvec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg = { .msg_flags = MSG_WAITALL | MSG_NOSIGNAL };
	int rcvd = 0, ret;

	while (rcvd < len) {
		iov.iov_base = (char *)buf + rcvd;
		iov.iov_len = len - rcvd;
		ret = kernel_recvmsg(sock, &msg, &iov, 1, iov.iov_len,
				     msg.msg_flags);
		if (ret <= 0)
			return ret ? ret : -EIO;
		rcvd += ret;
	}
	return rcvd;
}

/* Send header + optional payload in one call */
int uck_net_send_msg(struct socket *sock, struct uck_msg_hdr *hdr,
		     void *payload, int payload_len)
{
	int ret;

	ret = uck_sock_send(sock, hdr, sizeof(*hdr));
	if (ret < 0)
		return ret;

	if (payload && payload_len > 0) {
		ret = uck_sock_send(sock, payload, payload_len);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* ---- Handle incoming page request ---- */

static void uck_handle_page_request(struct socket *client,
				     struct uck_msg_hdr *hdr)
{
	struct uck_region *region;
	struct uck_page_entry *entry;
	struct uck_msg_hdr resp;
	void *kaddr;
	char *zero_page;

	zero_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!zero_page)
		return;

	mutex_lock(&uck_state.lock);
	region = uck_find_region(hdr->region_id);
	mutex_unlock(&uck_state.lock);

	memset(&resp, 0, sizeof(resp));
	resp.type = UCK_MSG_PAGE_RESP;
	resp.src_node = uck_state.local_node.node_id;
	resp.region_id = hdr->region_id;
	resp.page_offset = hdr->page_offset;

	if (!region) {
		resp.flags = 1; /* error flag */
		resp.payload_len = 0;
		uck_sock_send(client, &resp, sizeof(resp));
		kfree(zero_page);
		return;
	}

	mutex_lock(&region->lock);
	entry = uck_page_lookup(region, hdr->page_offset >> PAGE_SHIFT);
	mutex_unlock(&region->lock);

	resp.payload_len = PAGE_SIZE;

	pr_info_ratelimited("uck: serving page req region=%llu offset=%llu entry=%p\n",
			    hdr->region_id, hdr->page_offset, entry);

	if (entry && entry->page) {
		/* Send the page data */
		uck_sock_send(client, &resp, sizeof(resp));
		kaddr = kmap(entry->page);
		uck_sock_send(client, kaddr, PAGE_SIZE);
		kunmap(entry->page);
	} else {
		/* Page not materialized yet, send zeros */
		uck_sock_send(client, &resp, sizeof(resp));
		uck_sock_send(client, zero_page, PAGE_SIZE);
	}

	kfree(zero_page);
}

/* ---- Handle incoming invalidation ---- */

static void uck_handle_invalidate(struct socket *client,
				   struct uck_msg_hdr *hdr)
{
	struct uck_region *region;
	struct uck_page_entry *entry;
	struct uck_msg_hdr ack;

	mutex_lock(&uck_state.lock);
	region = uck_find_region(hdr->region_id);
	mutex_unlock(&uck_state.lock);

	if (region) {
		pgoff_t idx = hdr->page_offset >> PAGE_SHIFT;
		mutex_lock(&region->lock);
		entry = uck_page_lookup(region, idx);
		if (entry) {
			if (entry->page) {
				__free_page(entry->page);
				entry->page = NULL;
			}
			entry->state = UCK_PAGE_INVALID;
		}
		mutex_unlock(&region->lock);
	}

	memset(&ack, 0, sizeof(ack));
	ack.type = UCK_MSG_INVALIDATE_ACK;
	ack.src_node = uck_state.local_node.node_id;
	ack.region_id = hdr->region_id;
	ack.page_offset = hdr->page_offset;
	uck_sock_send(client, &ack, sizeof(ack));
}

/* ---- Client connection handler ---- */

static int uck_handle_client(void *data)
{
	struct socket *client = data;
	struct uck_msg_hdr hdr;
	int ret;

	while (!kthread_should_stop()) {
		ret = uck_sock_recv(client, &hdr, sizeof(hdr));
		if (ret <= 0)
			break;

		/* Rate limit check per-node */
		if (!uck_ratelimit_check_net(hdr.src_node)) {
			pr_warn_ratelimited("uck: rate limit exceeded for node %u\n",
					    hdr.src_node);
			atomic_long_inc(&uck_state.err_ratelimit);
			return 0;
		}

		/* Validate cluster epoch to reject stale messages */
		if (hdr.epoch != 0 && hdr.epoch < uck_state.cluster_epoch) {
			pr_warn_ratelimited("uck: stale epoch %u from node %u "
					    "(current %u)\n",
					    hdr.epoch, hdr.src_node,
					    uck_state.cluster_epoch);
			continue;
		}

		switch (hdr.type) {
		case UCK_MSG_PAGE_REQ:
			uck_handle_page_request(client, &hdr);
			break;
		case UCK_MSG_INVALIDATE:
			uck_handle_invalidate(client, &hdr);
			break;
		case UCK_MSG_HEARTBEAT:
			uck_handle_heartbeat(client, &hdr);
			break;
		case UCK_MSG_MIGRATE_REQ:
			uck_handle_migrate_request(client, &hdr);
			break;
		case UCK_MSG_PROC_STATE:
			uck_handle_proc_state(client, &hdr);
			break;
		case UCK_MSG_EXEC_REQ:
			uck_handle_exec_req(client, &hdr);
			break;
		case UCK_MSG_EXEC_STARTED:
			uck_handle_exec_started(client, &hdr);
			break;
		case UCK_MSG_EXEC_DONE:
			uck_handle_exec_done(client, &hdr);
			break;
		case UCK_MSG_BATCH_PAGE_REQ:
			uck_handle_batch_page_req(client, &hdr);
			break;
		case UCK_MSG_NODE_ANNOUNCE:
			uck_handle_node_announce(client, &hdr);
			break;
		case UCK_MSG_NODE_LEAVE:
			uck_handle_node_leave(client, &hdr);
			break;
		case UCK_MSG_FUTEX_WAIT:
			uck_handle_futex_wait(client, &hdr);
			break;
		case UCK_MSG_FUTEX_WAKE:
			uck_handle_futex_wake(client, &hdr);
			break;
		case UCK_MSG_FUTEX_WAKE_NACK:
			/* Remote node could not complete wake — waiter orphaned */
			pr_warn("uck: futex wake NACK from node %u for addr 0x%lx\n",
				hdr.src_node, (unsigned long)0UL);
			atomic_long_inc(&uck_state.err_futex);
			break;
		case UCK_MSG_CGROUP_STATS:
			uck_handle_cgroup_stats(client, &hdr);
			break;
		default:
			pr_warn("uck: unknown message type %u\n", hdr.type);
			break;
		}
	}

	sock_release(client);
	return 0;
}

/* ---- Listener thread ---- */

static int uck_listener_thread(void *data)
{
	struct socket *client;
	int ret;

	pr_info("uck: listener started\n");

	while (!kthread_should_stop() && uck_state.net_running) {
		ret = kernel_accept(uck_state.listen_sock, &client, 0);
		if (ret < 0) {
			if (ret != -EAGAIN && uck_state.net_running)
				pr_err("uck: accept error %d\n", ret);
			break;
		}

		/* Spawn a handler thread per connection */
		kthread_run(uck_handle_client, client, "uck_client");
	}

	pr_info("uck: listener stopped\n");
	return 0;
}

int uck_net_start_listener(u16 port)
{
	struct sockaddr_in addr;
	int ret, opt = 1;

	if (uck_state.listen_sock)
		return -EBUSY;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &uck_state.listen_sock);
	if (ret < 0) {
		pr_err("uck: failed to create listen socket: %d\n", ret);
		return ret;
	}

	/* Allow address reuse */
	sock_setsockopt(uck_state.listen_sock, SOL_SOCKET, SO_REUSEADDR,
			KERNEL_SOCKPTR(&opt), sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	ret = kernel_bind(uck_state.listen_sock, (struct sockaddr *)&addr,
			  sizeof(addr));
	if (ret < 0) {
		pr_err("uck: bind failed: %d\n", ret);
		sock_release(uck_state.listen_sock);
		uck_state.listen_sock = NULL;
		return ret;
	}

	ret = kernel_listen(uck_state.listen_sock, 16);
	if (ret < 0) {
		pr_err("uck: listen failed: %d\n", ret);
		sock_release(uck_state.listen_sock);
		uck_state.listen_sock = NULL;
		return ret;
	}

	uck_state.net_running = true;
	uck_state.listen_thread = kthread_run(uck_listener_thread, NULL,
					      "uck_listener");
	if (IS_ERR(uck_state.listen_thread)) {
		ret = PTR_ERR(uck_state.listen_thread);
		uck_state.listen_thread = NULL;
		uck_state.net_running = false;
		sock_release(uck_state.listen_sock);
		uck_state.listen_sock = NULL;
		return ret;
	}

	pr_info("uck: listening on port %u\n", port);
	return 0;
}

void uck_net_stop(void)
{
	uck_state.net_running = false;

	if (uck_state.listen_sock) {
		kernel_sock_shutdown(uck_state.listen_sock, SHUT_RDWR);
		sock_release(uck_state.listen_sock);
		uck_state.listen_sock = NULL;
	}

	if (uck_state.listen_thread) {
		kthread_stop(uck_state.listen_thread);
		uck_state.listen_thread = NULL;
	}

	/* Close all remote node connections */
	{
		int i;
		for (i = 0; i < uck_state.num_nodes; i++) {
			if (uck_state.nodes[i].sock) {
				sock_release(uck_state.nodes[i].sock);
				uck_state.nodes[i].sock = NULL;
				uck_state.nodes[i].connected = false;
			}
		}
	}
}

/* ---- Outbound: connect to a remote node ---- */

int uck_net_connect_node(struct uck_remote_node *node)
{
	struct sockaddr_in addr;
	int ret;

	if (node->connected)
		return 0;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &node->sock);
	if (ret < 0)
		return ret;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = node->info.ip_addr;
	addr.sin_port = htons(node->info.port);

	ret = kernel_connect(node->sock, (struct sockaddr *)&addr,
			     sizeof(addr), 0);
	if (ret < 0) {
		sock_release(node->sock);
		node->sock = NULL;
		return ret;
	}

	node->connected = true;
	pr_info("uck: connected to node %u\n", node->info.node_id);
	return 0;
}

/* ---- Outbound: fetch a page from a remote node ---- */

int uck_net_fetch_page(struct uck_region *region, pgoff_t page_index,
		       void *dst)
{
	struct uck_remote_node *owner = NULL;
	struct uck_msg_hdr req, resp;
	int i, ret;

	/* Find the owning node */
	for (i = 0; i < uck_state.num_nodes; i++) {
		if (uck_state.nodes[i].info.node_id ==
		    region->info.owner_node) {
			owner = &uck_state.nodes[i];
			break;
		}
	}

	if (!owner) {
		/* We might be the owner */
		if (region->info.owner_node ==
		    uck_state.local_node.node_id)
			return -ENOENT; /* Local page, just zero fill */
		pr_err("uck: owner node %u not found\n",
		       region->info.owner_node);
		return -ENOENT;
	}

	/* Connect if needed */
	if (!owner->connected) {
		ret = uck_net_connect_node(owner);
		if (ret < 0) {
			pr_err("uck: connect to node %u failed: %d\n",
			       owner->info.node_id, ret);
			return ret;
		}
	}

	/* Send page request */
	memset(&req, 0, sizeof(req));
	req.type = UCK_MSG_PAGE_REQ;
	req.src_node = uck_state.local_node.node_id;
	req.region_id = region->info.region_id;
	req.page_offset = (u64)page_index << PAGE_SHIFT;

	pr_info_ratelimited("uck: sending page req region=%llu offset=%llu to node %u\n",
			    req.region_id, req.page_offset, owner->info.node_id);

	ret = uck_sock_send(owner->sock, &req, sizeof(req));
	if (ret < 0) {
		pr_err("uck: send page req failed: %d\n", ret);
		return ret;
	}

	/* Receive response header + page data with retry logic */
	{
		int retries = 3;
		int backoff_ms = 100;

		while (retries-- > 0) {
			/* Set receive timeout */
			{
				struct __kernel_old_timeval tv;
				tv.tv_sec = 5;
				tv.tv_usec = 0;
				sock_setsockopt(owner->sock, SOL_SOCKET,
						SO_RCVTIMEO_OLD,
						KERNEL_SOCKPTR((char *)&tv),
						sizeof(tv));
			}

			ret = uck_sock_recv(owner->sock, &resp, sizeof(resp));
			if (ret >= 0)
				break;
			if (ret == -EAGAIN || ret == -ETIMEDOUT) {
				pr_warn("uck: page fetch timeout, retry %d "
					"(backoff %dms)\n",
					3 - retries, backoff_ms);
				msleep(backoff_ms);
				backoff_ms *= 2;
				continue;
			}
			/* Other errors: don't retry */
			break;
		}
	}
	if (ret < 0) {
		pr_err("uck: recv page resp hdr failed: %d\n", ret);
		return ret;
	}

	pr_info_ratelimited("uck: got page resp type=%u flags=%u payload=%u\n",
			    resp.type, resp.flags, resp.payload_len);

	if (resp.flags != 0) {
		pr_warn("uck: remote error fetching page\n");
		return -EIO;
	}

	/* Receive page data */
	if (resp.payload_len != PAGE_SIZE)
		return -EPROTO;

	ret = uck_sock_recv(owner->sock, dst, PAGE_SIZE);
	if (ret < 0)
		return ret;

	return 0;
}

/* ---- Node management ---- */

int uck_add_remote_node(struct uck_node_info *info)
{
	if (uck_state.num_nodes >= UCK_MAX_NODES)
		return -ENOMEM;

	uck_state.nodes[uck_state.num_nodes].info = *info;
	uck_state.nodes[uck_state.num_nodes].sock = NULL;
	mutex_init(&uck_state.nodes[uck_state.num_nodes].sock_lock);
	uck_state.nodes[uck_state.num_nodes].connected = false;
	uck_state.nodes[uck_state.num_nodes].alive = false;
	uck_state.nodes[uck_state.num_nodes].last_heartbeat = 0;
	memset(&uck_state.nodes[uck_state.num_nodes].stats, 0,
	       sizeof(struct uck_node_stats));
	uck_state.num_nodes++;
	return 0;
}

int uck_net_send_to_node(u32 node_id, void *buf, int len)
{
	int i, ret;

	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (node->info.node_id != node_id)
			continue;

		if (!node->connected) {
			ret = uck_net_connect_node(node);
			if (ret < 0)
				return ret;
		}

		mutex_lock(&node->sock_lock);
		ret = uck_sock_send(node->sock, buf, len);
		mutex_unlock(&node->sock_lock);
		return ret;
	}
	return -ENOENT;
}

/* Send header + payload to a specific node (locked) */
int uck_net_send_msg_to_node(u32 node_id, struct uck_msg_hdr *hdr,
			     void *payload, int payload_len)
{
	int i, ret;

	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (node->info.node_id != node_id)
			continue;

		if (!node->connected) {
			ret = uck_net_connect_node(node);
			if (ret < 0)
				return ret;
		}

		mutex_lock(&node->sock_lock);
		ret = uck_net_send_msg(node->sock, hdr, payload, payload_len);
		mutex_unlock(&node->sock_lock);
		return ret;
	}
	return -ENOENT;
}
