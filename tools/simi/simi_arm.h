/*
 * simi_arm.h — M0: SIMI-to-AArch64 (A64) native translator, the third
 * real target. Phase 5 proved the same unmodified .tmo bytecode object
 * retargets from x86-64 to RV64; this proves it retargets to a third,
 * architecturally distinct ISA (fixed 32-bit RISC words, no flags in the
 * base ALU, PC-relative branches, a genuine SP register) without touching
 * a single byte of the SIMI object itself. See
 * docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md for the phase plan and
 * AeroSLS-SIMI-ISA-v0.1.md §7 Phase 5 for the re-translation-on-migration
 * roadmap claim this extends.
 *
 * Portable, no-libc core, structured to mirror simi_riscv.c function-for-
 * function (same CodeBuf/fixup shape, same naive load-operate-store
 * register model) so the three translators are easy to audit against each
 * other. The A64-specific design decisions are documented in simi_arm.c's
 * top comment: the frame base-register trick (x28 = x29 - 592, so every
 * slot/tag offset is a positive scaled-immediate), movz/movk replacing
 * RV64's entire auipc+ld literal-pool machinery, and CBZ/CBNZ + subs/cset
 * covering BC/CMP with only the NZCV flags as implicit state.
 *
 * This header intentionally does NOT reuse simi_x86.h / simi_riscv.h's
 * struct/macro names (SimiObjHdrRV, TxEntryRecRV, SIMI_RV_MAGIC) even
 * though the on-disk layout they describe is identical — a future tool
 * that wants to compare all three targets' output in one translation unit
 * should be able to `#include` all headers without a redefinition error
 * (this project hit exactly that class of bug once already, see §9's
 * TxEntryRec collision with loader.h — not repeating it here).
 */
#ifndef SIMI_ARM_H
#define SIMI_ARM_H

#include <stdint.h>
#include <stddef.h>

/* v0.3 (Phase 6): num_names + TxNameRecAR added, mirroring simi_x86.h's
 * SimiObjHdr/TxNameRec. Same breaking change as the other targets — see
 * simi_isa.h's top comment. */
struct SimiObjHdrAR {
    uint32_t magic;
    uint32_t num_instr;
    uint32_t num_literals;
    uint32_t num_entries;
    uint32_t num_names;    /* v0.3 */
} __attribute__((packed));

struct TxEntryRecAR {
    char     name[32];
    uint32_t offset;
} __attribute__((packed));

struct TxNameRecAR {
    char name[32];
} __attribute__((packed));

#define SIMI_AR_MAGIC        0x314D4954u   /* "TIM1" — same object format as SIMI_RV_MAGIC */
#define TX_AR_MAX_REGS        64            /* symbolic registers per procedure, same v1 narrowing as x86 (§10) */
#define TX_AR_SCRATCH_ARG_IDX 7              /* r7 = trampoline-provided scratch pointer, same convention as x86 */
#define TX_AR_NAMEPOOL_ARG_IDX 6              /* r6 = trampoline-provided namepool_ptr (v0.3), same convention as x86 */
#define TX_AR_NAME_SIZE        32             /* must match struct TxNameRecAR / SIMI_MAX_NAME */

enum {
    TX_AR_OK = 0,
    TX_AR_ERR_BAD_HEADER,
    TX_AR_ERR_TOO_MANY_INSTR,
    TX_AR_ERR_REG_OUT_OF_RANGE,
    TX_AR_ERR_BUF_FULL,
    TX_AR_ERR_BAD_OPCODE,
    TX_AR_ERR_TOO_MANY_FIXUPS,
    TX_AR_ERR_ENTRY_NOT_FOUND,
    TX_AR_ERR_LITERAL_OUT_OF_RANGE,
    TX_AR_ERR_TOO_MANY_LITERALS,
    TX_AR_ERR_BRANCH_OUT_OF_RANGE,
    TX_AR_ERR_NAME_OUT_OF_RANGE,   /* v0.3: RESOLVE name-pool index out of range */
    TX_AR_ERR_FLOAT_UNSUPPORTED,   /* Gap Remediation SIMI Phase 10: T_F32/
                                    * T_F64 on ADD/SUB/MUL/DIV/MOD/NEG/CMP
                                    * is scoped out of M0 (same decision as
                                    * the RV64 target — rejected outright
                                    * rather than silently mis-executed as
                                    * integer ops on the raw IEEE bit
                                    * pattern) */
};

/* Same contract as simi_riscv_translate() (simi_riscv.h) — see that file
 * for the full parameter description. scratch_ptr plays the identical
 * role: the value the trampoline preloads into symbolic register 7 before
 * calling the entry procedure.
 *
 * v0.3 (Phase 6): rt_resolve_fn/rt_objsize_fn/rt_objtype_fn are addresses
 * baked in as translate-time constants (via movz/movk, same as any other
 * 64-bit immediate here) and called via a real `blr xN` — A64's analog of
 * x86's movabs+call reg and RV64's auipc+ld+jalr. namepool_ptr (r6) is
 * computed internally from obj_data, same as the other translators — not
 * a parameter here either. */
int simi_arm_translate(const uint8_t* obj_data, uint32_t obj_size,
                        uint8_t* out_buf, uint32_t out_cap,
                        const char* entry_name, uint64_t scratch_ptr,
                        uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                        uint64_t rt_objtype_fn,
                        uint32_t* out_len, uint32_t* entry_off);

const char* simi_arm_strerror(int code);

#endif /* SIMI_ARM_H */
