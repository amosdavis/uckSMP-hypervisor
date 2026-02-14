/*
 * uck_proc.c - Process checkpoint/restore for migration
 *
 * Captures the state of a running process (registers, memory map, fds)
 * and restores it on a remote node.
 *
 * Limitations:
 *   - Single-threaded processes only
 *   - File descriptors are reopened by path on destination
 *   - Anonymous pages are transferred inline
 *   - No support for sockets, pipes, or shared memory (yet)
 */

#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <linux/mman.h>
#include <linux/signal.h>
#include <linux/delay.h>
#include <asm/ptrace.h>

#include "uck_internal.h"

/*
 * Capture register state from a stopped task.
 */
static void uck_capture_regs(struct task_struct *task,
			      struct uck_proc_state_hdr *state)
{
	struct pt_regs *regs = task_pt_regs(task);

	state->ip = regs->ip;
	state->sp = regs->sp;
	state->flags = regs->flags;
	state->ax = regs->ax;
	state->bx = regs->bx;
	state->cx = regs->cx;
	state->dx = regs->dx;
	state->si = regs->si;
	state->di = regs->di;
	state->bp = regs->bp;
	state->r8 = regs->r8;
	state->r9 = regs->r9;
	state->r10 = regs->r10;
	state->r11 = regs->r11;
	state->r12 = regs->r12;
	state->r13 = regs->r13;
	state->r14 = regs->r14;
	state->r15 = regs->r15;
	state->cs = regs->cs;
	state->ss = regs->ss;
}

/*
 * Count the VMAs of a task using VMA iterator (kernel 6.1 maple tree).
 */
static int uck_count_vmas(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	struct vma_iterator vmi;
	int count = 0;

	mmap_read_lock(mm);
	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma)
		count++;
	mmap_read_unlock(mm);
	return count;
}

static int uck_count_fds(struct task_struct *task)
{
	struct files_struct *files = task->files;
	struct fdtable *fdt;
	int count = 0, fd;

	if (!files)
		return 0;

	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	for (fd = 0; fd < fdt->max_fds; fd++) {
		if (fdt->fd[fd])
			count++;
	}
	spin_unlock(&files->file_lock);
	return count;
}

/*
 * Serialize a task's VMA descriptors.
 */
static int uck_serialize_vmas(struct mm_struct *mm,
			       struct uck_vma_desc *descs, int max)
{
	struct vm_area_struct *vma;
	struct vma_iterator vmi;
	int i = 0;

	mmap_read_lock(mm);
	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma) {
		if (i >= max)
			break;

		descs[i].start = vma->vm_start;
		descs[i].end = vma->vm_end;
		descs[i].flags = vma->vm_flags;
		descs[i].pgoff = vma->vm_pgoff;

		if (vma->vm_file) {
			char *buf, *p;
			descs[i].is_anon = 0;
			buf = kmalloc(256, GFP_KERNEL);
			if (buf) {
				p = d_path(&vma->vm_file->f_path, buf, 256);
				if (!IS_ERR(p))
					strscpy(descs[i].path, p, 256);
				else
					descs[i].path[0] = '\0';
				kfree(buf);
			}
		} else {
			descs[i].is_anon = 1;
			descs[i].path[0] = '\0';
		}
		i++;
	}
	mmap_read_unlock(mm);
	return i;
}

/*
 * Serialize open file descriptors.
 */
static int uck_serialize_fds(struct task_struct *task,
			      struct uck_fd_desc *descs, int max)
{
	struct files_struct *files = task->files;
	struct fdtable *fdt;
	int count = 0, fd;

	if (!files)
		return 0;

	spin_lock(&files->file_lock);
	fdt = files_fdtable(files);
	for (fd = 0; fd < fdt->max_fds && count < max; fd++) {
		struct file *f = fdt->fd[fd];
		if (!f)
			continue;

		descs[count].fd_num = fd;
		descs[count].flags = f->f_flags;
		descs[count].pos = f->f_pos;

		{
			char *buf, *p;
			buf = kmalloc(256, GFP_KERNEL);
			if (buf) {
				p = d_path(&f->f_path, buf, 256);
				if (!IS_ERR(p))
					strscpy(descs[count].path, p, 256);
				else
					descs[count].path[0] = '\0';
				kfree(buf);
			}
		}
		count++;
	}
	spin_unlock(&files->file_lock);
	return count;
}

/*
 * Full process checkpoint: freeze, capture state, serialize.
 */
static void *uck_checkpoint_process(struct task_struct *task, size_t *out_len)
{
	struct mm_struct *mm;
	struct uck_proc_state_hdr *hdr;
	struct uck_vma_desc *vmas;
	struct uck_fd_desc *fds;
	int nr_vmas, nr_fds;
	size_t total_len;
	char *buf;

	mm = get_task_mm(task);
	if (!mm)
		return NULL;

	nr_vmas = uck_count_vmas(mm);
	nr_fds = uck_count_fds(task);

	total_len = sizeof(*hdr) +
		    nr_vmas * sizeof(struct uck_vma_desc) +
		    nr_fds * sizeof(struct uck_fd_desc);

	buf = kvmalloc(total_len, GFP_KERNEL);
	if (!buf) {
		mmput(mm);
		return NULL;
	}
	memset(buf, 0, total_len);

	hdr = (struct uck_proc_state_hdr *)buf;
	hdr->src_node = uck_state.local_node.node_id;
	hdr->pid = task->pid;
	hdr->nr_vmas = nr_vmas;
	hdr->nr_pages = 0;
	hdr->nr_fds = nr_fds;

	uck_capture_regs(task, hdr);

	vmas = (struct uck_vma_desc *)(buf + sizeof(*hdr));
	uck_serialize_vmas(mm, vmas, nr_vmas);

	fds = (struct uck_fd_desc *)((char *)vmas +
				      nr_vmas * sizeof(struct uck_vma_desc));
	uck_serialize_fds(task, fds, nr_fds);

	mmput(mm);

	*out_len = total_len;
	return buf;
}

