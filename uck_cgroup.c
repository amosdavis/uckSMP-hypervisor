/*
 * uck_cgroup.c - Distributed cgroup-based resource accounting
 *
 * Tracks CPU time, memory usage, and task counts for UCK-distributed
 * workloads. Each node reports its local cgroup stats via heartbeat
 * messages. Aggregated stats are visible via /proc/uck/cgroup.
 *
 * Uses the kernel's cgroup subsystem to track resources for processes
 * spawned by UCK (exec engine, fork distribution).
 */

#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/cputime.h>
#include <linux/mm.h>

#include "uck_internal.h"

/*
 * Update local cgroup statistics by scanning processes.
 * In a production system this would hook into actual cgroup controllers;
 * here we approximate by scanning runnable user tasks.
 */
int uck_cgroup_update_local(void)
{
	struct task_struct *task;
	u32 nr_tasks = 0;
	u64 cpu_usage = 0;
	u64 mem_usage = 0;

	rcu_read_lock();
	for_each_process(task) {
		struct mm_struct *mm;

		if (task->flags & PF_KTHREAD)
			continue;
		if (task->pid <= 2)
			continue;
		/* Count non-kernel tasks as part of the UCK workload */
		if (__kuid_val(task_uid(task)) == 0) {
			/* Only count root tasks that are UCK infrastructure */
			if (strncmp(task->comm, "uck", 3) != 0)
				continue;
		}

		nr_tasks++;
		cpu_usage += task->se.sum_exec_runtime;

		mm = task->mm;
		if (mm) {
			mem_usage += get_mm_counter(mm, MM_FILEPAGES) * PAGE_SIZE;
			mem_usage += get_mm_counter(mm, MM_ANONPAGES) * PAGE_SIZE;
		}
	}
	rcu_read_unlock();

	/* Enforce task limit per node */
	if (uck_state.max_tasks_per_node > 0 &&
	    nr_tasks >= uck_state.max_tasks_per_node) {
		pr_warn("uck: task limit reached (%u/%u)\n",
			nr_tasks, uck_state.max_tasks_per_node);
		return -ENOSPC;
	}

	/* Enforce page limit */
	if (!uck_memctl_can_alloc(mem_usage / PAGE_SIZE)) {
		pr_warn("uck: memory limit reached\n");
		return -ENOMEM;
	}

	uck_state.local_cgroup_stats.node_id =
		uck_state.local_node.node_id;
	uck_state.local_cgroup_stats.nr_tasks = nr_tasks;
	uck_state.local_cgroup_stats.cpu_usage_ns = cpu_usage;
	uck_state.local_cgroup_stats.mem_usage = mem_usage;
	uck_state.local_cgroup_stats.mem_limit = 0; /* unlimited */

	return 0;
}

/*
 * Handle incoming UCK_MSG_CGROUP_STATS from a remote node.
 */
void uck_handle_cgroup_stats(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_cgroup_stats stats;
	int i, ret;

	if (hdr->payload_len != sizeof(stats))
		return;

	ret = uck_sock_recv(client, &stats, sizeof(stats));
	if (ret < 0)
		return;

	for (i = 0; i < uck_state.num_nodes; i++) {
		if (uck_state.nodes[i].info.node_id == stats.node_id) {
			uck_state.nodes[i].cgroup_stats = stats;
			break;
		}
	}
}

/* /proc/uck/cgroup */
static int uck_proc_cgroup_show(struct seq_file *m, void *v)
{
	u64 total_cpu = 0, total_mem = 0;
	u32 total_tasks = 0;
	int i;

	uck_cgroup_update_local();

	seq_printf(m, "%-6s %-8s %-16s %-16s\n",
		   "NODE", "TASKS", "CPU_MS", "MEM_MB");

	/* Local */
	seq_printf(m, "%-6u %-8u %-16llu %-16llu\n",
		   uck_state.local_cgroup_stats.node_id,
		   uck_state.local_cgroup_stats.nr_tasks,
		   uck_state.local_cgroup_stats.cpu_usage_ns / 1000000ULL,
		   uck_state.local_cgroup_stats.mem_usage / (1024 * 1024));
	total_tasks += uck_state.local_cgroup_stats.nr_tasks;
	total_cpu += uck_state.local_cgroup_stats.cpu_usage_ns;
	total_mem += uck_state.local_cgroup_stats.mem_usage;

	/* Remote */
	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (!node->alive)
			continue;
		seq_printf(m, "%-6u %-8u %-16llu %-16llu\n",
			   node->cgroup_stats.node_id,
			   node->cgroup_stats.nr_tasks,
			   node->cgroup_stats.cpu_usage_ns / 1000000ULL,
			   node->cgroup_stats.mem_usage / (1024 * 1024));
		total_tasks += node->cgroup_stats.nr_tasks;
		total_cpu += node->cgroup_stats.cpu_usage_ns;
		total_mem += node->cgroup_stats.mem_usage;
	}

	seq_printf(m, "--- TOTAL ---\n");
	seq_printf(m, "%-6s %-8u %-16llu %-16llu\n",
		   "all", total_tasks,
		   total_cpu / 1000000ULL,
		   total_mem / (1024 * 1024));

	return 0;
}

static int uck_proc_cgroup_open(struct inode *inode, struct file *file)
{
	return single_open(file, uck_proc_cgroup_show, NULL);
}

static const struct proc_ops uck_proc_cgroup_ops = {
	.proc_open    = uck_proc_cgroup_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

int uck_cgroup_init(void)
{
	if (uck_state.proc_dir) {
		uck_state.proc_cgroup = proc_create("cgroup", 0444,
						     uck_state.proc_dir,
						     &uck_proc_cgroup_ops);
	}
	uck_cgroup_update_local();
	pr_info("uck: cgroup resource accounting initialized\n");
	return 0;
}

void uck_cgroup_exit(void)
{
	if (uck_state.proc_cgroup)
		proc_remove(uck_state.proc_cgroup);
	pr_info("uck: cgroup resource accounting stopped\n");
}
