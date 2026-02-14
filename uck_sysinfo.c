/*
 * uck_sysinfo.c - Procfs entries for cluster resource reporting
 *
 * Creates /proc/uck/ with:
 *   nodes    - per-node status (ID, CPUs, memory, load, alive)
 *   cpuinfo  - aggregated CPU count across cluster
 *   meminfo  - aggregated memory across cluster
 *   status   - one-line cluster summary
 */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mm.h>

#include "uck_internal.h"

/* /proc/uck/nodes */
static int uck_proc_nodes_show(struct seq_file *m, void *v)
{
	int i;

	uck_update_local_stats();

	seq_printf(m, "%-6s %-6s %-12s %-12s %-8s %-8s %-6s\n",
		   "NODE", "CPUs", "TOTAL_MEM", "FREE_MEM",
		   "LOAD", "RUNNING", "ALIVE");

	/* Local node */
	seq_printf(m, "%-6u %-6u %-12llu %-12llu %-8u %-8u %-6s\n",
		   uck_state.local_stats.node_id,
		   uck_state.local_stats.nr_cpus,
		   uck_state.local_stats.total_mem / 1024,
		   uck_state.local_stats.free_mem / 1024,
		   uck_state.local_stats.load_avg,
		   uck_state.local_stats.nr_running,
		   "local");

	/* Remote nodes */
	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		seq_printf(m, "%-6u %-6u %-12llu %-12llu %-8u %-8u %-6s\n",
			   node->stats.node_id,
			   node->stats.nr_cpus,
			   node->stats.total_mem / 1024,
			   node->stats.free_mem / 1024,
			   node->stats.load_avg,
			   node->stats.nr_running,
			   node->alive ? "yes" : "no");
	}

	return 0;
}

static int uck_proc_nodes_open(struct inode *inode, struct file *file)
{
	return single_open(file, uck_proc_nodes_show, NULL);
}

static const struct proc_ops uck_proc_nodes_ops = {
	.proc_open    = uck_proc_nodes_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* /proc/uck/cpuinfo */
static int uck_proc_cpuinfo_show(struct seq_file *m, void *v)
{
	struct uck_cluster_info info;

	uck_get_cluster_info(&info);

	seq_printf(m, "Cluster CPUs:    %u\n", info.total_cpus);
	seq_printf(m, "Active Nodes:    %u\n", info.num_nodes);
	seq_printf(m, "Total Running:   %u\n", info.total_running);

	return 0;
}

static int uck_proc_cpuinfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, uck_proc_cpuinfo_show, NULL);
}

static const struct proc_ops uck_proc_cpuinfo_ops = {
	.proc_open    = uck_proc_cpuinfo_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* /proc/uck/meminfo */
static int uck_proc_meminfo_show(struct seq_file *m, void *v)
{
	struct uck_cluster_info info;

	uck_get_cluster_info(&info);

	seq_printf(m, "ClusterTotal:  %8llu kB\n", info.total_mem / 1024);
	seq_printf(m, "ClusterFree:   %8llu kB\n", info.total_free_mem / 1024);
	seq_printf(m, "ClusterNodes:  %8u\n", info.num_nodes);

	return 0;
}

static int uck_proc_meminfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, uck_proc_meminfo_show, NULL);
}

static const struct proc_ops uck_proc_meminfo_ops = {
	.proc_open    = uck_proc_meminfo_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* /proc/uck/status */
static int uck_proc_status_show(struct seq_file *m, void *v)
{
	struct uck_cluster_info info;

	uck_get_cluster_info(&info);

	seq_printf(m, "UCK Cluster: %u nodes, %u CPUs, %llu MB total, "
		   "%llu MB free, %u running\n",
		   info.num_nodes, info.total_cpus,
		   info.total_mem / (1024 * 1024),
		   info.total_free_mem / (1024 * 1024),
		   info.total_running);

	return 0;
}

static int uck_proc_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, uck_proc_status_show, NULL);
}

