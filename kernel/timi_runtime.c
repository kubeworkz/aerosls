/*
 * timi_runtime.c — Phase 6 (v0.3) real object-catalog runtime for TIMI's
 * RESOLVE/OBJSIZE/OBJTYPE opcodes. See timi_runtime.h for the contract and
 * AeroSLS-TIMI-ISA-v0.1.md §13 for the design. No libc dependency, same
 * discipline as every other kernel source file that touches translated
 * TIMI code paths.
 *
 * Phase 7 adds an authority check to timi_rt_resolve(): before handing
 * back a base_vaddr (which the x86/RV64 translators will then tag as a
 * capability — see timi_x86.h's Phase 7 notes), it looks up the calling
 * process's owner_uid via process_find_current() and calls the existing
 * catalog_check_access(uid, name, PERM_READ) gate. Denial is made to
 * look exactly like "not found" (returns 0) — a distinct "denied" result
 * would let a process fish for which object names exist by RESOLVE-ing
 * guesses and comparing "not found" vs "exists but denied", even though
 * it could never get a capability for anything it can't already read.
 * See AeroSLS-TIMI-ISA-v0.1.md §14.
 */
#include "timi_runtime.h"
#include "object_catalog.h"
#include "process.h"
#include "../user/permissions.h"

#define TIMI_RT_NAME_MAXLEN 32   /* must match timi_isa.h's TIMI_MAX_NAME */
#define TIMI_RT_PAGE_SIZE   4096ULL

/* Bounded name compare: `name_ptr` is a raw pointer into the calling TIMI
 * object's own name-pool bytes (see timi_x86.h's TX_NAMEPOOL_ARG_IDX) —
 * guaranteed NUL-terminated within TIMI_RT_NAME_MAXLEN bytes by the
 * assembler that produced the object (tools/timi/timi_asm.c's
 * find_or_add_name always writes an explicit terminator), so this loop
 * never reads past that 32-byte slot. `catalog_name` is an
 * object_catalog[] entry's name[OBJECT_NAME_LEN] (64 bytes), always safe
 * to read the first 32 bytes of regardless of its own contents. */
static int rt_name_matches(const char* name_ptr, const char* catalog_name) {
    for (int i = 0; i < TIMI_RT_NAME_MAXLEN; i++) {
        char a = name_ptr[i];
        char b = catalog_name[i];
        if (a != b) return 0;
        if (a == '\0') return 1;
    }
    /* name_ptr ran the full 32 bytes without a NUL — shouldn't happen
     * given the assembler's format guarantee, but if it ever does, treat
     * it as "no match" rather than risk comparing catalog_name past index
     * 31 against bytes we can no longer vouch for. */
    return 0;
}

uint64_t timi_rt_resolve(const char* name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (rt_name_matches(name, object_catalog[i].name)) {
            /* Phase 7: authority check before minting a capability. No
             * Ring-3 process running (process_find_current() == NULL)
             * means this call originated from pure kernel context —
             * treat that as uid 0 / ROLE_SYSTEM_KERNEL, which
             * catalog_check_access() always passes. Denial returns 0,
             * identical to "not found" (see the top-of-file comment). */
            struct ProcessDescriptor* cur = process_find_current();
            uint32_t uid = cur ? cur->owner_uid : 0;
            if (!catalog_check_access(uid, object_catalog[i].name, PERM_READ))
                return 0;
            return object_catalog[i].base_vaddr;
        }
    }
    return 0;
}

uint64_t timi_rt_objsize(uint64_t base_vaddr) {
    if (base_vaddr == 0) return 0;   /* RESOLVE's own "not found" sentinel */
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (object_catalog[i].base_vaddr == base_vaddr) {
            return (uint64_t)object_catalog[i].size_pages * TIMI_RT_PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t timi_rt_objtype(uint64_t base_vaddr) {
    if (base_vaddr == 0) return 0xFFFFFFFFull;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (object_catalog[i].base_vaddr == base_vaddr) {
            return (uint64_t)object_catalog[i].type;
        }
    }
    return 0xFFFFFFFFull;
}
