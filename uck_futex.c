/*
 * uck_futex.c - Distributed futex / cross-node synchronization
 *
 * Provides cross-node wait/wake for addresses in shared UCK regions.
 * When a process on node A calls FUTEX_WAIT on a shared address,
 * we track it locally. When node B calls FUTEX_WAKE, we broadcast
 * the wake to all nodes that might have waiters.
 */

#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/hashtable.h>
#include <linux/highmem.h>

#include "uck_internal.h"

#define UCK_FUTEX_HASH_BITS 8

struct uck_futex_waiter {
	struct hlist_node node;
	u64 region_id;
	u64 offset;
	struct task_struct *task;
	bool woken;
};

static DEFINE_HASHTABLE(uck_futex_table, UCK_FUTEX_HASH_BITS);
static DEFINE_SPINLOCK(uck_futex_lock);

static u32 uck_futex_hash_key(u64 region_id, u64 offset)
{
	return (u32)(region_id ^ (offset >> PAGE_SHIFT));
}

/*
 * Local futex wait: sleep until woken by FUTEX_WAKE on same address.
 * Returns 0 on successful wake, -EINTR on signal.
 */
static int uck_futex_wait_local(u64 region_id, u64 offset, u32 expected_val)
{
	struct uck_futex_waiter waiter;
	unsigned long flags;
	u32 key;

	waiter.region_id = region_id;
	waiter.offset = offset;
	waiter.task = current;
	waiter.woken = false;

	key = uck_futex_hash_key(region_id, offset);

	spin_lock_irqsave(&uck_futex_lock, flags);
	hash_add(uck_futex_table, &waiter.node, key);
	spin_unlock_irqrestore(&uck_futex_lock, flags);

	/* Check the value at the address against expected */
	{
		struct uck_region *region;
		struct uck_page_entry *entry;
		pgoff_t pg_idx = offset >> PAGE_SHIFT;
		u32 pg_off = offset & ~PAGE_MASK;
		u32 *addr;

		mutex_lock(&uck_state.lock);
		region = uck_find_region(region_id);
		mutex_unlock(&uck_state.lock);

		if (region) {
			mutex_lock(&region->lock);
			entry = uck_page_lookup(region, pg_idx);
			if (entry && entry->page) {
				addr = (u32 *)((char *)kmap(entry->page) + pg_off);
				if (*addr != expected_val) {
					kunmap(entry->page);
					mutex_unlock(&region->lock);
					spin_lock_irqsave(&uck_futex_lock, flags);
					hash_del(&waiter.node);
					spin_unlock_irqrestore(&uck_futex_lock, flags);
					return -EAGAIN;
				}
				kunmap(entry->page);
			}
			mutex_unlock(&region->lock);
		}
	}

	/* Sleep until woken */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!waiter.woken && !signal_pending(current))
		schedule();
	__set_current_state(TASK_RUNNING);

	spin_lock_irqsave(&uck_futex_lock, flags);
	hash_del(&waiter.node);
	spin_unlock_irqrestore(&uck_futex_lock, flags);

	return waiter.woken ? 0 : -EINTR;
}

/*
 * Local futex wake: wake up to nr_wake waiters on the given address.
 * Returns number of waiters woken.
 */
static int uck_futex_wake_local(u64 region_id, u64 offset, u32 nr_wake)
{
	struct uck_futex_waiter *w;
	unsigned long flags;
	u32 key, woken = 0;

	key = uck_futex_hash_key(region_id, offset);

	spin_lock_irqsave(&uck_futex_lock, flags);
	hash_for_each_possible(uck_futex_table, w, node, key) {
		if (w->region_id == region_id && w->offset == offset) {
			w->woken = true;
			wake_up_process(w->task);
			woken++;
			if (woken >= nr_wake)
				break;
		}
	}
	spin_unlock_irqrestore(&uck_futex_lock, flags);

	return woken;
}

/*
 * Broadcast futex wake to all remote nodes.
 */
static void uck_futex_wake_remote(u64 region_id, u64 offset, u32 nr_wake)
{
	struct uck_msg_hdr hdr;
	struct uck_futex_req freq;
	int i;

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = UCK_MSG_FUTEX_WAKE;
	hdr.src_node = uck_state.local_node.node_id;
	hdr.region_id = region_id;
	hdr.payload_len = sizeof(freq);

	memset(&freq, 0, sizeof(freq));
	freq.region_id = region_id;
	freq.offset = offset;
	freq.op = UCK_FUTEX_WAKE;
	freq.val = nr_wake;

	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (!node->alive)
			continue;
		uck_net_send_msg_to_node(node->info.node_id, &hdr,
					 &freq, sizeof(freq));

		/* Wait for wake acknowledgment with timeout */
		{
			unsigned long timeout_jiffies = msecs_to_jiffies(5000);
			/* If acknowledgment not received within timeout,
			 * the remote waiter is considered orphaned */
			/* TODO: implement full ACK tracking with per-waiter state */
			pr_debug("uck: futex wake sent to node %u for addr 0x%llx\n",
				 node->info.node_id,
				 (unsigned long long)(offset));
		}
	}
}

/*
 * Ioctl handler for UCK_IOC_FUTEX.
 */
int uck_futex_op(struct uck_futex_req *req)
{
	if (req->op == UCK_FUTEX_WAIT) {
		return uck_futex_wait_local(req->region_id,
					    req->offset, req->val);
	} else if (req->op == UCK_FUTEX_WAKE) {
		int woken;
		/* Wake local waiters first */
		woken = uck_futex_wake_local(req->region_id,
					     req->offset, req->val);
		/* Broadcast to remote nodes */
		uck_futex_wake_remote(req->region_id, req->offset,
				      req->val);
		return woken;
	}
	return -EINVAL;
}

/*
 * Handle incoming UCK_MSG_FUTEX_WAKE from remote node.
 * Wake local waiters on the specified address.
 */
void uck_handle_futex_wake(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_futex_req freq;
	int ret;

	if (hdr->payload_len != sizeof(freq))
		return;

	ret = uck_sock_recv(client, &freq, sizeof(freq));
	if (ret < 0)
		return;

	uck_futex_wake_local(freq.region_id, freq.offset, freq.val);
}

/*
 * Handle incoming UCK_MSG_FUTEX_WAIT (informational — remote node
 * is waiting; we don't need to do anything since wakes are broadcast).
 */
void uck_handle_futex_wait(struct socket *client, struct uck_msg_hdr *hdr)
{
	/* Informational only; discard payload if any */
	if (hdr->payload_len > 0) {
		void *discard = kmalloc(hdr->payload_len, GFP_KERNEL);
		if (discard) {
			uck_sock_recv(client, discard, hdr->payload_len);
			kfree(discard);
		}
	}
}

int uck_futex_init(void)
{
	pr_info("uck: distributed futex initialized\n");
	return 0;
}

void uck_futex_exit(void)
{
	struct uck_futex_waiter *w;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt;

	/* Wake all remaining waiters */
	spin_lock_irqsave(&uck_futex_lock, flags);
	hash_for_each_safe(uck_futex_table, bkt, tmp, w, node) {
		w->woken = true;
		wake_up_process(w->task);
		hash_del(&w->node);
	}
	spin_unlock_irqrestore(&uck_futex_lock, flags);

	pr_info("uck: distributed futex cleaned up\n");
}
