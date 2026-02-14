/*
 * uck_migrate.c - Migration protocol handler (receiving side)
 *
 * When a remote node sends a process via UCK_MSG_PROC_STATE,
 * this module restores it as a new local process.
 *
 * Restoration approach:
 *   1. Receive process state header (registers, VMA list, FD list)
 *   2. Receive anonymous pages
 *   3. Fork a new userspace helper that exec's a stub which:
 *      - mmaps anonymous regions at the correct addresses
 *      - Opens file descriptors by path
 *      - Copies pages into the right locations
 *      - Restores registers via sigreturn
 *
 * For simplicity, we use call_usermodehelper to run a restore stub
 * that does the heavy lifting in userspace. The kernel sends the
 * state to a tmpfile which the stub reads.
 */

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/uaccess.h>
#include <linux/umh.h>
#include <linux/crc32.h>

#include "uck_internal.h"

/* Temporary storage for incoming migration data */
struct uck_incoming_proc {
	struct uck_proc_state_hdr hdr;
	void *state_buf;          /* VMA + FD descriptors */
	size_t state_len;

	/* Pages received */
	struct {
		unsigned long addr;
		char data[PAGE_SIZE];
	} *pages;
	int nr_pages;
	int pages_capacity;
};

static struct uck_incoming_proc *uck_alloc_incoming(void)
{
	struct uck_incoming_proc *inc;

	inc = kzalloc(sizeof(*inc), GFP_KERNEL);
	if (!inc)
		return NULL;

	inc->pages_capacity = 256;
	inc->pages = kvmalloc_array(inc->pages_capacity,
				     sizeof(inc->pages[0]), GFP_KERNEL);
	if (!inc->pages) {
		kfree(inc);
		return NULL;
	}
	return inc;
}

static void uck_free_incoming(struct uck_incoming_proc *inc)
{
	if (!inc)
		return;
	kvfree(inc->state_buf);
	kvfree(inc->pages);
	kfree(inc);
}

/*
 * Write the process state to a file in /tmp so the restore stub can read it.
 * Format: header + VMA descs + FD descs + (addr, page_data) pairs
 */
static int uck_write_state_file(struct uck_incoming_proc *inc)
{
	struct file *f;
	loff_t pos = 0;
	int i;
	ssize_t ret;

	f = filp_open("/run/uck/uck_migrate_state", O_WRONLY | O_CREAT | O_TRUNC,
		       0600);
	if (IS_ERR(f))
		return PTR_ERR(f);

	/* Write header */
	ret = kernel_write(f, &inc->hdr, sizeof(inc->hdr), &pos);
	if (ret < 0)
		goto out;

	/* Write VMA + FD descriptors */
	if (inc->state_buf && inc->state_len > 0) {
		ret = kernel_write(f, inc->state_buf, inc->state_len, &pos);
		if (ret < 0)
			goto out;
	}

	/* Write page count */
	ret = kernel_write(f, &inc->nr_pages, sizeof(inc->nr_pages), &pos);
	if (ret < 0)
		goto out;

	/* Write pages: (addr, data) pairs */
	for (i = 0; i < inc->nr_pages; i++) {
		ret = kernel_write(f, &inc->pages[i].addr,
				   sizeof(inc->pages[i].addr), &pos);
		if (ret < 0)
			goto out;
		ret = kernel_write(f, inc->pages[i].data, PAGE_SIZE, &pos);
		if (ret < 0)
			goto out;
	}

	ret = 0;
out:
	filp_close(f, NULL);
	return (int)ret;
}

/*
 * Launch the restore stub process.
 * The stub reads /run/uck/uck_migrate_state and restores the process.
 */
static int uck_launch_restored_process(struct uck_incoming_proc *inc)
{
	int ret;
	char *argv[] = { "/usr/sbin/uck_restore", NULL };
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };

	ret = uck_write_state_file(inc);
	if (ret < 0) {
		pr_err("uck: failed to write migration state file: %d\n", ret);
		return ret;
	}

	pr_info("uck: launching restore stub for pid %u from node %u\n",
		inc->hdr.pid, inc->hdr.src_node);

	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
	if (ret < 0)
		pr_err("uck: restore stub launch failed: %d\n", ret);
	else
		pr_info("uck: restore stub launched successfully\n");

	return ret;
}

