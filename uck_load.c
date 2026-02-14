/*
 * uck_load.c - Load balancer for process migration
 *
 * Periodically checks cluster load distribution.
 * When local load exceeds 1.5x the cluster average,
 * selects a process and migrates it to the least loaded node.
 */

#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/cred.h>

#include "uck_internal.h"

#define LOADBAL_INTERVAL_MS 5000
#define LOAD_THRESHOLD_PCT  150  /* 1.5x average */

/*
 * Find the least-loaded alive remote node.
 * Returns NULL if no suitable node found.
 */
static struct uck_remote_node *uck_find_least_loaded(void)
{
	struct uck_remote_node *best = NULL;
	u32 best_load = UINT_MAX;
	int i;

	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (!node->alive)
			continue;
		if (node->stats.load_avg < best_load) {
			best_load = node->stats.load_avg;
			best = node;
		}
	}
	return best;
}

/*
 * Find a suitable process to migrate away.
 * Picks the longest-running user process that is:
 *   - Not a kernel thread
 *   - Not PID 1 or 2
 *   - Single-threaded
 *   - Not a session leader
 *   - Not the uckd/uck_test processes (to avoid migrating infrastructure)
 */
static struct task_struct *uck_pick_migratable(void)
{
	struct task_struct *task, *best = NULL;
	u64 best_runtime = 0;

	rcu_read_lock();
	for_each_process(task) {
		if (task->pid <= 2)
			continue;
		if (task->flags & PF_KTHREAD)
			continue;
		if (task->signal->nr_threads > 1)
			continue;
		/* Skip session leaders (init, login, shells) */
		if (is_global_init(task))
			continue;
		/* Skip all system/root processes (only migrate user procs) */
		if (__kuid_val(task_uid(task)) == 0)
			continue;
		/* Skip infrastructure */
		if (strncmp(task->comm, "uck", 3) == 0)
			continue;
		if (strncmp(task->comm, "systemd", 7) == 0)
			continue;
		if (strcmp(task->comm, "bash") == 0 ||
		    strcmp(task->comm, "sh") == 0 ||
		    strcmp(task->comm, "login") == 0 ||
		    strcmp(task->comm, "getty") == 0 ||
		    strcmp(task->comm, "agetty") == 0 ||
		    strcmp(task->comm, "sshd") == 0 ||
		    strcmp(task->comm, "mosrun") == 0 ||
		    strcmp(task->comm, "mosd") == 0)
			continue;

		/* Pick by highest runtime (most work done = worth moving) */
		if (task->se.sum_exec_runtime > best_runtime) {
			best_runtime = task->se.sum_exec_runtime;
			best = task;
		}
	}
	if (best)
		get_task_struct(best);
	rcu_read_unlock();

	return best;
}

static int uck_loadbal_thread(void *data)
{
	pr_info("uck: load balancer started\n");

	while (!kthread_should_stop() && uck_state.net_running) {
		struct uck_cluster_info cinfo;
		u32 avg_load;

		msleep_interruptible(LOADBAL_INTERVAL_MS);

		if (!uck_state.loadbal_enabled)
			continue;

		uck_get_cluster_info(&cinfo);

		if (cinfo.num_nodes <= 1)
			continue;

		avg_load = uck_state.local_stats.load_avg;
		if (cinfo.num_nodes > 0) {
			u32 total_load = uck_state.local_stats.load_avg;
			int i;
			for (i = 0; i < uck_state.num_nodes; i++) {
				if (uck_state.nodes[i].alive)
					total_load +=
						uck_state.nodes[i].stats.load_avg;
			}
			avg_load = total_load / cinfo.num_nodes;
		}

		/* Check if local load exceeds threshold */
		if (avg_load == 0 ||
		    uck_state.local_stats.load_avg * 100 / avg_load <
		    LOAD_THRESHOLD_PCT)
			continue;

		pr_info("uck: load imbalance detected "
			"(local=%u, avg=%u)\n",
			uck_state.local_stats.load_avg, avg_load);

		{
			struct uck_remote_node *dest;
			struct task_struct *task;

			dest = uck_find_least_loaded();
			if (!dest)
				continue;

			task = uck_pick_migratable();
			if (!task)
				continue;

			pr_info("uck: auto-migrating pid %u (%s) "
				"to node %u\n",
				task->pid, task->comm,
				dest->info.node_id);

			uck_migrate_process(task->pid,
					    dest->info.node_id);
			put_task_struct(task);
		}
	}

	pr_info("uck: load balancer stopped\n");
	return 0;
}

int uck_loadbal_init(void)
{
	uck_state.loadbal_enabled = true;

	uck_state.loadbal_thread = kthread_run(uck_loadbal_thread,
					       NULL, "uck_loadbal");
	if (IS_ERR(uck_state.loadbal_thread)) {
		int ret = PTR_ERR(uck_state.loadbal_thread);
		uck_state.loadbal_thread = NULL;
		return ret;
	}
	return 0;
}

void uck_loadbal_stop(void)
{
	uck_state.loadbal_enabled = false;
	if (uck_state.loadbal_thread) {
		kthread_stop(uck_state.loadbal_thread);
		uck_state.loadbal_thread = NULL;
	}
}
