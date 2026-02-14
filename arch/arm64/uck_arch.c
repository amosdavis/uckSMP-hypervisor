/*
 * arch/arm64/uck_arch.c - ARM64 HAL stub for UCK
 *
 * Placeholder for future ARM64 support.
 * All functions return sensible defaults or error codes.
 */

#include <linux/module.h>
#include <linux/sched.h>

#include "../uck_arch.h"

u32 uck_arch_detect_features(void)
{
	return 0;  /* No x86 features on ARM64 */
}

bool uck_arch_features_compatible(u32 src_features, u32 dst_features)
{
	/* Cross-architecture migration not supported */
	return src_features == 0 && dst_features == 0;
}

int uck_arch_capture_regs(struct task_struct *task, void *buf, size_t buflen)
{
	/* TODO: Implement ARM64 register capture */
	return -ENOSYS;
}

size_t uck_arch_reg_state_size(void)
{
	return 0;  /* Not yet implemented */
}

void uck_arch_flush_page(struct page *page)
{
	/* ARM64 requires explicit cache maintenance for DMA */
	/* TODO: Use arm64 cache flush APIs */
}

void uck_arch_mb(void)
{
	/* ARM64 full memory barrier */
	__asm__ __volatile__("dmb sy" ::: "memory");
}

const char *uck_arch_name(void)
{
	return "arm64";
}
