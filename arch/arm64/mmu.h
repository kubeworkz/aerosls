/* arch/arm64/mmu.h — M4c/M5: the VMSAv8-64 maps (plan doc §6 M4c/M5,
 * §10.190/§10.191).
 *
 * The KERNEL map is built in boot_arm64.S (assembly, MMU-off): a
 * TTBR1 tree of 2 MiB blocks + the UART device page, so the kernel
 * lives in the high half. mmu.c owns the USER half: the TTBR0 table
 * tree for the EL0 SIMI program, built by C once the kernel runs. */
#ifndef ARCH_ARM64_MMU_H
#define ARCH_ARM64_MMU_H

#include <stdint.h>

/* The kernel is linked at 0xFFFF000040080000 (VMA) and loaded at
 * 0x40080000 (LMA): physical = virtual - KERNEL_VIRT_OFF. Parentheses
 * are load-bearing: unparenthesized, `p - KERNEL_VIRT_OFF` would
 * expand to `p - 0xFFFF000040080000ULL - 0x40080000ULL` — subtracting
 * the SUM — and to_phys() would produce sign-extended garbage
 * (0xFFFFFFFFC146E003 in the user L0[0], the first M5 table bug,
 * §10.191). */
#define KERNEL_VIRT_OFF (0xFFFF000040080000ULL - 0x40080000ULL)

/* The EL0 SIMI program's VA layout — deliberately in the low TTBR0
 * half at addresses the kernel's own tables never map (the proof is
 * the [M5] walk line: kernel-root walk of these VAs returns 0). */
#define USER_CODE_VA      0x10000000ULL   /* the translated A64 words */
#define USER_STACK_VA     0x10001000ULL   /* 8 KiB EL0 stack + frame */
#define USER_STACK_TOP_VA (USER_STACK_VA + 0x2000ULL)
#define USER_STUB_VA      0x10004000ULL   /* user-mode `svc #0` stub */
#define USER_SCRATCH_VA   0x10005000ULL   /* r7 scratch (unused by smoke) */

/* Build the user TTBR0 tree: maps the code page (EL0 R+X), an 8 KiB
 * stack, the svc stub page (EL0 R+X) and a scratch page at the VAs
 * above, backed by kernel-BSS physical pages (code_pa is the physical
 * address of the translated-code buffer). All user pages are AP=01
 * (EL1 RW + EL0 RW) so EL0 can run them; code/stub are UXN=0
 * (executable), stack/scratch UXN=1. Call after translating. */
void mmu_build_user(uint64_t code_pa);

/* Physical address of the user root table (for msr ttbr0_el1), and
 * its kernel VA (for mmu_walk). */
uint64_t mmu_user_root_phys(void);
uint64_t mmu_user_root_va(void);

/* Kernel VA of the svc-stub page (the kernel writes the `svc #0` word
 * there and flushes the icache before mmu_build_user). */
void *mmu_user_stub_addr(void);

/* Walk a 4-level tree whose root is at kernel VA `root_va` and return
 * the leaf descriptor (L3 page, or an L1/L2 block), or 0 if any level
 * is invalid. */
uint64_t mmu_walk(uint64_t root_va, uint64_t va);

/* Walk the kernel (TTBR1) tree. */
uint64_t mmu_walk_kernel(uint64_t va);

#endif /* ARCH_ARM64_MMU_H */
