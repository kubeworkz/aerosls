/*
 * simi_runtime.h — Phase 6 (v0.3): the kernel's real binding for SIMI's
 * object-typed runtime calls. Translated x86-64 code emitted by
 * kernel/simi_x86.c calls these three functions directly (real C-ABI
 * calls, addresses baked in as translate-time constants — see
 * simi_x86.h's top comment) whenever a SIMI program executes
 * RESOLVE/OBJSIZE/OBJTYPE.
 *
 * This is the kernel-specific half of a pattern repeated across every
 * environment that executes v0.3 SIMI: the host toolchain's Phase 1
 * interpreter (simi_interp.c) and its native-target test harnesses
 * (simi_jit_test.c, simi_riscv_verify.c) all bind RESOLVE/OBJSIZE/OBJTYPE
 * to small mock catalogs with the same three names/values, so cross-
 * implementation test agreement is meaningful. This file is the one place
 * in the whole project where those calls resolve against the REAL SLS
 * object_catalog[] instead of a mock — see AeroSLS-SIMI-ISA-v0.1.md §13.
 */
#ifndef SIMI_RUNTIME_H
#define SIMI_RUNTIME_H

#include <stdint.h>

/* rD = base_vaddr of the active object_catalog[] entry named `name`
 * (a raw pointer straight into the calling SIMI object's own name-pool
 * bytes — see simi_x86.h's TX_NAMEPOOL_ARG_IDX), or 0 if no active object
 * with that name exists. Names longer than 31 characters can never match
 * (SIMI's name-pool slot is a fixed 32-byte NUL-terminated buffer, see
 * simi_isa.h's SIMI_MAX_NAME) — a v1 narrowing versus object_catalog's
 * own 64-byte OBJECT_NAME_LEN, flagged not silently truncated-and-matched.
 */
uint64_t simi_rt_resolve(const char* name);

/* rD = byte size (size_pages * 4096) of the active object_catalog[] entry
 * whose base_vaddr equals `base_vaddr`, or 0 if none matches (including
 * base_vaddr == 0, RESOLVE's own "not found" sentinel — never a valid
 * object address). */
uint64_t simi_rt_objsize(uint64_t base_vaddr);

/* rD = SLSObjectType tag (see object_catalog.h) of the active
 * object_catalog[] entry whose base_vaddr equals `base_vaddr`, or
 * 0xFFFFFFFF if none matches. */
uint64_t simi_rt_objtype(uint64_t base_vaddr);

#endif /* SIMI_RUNTIME_H */
