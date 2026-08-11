#ifndef SIMI_INTERP_H
#define SIMI_INTERP_H

#include <stdint.h>

/*
 * simi_interp.h — in-kernel SIMI interpreter: a SECOND, checkpointable
 * execution mode alongside the existing AOT translator.
 *
 * ─── Why this exists ──────────────────────────────────────────────────
 * The kernel already runs SIMI by translating it to native x86-64
 * (kernel/simi_translate.c -> simi_x86.c) and jumping to it. That is fast
 * and stays the default. But native code cannot be checkpointed at an
 * arbitrary point: there is no portable, serialisable representation of
 * "where a running x86-64 function is" -- which is exactly the wall
 * ucontext/setjmp designs hit.
 *
 * An interpreter has one. This file's `struct SimiContext` IS the
 * execution state, in full, and every field in it is plain data:
 *
 *   - `pc` is an INSTRUCTION INDEX, not an address. It survives being
 *     written to disk, read back on a different machine, or moved to a
 *     node with a different code layout.
 *   - the register file is plain integers -- no host pointers, no stack
 *     addresses, nothing that means anything only in this process.
 *   - the call stack is an ARRAY with explicit caller links, not the host
 *     C call stack. Unwinding is data, not control flow.
 *   - memory is one flat array.
 *
 * Checkpointing this is writing three arrays and two scalars. Nothing has
 * to be reconstructed. That property is the whole reason Persistent
 * Execution Contexts are tractable here and are not tractable for a Linux
 * container, whose state is spread through the host kernel's page tables,
 * file descriptors and socket buffers.
 *
 * See docs/AeroSLS-Orchestration-Implementation-Plan-v0.1.md.
 *
 * ─── Relationship to the reference interpreter ────────────────────────
 * Ported from tools/simi/simi_interp.c, which remains the cross-validation
 * ground truth (it, the x86 JIT and the RV64 backend already agree across
 * a 90-file corpus; this joins that discipline rather than inventing a new
 * one). Behavioural differences from the reference are deliberate, few,
 * and listed here rather than discovered:
 *
 *   1. NO unbounded run loop. simi_interp_run() takes an instruction
 *      budget and returns. This is the change that makes checkpointing
 *      possible at all -- an interpreter that only exposes "run to
 *      completion" has no point at which state can be captured.
 *   2. NO die()/exit(). Traps set ctx->status and return; the kernel has
 *      no process to exit and must never longjmp out of a syscall.
 *   3. Float ops (T_F32/T_F64) TRAP rather than execute -- see
 *      SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED below for the full reason.
 *   4. RESOLVE/OBJSIZE/OBJTYPE bind to the REAL object catalog via
 *      kernel/simi_runtime.c, not the reference's mock table. This is an
 *      improvement, not a divergence: the reference uses a mock precisely
 *      because it has no catalog.
 *   5. SIMI_MAX_FRAMES is smaller by default (see below).
 */

/* ─── ISA definitions, mirrored not #included ─────────────────────────
 * kernel/simi_x86.h states the convention explicitly: "Duplicated (not
 * #included) so this file has zero dependency on either tree", and
 * simi_x86.c carries its own copy of exactly these enums and bit
 * accessors for that reason. This file follows suit rather than reaching
 * into tools/simi/, which would make the kernel build depend on the host
 * toolchain tree and drag in host-only name tables.
 *
 * Numbering MUST match tools/simi/simi_isa.h. It is checked, not assumed:
 * simi_interp_isa_fingerprint() (below) packs the values as THIS header
 * sees them, and tests/simi_interp_host_test.c -- the one place both
 * definitions legitimately coexist, in separate translation units --
 * compares that against the real header's values. A renumbered opcode
 * therefore fails a test rather than silently executing the wrong
 * instruction. simi_x86.c carries the same duplication with only a
 * comment to protect it; this is a little stricter.
 *
 * The block is skipped entirely if the authoritative header was included
 * first (SIMI_ISA_H defined), so the two can coexist in one TU without
 * clashing.
 */
