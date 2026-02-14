/*
 * uck_hyper.c - UCK Hypervisor Layer: Transparent SMP across nodes
 *
 * Provides true transparent CPU sharing by intercepting process
 * creation syscalls (fork/clone) and distributing child processes
 * across cluster nodes. Combined with UCK's shared memory (DSM),
 * this gives the appearance of a single SMP system.
 *
 * Architecture:
 *   1. Processes register with UCK via ioctl (UCK_IOC_ENABLE_SMP)
 *   2. When a registered process calls fork()/clone(), the kretprobe
 *      intercepts the return and may migrate the child to a remote node
 *   3. Remote node receives the child's binary + args and spawns it
 *   4. Both parent and child access shared memory through the DSM layer
 *   5. Exit status is propagated back to the parent node
 *
 * This effectively turns the cluster into an SMP machine where:
 *   - fork() can produce children on any node
 *   - All children share memory via DSM
 *   - CPU count = sum of all node CPUs
 *   - Memory = sum of all node RAM (with DSM coherence)
 *
 * Targeting Linux 6.1 LTS (Debian Bookworm)
 */

#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/umh.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "uck_internal.h"

/*
 * Tracked processes: processes that have opted into cluster-wide SMP.
 * When these processes fork, children may be placed on remote nodes.
 */
#define UCK_TRACKED_HASH_BITS 8
static DEFINE_HASHTABLE(uck_tracked_pids, UCK_TRACKED_HASH_BITS);
static DEFINE_SPINLOCK(uck_tracked_lock);

struct uck_tracked_proc {
	struct hlist_node node;
	pid_t pid;
	pid_t tgid;
	u32 flags;
	int fork_count;         /* Number of forks from this process */
};

/* Round-robin node index for distributing forks */
static atomic_t uck_rr_index = ATOMIC_INIT(0);

static struct uck_tracked_proc *uck_find_tracked(pid_t pid)
{
	struct uck_tracked_proc *tp;

	hash_for_each_possible(uck_tracked_pids, tp, node, pid) {
		if (tp->pid == pid)
			return tp;
	}
	return NULL;
}

/*
 * Register a process for SMP mode.
 * All future fork/clone calls from this process (and its children)
 * will be candidates for remote execution.
 */
int uck_hyper_register_pid(pid_t pid)
{
	struct uck_tracked_proc *tp;
	unsigned long flags;

	spin_lock_irqsave(&uck_tracked_lock, flags);
	tp = uck_find_tracked(pid);
	if (tp) {
		spin_unlock_irqrestore(&uck_tracked_lock, flags);
		return 0; /* already registered */
	}

	tp = kzalloc(sizeof(*tp), GFP_ATOMIC);
	if (!tp) {
		spin_unlock_irqrestore(&uck_tracked_lock, flags);
		return -ENOMEM;
	}

	tp->pid = pid;
	tp->tgid = pid;
	tp->fork_count = 0;
	hash_add(uck_tracked_pids, &tp->node, pid);
	spin_unlock_irqrestore(&uck_tracked_lock, flags);

	pr_info("uck_hyper: registered pid %d for SMP distribution\n", pid);
	return 0;
}

void uck_hyper_unregister_pid(pid_t pid)
{
	struct uck_tracked_proc *tp;
	unsigned long flags;

	spin_lock_irqsave(&uck_tracked_lock, flags);
	tp = uck_find_tracked(pid);
	if (tp) {
		hash_del(&tp->node);
		kfree(tp);
	}
	spin_unlock_irqrestore(&uck_tracked_lock, flags);
}

/*
 * Pick the next node for fork distribution.
 * Uses weighted fair scheduling based on multi-factor scoring.
 * Returns 0 if we should keep the child local.
 */
