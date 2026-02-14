/*
 * uck_heartbeat.c - Node health monitoring and stats exchange
 *
 * Periodically sends local CPU/memory stats to all peers.
 * Receives stats from remote nodes. Detects node failures.
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/cpumask.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/stat.h>
#include <linux/crypto.h>
#include <crypto/hash.h>

#include "uck_internal.h"

/* Cluster-wide shared secret for heartbeat HMAC (set via module param) */
static char uck_cluster_secret[64] = "uck-default-secret-change-me";
module_param_string(cluster_secret, uck_cluster_secret,
		    sizeof(uck_cluster_secret), 0600);
MODULE_PARM_DESC(cluster_secret, "Shared secret for heartbeat HMAC");

static void uck_compute_hmac(const void *data, size_t len, u8 *out_hmac)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	size_t desc_size;

	tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(tfm)) {
		memset(out_hmac, 0, 32);
		return;
	}

	crypto_shash_setkey(tfm, uck_cluster_secret,
			    strlen(uck_cluster_secret));

	desc_size = sizeof(*desc) + crypto_shash_descsize(tfm);
	desc = kzalloc(desc_size, GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm);
		memset(out_hmac, 0, 32);
		return;
	}

	desc->tfm = tfm;
	crypto_shash_digest(desc, data, len, out_hmac);

	kfree(desc);
	crypto_free_shash(tfm);
}

void uck_update_local_stats(void)
{
	struct sysinfo si;

	si_meminfo(&si);

	uck_state.local_stats.node_id = uck_state.local_node.node_id;
	uck_state.local_stats.nr_cpus = num_online_cpus();
	uck_state.local_stats.total_mem = si.totalram * PAGE_SIZE;
	uck_state.local_stats.free_mem =
		(si.freeram + si.bufferram) * PAGE_SIZE;
	uck_state.local_stats.load_avg =
		(unsigned int)(avenrun[0] * 100 / FIXED_1);
	uck_state.local_stats.nr_running = num_online_cpus(); /* approximation */
	uck_state.local_stats.timestamp = jiffies;
}

