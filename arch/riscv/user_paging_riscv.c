/*
 * user_paging_riscv.c — see user_paging_riscv.h for the design notes,
 * scope, and the "not a live path yet" caveat on user_clone_page_table().
 * Gap Remediation SIMI Phase 9 (sub-phase 9b remainder).
 */
#include "user_paging_riscv.h"
#include "paging_riscv.h"
#include <stddef.h>

extern void* allocate_physical_ram_frame(void);
extern uint64_t* walk_page_tables_sv39(uint64_t root_pt_phys, uint64_t virtual_address);

/* ─── user_clone_page_table ────────────────────────────────────────────────
 * Direct Sv39 analog of arch/x86/user_paging.c's user_clone_page_table():
 * allocate a fresh root table, copy the CURRENT root table's entries into
 * it (Sv39's root level has 512 entries too, same shape as x86's PML4),
 * so kernel-half mappings stay reachable after a future satp switch into
 * a per-process address space. Unlike the x86 version, this does not
 * split "kernel indices" from "user indices" by number — Sv39's canonical
 * split is usually done by placing the kernel in the top of the negative/
 * higher half of the 39-bit VA space, but since nothing in this kernel
 * has chosen that layout yet (paging is Bare everywhere today — see this
 * file's header comment), copying every entry unconditionally (mirroring
 * x86's own "copy all 512, rely on U=0 to keep Ring-3 out of the kernel
 * half" approach) is the correct placeholder: harmless once a real kernel
 * mapping exists to copy, inert (copies 512 zero entries) until then. */
uint64_t user_clone_page_table(void) {
    uint64_t satp;
    __asm__ volatile("csrr %0, satp" : "=r"(satp));
    uint64_t* current_root = (uint64_t*)(uintptr_t)((satp & 0xFFFFFFFFFFFULL) << 12);

    uint64_t* new_root = (uint64_t*)allocate_physical_ram_frame();
    if (!new_root) return 0;

    for (int i = 0; i < 512; i++) {
        new_root[i] = current_root ? current_root[i] : 0;
    }

    return (uint64_t)(uintptr_t)new_root;
}

/* ─── user_map_page ────────────────────────────────────────────────────────
 * walk_page_tables_sv39() already does the real work (3-level Sv39 walk,
 * allocating intermediate tables as needed) — this just translates the
 * USER_PTE_* flag convention into real Sv39 PTE bits and writes the leaf.
 * A leaf Sv39 PTE requires PTE_V and at least one of R/W/X set (that's
 * what makes it a leaf rather than a pointer to the next level) — every
 * mapping through this function is therefore always Valid+Readable, with
 * Write/Exec/User layered on per the caller's flags, matching the exact
 * same "always readable once present" behavior x86's PTE encoding gives
 * for free (x86 has no separate R bit at all). */
void user_map_page(uint64_t* root_pt, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    if (!root_pt) return;
    if (!(flags & USER_PTE_PRESENT)) return;

    uint64_t* leaf = walk_page_tables_sv39((uint64_t)(uintptr_t)root_pt, vaddr);
    if (!leaf) return;

    uint64_t pte = PTE_V | PTE_R | PTE_A | PTE_D;
    if (flags & USER_PTE_WRITE) pte |= PTE_W;
    if (flags & USER_PTE_USER)  pte |= PTE_U;
    if (flags & USER_PTE_EXEC)  pte |= PTE_X;

    *leaf = PA_TO_PTE(paddr) | pte;
}
