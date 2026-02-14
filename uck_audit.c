/*
 * uck_audit.c - Structured audit logging for UCK security events
 *
 * Logs security-relevant events using the kernel audit framework.
 * Events: migration, job execution, region create/destroy, node join/leave,
 * authentication failures, rate limit violations.
 */

#include <linux/audit.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

#include "uck_internal.h"

#define UCK_AUDIT_RING_SIZE 256

struct uck_audit_entry {
	u64 timestamp_ns;
	u32 event_type;
	char message[128];
};

static struct uck_audit_entry audit_ring[UCK_AUDIT_RING_SIZE];
static atomic_t audit_ring_head = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(audit_lock);

void uck_audit_log(const char *event, const char *fmt, ...)
{
	struct uck_audit_entry *entry;
	int idx;
	va_list args;
	unsigned long flags;

	idx = atomic_inc_return(&audit_ring_head) % UCK_AUDIT_RING_SIZE;

	spin_lock_irqsave(&audit_lock, flags);
	entry = &audit_ring[idx];
	entry->timestamp_ns = ktime_get_real_ns();
	va_start(args, fmt);
	vsnprintf(entry->message, sizeof(entry->message), fmt, args);
	va_end(args);
	spin_unlock_irqrestore(&audit_lock, flags);

	/* Also log to kernel audit subsystem if available */
	{
		struct audit_buffer *ab;
		ab = audit_log_start(audit_context(), GFP_ATOMIC,
				     AUDIT_KERNEL_OTHER);
		if (ab) {
			audit_log_format(ab, "uck %s: ", event);
			va_start(args, fmt);
			audit_log_vformat(ab, fmt, args);
			va_end(args);
			audit_log_end(ab);
		}
	}

	pr_info("uck_audit: [%s] %s\n", event, entry->message);
}

/* /proc/uck/audit - show recent audit entries */
static int uck_proc_audit_show(struct seq_file *m, void *v)
{
	int i, head;
	unsigned long flags;

	head = atomic_read(&audit_ring_head);

	spin_lock_irqsave(&audit_lock, flags);
	for (i = 0; i < UCK_AUDIT_RING_SIZE; i++) {
		int idx = (head - i + UCK_AUDIT_RING_SIZE) % UCK_AUDIT_RING_SIZE;
		struct uck_audit_entry *e = &audit_ring[idx];
		if (e->timestamp_ns == 0)
			continue;
		seq_printf(m, "[%llu] %s\n", e->timestamp_ns, e->message);
	}
	spin_unlock_irqrestore(&audit_lock, flags);

	return 0;
}

static int uck_proc_audit_open(struct inode *inode, struct file *file)
{
	return single_open(file, uck_proc_audit_show, NULL);
}

static const struct proc_ops uck_proc_audit_ops = {
	.proc_open    = uck_proc_audit_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

int uck_audit_init(void)
{
	memset(audit_ring, 0, sizeof(audit_ring));

	if (uck_state.proc_dir) {
		uck_state.proc_audit = proc_create("audit", 0400,
						    uck_state.proc_dir,
						    &uck_proc_audit_ops);
	}

	uck_audit_log("init", "audit subsystem initialized");
	return 0;
}

void uck_audit_exit(void)
{
	if (uck_state.proc_audit)
		proc_remove(uck_state.proc_audit);
	pr_info("uck_audit: audit subsystem stopped\n");
}