static int uck_send_heartbeat_to(struct uck_remote_node *node)
{
	struct uck_msg_hdr hdr;
	struct uck_node_stats stats;
	int ret;

	if (!node->connected) {
		ret = uck_net_connect_node(node);
		if (ret < 0)
			return ret;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = UCK_MSG_HEARTBEAT;
	hdr.src_node = uck_state.local_node.node_id;
	hdr.payload_len = sizeof(stats);

	stats = uck_state.local_stats;
	uck_compute_hmac(&stats, offsetof(struct uck_node_stats, hmac),
			 stats.hmac);

	mutex_lock(&node->sock_lock);
	ret = uck_net_send_msg(node->sock, &hdr, &stats, sizeof(stats));
	mutex_unlock(&node->sock_lock);

	return ret;
}

static int uck_heartbeat_thread(void *data)
{
	int i;

	pr_info("uck: heartbeat thread started\n");

	while (!kthread_should_stop() && uck_state.net_running) {
		uck_update_local_stats();
		uck_quorum_check();
		uck_cgroup_update_local();

		for (i = 0; i < uck_state.num_nodes; i++) {
			struct uck_remote_node *node = &uck_state.nodes[i];
			uck_send_heartbeat_to(node);

			/* Also send cgroup stats */
			{
				struct uck_msg_hdr chdr;
				memset(&chdr, 0, sizeof(chdr));
				chdr.type = UCK_MSG_CGROUP_STATS;
				chdr.src_node = uck_state.local_node.node_id;
				chdr.payload_len = sizeof(struct uck_cgroup_stats);
				uck_net_send_msg_to_node(node->info.node_id,
							 &chdr,
							 &uck_state.local_cgroup_stats,
							 sizeof(uck_state.local_cgroup_stats));
			}

			/* Adaptive heartbeat failure detection */
			if (node->alive &&
			    time_after(jiffies,
				       node->last_heartbeat +
				       msecs_to_jiffies(UCK_HEARTBEAT_INTERVAL_MS))) {
				node->missed_heartbeats++;
				if (node->missed_heartbeats == 1) {
					/* First miss: mark suspect */
					node->suspect = true;
					pr_info("uck: node %u suspect "
						"(1 missed heartbeat)\n",
						node->info.node_id);
				} else if (node->missed_heartbeats >= 3) {
					/* Three consecutive: mark dead */
					pr_warn("uck: node %u declared dead "
						"(%u missed heartbeats)\n",
						node->info.node_id,
						node->missed_heartbeats);
					node->alive = false;
					node->suspect = false;
					uck_state.cluster_epoch++;
					pr_info("uck: cluster epoch incremented to %u after node failure\n",
						uck_state.cluster_epoch);
					uck_audit_log("node_failure",
						      "node %u declared dead "
						      "after %u missed heartbeats",
						      node->info.node_id,
						      node->missed_heartbeats);
				}
			}
		}

		msleep_interruptible(UCK_HEARTBEAT_INTERVAL_MS);
	}

	pr_info("uck: heartbeat thread stopped\n");
	return 0;
}

/* Called from network handler when we receive a heartbeat */
void uck_handle_heartbeat(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_node_stats stats;
	int i, ret;

	if (hdr->payload_len != sizeof(stats))
		return;

	ret = uck_sock_recv(client, &stats, sizeof(stats));
	if (ret < 0)
		return;

	/* Verify HMAC */
	{
		u8 expected_hmac[32];
		uck_compute_hmac(&stats,
				 offsetof(struct uck_node_stats, hmac),
				 expected_hmac);
		if (memcmp(stats.hmac, expected_hmac, 32) != 0) {
			pr_warn_ratelimited("uck: heartbeat HMAC mismatch "
					    "from node %u — dropped\n",
					    stats.node_id);
			atomic_long_inc(&uck_state.err_auth);
			uck_audit_log("auth",
				      "heartbeat HMAC mismatch from node %u",
				      stats.node_id);
			return;
		}
	}

	for (i = 0; i < uck_state.num_nodes; i++) {
		if (uck_state.nodes[i].info.node_id == stats.node_id) {
			uck_state.nodes[i].stats = stats;
			uck_state.nodes[i].last_heartbeat = jiffies;
			uck_state.nodes[i].alive = true;
			uck_state.nodes[i].missed_heartbeats = 0;
			uck_state.nodes[i].suspect = false;
			break;
		}
	}
}

void uck_get_cluster_info(struct uck_cluster_info *info)
{
	int i;

	uck_update_local_stats();

	memset(info, 0, sizeof(*info));
	info->num_nodes = 1; /* count self */
	info->total_cpus = uck_state.local_stats.nr_cpus;
	info->total_mem = uck_state.local_stats.total_mem;
	info->total_free_mem = uck_state.local_stats.free_mem;
	info->total_running = uck_state.local_stats.nr_running;

	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (!node->alive)
			continue;
		info->num_nodes++;
		info->total_cpus += node->stats.nr_cpus;
		info->total_mem += node->stats.total_mem;
		info->total_free_mem += node->stats.free_mem;
		info->total_running += node->stats.nr_running;
	}
}

int uck_heartbeat_init(void)
{
	uck_update_local_stats();

	uck_state.heartbeat_thread = kthread_run(uck_heartbeat_thread,
						  NULL, "uck_heartbeat");
	if (IS_ERR(uck_state.heartbeat_thread)) {
		int ret = PTR_ERR(uck_state.heartbeat_thread);
		uck_state.heartbeat_thread = NULL;
		return ret;
	}
	return 0;
}

void uck_heartbeat_stop(void)
{
	if (uck_state.heartbeat_thread) {
		kthread_stop(uck_state.heartbeat_thread);
		uck_state.heartbeat_thread = NULL;
	}
}
