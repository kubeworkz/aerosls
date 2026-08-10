/* arch/arm64/mmu.c — M4c/M5: the VMSAv8-64 maps (plan doc §6 M4c/M5,
 * §10.190/§10.191).
 *
 * The KERNEL map is built in boot_arm64.S (assembly, MMU off, physical
 * addressing): one L0/L1/L2 tree with 2 MiB blocks over the image
 * region plus an L3 device page for the PL011, serving TTBR1 (the
 * kernel's high VAs) — the kernel is TTBR1-pure, with TTBR0 parked on
 * an all-invalid root (§10.191).
 *
 * This file owns the USER half: the TTBR0 tree for the EL0 SIMI
 * program. Its VAs (0x10000000..0x10005000, USER_* in mmu.h) live in
 * the low half where the kernel maps NOTHING — the [M5] selfcheck
 * proves it by walking the kernel root at a user VA and reading 0. The
 * user tree is a full 4-level 4 KiB walk (L0[0] -> L1[0] -> L2[128] ->
 * L3 with five pages), built by C now that the kernel runs translated.
 * AP=01 (EL1 RW + EL0 RW) on every user page; code and the svc stub
 * are UXN=0 (EL0-executable), the stack and scratch are UXN=1.
 *
 * mmu_walk() dereferences tables through the kernel VA space:
 * descriptors carry PHYSICAL addresses (the walker's native currency),
 * so pa + KERNEL_VIRT_OFF makes them readable C pointers. */
#include <stdint.h>

#include "arch/arm64/mmu.h"

/* ---- descriptor bits (VMSAv8-64): low 2 bits are 0b11 for a valid
 * table (L0-L2) / page (L3), 0b01 for a block (L1/L2) — they must stay
 * distinct constants; the first M4c walk bug was defining them both as
 * 0b11, dead-code-eliminating the L3 descent (§10.190). ---- */
#define DESC_VALID      0b11UL
#define DESC_BLOCK      0b01UL
#define DESC_AF         (1UL << 10)
#define DESC_ATTR0      (0UL << 2)      /* MAIR 0: device */
#define DESC_ATTR1      (1UL << 2)      /* MAIR 1: normal WBWA */
#define DESC_PXN        (1UL << 53)     /* no EL1 execute */
#define DESC_AP_EL0RW   (1UL << 6)      /* AP=0b01 (bits 7:6): EL1 RW + EL0 RW — bit 8 is SH, not AP (the first EL0 permission-fault bug, §10.191) */
#define DESC_UXN        (1UL << 54)     /* no EL0 execute */
#define DESC_ADDRMASK   (~0xFFFUL)

/* The kernel TTBR1 root (built by boot_arm64.S; the linker gives the
 * symbol its high VMA). */
extern uint64_t g_boot_pg_l0[];

/* ---- the user tree (kernel BSS, 4 KiB aligned) ---- */
static uint64_t g_user_pg_l0[512] __attribute__((aligned(4096)));
static uint64_t g_user_pg_l1[512] __attribute__((aligned(4096)));
static uint64_t g_user_pg_l2[512] __attribute__((aligned(4096)));
static uint64_t g_user_pg_l3[512] __attribute__((aligned(4096)));

/* EL0 backing pages: the stack (8 KiB), the svc stub, the scratch. The
 * kernel writes the stub's `svc #0` word before mapping (mmu.h). */
static uint8_t g_user_stack[0x2000] __attribute__((aligned(4096)));
static uint8_t g_user_stub[0x1000] __attribute__((aligned(4096)));
static uint8_t g_user_scratch[0x1000] __attribute__((aligned(4096)));

/* phys of a kernel-VA pointer: identity minus the link offset. */
static inline uint64_t to_phys(const void *p)
{
    return (uint64_t)(uintptr_t)p - KERNEL_VIRT_OFF;
}

