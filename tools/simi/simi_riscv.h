/*
 * simi_riscv.h — Phase 5 SIMI-to-RV64 native translator: SLIC's second real
 * target, proving the same unmodified .tmo bytecode object retargets to a
 * different ISA without touching a single byte of the SIMI object itself.
 * That's the concrete claim behind "re-translation-on-migration" in the
 * roadmap (AeroSLS-SIMI-ISA-v0.1.md §7 Phase 5) — this file and
 * simi_riscv.c are the proof, not just an assertion of it.
 *
 * Portable, no-libc core, structured to mirror simi_x86.c function-for-
 * function (same CodeBuf/fixup shape, same naive load-operate-store
 * register model, same r7 scratch-pointer convention) so the two
 * translators are easy to audit against each other. See simi_riscv.c's
 * top comment and AeroSLS-SIMI-ISA-v0.1.md §12 for the RV64-specific
 * design decisions (calling convention, the auipc+ld literal-pool
 * technique standing in for x86's movabs, branch/call encoding).
 *
 * This header intentionally does NOT reuse simi_x86.h's struct/macro
 * names (SimiObjHdr, TxEntryRec, SIMI_X_MAGIC) even though the on-disk
 * layout they describe is identical — a future tool that wants to compare
 * both targets' output in one translation unit should be able to
 * `#include` both headers without a redefinition error (this project hit
 * exactly that class of bug once already, see §9's TxEntryRec collision
 * with loader.h — not repeating it here).
 *
 * v0.3 (Phase 7) adds the same capability-tag mechanism as simi_x86.c —
 * a per-register tag byte enforcing that T_OBJREF values are tagged
 * (unforgeable) and only RESOLVE/MOV can produce a tagged register — no
 * public-API change here either, entirely internal to simi_riscv.c's
 * codegen. Verified via tests/cap_forge.simi executing identically on
 * this target through rv64_exec.c. See AeroSLS-SIMI-ISA-v0.1.md §14.
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