static const struct proc_ops uck_proc_status_ops = {
	.proc_open    = uck_proc_status_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* /proc/uck/jobs */
static int uck_proc_jobs_show(struct seq_file *m, void *v)
{
	static const char *state_names[] = {
		"empty", "pending", "running", "done", "failed"
	};
	int i;

	seq_printf(m, "%-6s %-6s %-8s %-6s %-30s\n",
		   "JOB", "NODE", "STATE", "EXIT", "COMMAND");

	mutex_lock(&uck_state.jobs_lock);
	for (i = 0; i < UCK_MAX_JOBS; i++) {
		struct uck_job_info *j = &uck_state.jobs[i];
		if (j->state == UCK_JOB_EMPTY)
			continue;
		seq_printf(m, "%-6u %-6u %-8s %-6d %.30s\n",
			   j->job_id, j->node_id,
			   (j->state <= UCK_JOB_FAILED) ?
				state_names[j->state] : "???",
			   j->exit_code, j->command);
	}
	mutex_unlock(&uck_state.jobs_lock);

	return 0;
}

static int uck_proc_jobs_open(struct inode *inode, struct file *file)
{
	return single_open(file, uck_proc_jobs_show, NULL);
}

static const struct proc_ops uck_proc_jobs_ops = {
	.proc_open    = uck_proc_jobs_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* /proc/uck/latency - Page fetch latency histogram */
static int uck_latency_show(struct seq_file *m, void *v)
{
	int i;

	seq_puts(m, "Page fetch latency histogram (microseconds):\n");
	seq_puts(m, "  Bucket         Count\n");
	for (i = 0; i < 8; i++) {
		unsigned long bucket_us = 1UL << (i + 4);  /* 16, 32, ... */
		seq_printf(m, "  <%5lu us    %8lu\n", bucket_us,
			   atomic_long_read(&uck_state.latency_hist[i]));
	}
	return 0;
}

/* /proc/uck/pages - Page state distribution */
static int uck_pages_show(struct seq_file *m, void *v)
{
	seq_printf(m, "Page states:\n");
	seq_printf(m, "  Total allocated:  %lu\n",
		   atomic_long_read(&uck_state.total_pages));
	seq_printf(m, "  Shared:           %lu\n",
		   atomic_long_read(&uck_state.pages_shared));
	seq_printf(m, "  Exclusive:        %lu\n",
		   atomic_long_read(&uck_state.pages_exclusive));
	seq_printf(m, "  In-transit:       %lu\n",
		   atomic_long_read(&uck_state.pages_transit));
	return 0;
}

/* /proc/uck/errors - Error counters */
static int uck_errors_show(struct seq_file *m, void *v)
{
	seq_printf(m, "Error counters:\n");
	seq_printf(m, "  auth:       %lu\n",
		   atomic_long_read(&uck_state.err_auth));
	seq_printf(m, "  migrate:    %lu\n",
		   atomic_long_read(&uck_state.err_migrate));
	seq_printf(m, "  ratelimit:  %lu\n",
		   atomic_long_read(&uck_state.err_ratelimit));
	seq_printf(m, "  futex:      %lu\n",
		   atomic_long_read(&uck_state.err_futex));
	seq_printf(m, "  page_fault: %lu\n",
		   atomic_long_read(&uck_state.err_page_fault));
	return 0;
}

/* /proc/uck/config - Configuration dump */
static int uck_config_show(struct seq_file *m, void *v)
{
	seq_printf(m, "Configuration:\n");
	seq_printf(m, "  protocol_version: %u\n", UCK_PROTOCOL_VERSION);
	seq_printf(m, "  cluster_epoch:    %u\n", uck_state.cluster_epoch);
	seq_printf(m, "  quorum_state:     %s\n",
		   uck_state.quorum_active ? "active" : "degraded");
	seq_printf(m, "  max_tasks/node:   %u\n",
		   uck_state.max_tasks_per_node);
	seq_printf(m, "  max_pages:        %llu\n",
		   uck_state.max_pages);
	return 0;
}

int uck_sysinfo_init(void)
{
	uck_state.proc_dir = proc_mkdir("uck", NULL);
	if (!uck_state.proc_dir)
		return -ENOMEM;

	uck_state.proc_nodes = proc_create("nodes", 0444,
					    uck_state.proc_dir,
					    &uck_proc_nodes_ops);
	uck_state.proc_cpuinfo = proc_create("cpuinfo", 0444,
					      uck_state.proc_dir,
					      &uck_proc_cpuinfo_ops);
	uck_state.proc_meminfo = proc_create("meminfo", 0444,
					      uck_state.proc_dir,
					      &uck_proc_meminfo_ops);
	uck_state.proc_status = proc_create("status", 0444,
					     uck_state.proc_dir,
					     &uck_proc_status_ops);
	uck_state.proc_jobs = proc_create("jobs", 0444,
					   uck_state.proc_dir,
					   &uck_proc_jobs_ops);

	proc_create_single("latency", 0444, uck_state.proc_dir, uck_latency_show);
	proc_create_single("pages", 0444, uck_state.proc_dir, uck_pages_show);
	proc_create_single("errors", 0444, uck_state.proc_dir, uck_errors_show);
	proc_create_single("config", 0444, uck_state.proc_dir, uck_config_show);

	pr_info("uck: /proc/uck created\n");
	return 0;
}

void uck_sysinfo_exit(void)
{
	remove_proc_entry("latency", uck_state.proc_dir);
	remove_proc_entry("pages", uck_state.proc_dir);
	remove_proc_entry("errors", uck_state.proc_dir);
	remove_proc_entry("config", uck_state.proc_dir);

	if (uck_state.proc_jobs)
		proc_remove(uck_state.proc_jobs);
	if (uck_state.proc_status)
		proc_remove(uck_state.proc_status);
	if (uck_state.proc_meminfo)
		proc_remove(uck_state.proc_meminfo);
	if (uck_state.proc_cpuinfo)
		proc_remove(uck_state.proc_cpuinfo);
	if (uck_state.proc_nodes)
		proc_remove(uck_state.proc_nodes);
	if (uck_state.proc_dir)
		proc_remove(uck_state.proc_dir);
}
