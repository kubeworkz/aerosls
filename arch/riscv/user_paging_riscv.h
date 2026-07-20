/*
 * user_paging_riscv.h — Gap Remediation SIMI Phase 9 (sub-phase 9b
 * remainder): RV64 Sv39 counterpart of arch/x86/user_paging.h's paging
 * half. See AeroSLS-SIMI-ISA-v0.1.md §16 Phase 9 for the full scope
 * statement.
 *
 * Deliberately mirrors arch/x86/user_paging.h's function NAMES
 * (user_clone_page_table / user_map_page) and PTE-flag macro NAMES
 * (USER_PTE_PRESENT / USER_PTE_WRITE / USER_PTE_USER / USER_PTE_EXEC /
 * USER_PTE_NOEXEC), even though the underlying bit layouts differ (x86's
 * present/NX-bit scheme vs. Sv39's valid+R/W/X scheme) — so that a future
 * arch-neutral caller (the eventual kernel/simi_translate.c-for-RISCV
 * analog, or a ported kernel/process.c) can call the same four symbols
 * regardless of which of arch/x86/user_paging.h or this header got
 * compiled in for the target, with no #ifdef at the call site. This is
 * the same "swap the whole implementation file, keep the call sites
 * unchanged" philosophy this project already used for
 * simi_x86.c/simi_riscv.c.
 *
 * NOT done here (see AeroSLS-SIMI-ISA-v0.1.md §16 Phase 9 for the honest
 * accounting): there is no syscall_gate_init()-equivalent in this file —
 * unlike x86's SYSCALL/SYSRET MSR setup, RISC-V's ecall trap path is a
 * genuinely different mechanism (stvec + sscratch, not per-syscall MSRs)
 * and belongs with the new trap-entry work (arch/riscv/trap_riscv.{S,c}),
 * not bundled into the paging file the way x86 happened to combine both
 * concerns into one user_paging.c. Also NOT done: actually flipping Sv39
 * paging on at boot (writing satp) — this header/implementation provide
 * the mapping API a future caller would need, but nothing in this
 * project's boot path calls it yet, so the kernel still runs in Bare
 * (paging-off) mode exactly as it always has. user_clone_page_table_
 * riscv() below is therefore unreachable/untested code today, not a
 * verified live path — stated plainly rather than left implicit.
 */
#ifndef USER_PAGING_RISCV_H
#define USER_PAGING_RISCV_H

#include <stdint.h>

/* ─── Page flag constants for user-space mappings ─────────────────────────
 * Sv39 PTE bits (paging_riscv.h's PTE_V/R/W/X/U/A/D) are lower-level than
 * these — USER_PTE_* here compose them the way a caller actually wants to
 * think about a mapping (present/writable/user/executable), same
 * abstraction level as arch/x86/user_paging.h's USER_PTE_* macros. Every
 * leaf mapping this file creates is Valid + Readable by construction
 * (Sv39 has no "writable but not readable" leaf encoding — R=0,W=1 is
 * reserved/invalid per the privileged spec — so USER_PTE_WRITE always
 * implies PTE_R too, handled inside user_map_page()). */
#define USER_PTE_PRESENT  (1ULL << 0)   /* -> PTE_V (+ PTE_R, see above) */
#define USER_PTE_WRITE    (1ULL << 1)   /* -> PTE_W */
#define USER_PTE_USER     (1ULL << 2)   /* -> PTE_U */
#define USER_PTE_ACCESSED (1ULL << 5)   /* -> PTE_A, pre-set so hardware/SBI
                                          * emulation never needs to fault
                                          * just to set it (mirrors x86 side
                                          * leaving ACCESSED unused today) */
#define USER_PTE_DIRTY    (1ULL << 6)   /* -> PTE_D, same rationale */
/* Sv39 has an explicit X bit (unlike x86's "absence of NX"): EXEC sets it,
 * NOEXEC leaves it clear. Named to match x86's USER_PTE_EXEC/NOEXEC call-
 * site shape even though the polarity story underneath is the mirror
 * image (x86: NX bit set = non-exec; Sv39: X bit SET = exec). */
#define USER_PTE_EXEC     (1ULL << 3)   /* -> PTE_X */
#define USER_PTE_NOEXEC   0ULL          /* no PTE_X = non-executable */

#define USER_PTE_FRAME_MASK 0x000FFFFFFFFFF000ULL

/* Clone the current kernel Sv39 root table, copying kernel-half entries
 * only, exactly mirroring arch/x86/user_paging.h's user_clone_page_table()
 * contract. Returns the physical address of the new root table (suitable
 * for a future satp write), or 0 on failure.
 *
 * NOT a live path yet — see this header's top comment. Reads satp to find
 * the "current" root table; if paging is still Bare (satp.MODE == 0,
 * which is true everywhere in this kernel today), satp's PPN field is
 * meaningless and this function has nothing real to clone from. Callers
 * must not invoke this until something has actually turned Sv39 paging on
 * — not yet true anywhere in this project. */
uint64_t user_clone_page_table(void);

/* Map one 4-KiB page in the given Sv39 root table with the specified
 * USER_PTE_* flags. Allocates intermediate levels from the physical frame
 * pool as needed (delegates to walk_page_tables_sv39(), which already does
 * exactly this — see arch/riscv/walk_page_tables_riscv.c). Mirrors
 * arch/x86/user_paging.h's user_map_page() contract exactly (same
 * parameter order/meaning: root table pointer, vaddr, paddr, flags). */
void user_map_page(uint64_t* root_pt, uint64_t vaddr, uint64_t paddr, uint64_t flags);

#endif /* USER_PAGING_RISCV_H */