static u32 uck_hyper_pick_node(void)
{
	u32 target_node;

	if (uck_state.num_nodes == 0)
		return 0; /* No remote nodes, keep local */

	/* Weighted fair scheduling: score = cpu_count * 100 - current_load * 50
	 * Higher score = better target for fork distribution */
	{
		int best_node = -1;
		int best_score = -1;
		int i;

		for (i = 0; i < uck_state.num_nodes; i++) {
			struct uck_remote_node *node = &uck_state.nodes[i];
			int score;

			if (!node->alive || node->suspect)
				continue;

			/* Multi-factor scoring */
			score = node->stats.nr_cpus * 100;
			score -= node->stats.load_avg * 50;
			score -= node->stats.nr_running * 10;

			if (score > best_score) {
				best_score = score;
				best_node = i;
			}
		}

		if (best_node < 0) {
			pr_warn("uck: no suitable node for fork distribution\n");
			return 0;
		}

		/* Check resource limits on target node */
		{
			struct uck_remote_node *target = &uck_state.nodes[best_node];
			if (target->cgroup_stats.nr_tasks >= uck_state.max_tasks_per_node) {
				pr_info("uck: node %u at task limit (%u), "
					"keeping fork local\n",
					target->info.node_id,
					uck_state.max_tasks_per_node);
				return 0;
			}
		}

		target_node = uck_state.nodes[best_node].info.node_id;
	}

	return target_node;
}

/*
 * Check if a child process should be migrated to a remote node.
 * Called from the fork kretprobe handler.
 *
 * Policy:
 *   - Parent must be registered for SMP
 *   - Child inherits SMP registration
 *   - Alternate children across nodes (round-robin)
 *   - First child always stays local (to avoid startup latency)
 */
static bool uck_hyper_should_distribute(pid_t parent_pid, pid_t child_pid)
{
	struct uck_tracked_proc *tp;
	unsigned long flags;
	bool distribute = false;

	if (!uck_state.initialized || uck_state.num_nodes == 0)
		return false;

	spin_lock_irqsave(&uck_tracked_lock, flags);
	tp = uck_find_tracked(parent_pid);
	if (!tp) {
		/* Check if parent's tgid is tracked (thread group leader) */
		struct task_struct *task;
		rcu_read_lock();
		{
			struct pid *p = find_vpid(parent_pid);
			task = p ? pid_task(p, PIDTYPE_PID) : NULL;
		}
		if (task)
			tp = uck_find_tracked(task->tgid);
		rcu_read_unlock();
	}

	if (tp) {
		tp->fork_count++;

		/* Register the child too */
		struct uck_tracked_proc *child_tp;
		child_tp = kzalloc(sizeof(*child_tp), GFP_ATOMIC);
		if (child_tp) {
			child_tp->pid = child_pid;
			child_tp->tgid = child_pid;
			hash_add(uck_tracked_pids, &child_tp->node, child_pid);
		}

		/* First fork stays local, subsequent ones get distributed */
		if (tp->fork_count > 1)
			distribute = true;
	}
	spin_unlock_irqrestore(&uck_tracked_lock, flags);

	return distribute;
}

/*
 * kretprobe handler for kernel_clone / _do_fork.
 * Fires after a fork/clone completes. If the parent is tracked,
 * we may migrate the child to a remote node.
 *
 * On kernel 6.1, the function is kernel_clone().
 */

struct uck_fork_ctx {
	pid_t parent_pid;
};

static int uck_fork_entry(struct kretprobe_instance *ri,
			   struct pt_regs *regs)
{
	struct uck_fork_ctx *ctx;

	ctx = (struct uck_fork_ctx *)ri->data;
	ctx->parent_pid = current->pid;
	return 0;
}

static int uck_fork_handler(struct kretprobe_instance *ri,
			     struct pt_regs *regs)
{
	struct uck_fork_ctx *ctx;
	long child_pid;
	u32 dest_node;

	ctx = (struct uck_fork_ctx *)ri->data;
	child_pid = regs_return_value(regs);

	/* child_pid < 0 means fork failed, == 0 means we're in child */
	if (child_pid <= 0)
		return 0;

	if (!uck_hyper_should_distribute(ctx->parent_pid, child_pid))
		return 0;

	dest_node = uck_hyper_pick_node();
	if (dest_node == 0 || dest_node == uck_state.local_node.node_id)
		return 0;

	pr_info("uck_hyper: distributing child pid %ld from parent %d to node %u\n",
		child_pid, ctx->parent_pid, dest_node);

	/* Schedule the migration asynchronously (can't block in probe) */
	uck_hyper_schedule_migration(child_pid, dest_node);

	return 0;
}

static struct kretprobe uck_fork_probe = {
	.handler = uck_fork_handler,
	.entry_handler = uck_fork_entry,
	.data_size = sizeof(struct uck_fork_ctx),
	.maxactive = 32,
};

