/*
 * arch/x86_64/uck_arch.c - x86_64 HAL implementation for UCK
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/cacheflush.h>

#include "../uck_arch.h"

u32 uck_arch_detect_features(void)
{
	u32 features = 0;

	if (boot_cpu_has(X86_FEATURE_XMM))
		features |= UCK_CPU_FEAT_SSE;
	if (boot_cpu_has(X86_FEATURE_XMM2))
		features |= UCK_CPU_FEAT_SSE2;
	if (boot_cpu_has(X86_FEATURE_XMM3))
		features |= UCK_CPU_FEAT_SSE3;
	if (boot_cpu_has(X86_FEATURE_SSSE3))
		features |= UCK_CPU_FEAT_SSSE3;
	if (boot_cpu_has(X86_FEATURE_XMM4_1))
		features |= UCK_CPU_FEAT_SSE41;
	if (boot_cpu_has(X86_FEATURE_XMM4_2))
		features |= UCK_CPU_FEAT_SSE42;
	if (boot_cpu_has(X86_FEATURE_AVX))
		features |= UCK_CPU_FEAT_AVX;
	if (boot_cpu_has(X86_FEATURE_AVX2))
		features |= UCK_CPU_FEAT_AVX2;
	if (boot_cpu_has(X86_FEATURE_AVX512F))
		features |= UCK_CPU_FEAT_AVX512;
	if (boot_cpu_has(X86_FEATURE_AES))
		features |= UCK_CPU_FEAT_AES;
	if (boot_cpu_has(X86_FEATURE_PCLMULQDQ))
		features |= UCK_CPU_FEAT_PCLMUL;

	return features;
}

bool uck_arch_features_compatible(u32 src_features, u32 dst_features)
{
	/*
	 * Destination must have all features that the source process uses.
	 * Check that all source feature bits are present in destination.
	 */
	return (src_features & dst_features) == src_features;
}

int uck_arch_capture_regs(struct task_struct *task, void *buf, size_t buflen)
{
	struct pt_regs *regs;

	if (buflen < sizeof(struct pt_regs))
		return -EINVAL;

	regs = task_pt_regs(task);
	memcpy(buf, regs, sizeof(struct pt_regs));
	return sizeof(struct pt_regs);
}

size_t uck_arch_reg_state_size(void)
{
	return sizeof(struct pt_regs);
}

void uck_arch_flush_page(struct page *page)
{
	/* x86 has coherent caches for DMA, but CLFLUSH for RDMA */
	void *addr = page_address(page);
	if (addr)
		clflush_cache_range(addr, PAGE_SIZE);
}

void uck_arch_mb(void)
{
	mb();
}

const char *uck_arch_name(void)
{
	return "x86_64";
}