#ifndef SIMI_ISA_H
enum {
    OP_ADD=0, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_SHL, OP_SHR, OP_SAR, OP_NEG, OP_MOV, OP_LOADI, OP_LOADI64, OP_CMP,
    OP_BR, OP_BC, OP_CALL, OP_RET, OP_LOAD, OP_STORE, OP_LEA, OP_PTRADD,
    OP_ENTER, OP_LEAVE,
    OP_RESOLVE, OP_OBJSIZE, OP_OBJTYPE,   /* v0.3 (Phase 6) */
    OP_JMPR,                              /* Phase 14 */
    /* Phase 15 shared-memory atomics. Appended after OP_JMPR, matching
     * tools/simi/simi_isa.h exactly -- the order IS the numbering, and the ISA
     * drift guard in tests/simi_interp_host_test.c fingerprints OP_COUNT and
     * four named opcodes to catch a mismatch. These arrived in the ISA and in
     * all three code generators and were missed here, which is precisely what
     * that guard is for: it went red rather than letting the interpreter and
     * the encoders disagree quietly about what an opcode number means. */
    OP_CAS, OP_ATOMIC_ADD,                /* Phase 15 */
    OP_COUNT
};
typedef enum {
    T_I8=0,T_I16,T_I32,T_I64,T_U8,T_U16,T_U32,T_U64,T_F32,T_F64,T_PTR,T_BOOL,T_OBJREF
} SimiType;
enum { REL_EQ=0,REL_NE,REL_LT,REL_LE,REL_GT,REL_GE,REL_LTU,REL_LEU,REL_GTU,REL_GEU };

#define FLAG_IMM    0x1u
#define FLAG_INVERT 0x1u

static inline int      simi_type_signed(SimiType t) { return t==T_I8||t==T_I16||t==T_I32||t==T_I64; }
static inline uint8_t  simi_op(uint64_t w)     { return (uint8_t)((w>>56)&0xFFu); }
static inline uint8_t  simi_type(uint64_t w)   { return (uint8_t)((w>>52)&0xFu); }
static inline uint16_t simi_rd(uint64_t w)     { return (uint16_t)((w>>42)&0x3FFu); }
static inline uint16_t simi_ra(uint64_t w)     { return (uint16_t)((w>>32)&0x3FFu); }
static inline uint32_t simi_rb_raw(uint64_t w) { return (uint32_t)((w>>4)&0xFFFFFFFu); }
static inline uint8_t  simi_flags(uint64_t w)  { return (uint8_t)(w&0xFu); }
static inline uint16_t simi_rb_reg(uint64_t w) { return (uint16_t)(simi_rb_raw(w)&0x3FFu); }
static inline int32_t  simi_imm28(uint64_t w) {
    uint32_t raw = simi_rb_raw(w);
    if (raw & 0x8000000u) raw |= 0xF0000000u;
    return (int32_t)raw;
}

/* ─── Program image, mirrored from simi_isa.h's SimiObject ────────────
 * Same reason as above. The interpreter only reads these; the loader
 * populates them from an uploaded ServiceBinary. */
#define SIMI_MAX_NAME 32

typedef struct { char name[SIMI_MAX_NAME]; uint32_t offset; } SimiEntry;
typedef struct { char name[SIMI_MAX_NAME]; }                  SimiName;

typedef struct {
    uint32_t   magic;
    uint32_t   num_instr;
    uint32_t   num_literals;
    uint32_t   num_entries;
    uint32_t   num_names;
    uint64_t*  instr;
    uint64_t*  literals;
    SimiEntry* entries;
    SimiName*  names;
} SimiObject;
#endif /* SIMI_ISA_H */

