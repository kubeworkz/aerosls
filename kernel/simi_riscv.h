/*
 * simi_riscv.h — SIMI-to-RV64 native translator (kernel copy).
 *
 * This file is a direct, unmodified copy of the host toolchain's
 * simi_riscv.h — see that copy (tools/simi/simi_riscv.h) for the full
 * design-notes block (portable no-libc core structured to mirror
 * simi_x86.h/.c function-for-function, the r7/r6 scratch-pointer/
 * namepool-pointer conventions, the v0.3 capability-tag mechanism, and
 * the Gap Remediation Phase 10 float-rejection rationale). Keeping the
 * two copies byte-identical is deliberate: it's what makes the host
 * toolchain's rv64_exec.c-based verification (see AeroSLS-SIMI-ISA-v0.1.md
 * §12) meaningful for this kernel copy too — same encoder, same bugs or
 * lack thereof.
 *
 * Gap Remediation SIMI Phase 9: this is the "groundwork" half of RISC-V
 * kernel wiring — the translator itself now lives in the freestanding
 * kernel build (see kernel/simi_riscv.c and the RV_C_SRC Makefile entry).
 * Unlike kernel/simi_x86.c, there is deliberately no kernel/simi_translate_
 * riscv.c-equivalent glue yet: that would need a real RISC-V user-mode
 * paging path (arch/riscv/paging_riscv.h + walk_page_tables_riscv.c handle
 * kernel-side page tables today, not a per-process user address space), an
 * activation cache, and a RISC-V object-catalog/syscall-dispatch/exit-stub
 * story — exactly the "process/loader/object-catalog infrastructure that
 * doesn't exist yet" the roadmap doc named as this phase's blocker. See
 * AeroSLS-SIMI-ISA-v0.1.md §16 Phase 9 for the honest scope statement.
 */
#ifndef SIMI_RISCV_H
#define SIMI_RISCV_H

#include <stdint.h>
#include <stddef.h>

/* v0.3 (Phase 6): num_names + TxNameRecRV added, mirroring simi_x86.h's
 * SimiObjHdr/TxNameRec. Same breaking change as the x86 side — see
 * simi_isa.h's top comment. */
struct SimiObjHdrRV {
    uint32_t magic;
    uint32_t num_instr;
    uint32_t num_literals;
    uint32_t num_entries;
    uint32_t num_names;    /* v0.3 */
} __attribute__((packed));

struct TxEntryRecRV {
    char     name[32];
    uint32_t offset;
} __attribute__((packed));

struct TxNameRecRV {
    char name[32];
} __attribute__((packed));

#define SIMI_RV_MAGIC        0x314D4954u   /* "TIM1" — same object format as x86's SIMI_X_MAGIC */
#define TX_RV_MAX_REGS        64            /* symbolic registers per procedure, same v1 narrowing as x86 (§10) */
#define TX_RV_SCRATCH_ARG_IDX 7              /* r7 = trampoline-provided scratch pointer, same convention as x86 */
#define TX_RV_NAMEPOOL_ARG_IDX 6              /* r6 = trampoline-provided namepool_ptr (v0.3), same convention as x86 */
#define TX_RV_NAME_SIZE        32             /* must match struct TxNameRecRV / SIMI_MAX_NAME */

enum {
    TX_RV_OK = 0,
    TX_RV_ERR_BAD_HEADER,
    TX_RV_ERR_TOO_MANY_INSTR,
    TX_RV_ERR_REG_OUT_OF_RANGE,
    TX_RV_ERR_BUF_FULL,
    TX_RV_ERR_BAD_OPCODE,
    TX_RV_ERR_TOO_MANY_FIXUPS,
    TX_RV_ERR_ENTRY_NOT_FOUND,
    TX_RV_ERR_LITERAL_OUT_OF_RANGE,
    TX_RV_ERR_TOO_MANY_LITERALS,
    TX_RV_ERR_BRANCH_OUT_OF_RANGE,
    TX_RV_ERR_NAME_OUT_OF_RANGE,   /* v0.3: RESOLVE name-pool index out of range */
    TX_RV_ERR_FLOAT_UNSUPPORTED,  /* Gap Remediation SIMI Phase 10: T_F32/
                                    * T_F64 on ADD/SUB/MUL/DIV/MOD/NEG/CMP.
                                    * RV64 float codegen is explicitly
                                    * scoped OUT of Phase 10 v1 (deferred to
                                    * ride alongside Phase 9's RISC-V kernel
                                    * wiring, which hasn't happened yet
                                    * either) -- rejected outright rather
                                    * than silently mis-executed as integer
                                    * ops on the raw IEEE bit pattern, which
                                    * is what would happen without this
                                    * check (this translator has no type
                                    * dispatch at all otherwise; every
                                    * arithmetic/compare opcode just reads
                                    * the 64-bit slot as a plain integer). */
};

/* Same contract as simi_x86_translate() (simi_x86.h) — see that file for
 * the full parameter description. scratch_ptr here plays the identical
 * role: the value the trampoline preloads into symbolic register 7 before
 * calling the entry procedure, standing in for real SIMI pointer
 * allocation (still §7 Phase 6/7 future work either way).
 *
 * v0.3 (Phase 6): rt_resolve_fn/rt_objsize_fn/rt_objtype_fn are addresses
 * baked in as translate-time constants (via the auipc+ld literal-pool
 * technique, same as any other 64-bit immediate here) and called via a
 * real `jalr ra,0(reg)` — RV64's direct analog of x86's movabs+call reg.
 * namepool_ptr (r6) is computed internally from obj_data, same as the x86
 * translator — not a parameter here either, for the same reason (see
 * simi_x86.h). */
int simi_riscv_translate(const uint8_t* obj_data, uint32_t obj_size,
                          uint8_t* out_buf, uint32_t out_cap,
                          const char* entry_name, uint64_t scratch_ptr,
                          uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                          uint64_t rt_objtype_fn,
                          uint32_t* out_len, uint32_t* entry_off);

const char* simi_riscv_strerror(int code);

#endif /* SIMI_RISCV_H */
