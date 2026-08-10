/* arch/arm64/mmu.h — M4c: the VMSAv8-64 identity map (plan doc §6 M4c,
 * §10.187). Enables the MMU at EL1 with a 4-level 4 KiB page-table walk,
 * then exposes a walk for self-verification. */
#ifndef ARCH_ARM64_MMU_H
#define ARCH_ARM64_MMU_H

#include <stdint.h>

/* Build the page tables in BSS, program MAIR/TCR/TTBR0, and set
 * SCTLR_EL1.M|C|I. After this returns, every load/store/fetch goes
 * through the translation tables (identity for the kernel image region,
 * a device page for the PL011). Must be called with the MMU off (boot). */
void mmu_enable_identity(void);

/* Walk the built 4-level tables for va and return the leaf descriptor:
 * an L3 page descriptor (or an L2 block descriptor, which this kernel
 * does not use), or 0 if any level is invalid. Reads the tables through
 * the identity map, so it is only meaningful after mmu_enable_identity. */
uint64_t mmu_walk_va(uint64_t va);

#endif /* ARCH_ARM64_MMU_H */
