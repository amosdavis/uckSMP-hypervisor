/*
 * uck_join.c - Dynamic node join/leave protocol
 *
 * Handles:
 *   - New nodes joining the cluster at runtime
 *   - Graceful node departure with page migration
 *   - Node failure detection and cleanup
 *
 * Join protocol:
 *   1. New node sends UCK_MSG_NODE_ANNOUNCE to any existing node
 *   2. Existing node adds new node, broadcasts to all other nodes
 *   3. Each node sends UCK_MSG_NODE_ANNOUNCE_ACK back
 *   4. New node receives cluster membership list
 *
 * Leave protocol:
 *   1. Leaving node sends UCK_MSG_NODE_LEAVE to all peers
 *   2. Each peer migrates any pages owned by leaving node
 *   3. Peers send UCK_MSG_NODE_LEAVE_ACK
 *   4. Leaving node shuts down UCK
 */

#include <linux/slab.h>
#include <linux/delay.h>

#include "uck_internal.h"

/*
 * Handle incoming UCK_MSG_NODE_ANNOUNCE from a new node.
 * Add it to our node list and broadcast to other nodes.
 */
void uck_handle_node_announce(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_node_announce ann;
	struct uck_msg_hdr resp_hdr;
	struct uck_node_announce resp_ann;
	struct uck_node_info new_info;
	int ret, i;
	bool already_known = false;

	if (hdr->payload_len != sizeof(ann))
		return;

	ret = uck_sock_recv(client, &ann, sizeof(ann));
	if (ret < 0)
		return;

	new_info = ann.info;

	/* Epoch validation: returning nodes must have current epoch */
	if (ann.epoch != 0 && ann.epoch < uck_state.cluster_epoch) {
		pr_warn("uck: rejecting stale node %u with epoch %u "
			"(current epoch %u)\n",
			new_info.node_id, ann.epoch,
			uck_state.cluster_epoch);
		uck_audit_log("node_join",
			      "rejected stale node %u epoch %u (current %u)",
			      new_info.node_id, ann.epoch,
			      uck_state.cluster_epoch);
		return;
	}

	/* Check if we already know this node */
	mutex_lock(&uck_state.lock);
	for (i = 0; i < uck_state.num_nodes; i++) {
		if (uck_state.nodes[i].info.node_id == new_info.node_id) {
			already_known = true;
			/* Update its info in case IP/port changed */
			uck_state.nodes[i].info = new_info;
			uck_state.nodes[i].stats = ann.stats;
			uck_state.nodes[i].alive = true;
			uck_state.nodes[i].last_heartbeat = jiffies;
			break;
		}
	}

	if (!already_known) {
		ret = uck_add_remote_node(&new_info);
		if (ret == 0) {
			int idx = uck_state.num_nodes - 1;
			uck_state.nodes[idx].stats = ann.stats;
			uck_state.nodes[idx].alive = true;
			uck_state.nodes[idx].last_heartbeat = jiffies;
		}
	}
	mutex_unlock(&uck_state.lock);

	pr_info("uck: node %u announced (ip=0x%08x port=%u) %s\n",
		new_info.node_id, new_info.ip_addr, new_info.port,
		already_known ? "(known)" : "(new)");

	/* Send our own info as ACK */
	memset(&resp_hdr, 0, sizeof(resp_hdr));
	resp_hdr.type = UCK_MSG_NODE_ANNOUNCE_ACK;
	resp_hdr.src_node = uck_state.local_node.node_id;
	resp_hdr.payload_len = sizeof(resp_ann);

	memset(&resp_ann, 0, sizeof(resp_ann));
	resp_ann.info = uck_state.local_node;
	resp_ann.stats = uck_state.local_stats;

	uck_net_send_msg(client, &resp_hdr, &resp_ann, sizeof(resp_ann));

	/* Broadcast the new node to all other peers (if this is new) */
	if (!already_known) {
		struct uck_msg_hdr bcast_hdr;

		memset(&bcast_hdr, 0, sizeof(bcast_hdr));
		bcast_hdr.type = UCK_MSG_NODE_ANNOUNCE;
		bcast_hdr.src_node = uck_state.local_node.node_id;
		bcast_hdr.payload_len = sizeof(ann);

		for (i = 0; i < uck_state.num_nodes; i++) {
			struct uck_remote_node *peer = &uck_state.nodes[i];
			if (peer->info.node_id == new_info.node_id)
				continue;
			if (!peer->alive)
				continue;
			uck_net_send_msg_to_node(peer->info.node_id,
						 &bcast_hdr,
						 &ann, sizeof(ann));
		}
	}

	uck_audit_log("node_join", "node %u joined at 0x%08x:%u",
		      new_info.node_id, new_info.ip_addr, new_info.port);
}

/*
 * Handle incoming UCK_MSG_NODE_LEAVE from a departing node.
 * Mark it as dead and invalidate pages it owned.
 */
void uck_handle_node_leave(struct socket *client, struct uck_msg_hdr *hdr)
{
	u32 leaving_node = hdr->src_node;
	struct uck_msg_hdr ack;
	int i;

	pr_info("uck: node %u leaving cluster\n", leaving_node);

	mutex_lock(&uck_state.lock);
	for (i = 0; i < uck_state.num_nodes; i++) {
		if (uck_state.nodes[i].info.node_id == leaving_node) {
			uck_state.nodes[i].alive = false;
			uck_state.nodes[i].connected = false;
			if (uck_state.nodes[i].sock) {
				/* Don't release — we're using it */
				uck_state.nodes[i].sock = NULL;
			}
			break;
		}
	}

	/* Invalidate pages owned by the leaving node */
	for (i = 0; i < uck_state.num_regions; i++) {
		struct uck_region *region = &uck_state.regions[i];
		struct rb_node *n;

		if (!region->active)
			continue;
		if (region->info.owner_node != leaving_node)
			continue;

		mutex_lock(&region->lock);
		for (n = rb_first(&region->pages); n; n = rb_next(n)) {
			struct uck_page_entry *entry =
				container_of(n, struct uck_page_entry, rb_node);
			/* Pages from this owner are now stale */
			if (entry->owner_node == leaving_node)
				atomic_set(&entry->state, UCK_PAGE_INVALID);
		}
		mutex_unlock(&region->lock);
	}
	mutex_unlock(&uck_state.lock);

	uck_state.cluster_epoch++;
	uck_audit_log("node_leave", "node %u left, epoch now %u",
		      leaving_node, uck_state.cluster_epoch);

	/* Send ACK */
	memset(&ack, 0, sizeof(ack));
	ack.type = UCK_MSG_NODE_LEAVE_ACK;
	ack.src_node = uck_state.local_node.node_id;
	uck_sock_send(client, &ack, sizeof(ack));
}

/*
 * Initiate graceful departure from the cluster.
 * Notifies all peers so they can invalidate our pages.
 */
int uck_node_leave_graceful(void)
{
	struct uck_msg_hdr hdr;
	int i;

	pr_info("uck: initiating graceful leave\n");

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = UCK_MSG_NODE_LEAVE;
	hdr.src_node = uck_state.local_node.node_id;
	hdr.payload_len = 0;

	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (!node->alive)
			continue;
		uck_net_send_to_node(node->info.node_id, &hdr, sizeof(hdr));
	}

	/* Wait briefly for ACKs */
	msleep(500);

	pr_info("uck: graceful leave complete\n");
	return 0;
}
