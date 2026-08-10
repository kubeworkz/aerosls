/*
 * simi_arm.h — SIMI-to-AArch64 (A64) native translator (kernel copy).
 *
 * This file is a direct, unmodified copy of the host toolchain's
 * simi_arm.h — see that copy (tools/simi/simi_arm.h) for the full
 * framing block (the third real target after x86-64/RV64, the portable
 * no-libc core structured to mirror simi_riscv.h function-for-function,
 * the header-name independence that lets all three targets' headers
 * coexist in one translation unit, and the TX_AR_ERR_FLOAT_UNSUPPORTED
 * rejection boundary). Keeping the two copies byte-identical is
 * deliberate: it's what makes the host toolchain's qemu-aarch64
 * real-execution verification (M3, docs/AeroSLS-SIMI-ARM-Backend-Plan-
 * v0.1.md §10.178/10.179) meaningful for this kernel copy too — same
 * encoder, same bugs or lack thereof.
 *
 * Gap Remediation SIMI M3 (kernel half): the kernel-side copy of the
 * AArch64 translator, staged exactly like the Phase 5 RV64 kernel copy
 * (kernel/simi_riscv.h). Unlike the RV64 copy there is deliberately no
 * arm64 kernel build, so nothing links this yet — see kernel/simi_arm.c's
 * top comment for the full honest scope statement (the kernel-translate
 * glue, activation/spawn path, user-mode paging, and the
 * object-catalog/syscall-dispatch/exit-stub story all await an arm64
 * kernel target the roadmap does not grow).
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
    TX_AR_ERR_FLOAT_UNSUPPORTED,   /* Gap Remediation SIMI Phase 10: F1
                                    * landed ADD/SUB/MUL/DIV/NEG/CMP float
                                    * codegen (plan D4 GP-bounce); the two
                                    * remaining rejections are permanent
                                    * boundaries, not gaps: float MOD has
                                    * no instruction on any target (compose
                                    * DIV+MUL+SUB instead), and a float op
                                    * with an immediate operand has no
                                    * float meaning (x86 parity). RV64
                                    * still rejects the whole family. */
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
/* M2.71: the safety net's trip count (the M2.18/M2.70 2-cycle net in
 * simi_arm.c's fixpoint loop; default 16, tunable) and the fixpoint's
 * scan count for the last translate. Exposed so bench_net.c can measure
 * the M2.70 translate-time win (16 vs the M2.69-era 512) from one
 * binary — see bench_net.c's top comment. */
extern int g_ar_net_trip;
extern int g_ar_net_scans;

int simi_arm_translate(const uint8_t* obj_data, uint32_t obj_size,
                        uint8_t* out_buf, uint32_t out_cap,
                        const char* entry_name, uint64_t scratch_ptr,
                        uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                        uint64_t rt_objtype_fn,
                        uint32_t* out_len, uint32_t* entry_off);

const char* simi_arm_strerror(int code);

#endif /* SIMI_ARM_H */