/*
 * Send checkpointed process state + pages to destination node.
 */
static int uck_send_process(u32 dest_node, struct task_struct *task,
			     void *state_buf, size_t state_len)
{
	struct uck_remote_node *dest = NULL;
	struct uck_msg_hdr hdr;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct vma_iterator vmi;
	int i, ret;

	for (i = 0; i < uck_state.num_nodes; i++) {
		if (uck_state.nodes[i].info.node_id == dest_node) {
			dest = &uck_state.nodes[i];
			break;
		}
	}
	if (!dest)
		return -ENOENT;

	if (!dest->connected) {
		ret = uck_net_connect_node(dest);
		if (ret < 0)
			return ret;
	}

	/* Send process state */
	memset(&hdr, 0, sizeof(hdr));
	hdr.type = UCK_MSG_PROC_STATE;
	hdr.src_node = uck_state.local_node.node_id;
	hdr.payload_len = state_len;

	mutex_lock(&dest->sock_lock);
	ret = uck_net_send_msg(dest->sock, &hdr, state_buf, state_len);
	if (ret < 0) {
		mutex_unlock(&dest->sock_lock);
		return ret;
	}

	/* Send anonymous pages */
	mm = get_task_mm(task);
	if (mm) {
		struct uck_msg_hdr phdr;

		mmap_read_lock(mm);
		vma_iter_init(&vmi, mm, 0);
		for_each_vma(vmi, vma) {
			unsigned long addr;

			if (vma->vm_file)
				continue;

			for (addr = vma->vm_start; addr < vma->vm_end;
			     addr += PAGE_SIZE) {
				struct page *page;
				void *kaddr;
				char *page_buf;

				ret = get_user_pages_remote(mm, addr, 1,
							    0, &page, NULL, NULL);
				if (ret != 1)
					continue;

				page_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
				if (!page_buf) {
					put_page(page);
					continue;
				}

				kaddr = kmap(page);
				memcpy(page_buf, kaddr, PAGE_SIZE);
				kunmap(page);
				put_page(page);

				memset(&phdr, 0, sizeof(phdr));
				phdr.type = UCK_MSG_PROC_PAGES;
				phdr.src_node = uck_state.local_node.node_id;
				phdr.page_offset = addr;
				phdr.payload_len = PAGE_SIZE;

				uck_net_send_msg(dest->sock, &phdr,
						 page_buf, PAGE_SIZE);
				kfree(page_buf);
			}
		}
		mmap_read_unlock(mm);
		mmput(mm);
	}

	/* Send migration done marker */
	memset(&hdr, 0, sizeof(hdr));
	hdr.type = UCK_MSG_MIGRATE_DONE;
	hdr.src_node = uck_state.local_node.node_id;
	uck_net_send_msg(dest->sock, &hdr, NULL, 0);

	mutex_unlock(&dest->sock_lock);

	return 0;
}

/*
 * Migrate a process to a remote node.
 */
int uck_migrate_process(u32 pid, u32 dest_node)
{
	struct task_struct *task;
	struct pid *pid_struct;
	void *state_buf;
	size_t state_len;
	int ret;

	if (dest_node == uck_state.local_node.node_id)
		return -EINVAL;

	rcu_read_lock();
	pid_struct = find_vpid(pid);
	task = pid_struct ? pid_task(pid_struct, PIDTYPE_PID) : NULL;
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (!task) {
		pr_err("uck: migrate: pid %u not found\n", pid);
		return -ESRCH;
	}

	if (task->signal->nr_threads > 1) {
		pr_err("uck: migrate: pid %u is multi-threaded\n", pid);
		put_task_struct(task);
		return -EOPNOTSUPP;
	}

	pr_info("uck: migrating pid %u (%s) to node %u\n",
		pid, task->comm, dest_node);

	/* Freeze the process */
	send_sig(SIGSTOP, task, 1);
	msleep(100);

	/* Checkpoint */
	state_buf = uck_checkpoint_process(task, &state_len);
	if (!state_buf) {
		pr_err("uck: migrate: checkpoint failed for pid %u\n", pid);
		send_sig(SIGCONT, task, 1);
		put_task_struct(task);
		return -ENOMEM;
	}

	pr_info("uck: checkpoint complete: %zu bytes, sending to node %u\n",
		state_len, dest_node);

	ret = uck_send_process(dest_node, task, state_buf, state_len);

	kvfree(state_buf);

	if (ret < 0) {
		pr_err("uck: migrate: send failed: %d\n", ret);
		send_sig(SIGCONT, task, 1);
		put_task_struct(task);
		return ret;
	}

	/* Kill the local copy */
	send_sig(SIGKILL, task, 1);
	put_task_struct(task);

	pr_info("uck: migration of pid %u to node %u complete\n",
		pid, dest_node);
	return 0;
}