/*
 * Handle incoming UCK_MSG_PROC_STATE message.
 * This starts a migration receive — more messages (PROC_PAGES, MIGRATE_DONE)
 * will follow on the same connection.
 */
void uck_handle_proc_state(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_incoming_proc *inc;
	struct uck_msg_hdr msg;
	int ret;

	inc = uck_alloc_incoming();
	if (!inc)
		return;

	/* Read the proc state header + VMA/FD descriptors */
	if (hdr->payload_len < sizeof(struct uck_proc_state_hdr)) {
		pr_err("uck: migrate: payload too small\n");
		uck_free_incoming(inc);
		return;
	}

	ret = uck_sock_recv(client, &inc->hdr, sizeof(inc->hdr));
	if (ret < 0) {
		uck_free_incoming(inc);
		return;
	}

	/* Read remaining VMA + FD descriptors */
	inc->state_len = hdr->payload_len - sizeof(inc->hdr);
	if (inc->state_len > 0) {
		inc->state_buf = kvmalloc(inc->state_len, GFP_KERNEL);
		if (!inc->state_buf) {
			uck_free_incoming(inc);
			return;
		}
		ret = uck_sock_recv(client, inc->state_buf, inc->state_len);
		if (ret < 0) {
			uck_free_incoming(inc);
			return;
		}
	}

	uck_audit_log("migration", "received migration for pid %d from node %u",
		      hdr->orig_pid, hdr->src_node);

	pr_info("uck: receiving process pid=%u from node %u "
		"(%llu VMAs, %u FDs)\n",
		inc->hdr.pid, inc->hdr.src_node,
		inc->hdr.nr_vmas, inc->hdr.nr_fds);

	/* Now receive page data and done marker */
	while (1) {
		ret = uck_sock_recv(client, &msg, sizeof(msg));
		if (ret < 0)
			break;

		if (msg.type == UCK_MSG_PROC_PAGES) {
			/* Grow pages array if needed */
			if (inc->nr_pages >= inc->pages_capacity) {
				int new_cap = inc->pages_capacity * 2;
				void *new_arr;
				new_arr = kvmalloc_array(new_cap,
							  sizeof(inc->pages[0]),
							  GFP_KERNEL);
				if (!new_arr)
					break;
				memcpy(new_arr, inc->pages,
				       inc->nr_pages * sizeof(inc->pages[0]));
				kvfree(inc->pages);
				inc->pages = new_arr;
				inc->pages_capacity = new_cap;
			}

			inc->pages[inc->nr_pages].addr = msg.page_offset;
			ret = uck_sock_recv(client,
					    inc->pages[inc->nr_pages].data,
					    PAGE_SIZE);
			if (ret < 0)
				break;
			inc->nr_pages++;
		} else if (msg.type == UCK_MSG_MIGRATE_DONE) {
			pr_info("uck: received %d pages for migrated process\n",
				inc->nr_pages);
			break;
		} else {
			pr_warn("uck: unexpected msg type %u during migration\n",
				msg.type);
			break;
		}
	}

	/* Verify CRC32 integrity of received state */
	if (hdr->crc32 != 0) {
		u32 computed_crc = crc32(0, (const u8 *)&hdr,
					 offsetof(struct uck_proc_state_hdr,
						  crc32));
		if (computed_crc != hdr->crc32) {
			pr_err("uck: migration CRC32 mismatch for pid %d "
			       "(expected 0x%08x, got 0x%08x)\n",
			       hdr->orig_pid, hdr->crc32, computed_crc);
			uck_audit_log("migration",
				      "CRC32 mismatch for pid %d — aborting",
				      hdr->orig_pid);
			atomic_long_inc(&uck_state.err_migrate);
			return;
		}
	}

	/* Restore the process */
	uck_launch_restored_process(inc);

	uck_audit_log("migration", "pid %d migration from node %u complete",
		      hdr->orig_pid, hdr->src_node);

	uck_free_incoming(inc);
}

/*
 * Handle incoming UCK_MSG_MIGRATE_REQ.
 * A remote node is asking us to accept a process.
 * For now, always accept.
 */
void uck_handle_migrate_request(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_msg_hdr resp;

	memset(&resp, 0, sizeof(resp));
	resp.type = UCK_MSG_MIGRATE_ACCEPT;
	resp.src_node = uck_state.local_node.node_id;

	uck_sock_send(client, &resp, sizeof(resp));
}
