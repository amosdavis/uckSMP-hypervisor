/*
 * uck_ratelimit.c - Token bucket rate limiting for UCK
 *
 * Prevents DoS attacks by limiting:
 *   - Page requests per node per second
 *   - Ioctl calls per process per second
 *   - Network messages per node per second
 */

#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>

#include "uck_internal.h"

/* Per-node rate limiter */
struct uck_node_ratelimit {
	u32 node_id;
	unsigned long tokens;
	unsigned long last_refill;
	unsigned long max_tokens;
	unsigned long refill_rate;    /* tokens per second */
	spinlock_t lock;
};

static struct uck_node_ratelimit node_limits[UCK_MAX_NODES];
static int num_node_limits;

/* Module parameters for rate limits */
static unsigned int uck_page_req_rate = 1000;  /* per second per node */
static unsigned int uck_ioctl_rate = 100;       /* per second per process */

module_param(uck_page_req_rate, uint, 0644);
MODULE_PARM_DESC(uck_page_req_rate, "Max page requests per node per second");
module_param(uck_ioctl_rate, uint, 0644);
MODULE_PARM_DESC(uck_ioctl_rate, "Max ioctl calls per second");

static void uck_ratelimit_refill(struct uck_node_ratelimit *rl)
{
	unsigned long now = jiffies;
	unsigned long elapsed_ms;

	if (time_before(now, rl->last_refill))
		return;

	elapsed_ms = jiffies_to_msecs(now - rl->last_refill);
	if (elapsed_ms > 0) {
		unsigned long new_tokens = rl->refill_rate * elapsed_ms / 1000;
		rl->tokens = min(rl->tokens + new_tokens, rl->max_tokens);
		rl->last_refill = now;
	}
}

bool uck_ratelimit_check_net(u32 node_id)
{
	int i;
	unsigned long flags;

	for (i = 0; i < num_node_limits; i++) {
		if (node_limits[i].node_id != node_id)
			continue;

		spin_lock_irqsave(&node_limits[i].lock, flags);
		uck_ratelimit_refill(&node_limits[i]);

		if (node_limits[i].tokens > 0) {
			node_limits[i].tokens--;
			spin_unlock_irqrestore(&node_limits[i].lock, flags);
			return true;
		}
		spin_unlock_irqrestore(&node_limits[i].lock, flags);

		atomic_long_inc(&uck_state.err_ratelimit);
		uck_audit_log("ratelimit",
			      "rate limit exceeded for node %u", node_id);
		return false;
	}

	/* Unknown node, allow but register it */
	if (num_node_limits < UCK_MAX_NODES) {
		i = num_node_limits++;
		node_limits[i].node_id = node_id;
		node_limits[i].tokens = uck_page_req_rate;
		node_limits[i].last_refill = jiffies;
		node_limits[i].max_tokens = uck_page_req_rate;
		node_limits[i].refill_rate = uck_page_req_rate;
		spin_lock_init(&node_limits[i].lock);
	}

	return true;
}

bool uck_ratelimit_check_ioctl(void)
{
	return __ratelimit(&uck_state.ioctl_ratelimit);
}

int uck_ratelimit_init(void)
{
	int i;

	num_node_limits = 0;
	for (i = 0; i < UCK_MAX_NODES; i++) {
		memset(&node_limits[i], 0, sizeof(node_limits[i]));
		spin_lock_init(&node_limits[i].lock);
	}

	ratelimit_state_init(&uck_state.ioctl_ratelimit,
			     HZ, uck_ioctl_rate);

	pr_info("uck_ratelimit: initialized (page_rate=%u, ioctl_rate=%u)\n",
		uck_page_req_rate, uck_ioctl_rate);
	return 0;
}

void uck_ratelimit_exit(void)
{
	pr_info("uck_ratelimit: stopped\n");
}