/*
 * Asynchronous migration work.
 * We can't call uck_migrate_process from a kprobe context,
 * so we queue it to a workqueue.
 */
struct uck_migration_work {
	struct work_struct work;
	pid_t child_pid;
	u32 dest_node;
};

static struct workqueue_struct *uck_hyper_wq;

static void uck_hyper_do_migrate(struct work_struct *work)
{
	struct uck_migration_work *mw =
		container_of(work, struct uck_migration_work, work);

	/* Give the child a moment to initialize */
	msleep(50);

	pr_info("uck_hyper: migrating pid %d to node %u\n",
		mw->child_pid, mw->dest_node);

	uck_migrate_process(mw->child_pid, mw->dest_node);
	kfree(mw);
}

void uck_hyper_schedule_migration(pid_t child_pid, u32 dest_node)
{
	struct uck_migration_work *mw;

	mw = kzalloc(sizeof(*mw), GFP_ATOMIC);
	if (!mw)
		return;

	mw->child_pid = child_pid;
	mw->dest_node = dest_node;
	INIT_WORK(&mw->work, uck_hyper_do_migrate);

	if (uck_hyper_wq)
		queue_work(uck_hyper_wq, &mw->work);
	else
		kfree(mw);
}

/*
 * Process exit handler: unregister tracked processes when they die.
 */
static int uck_exit_entry(struct kretprobe_instance *ri,
			   struct pt_regs *regs)
{
	uck_hyper_unregister_pid(current->pid);
	return 0;
}

static int uck_exit_handler(struct kretprobe_instance *ri,
			     struct pt_regs *regs)
{
	return 0;
}

static struct kretprobe uck_exit_probe = {
	.handler = uck_exit_handler,
	.entry_handler = uck_exit_entry,
	.data_size = 0,
	.maxactive = 32,
};

/*
 * Initialize the hypervisor layer.
 */
int uck_hyper_init(void)
{
	int ret;

	uck_hyper_wq = alloc_workqueue("uck_hyper", WQ_UNBOUND, 4);
	if (!uck_hyper_wq) {
		pr_err("uck_hyper: failed to create workqueue\n");
		return -ENOMEM;
	}

	/* Hook kernel_clone for fork/clone interception */
	uck_fork_probe.kp.symbol_name = "kernel_clone";
	ret = register_kretprobe(&uck_fork_probe);
	if (ret < 0) {
		pr_warn("uck_hyper: kernel_clone probe failed (%d), "
			"trying _do_fork\n", ret);
		/* Fallback for older kernels */
		uck_fork_probe.kp.symbol_name = "_do_fork";
		ret = register_kretprobe(&uck_fork_probe);
		if (ret < 0) {
			pr_err("uck_hyper: fork probe registration failed: %d\n",
			       ret);
			destroy_workqueue(uck_hyper_wq);
			uck_hyper_wq = NULL;
			return ret;
		}
	}

	/* Hook do_exit for cleanup */
	uck_exit_probe.kp.symbol_name = "do_exit";
	ret = register_kretprobe(&uck_exit_probe);
	if (ret < 0) {
		pr_warn("uck_hyper: do_exit probe failed: %d (non-fatal)\n",
			ret);
		/* Non-fatal: tracked PIDs just won't be cleaned up on exit */
	}

	pr_info("uck_hyper: SMP hypervisor layer initialized "
		"(fork interception active)\n");
	return 0;
}

void uck_hyper_stop(void)
{
	struct uck_tracked_proc *tp;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt;

	unregister_kretprobe(&uck_fork_probe);
	unregister_kretprobe(&uck_exit_probe);

	if (uck_hyper_wq) {
		flush_workqueue(uck_hyper_wq);
		destroy_workqueue(uck_hyper_wq);
		uck_hyper_wq = NULL;
	}

	/* Free all tracked entries */
	spin_lock_irqsave(&uck_tracked_lock, flags);
	hash_for_each_safe(uck_tracked_pids, bkt, tmp, tp, node) {
		hash_del(&tp->node);
		kfree(tp);
	}
	spin_unlock_irqrestore(&uck_tracked_lock, flags);

	pr_info("uck_hyper: SMP hypervisor layer stopped\n");
}
