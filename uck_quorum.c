/*
 * uck_quorum.c - Majority quorum for split-brain prevention
 *
 * Implements a simple majority quorum: a partition must have >50%
 * of configured nodes to remain in full operational mode. Nodes
 * in the minority partition enter degraded (read-only) mode.
 *
 * Also manages cluster epochs (generation numbers) for fencing
 * stale nodes that return after being declared dead.
 */

#include <linux/slab.h>
#include <linux/moduleparam.h>

#include "uck_internal.h"

/* Module parameter: minimum quorum percentage (default 50%) */
static unsigned int uck_quorum_pct = 50;
module_param(uck_quorum_pct, uint, 0644);
MODULE_PARM_DESC(uck_quorum_pct, "Minimum percentage of nodes for quorum");

void uck_quorum_check(void)
{
	int total_configured;
	int alive_count = 1;  /* count self */
	int i;

	total_configured = uck_state.num_nodes + 1;  /* remotes + self */
	if (total_configured <= 1) {
		uck_state.degraded = false;
		return;
	}

	for (i = 0; i < uck_state.num_nodes; i++) {
		if (uck_state.nodes[i].alive)
			alive_count++;
	}

	uck_state.quorum_threshold = (total_configured / 2) + 1;

	if (alive_count < uck_state.quorum_threshold) {
		if (!uck_state.degraded) {
			uck_state.degraded = true;
			uck_audit_log("quorum",
				      "lost quorum: %d/%d alive (need %d)",
				      alive_count, total_configured,
				      uck_state.quorum_threshold);
			pr_warn("uck: DEGRADED MODE - lost quorum "
				"(%d/%d nodes alive, need %d)\n",
				alive_count, total_configured,
				uck_state.quorum_threshold);
		}
	} else {
		if (uck_state.degraded) {
			uck_state.degraded = false;
			uck_audit_log("quorum",
				      "quorum restored: %d/%d alive",
				      alive_count, total_configured);
			pr_info("uck: quorum restored (%d/%d nodes alive)\n",
				alive_count, total_configured);
		}
	}
}

bool uck_quorum_is_degraded(void)
{
	return uck_state.degraded;
}

/*
 * Increment cluster epoch (called when a node is declared dead).
 * All future messages must carry the new epoch.
 */
void uck_epoch_increment(void)
{
	uck_state.cluster_epoch++;
	uck_audit_log("epoch", "cluster epoch incremented to %u",
		      uck_state.cluster_epoch);
	pr_info("uck: cluster epoch now %u\n", uck_state.cluster_epoch);
}

/*
 * Validate that an incoming message epoch matches our current epoch.
 * Returns true if valid, false if stale.
 */
bool uck_epoch_validate(u32 msg_epoch)
{
	if (msg_epoch < uck_state.cluster_epoch) {
		uck_audit_log("epoch",
			      "rejected stale message with epoch %u "
			      "(current %u)",
			      msg_epoch, uck_state.cluster_epoch);
		return false;
	}
	return true;
}

int uck_quorum_init(void)
{
	uck_state.cluster_epoch = 1;
	uck_state.degraded = false;
	uck_state.quorum_threshold = 1;
	pr_info("uck_quorum: initialized (quorum_pct=%u%%)\n", uck_quorum_pct);
	return 0;
}

void uck_quorum_exit(void)
{
	pr_info("uck_quorum: stopped\n");
}