void mmu_build_user(uint64_t code_pa)
{
    /* Descriptors carry PHYSICAL addresses — the table walker reads
     * physical memory, and the kernel VA of a table is never a valid
     * descriptor (the first M5 table bug would have been walking a
     * kernel VA as if it were a physical address). */
    uint64_t l1 = to_phys(g_user_pg_l1);
    uint64_t l2 = to_phys(g_user_pg_l2);
    uint64_t l3 = to_phys(g_user_pg_l3);

    g_user_pg_l0[0] = l1 | DESC_VALID;                  /* L0[0] -> L1 */
    g_user_pg_l1[0] = l2 | DESC_VALID;                  /* L1[0] -> L2 */
    g_user_pg_l2[(USER_CODE_VA >> 21) & 0x1FF] = l3 | DESC_VALID;

    /* Five L3 pages: code, stack x2, stub, scratch. Code and stub are
     * EL0-executable (UXN=0); stack and scratch are not. */
    g_user_pg_l3[(USER_CODE_VA >> 12) & 0x1FF] =
        code_pa | DESC_VALID | DESC_AF | DESC_ATTR1 | DESC_AP_EL0RW;
    g_user_pg_l3[(USER_STACK_VA >> 12) & 0x1FF] =
        to_phys(g_user_stack) | DESC_VALID | DESC_AF | DESC_ATTR1
        | DESC_AP_EL0RW | DESC_UXN;
    g_user_pg_l3[((USER_STACK_VA + 0x1000) >> 12) & 0x1FF] =
        to_phys(g_user_stack + 0x1000) | DESC_VALID | DESC_AF | DESC_ATTR1
        | DESC_AP_EL0RW | DESC_UXN;
    g_user_pg_l3[(USER_STUB_VA >> 12) & 0x1FF] =
        to_phys(g_user_stub) | DESC_VALID | DESC_AF | DESC_ATTR1
        | DESC_AP_EL0RW;
    g_user_pg_l3[(USER_SCRATCH_VA >> 12) & 0x1FF] =
        to_phys(g_user_scratch) | DESC_VALID | DESC_AF | DESC_ATTR1
        | DESC_AP_EL0RW | DESC_UXN;
}

uint64_t mmu_user_root_phys(void)
{
    return to_phys(g_user_pg_l0);
}

uint64_t mmu_user_root_va(void)
{
    return (uint64_t)(uintptr_t)g_user_pg_l0;
}

void *mmu_user_stub_addr(void)
{
    return g_user_stub;
}

uint64_t mmu_walk(uint64_t root_va, uint64_t va)
{
    const uint64_t *l0 = (const uint64_t *)(uintptr_t)root_va;
    uint64_t d0 = l0[(va >> 39) & 0x1FF];
    if ((d0 & 3) != DESC_VALID)
        return 0;
    const uint64_t *l1 = (const uint64_t *)(uintptr_t)
        ((d0 & DESC_ADDRMASK) + KERNEL_VIRT_OFF);
    uint64_t d1 = l1[(va >> 30) & 0x1FF];
    if ((d1 & 3) == DESC_BLOCK)
        return d1;                        /* 1 GiB block */
    if ((d1 & 3) != DESC_VALID)
        return 0;
    const uint64_t *l2 = (const uint64_t *)(uintptr_t)
        ((d1 & DESC_ADDRMASK) + KERNEL_VIRT_OFF);
    uint64_t d2 = l2[(va >> 21) & 0x1FF];
    if ((d2 & 3) == DESC_BLOCK)
        return d2;                        /* 2 MiB block */
    if ((d2 & 3) != DESC_VALID)
        return 0;
    const uint64_t *l3 = (const uint64_t *)(uintptr_t)
        ((d2 & DESC_ADDRMASK) + KERNEL_VIRT_OFF);
    uint64_t d3 = l3[(va >> 12) & 0x1FF];
    return (d3 & 3) == DESC_VALID ? d3 : 0;
}

uint64_t mmu_walk_kernel(uint64_t va)
{
    return mmu_walk((uint64_t)(uintptr_t)g_boot_pg_l0, va);
}
