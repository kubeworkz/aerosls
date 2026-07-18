/*
 * timi_x86.h — Phase 3 TIMI-to-x86-64 native translator (SLIC's first real
 * target). Portable, no-libc core: compiles unmodified under host gcc (for
 * the timi-jit-test harness) and under a freestanding x86_64 kernel build
 * (kernel/timi_translate.c wraps this with kernel-specific buffer/mapping
 * calls). See AeroSLS-TIMI-ISA-v0.1.md §7 Phase 3 and §9 for the design.
 *
 * ─── Phase 3 v1 scope, stated up front ────────────────────────────────────
 *   - Naive load-operate-store codegen: every symbolic register is a memory
 *     slot in the procedure's stack frame (rbp-relative), not a real
 *     allocated host register. Correct, not fast — a first cut proves the
 *     translate-and-run pipeline works; real register allocation is future
 *     work, not required to validate the architecture.
 *   - TX_MAX_REGS (64) symbolic registers per procedure, not the ISA's full
 *     1024 ceiling (§8 Q1). A documented v1 narrowing, same spirit as the
 *     Phase 1 reference interpreter's own documented simplifications.
 *   - Arithmetic ops operate on full 64-bit slots regardless of the
 *     instruction's declared type width (no truncation to i8/i16/i32) —
 *     intentionally matching the Phase 1 interpreter's behavior so the two
 *     implementations agree on every test. LOAD/STORE, by contrast, DO
 *     honor the declared width/signedness, also matching the interpreter.
 *   - Register r7 is reserved, by Phase 3 trampoline convention, as a
 *     pointer to a small legitimately-mapped scratch buffer. TIMI has no
 *     real object/pointer allocation model yet (that's §7 Phase 6/7), so a
 *     translated program has no other way to obtain a valid memory address
 *     to LOAD/STORE against. This is a stopgap, not part of the ISA spec.
 *   - v0.3 (Phase 6): register r6 is reserved, mirroring r7's convention,
 *     as namepool_ptr — a pointer straight into the TIMI object's own raw
 *     name-pool bytes (no copying). RESOLVE reads its operand's name
 *     directly out of there. OP_RESOLVE/OP_OBJSIZE/OP_OBJTYPE are the
 *     first opcodes that make real C-ABI calls out of translated code —
 *     to timi_rt_resolve/timi_rt_objsize/timi_rt_objtype, whose addresses
 *     the caller bakes in as translate-time constants (see below), the
 *     same way scratch_ptr gets baked into every trampoline. Each runtime
 *     call is wrapped in an explicit rsp realign (save/and -16/restore)
 *     since, unlike this file's own intra-object CALLs, the callee here is
 *     a normal compiler-generated C function that may assume strict SysV
 *     16-byte stack alignment (e.g. for stack-local SSE spills).
 *   - Calls are intra-object only (CALL targets a label in the same
 *     translated instruction stream), matching Phase 1's interpreter.
 *   - DIV/MOD-by-zero and signed MIN/-1 overflow are left to real x86-64
 *     hardware #DE (divide error) rather than software-checked traps. The
 *     ISA spec's §4.7 contract calls these trap conditions; on real
 *     hardware that's exactly what happens, but AeroSLS does not yet wire
 *     an ISR0 handler for user processes, so an unhandled #DE today likely
 *     hangs or resets rather than delivering a clean fault to the process.
 *     Flagged here rather than silently ignored.
 */
#ifndef TIMI_X86_H
#define TIMI_X86_H

#include <stdint.h>
#include <stddef.h>

/* Byte-identical to kernel/loader.h's TimiObjectHeader/TxEntryRec and the
 * host toolchain's timi_isa.h TimiObject on-disk form. Duplicated (not
 * #included) so this file has zero dependency on either tree.
 *
 * v0.3 (Phase 6): num_names + TxNameRec added, mirroring timi_isa.h's
 * num_names/TimiName. Breaking change from v0.2's 4-field header — see
 * timi_isa.h's top comment for the rationale (no back-compat shim). */
struct TimiObjHdr {
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
 * timi_isa.h's TimiName. RESOLVE's operand is an index into the array of
 * these that sits in the object right after the entry table. */
struct TxNameRec {
    char name[32];
} __attribute__((packed));

#define TIMI_X_MAGIC        0x314D4954u   /* "TIM1" */
#define TX_MAX_REGS         64            /* symbolic registers per procedure, see above */
#define TX_SCRATCH_ARG_IDX  7             /* r7 = trampoline-provided scratch pointer */
#define TX_NAMEPOOL_ARG_IDX 6             /* r6 = trampoline-provided namepool_ptr (v0.3) */
#define TX_NAME_SIZE        32            /* must match struct TxNameRec / TIMI_MAX_NAME */

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
};

/* Translates the TIMI object at obj_data[0..obj_size) into x86-64 machine
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
 *   rt_resolve_fn  — address of `uint64_t timi_rt_resolve(const char*)`,
 *                    called by RESOLVE with rdi = namepool_ptr + idx*32.
 *   rt_objsize_fn  — address of `uint64_t timi_rt_objsize(uint64_t)`,
 *                    called by OBJSIZE with rdi = the T_OBJREF value.
 *   rt_objtype_fn  — address of `uint64_t timi_rt_objtype(uint64_t)`,
 *                    called by OBJTYPE with rdi = the T_OBJREF value.
 * Every environment binds these three to something different (the kernel
 * to real object_catalog[] queries, this host toolchain's test harnesses
 * to small mock catalogs) — see AeroSLS-TIMI-ISA-v0.1.md §13.
 *
 * Returns one of the TX_ERR_* codes above on failure.
 */
int timi_x86_translate(const uint8_t* obj_data, uint32_t obj_size,
                        uint8_t* out_buf, uint32_t out_cap,
                        const char* entry_name, uint64_t scratch_ptr,
                        uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                        uint64_t rt_objtype_fn,
                        uint32_t* out_len, uint32_t* entry_off);

const char* timi_x86_strerror(int code);

#endif /* TIMI_X86_H */