/* ─── Resource bounds ─────────────────────────────────────────────────
 * The reference uses MAX_FRAMES=256, which at 9 KiB per frame is a 2.3 MiB
 * context. That is fine for a host tool that runs one program and exits;
 * it is a poor default for a kernel that may hold many live contexts, and
 * it makes every checkpoint 2.3 MiB regardless of actual call depth.
 *
 * 32 frames (288 KiB) is the kernel default. Deep recursion traps with
 * SIMI_STATUS_TRAP_CALL_OVERFLOW -- the SAME trap the reference raises,
 * just sooner, so behaviour differs only for programs that would have
 * recursed past 32 deep.
 *
 * Overridable at compile time so the cross-validation build can set 256
 * and exercise byte-identical behaviour against the reference on the
 * corpus. Checkpoints only ever serialise LIVE frames (frame_top + 1), so
 * a raised bound costs nothing on disk. */
#ifndef SIMI_MAX_FRAMES
#define SIMI_MAX_FRAMES 32
#endif

#define SIMI_NREGS     1024
#define SIMI_MEM_SIZE  (64 * 1024)

/* ─── Execution status ────────────────────────────────────────────────
 * Every way a step can end. Distinct codes rather than a single "error"
 * because a checkpoint/restore layer, a scheduler and an operator all
 * need to tell "ran out of budget, resume me" apart from "this program is
 * broken" -- and telling them apart is the difference between a context
 * that migrates and one that gets killed. */
typedef enum {
    SIMI_STATUS_OK = 0,             /* budget exhausted, still runnable -- resume by calling run() again */
    SIMI_STATUS_HALTED,             /* returned from the top-level frame; ctx->result holds r0 */
    SIMI_STATUS_TRAP_PC_RANGE,      /* pc ran off the end of the instruction stream */
    SIMI_STATUS_TRAP_BAD_OPCODE,
    SIMI_STATUS_TRAP_DIV_ZERO,
    SIMI_STATUS_TRAP_DIV_OVERFLOW,  /* signed MIN / -1 */
    SIMI_STATUS_TRAP_MOD_ZERO,
    SIMI_STATUS_TRAP_MEM_BOUNDS,    /* LOAD/STORE outside the flat memory array */
    SIMI_STATUS_TRAP_CALL_OVERFLOW, /* call depth exceeded SIMI_MAX_FRAMES */
    SIMI_STATUS_TRAP_LITERAL_RANGE, /* LOADI64 index past the literal pool */
    SIMI_STATUS_TRAP_NAME_RANGE,    /* RESOLVE index past the name pool */
    SIMI_STATUS_TRAP_JMPR_RANGE,    /* indirect jump target out of range */
    SIMI_STATUS_TRAP_BAD_RELATION,  /* CMP with an undefined relation code */
    SIMI_STATUS_TRAP_SAR_UNSIGNED,  /* SAR on an unsigned type */
    SIMI_STATUS_TRAP_MOD_FLOAT,     /* float MOD -- permanently unimplemented by ISA design, not a gap */

    /* Float arithmetic is NOT executed by this interpreter, and that is a
     * deliberate v1 boundary rather than an oversight.
     *
     * The kernel builds with -mno-sse (see the Makefile's X86_CFLAGS). The
     * reference interpreter's float helpers return `double`/`float` BY
     * VALUE, which is exactly the construct that broke this project's real
     * cross-compile once before (see kernel/vecstore.c's Phase-A note:
     * "SSE register return with SSE disabled"). That is avoidable by using
     * out-parameters, as vecstore.c does -- but a second, subtler problem
     * is not: with SSE disabled, x86-64 falls back to x87, whose 80-bit
     * intermediates can differ in the last bit from the SSE arithmetic the
     * host reference and the JIT both use. Bit-exact cross-validation
     * against the corpus would silently stop holding for float programs.
     *
     * Trapping is the honest option: integer SIMI is fully supported and
     * exactly cross-validated; float SIMI is refused loudly rather than
     * computed slightly differently. Revisit when the SIMD enablement work
     * lands (CR4.OSXSAVE/xsetbv -- see the LLM feasibility doc), at which
     * point real SSE arithmetic becomes available and this trap can be
     * replaced with a real implementation. */
    SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED,
} SimiStatus;

