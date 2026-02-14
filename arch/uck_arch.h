/*
 * arch/uck_arch.h - Hardware Abstraction Layer for UCK
 *
 * Provides an ISA-independent interface for architecture-specific
 * operations: register capture/restore, CPU feature detection,
 * cache management, and memory barriers.
 */

#ifndef _UCK_ARCH_H_
#define _UCK_ARCH_H_

#include <linux/types.h>

/* Maximum register state size in bytes */
#define UCK_MAX_REG_STATE 512

/* CPU feature flags (bitmask) */
#define UCK_CPU_FEAT_SSE     (1 << 0)
#define UCK_CPU_FEAT_SSE2    (1 << 1)
#define UCK_CPU_FEAT_SSE3    (1 << 2)
#define UCK_CPU_FEAT_SSSE3   (1 << 3)
#define UCK_CPU_FEAT_SSE41   (1 << 4)
#define UCK_CPU_FEAT_SSE42   (1 << 5)
#define UCK_CPU_FEAT_AVX     (1 << 6)
#define UCK_CPU_FEAT_AVX2    (1 << 7)
#define UCK_CPU_FEAT_AVX512  (1 << 8)
#define UCK_CPU_FEAT_AES     (1 << 9)
#define UCK_CPU_FEAT_PCLMUL  (1 << 10)

/* HAL interface - implemented per architecture */

/* Detect CPU features, returns bitmask of UCK_CPU_FEAT_* */
u32 uck_arch_detect_features(void);

/* Check if destination CPU features are compatible with source */
bool uck_arch_features_compatible(u32 src_features, u32 dst_features);

/* Capture register state from a task */
int uck_arch_capture_regs(struct task_struct *task, void *buf, size_t buflen);

/* Get size of register state for current architecture */
size_t uck_arch_reg_state_size(void);

/* Flush cache for a page (needed before DMA/RDMA) */
void uck_arch_flush_page(struct page *page);

/* Full memory barrier (architecture-specific) */
void uck_arch_mb(void);

/* Architecture name string */
const char *uck_arch_name(void);

#endif /* _UCK_ARCH_H_ */
