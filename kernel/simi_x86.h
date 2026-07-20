/*
 * simi_x86.h — Phase 3 SIMI-to-x86-64 native translator (SLIC's first real
 * target). Portable, no-libc core: compiles unmodified under host gcc (for
 * the simi-jit-test harness in the Phase 1/3 host toolchain) and under this
 * freestanding x86_64 kernel build. kernel/simi_translate.c wraps this with
 * kernel-specific buffer/mapping calls. See AeroSLS-SIMI-ISA-v0.1.md §7
 * Phase 3 and §9 for the design.
 *
 * This file is a direct, unmodified copy of the host toolchain's
 * simi_x86.h — see that copy for the full "Phase 3 v1 scope, stated up
 * front" design-notes block (naive load-operate-store codegen, TX_MAX_REGS
 * cap, r7 scratch-pointer convention, intra-object-only calls, hardware
 * #DE for div traps) plus the v0.3/Phase 6 addendum (r6 namepool_ptr
 * convention, RESOLVE/OBJSIZE/OBJTYPE runtime-call ABI). Keeping the two
 * copies byte-identical is deliberate: it's what makes the host JIT test
 * harness's verification meaningful for the kernel build — same encoder,
 * same bugs or lack thereof. See AeroSLS-SIMI-ISA-v0.1.md §13 for how this
 * kernel binds rt_resolve_fn/rt_objsize_fn/rt_objtype_fn to real
 * object_catalog[] queries (kernel/simi_runtime.c) where the host
 * toolchain's test harnesses bind them to small mock catalogs instead.
 */
#ifndef SIMI_X86_H
#define SIMI_X86_H

#include <stdint.h>
#include <stddef.h>

/* Byte-identical to kernel/loader.h's SimiObjectHeader/TxEntryRec and the
 * host toolchain's simi_isa.h SimiObject on-disk form. Duplicated (not
 * #included) so this file has zero dependency on either tree.
 *
 * v0.3 (Phase 6): num_names + TxNameRec added, mirroring simi_isa.h's
 * num_names/SimiName. Breaking change from v0.2's 4-field header — see
 * simi_isa.h's top comment for the rationale (no back-compat shim). */
struct SimiObjHdr {
    uint32_t magic;
    uint32_t num_instr;
    uint32_t num_literals;
    uint32_t num_entries;
    uint32_t num_names;    /* v0.3 */
} __attribute__((packed));

struct TxEntryRec {
    char     name[32];
    uint32_t offset;
} __attribute__((packed));

/* v0.3 (Phase 6): name-pool entry — fixed 32-byte slot, same shape as
 * simi_isa.h's SimiName. RESOLVE's operand is an index into the array of
 * these that sits in the object right after the entry table. */
struct TxNameRec {
    char name[32];
} __attribute__((packed));

#define SIMI_X_MAGIC        0x314D4954u   /* "TIM1" */
#define TX_MAX_REGS         64            /* symbolic registers per procedure, see above */
#define TX_SCRATCH_ARG_IDX  7             /* r7 = trampoline-provided scratch pointer */
#define TX_NAMEPOOL_ARG_IDX 6             /* r6 = trampoline-provided namepool_ptr (v0.3) */
#define TX_NAME_SIZE        32            /* must match struct TxNameRec / SIMI_MAX_NAME */

enum {
    TX_OK = 0,
    TX_ERR_BAD_HEADER,
    TX_ERR_TOO_MANY_INSTR,
    TX_ERR_REG_OUT_OF_RANGE,
    TX_ERR_BUF_FULL,
    TX_ERR_BAD_OPCODE,
    TX_ERR_TOO_MANY_FIXUPS,
    TX_ERR_ENTRY_NOT_FOUND,
    TX_ERR_LITERAL_OUT_OF_RANGE,
    TX_ERR_NAME_OUT_OF_RANGE,   /* v0.3: RESOLVE name-pool index out of range */
    TX_ERR_FLOAT_UNSUPPORTED,  /* Gap Remediation SIMI Phase 10: an operand
                                 * combination with no float meaning --
                                 * FLAG_IMM on a T_F32/T_F64 ADD/SUB/MUL/DIV
                                 * (a 28-bit immediate can't hold a
                                 * meaningful IEEE bit pattern), or MOD on a
                                 * float type (no hardware instruction on
                                 * any target, permanently, not a temporary
                                 * gap -- compose from DIV+MUL+SUB instead,
                                 * matching simi_interp.c's die() message). */
};

/* Translates the SIMI object at obj_data[0..obj_size) into x86-64 machine
 * code written into out_buf (capacity out_cap). On TX_OK, *out_len holds
 * the number of bytes written and *entry_off holds the byte offset (within
 * out_buf) of the trampoline for the entry point named entry_name.
 *
 * scratch_ptr is the value to pass in r7 for every invocation of this
 * entry point (see TX_SCRATCH_ARG_IDX above) — the caller (host test
 * harness or kernel loader) owns deciding what that memory actually is.
 *
 * v0.3 (Phase 6) additions:
 *   r6 (TX_NAMEPOOL_ARG_IDX) is baked in automatically as a pointer
 *   straight into obj_data's own name-pool bytes — not a copy, and not a
 *   caller-supplied parameter, since the translator already knows exactly
 *   where that pool lives within obj_data. This does mean obj_data must
 *   stay mapped/valid for as long as the translated code might run
 *   RESOLVE — true of every caller in this project (the host harnesses
 *   keep obj_data alive for the process lifetime; the kernel's object
 *   bytes live in the persistent binary store, not a transient buffer).
 *   rt_resolve_fn  — address of `uint64_t simi_rt_resolve(const char*)`,
 *                    called by RESOLVE with rdi = namepool_ptr + idx*32.
 *   rt_objsize_fn  — address of `uint64_t simi_rt_objsize(uint64_t)`,
 *                    called by OBJSIZE with rdi = the T_OBJREF value.
 *   rt_objtype_fn  — address of `uint64_t simi_rt_objtype(uint64_t)`,
 *                    called by OBJTYPE with rdi = the T_OBJREF value.
 * Every environment binds these three to something different (the kernel
 * to real object_catalog[] queries, this host toolchain's test harnesses
 * to small mock catalogs) — see AeroSLS-SIMI-ISA-v0.1.md §13.
 *
 * Returns one of the TX_ERR_* codes above on failure.
 */
int simi_x86_translate(const uint8_t* obj_data, uint32_t obj_size,
                        uint8_t* out_buf, uint32_t out_cap,
                        const char* entry_name, uint64_t scratch_ptr,
                        uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                        uint64_t rt_objtype_fn,
                        uint32_t* out_len, uint32_t* entry_off);

const char* simi_x86_strerror(int code);

#endif /* SIMI_X86_H */
