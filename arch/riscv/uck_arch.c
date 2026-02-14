/*
 * arch/riscv/uck_arch.c - RISC-V HAL stub for UCK
 *
 * Placeholder for future RISC-V support.
 */

#include <linux/module.h>
#include <linux/sched.h>

#include "../uck_arch.h"

u32 uck_arch_detect_features(void)
{
	return 0;
}

bool uck_arch_features_compatible(u32 src_features, u32 dst_features)
{
	return src_features == 0 && dst_features == 0;
}

int uck_arch_capture_regs(struct task_struct *task, void *buf, size_t buflen)
{
	return -ENOSYS;
}

size_t uck_arch_reg_state_size(void)
{
	return 0;
}

void uck_arch_flush_page(struct page *page)
{
	/* RISC-V: FENCE instruction for cache coherence */
}

void uck_arch_mb(void)
{
	__asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

const char *uck_arch_name(void)
{
	return "riscv";
}
