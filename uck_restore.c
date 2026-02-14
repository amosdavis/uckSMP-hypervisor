/*
 * uck_restore.c - Userspace process restore stub
 *
 * Called by the kernel module (via call_usermodehelper) after receiving
 * a migrated process. Reads serialized state from /run/uck/uck_migrate_state,
 * reconstructs the process, and jumps to the saved instruction pointer.
 *
 * This is intentionally simple: it restores anonymous mappings and
 * re-opens file descriptors, then uses sigreturn to restore registers.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <ucontext.h>
#include <errno.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>

#include "uck.h"

static struct uck_proc_state_hdr state_hdr;

/* Seccomp-bpf filter for restored processes */
static void uck_apply_seccomp_filter(void)
{
	struct sock_filter filter[] = {
		/* Load syscall number */
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		/* Block ptrace (101) */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ptrace, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
		/* Block mount (165) */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mount, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
		/* Block reboot (169) */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_reboot, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
		/* Block kexec_load (246) */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_kexec_load, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
		/* Block init_module (175) */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_init_module, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
		/* Allow all other syscalls */
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = sizeof(filter) / sizeof(filter[0]),
		.filter = filter,
	};

	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}

/* Restore saved registers and jump using setcontext (reliable) */
static void restore_and_jump(void)
{
	ucontext_t uc;
	getcontext(&uc);

	/* Populate the machine context from our saved state */
	mcontext_t *mc = &uc.uc_mcontext;
	mc->gregs[REG_RIP] = (greg_t)state_hdr.ip;
	mc->gregs[REG_RSP] = (greg_t)state_hdr.sp;
	mc->gregs[REG_EFL] = (greg_t)state_hdr.flags;
	mc->gregs[REG_RAX] = (greg_t)state_hdr.ax;
	mc->gregs[REG_RBX] = (greg_t)state_hdr.bx;
	mc->gregs[REG_RCX] = (greg_t)state_hdr.cx;
	mc->gregs[REG_RDX] = (greg_t)state_hdr.dx;
	mc->gregs[REG_RSI] = (greg_t)state_hdr.si;
	mc->gregs[REG_RDI] = (greg_t)state_hdr.di;
	mc->gregs[REG_RBP] = (greg_t)state_hdr.bp;
	mc->gregs[REG_R8]  = (greg_t)state_hdr.r8;
	mc->gregs[REG_R9]  = (greg_t)state_hdr.r9;
	mc->gregs[REG_R10] = (greg_t)state_hdr.r10;
	mc->gregs[REG_R11] = (greg_t)state_hdr.r11;
	mc->gregs[REG_R12] = (greg_t)state_hdr.r12;
	mc->gregs[REG_R13] = (greg_t)state_hdr.r13;
	mc->gregs[REG_R14] = (greg_t)state_hdr.r14;
	mc->gregs[REG_R15] = (greg_t)state_hdr.r15;

	setcontext(&uc);
	/* Should not return */
	_exit(99);
}