const char* simi_status_name(SimiStatus s);

/* One call frame. Layout mirrors the reference's Frame exactly, so the
 * cross-validation comparison is meaningful field by field. */
struct SimiFrame {
    uint64_t regs[SIMI_NREGS];
    uint8_t  cap_tag[SIMI_NREGS];   /* 1 = holds an unforged T_OBJREF capability (ISA §Phase 7) */
    uint32_t declared_nregs;
    uint32_t return_pc;             /* instruction index to resume the caller at */
    int32_t  caller_frame;          /* index into frames[], -1 at top level */
};

/*
 * The complete execution state of a running SIMI program.
 *
 * THE INVARIANT THAT MATTERS: everything needed to resume execution is in
 * here, and none of it is a host pointer. Serialise this struct (live
 * frames only) plus the program's instruction stream, and execution can
 * resume on another boot, another node, or another machine.
 *
 * `obj` is the one exception, and deliberately so: it points at the
 * immutable program image (instructions/literals/names), which is content
 * addressed by the catalog and re-resolved on restore rather than
 * serialised into the checkpoint. Checkpointing the code alongside the
 * state would duplicate a potentially large, unchanging blob on every
 * checkpoint.
 */
struct SimiContext {
    /* ── Serialisable execution state ── */
    uint32_t pc;                    /* INSTRUCTION INDEX -- not an address */
    int32_t  frame_top;             /* index of the current frame; -1 = not started */
    uint64_t steps;                 /* total instructions retired (diagnostics/fairness) */
    uint64_t result;                /* r0 of the top-level frame, valid once HALTED */
    SimiStatus status;
    uint32_t trap_pc;               /* pc at which a trap fired */

    struct SimiFrame frames[SIMI_MAX_FRAMES];
    uint8_t  mem[SIMI_MEM_SIZE];

    /* ── Not serialised: re-bound on restore ── */
    const SimiObject* obj;          /* the immutable program image */
};

/* Prepares `ctx` to execute `obj` starting at the entry point named
 * `entry_name`. Zeroes all state. Returns 0 on success, 1 if no such entry
 * point exists (ctx is left unusable, status set). */
int simi_interp_init(struct SimiContext* ctx, const SimiObject* obj, const char* entry_name);

/* Executes at most `budget` instructions.
 *
 * The budget is what makes this checkpointable and what keeps a runaway
 * program from wedging the caller -- the reference interpreter's guard is
 * a hard-coded 10M-step limit and then exit(), neither of which a kernel
 * can use. Returns:
 *   SIMI_STATUS_OK      -- budget spent, ctx is mid-execution and resumable
 *   SIMI_STATUS_HALTED  -- program returned; ctx->result is valid
 *   SIMI_STATUS_TRAP_*  -- faulted; ctx->trap_pc says where
 *
 * A context that has HALTED or TRAPPED is not resumable; calling again
 * returns the same status without executing anything. */
SimiStatus simi_interp_run(struct SimiContext* ctx, uint64_t budget);

/* Bytes of `ctx` that a checkpoint must actually store: the fixed header
 * plus LIVE frames only, plus memory. A depth-1 context is ~73 KiB even
 * though sizeof(struct SimiContext) is far larger, because unentered
 * frames hold nothing. Phase 2 (checkpoint/restore) uses this to size its
 * writes; exposed here so the saving is visible rather than implied. */
uint32_t simi_interp_live_bytes(const struct SimiContext* ctx);

/* Packs key ISA constants AS THIS TRANSLATION UNIT SEES THEM, so a test
 * compiled against the authoritative tools/simi/simi_isa.h can verify the
 * mirrored copy above has not drifted. See the ISA block's comment. */
uint64_t simi_interp_isa_fingerprint(void);

#endif /* SIMI_INTERP_H */