int main(int argc, char *argv[])
{
	int fd, i;
	ssize_t n;
	struct uck_vma_desc *vmas = NULL;
	struct uck_fd_desc *fds = NULL;
	int nr_pages = 0;

	fd = open("/run/uck/uck_migrate_state", O_RDONLY);
	if (fd < 0) {
		perror("uck_restore: open state file");
		return 1;
	}

	/* Read header */
	n = read(fd, &state_hdr, sizeof(state_hdr));
	if (n != sizeof(state_hdr)) {
		fprintf(stderr, "uck_restore: short read on header\n");
		close(fd);
		return 1;
	}

	fprintf(stderr, "uck_restore: restoring pid %u from node %u "
		"(%llu VMAs, %u FDs)\n",
		state_hdr.pid, state_hdr.src_node,
		(unsigned long long)state_hdr.nr_vmas, state_hdr.nr_fds);

	/* Read VMA descriptors */
	if (state_hdr.nr_vmas > 0) {
		size_t vma_size = state_hdr.nr_vmas * sizeof(struct uck_vma_desc);
		vmas = malloc(vma_size);
		if (!vmas) {
			close(fd);
			return 1;
		}
		n = read(fd, vmas, vma_size);
		if (n != (ssize_t)vma_size) {
			fprintf(stderr, "uck_restore: short read on VMAs\n");
			free(vmas);
			close(fd);
			return 1;
		}
	}

	/* Read FD descriptors */
	if (state_hdr.nr_fds > 0) {
		size_t fd_size = state_hdr.nr_fds * sizeof(struct uck_fd_desc);
		fds = malloc(fd_size);
		if (!fds) {
			free(vmas);
			close(fd);
			return 1;
		}
		n = read(fd, fds, fd_size);
		if (n != (ssize_t)fd_size) {
			fprintf(stderr, "uck_restore: short read on FDs\n");
			free(fds);
			free(vmas);
			close(fd);
			return 1;
		}
	}

	/* Read page count */
	n = read(fd, &nr_pages, sizeof(nr_pages));
	if (n != sizeof(nr_pages))
		nr_pages = 0;

	/* Restore VMAs (anonymous only — skip [vdso], [vsyscall], etc.) */
	for (i = 0; i < (int)state_hdr.nr_vmas; i++) {
		struct uck_vma_desc *v = &vmas[i];
		int prot = 0;
		size_t len = v->end - v->start;

		if (!v->is_anon)
			continue;
		if (len == 0)
			continue;
		/* Skip special kernel mappings */
		if (v->start >= 0x7ffffffffffff000ULL)
			continue;

		if (v->flags & 0x1) prot |= PROT_READ;  /* VM_READ */
		if (v->flags & 0x2) prot |= PROT_WRITE; /* VM_WRITE */
		if (v->flags & 0x4) prot |= PROT_EXEC;  /* VM_EXEC */

		void *p = mmap((void *)v->start, len,
			       prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
			       -1, 0);
		if (p == MAP_FAILED) {
			fprintf(stderr, "uck_restore: mmap %lx-%lx failed: %s\n",
				(unsigned long)v->start,
				(unsigned long)v->end,
				strerror(errno));
		}
	}

	/* Read and place pages */
	for (i = 0; i < nr_pages; i++) {
		unsigned long addr;
		char page_data[4096];

		n = read(fd, &addr, sizeof(addr));
		if (n != sizeof(addr))
			break;
		n = read(fd, page_data, 4096);
		if (n != 4096)
			break;

		/* Copy page data to the mapped address */
		memcpy((void *)addr, page_data, 4096);
	}

	close(fd);

	/* Restore file descriptors */
	for (i = 0; i < (int)state_hdr.nr_fds; i++) {
		struct uck_fd_desc *f = &fds[i];
		int newfd;

		if (f->path[0] == '\0')
			continue;
		/* Skip special fds */
		if (strncmp(f->path, "/dev/", 5) == 0 ||
		    strncmp(f->path, "/proc/", 6) == 0 ||
		    strncmp(f->path, "pipe:", 5) == 0 ||
		    strncmp(f->path, "socket:", 7) == 0 ||
		    strncmp(f->path, "anon_inode:", 11) == 0)
			continue;

		newfd = open(f->path, f->flags & O_ACCMODE, 0);
		if (newfd < 0)
			continue;

		if (newfd != (int)f->fd_num) {
			dup2(newfd, f->fd_num);
			close(newfd);
		}
		if (f->pos > 0)
			lseek(f->fd_num, f->pos, SEEK_SET);
	}

	free(vmas);
	free(fds);

	fprintf(stderr, "uck_restore: jumping to IP=%llx SP=%llx\n",
		(unsigned long long)state_hdr.ip,
		(unsigned long long)state_hdr.sp);

	/* Remove the state file */
	unlink("/run/uck/uck_migrate_state");

	/* Apply seccomp-bpf filter to restrict dangerous syscalls */
	uck_apply_seccomp_filter();

	/* Jump to the restored process */
	restore_and_jump();

	/* Should not reach here */
	return 1;
}
