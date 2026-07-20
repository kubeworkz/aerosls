/*
 * simi_interp.c — Phase 1 SIMI reference interpreter.
 *
 * This is a DEVELOPMENT-ONLY tool (per design principle #1 in the ISA
 * spec: production SIMI is translated, never interpreted). Its only job
 * is to be ground truth for testing the assembler/encoding and, later,
 * the native translator's output.
 *
 * Known Phase 1 simplifications (documented, not silent):
 *   - Registers are stored as full 64-bit words; values are NOT masked
 *     to their declared type's bit width (e.g. an i8 add can produce a
 *     value outside -128..127 without complaint). Width-correct
 *     truncation is a Phase 3 (real translator) correctness concern;
 *     the reference interpreter's job right now is proving the
 *     control-flow/encoding/ABI model works, not bit-exact arithmetic.
 *   - Overflow-trap check for signed MIN/-1 division uses the full
 *     64-bit range regardless of the operand's declared width, for the
 *     same reason.
 *   - Gap Remediation SIMI Phase 10: ADD/SUB/MUL/DIV/NEG/CMP now execute
 *     real IEEE-754 arithmetic for T_F32/T_F64 (bit-reinterpretation of
 *     the same 64-bit register slot, not a converted copy). MOD stays
 *     permanently unimplemented for float types (no hardware instruction
 *     on any target this project ships for; compose it from
 *     DIV+MUL+SUB), which is a deliberate, permanent scope boundary, not
 *     a temporary gap. See docs/AeroSLS-SIMI-ISA-v0.1.md §16 Phase 10.
 *   - "Memory" is a flat simulated byte array; pointers are simply
 *     byte offsets into it, not real SLS single-level-store pointers.
 */
#include "simi_isa.h"
#include "simi_obj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MEM_SIZE (64 * 1024)
#define MAX_FRAMES 256
#define NREGS 1024

/* v0.3 (Phase 7): capability tags — see the design-notes block below,
 * right after the mock catalog, for the full contract. One byte per
 * register (not a bitset) trades a little memory for code simplicity;
 * NREGS=1024 means this is a 1 KiB array per frame, negligible. */
typedef struct {
    uint64_t regs[NREGS];
    uint8_t  cap_tag[NREGS];  /* Phase 7: 1 = holds a valid, unforged T_OBJREF capability */
    uint32_t declared_nregs;
    uint32_t return_pc;   /* instruction index to resume caller at */
    int caller_frame;     /* index into frame stack, -1 for top-level */
} Frame;

static Frame frames[MAX_FRAMES];
static int frame_top = -1; /* index of current frame */

static uint8_t mem[MEM_SIZE];

static int width_of(SimiType t) {
    switch (t) {
        case T_I8: case T_U8: case T_BOOL: return 1;
        case T_I16: case T_U16: return 2;
        case T_I32: case T_U32: case T_F32: return 4;
        default: return 8;
    }
}

static void die(const char *msg, uint32_t pc) {
    fprintf(stderr, "SIMI trap at instr %u: %s\n", pc, msg);
    exit(2);
}

static uint64_t fetch_operand_b(uint64_t w, Frame *fr) {
    uint8_t flags = simi_flags(w);
    if (flags & FLAG_IMM) {
        return (uint64_t)(int64_t)simi_imm28(w);
    }
    return fr->regs[simi_rb_reg(w)];
}

/* ─── Gap Remediation SIMI Phase 10: float bit-reinterpretation helpers ───
 * T_F64 reinterprets the full 64-bit register slot as an IEEE-754 double,
 * bit-for-bit; T_F32 reinterprets the low 32 bits as an IEEE-754 single
 * (high 32 bits zeroed on write) -- no conversion, just a different read
 * of the same bits a register slot already holds, per the design in
 * docs/AeroSLS-SIMI-ISA-v0.1.md §16 Phase 10. This file is a host-only
 * tool (never compiled under the kernel's -mno-sse), so using C's real
 * `double`/`float` types here is safe -- unlike simi_x86.c, which must
 * never introduce them (see that file's own Phase 10 notes). */
static double f64_of_bits(uint64_t b) { union { uint64_t u; double d; } c; c.u = b; return c.d; }
static uint64_t bits_of_f64(double d) { union { uint64_t u; double d; } c; c.d = d; return c.u; }
static float f32_of_bits(uint64_t b) { union { uint32_t u; float f; } c; c.u = (uint32_t)b; return c.f; }
static uint64_t bits_of_f32(float f) { union { uint32_t u; float f; } c; c.f = f; return (uint64_t)c.u; }

/* v0.3 (Phase 6): the reference interpreter has no real SLS object
 * catalog to query (it's a flat simulated-memory sandbox, not the
 * kernel), so RESOLVE/OBJSIZE/OBJTYPE run against a small mock catalog
 * instead. Every environment that executes v0.3 SIMI gets its own
 * catalog binding — the kernel's is real (kernel/simi_runtime.c), the
 * host JIT test / RV64 verify harnesses get mocks of their own, and
 * this interpreter is the simplest case: no address-baking needed,
 * just a direct in-memory lookup table. See §13.
 *
 * base_vaddr values are arbitrary but non-zero and distinct, since 0 is
 * RESOLVE's "not found" sentinel. */
typedef struct {
    const char *name;
    uint64_t base_vaddr;
    uint32_t byte_size;
    uint32_t obj_type;  /* mirrors the kernel's OBJ_TYPE_* enum by convention only */
} MockCatalogEntry;

#define MOCK_OBJ_TYPE_PROGRAM 1
#define MOCK_OBJ_TYPE_DATA    2

static const MockCatalogEntry g_mock_catalog[] = {
    { "simi_add_test2", 0x2000, 303, MOCK_OBJ_TYPE_PROGRAM },
    { "simi_verify3",   0x3000, 100, MOCK_OBJ_TYPE_DATA },
};
#define MOCK_CATALOG_N (sizeof(g_mock_catalog) / sizeof(g_mock_catalog[0]))

static uint64_t mock_resolve(const char *name) {
    for (size_t i = 0; i < MOCK_CATALOG_N; i++)
        if (strcmp(g_mock_catalog[i].name, name) == 0) return g_mock_catalog[i].base_vaddr;
    return 0;
}
static const MockCatalogEntry *mock_lookup_vaddr(uint64_t vaddr) {
    for (size_t i = 0; i < MOCK_CATALOG_N; i++)
        if (g_mock_catalog[i].base_vaddr == vaddr) return &g_mock_catalog[i];
    return NULL;
}

/* v0.3 (Phase 7): capability tags — tagged, unforgeable T_OBJREF values.
 * System/38's ODT pointers combined two properties: an authority check at
 * the point a pointer is minted (who's allowed to obtain one — the
 * kernel's job, see kernel/simi_runtime.c's authority-checked
 * simi_rt_resolve()) and unforgeability afterward (no sequence of
 * ordinary instructions can synthesize a valid one from scratch — this
 * interpreter's job, and every native translator's).
 *
 * The rule, enforced identically here and in both native translators:
 *   - RESOLVE sets its destination register's tag IFF the returned value
 *     is non-zero (a successful, already-authority-checked resolve). A
 *     failed RESOLVE (0) leaves the register untagged — a "null
 *     capability" is not usable as one.
 *   - MOV propagates the source register's tag to the destination —
 *     copying a valid capability around is fine, that's how it gets
 *     passed to where it's needed.
 *   - Every other register-writing opcode clears the destination's tag.
 *     This is the actual unforgeability property: LOADI, arithmetic,
 *     LEA/PTRADD, LOAD from memory, CMP, OBJSIZE/OBJTYPE's own results —
 *     none of them can ever produce a tagged register, no matter what
 *     value ends up in it or what TYPE field the instruction declares.
 *     Claiming `LOADI r0, #0x2000, objref` makes r0 a capability is a
 *     lie the type system doesn't enforce; the tag is what actually does.
 *   - OBJSIZE/OBJTYPE require rA's tag to be set. An untagged operand —
 *     however it got its bit pattern — is rejected with the same
 *     "invalid" sentinel used for "no such object" (0 / 0xFFFFFFFF),
 *     without ever asking the mock catalog about it. This is what closes
 *     the forgery hole: pre-Phase-7, any process could guess/compute an
 *     address, claim it as objref-typed, and get real answers back from
 *     OBJSIZE/OBJTYPE about whatever real object happened to live there.
 *   - Gap Remediation SIMI Phase 12: CALL/RET now propagate tags across
 *     the call boundary too — the r0-r7 argument copy (CALL) and the r0
 *     return-value copy (RET) carry the source register's tag along with
 *     its value, exactly the same plain-copy rule MOV already uses above,
 *     just now crossing a frame boundary via a call/return instead of
 *     staying within one. This is safe for the same reason MOV is safe:
 *     neither copy computes or synthesizes a value, so a callee/caller
 *     can only ever receive a bit-identical copy of a capability the
 *     other side already legitimately held — never a forged one. See
 *     docs/AeroSLS-SIMI-ISA-v0.1.md §16 Phase 12 for the full design. */
static void set_tag(Frame *fr, uint16_t reg, int val) { fr->cap_tag[reg] = (uint8_t)(val ? 1 : 0); }

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s program.tmo entry_name\n", argv[0]);
        return 1;
    }
    SimiObject obj = {0};
    if (simi_obj_read(argv[1], &obj) != 0) return 1;

    uint32_t entry_pc = UINT32_MAX;
    for (uint32_t i = 0; i < obj.num_entries; i++) {
        if (strcmp(obj.entries[i].name, argv[2]) == 0) { entry_pc = obj.entries[i].offset; break; }
    }
    if (entry_pc == UINT32_MAX) {
        fprintf(stderr, "no such entry point '%s'\n", argv[2]);
        return 1;
    }

    frame_top = 0;
    frames[0].caller_frame = -1;
    frames[0].return_pc = 0;
    memset(frames[0].regs, 0, sizeof(frames[0].regs));
    memset(frames[0].cap_tag, 0, sizeof(frames[0].cap_tag));

    uint32_t pc = entry_pc;
    long steps = 0;
    const long MAX_STEPS = 10000000;

    for (;;) {
        if (++steps > MAX_STEPS) die("step limit exceeded (probable infinite loop)", pc);
        if (pc >= obj.num_instr) die("pc ran off the end of the instruction stream", pc);
        uint64_t w = obj.instr[pc];
        uint8_t op = simi_op(w);
        uint8_t type = simi_type(w);
        Frame *fr = &frames[frame_top];
        uint16_t rd = simi_rd(w), ra = simi_ra(w);

        switch (op) {
        case OP_ENTER:
            fr->declared_nregs = (uint32_t)simi_rb_raw(w);
            pc++; break;
        case OP_LEAVE:
            pc++; break;

        case OP_ADD: {
            uint64_t b = fetch_operand_b(w, fr);
            if (type == T_F64)      fr->regs[rd] = bits_of_f64(f64_of_bits(fr->regs[ra]) + f64_of_bits(b));
            else if (type == T_F32) fr->regs[rd] = bits_of_f32(f32_of_bits(fr->regs[ra]) + f32_of_bits(b));
            else                    fr->regs[rd] = fr->regs[ra] + b;
            set_tag(fr, rd, 0); pc++; break;
        }
        case OP_SUB: {
            uint64_t b = fetch_operand_b(w, fr);
            if (type == T_F64)      fr->regs[rd] = bits_of_f64(f64_of_bits(fr->regs[ra]) - f64_of_bits(b));
            else if (type == T_F32) fr->regs[rd] = bits_of_f32(f32_of_bits(fr->regs[ra]) - f32_of_bits(b));
            else                    fr->regs[rd] = fr->regs[ra] - b;
            set_tag(fr, rd, 0); pc++; break;
        }
        case OP_MUL: {
            uint64_t b = fetch_operand_b(w, fr);
            if (type == T_F64)      fr->regs[rd] = bits_of_f64(f64_of_bits(fr->regs[ra]) * f64_of_bits(b));
            else if (type == T_F32) fr->regs[rd] = bits_of_f32(f32_of_bits(fr->regs[ra]) * f32_of_bits(b));
            else                    fr->regs[rd] = fr->regs[ra] * b;
            set_tag(fr, rd, 0); pc++; break;
        }
        case OP_DIV: {
            uint64_t b = fetch_operand_b(w, fr);
            /* Gap Remediation SIMI Phase 10: unlike integer DIV, IEEE-754
             * float division by zero is a DEFINED result (signed
             * infinity or NaN, no trap) -- the die()-on-float this case
             * used to have wasn't protecting against a real hazard, it
             * was just "not implemented yet". Real division now runs. */
            if (type == T_F64) { fr->regs[rd] = bits_of_f64(f64_of_bits(fr->regs[ra]) / f64_of_bits(b)); set_tag(fr, rd, 0); pc++; break; }
            if (type == T_F32) { fr->regs[rd] = bits_of_f32(f32_of_bits(fr->regs[ra]) / f32_of_bits(b)); set_tag(fr, rd, 0); pc++; break; }
            if (b == 0) die("division by zero", pc);
            if (simi_type_signed((SimiType)type)) {
                int64_t a = (int64_t)fr->regs[ra], bs = (int64_t)b;
                if (a == INT64_MIN && bs == -1) die("signed division overflow (MIN_INT / -1)", pc);
                fr->regs[rd] = (uint64_t)(a / bs);
            } else {
                fr->regs[rd] = fr->regs[ra] / b;
            }
            set_tag(fr, rd, 0);
            pc++; break;
        }
        case OP_MOD: {
            /* Gap Remediation SIMI Phase 10: float MOD stays permanently
             * unimplemented, by design, not "not yet" -- neither x86 nor
             * RV64 has a single hardware instruction for float remainder;
             * a program needing it composes it from DIV+MUL+SUB, the same
             * way a real fmod() library implementation does. See §16
             * Phase 10's design decision 3. */
            if (type == T_F32 || type == T_F64) die("float MOD has no hardware instruction on any target -- compose it from DIV+MUL+SUB instead (permanent, not a temporary gap)", pc);
            uint64_t b = fetch_operand_b(w, fr);
            if (b == 0) die("modulo by zero", pc);
            if (simi_type_signed((SimiType)type)) {
                int64_t a = (int64_t)fr->regs[ra], bs = (int64_t)b;
                if (a == INT64_MIN && bs == -1) fr->regs[rd] = 0;
                else fr->regs[rd] = (uint64_t)(a % bs);
            } else {
                fr->regs[rd] = fr->regs[ra] % b;
            }
            set_tag(fr, rd, 0);
            pc++; break;
        }
        case OP_AND: fr->regs[rd] = fr->regs[ra] & fetch_operand_b(w, fr); set_tag(fr, rd, 0); pc++; break;
        case OP_OR:  fr->regs[rd] = fr->regs[ra] | fetch_operand_b(w, fr); set_tag(fr, rd, 0); pc++; break;
        case OP_XOR: fr->regs[rd] = fr->regs[ra] ^ fetch_operand_b(w, fr); set_tag(fr, rd, 0); pc++; break;
        case OP_NOT: fr->regs[rd] = ~fr->regs[ra]; set_tag(fr, rd, 0); pc++; break;
        case OP_NEG: {
            if (type == T_F64)      fr->regs[rd] = bits_of_f64(-f64_of_bits(fr->regs[ra]));
            else if (type == T_F32) fr->regs[rd] = bits_of_f32(-f32_of_bits(fr->regs[ra]));
            else                    fr->regs[rd] = (uint64_t)(-(int64_t)fr->regs[ra]);
            set_tag(fr, rd, 0); pc++; break;
        }

        case OP_SHL: { uint64_t amt = fetch_operand_b(w, fr) & 0x3F; fr->regs[rd] = fr->regs[ra] << amt; set_tag(fr, rd, 0); pc++; break; }
        case OP_SHR: { uint64_t amt = fetch_operand_b(w, fr) & 0x3F; fr->regs[rd] = fr->regs[ra] >> amt; set_tag(fr, rd, 0); pc++; break; }
        case OP_SAR: {
            if (!simi_type_signed((SimiType)type)) die("SAR used on an unsigned type (assembler should have rejected this)", pc);
            uint64_t amt = fetch_operand_b(w, fr) & 0x3F;
            fr->regs[rd] = (uint64_t)((int64_t)fr->regs[ra] >> amt);
            set_tag(fr, rd, 0);
            pc++; break;
        }

        case OP_MOV: fr->regs[rd] = fr->regs[ra]; set_tag(fr, rd, fr->cap_tag[ra]); pc++; break;
        case OP_LOADI: fr->regs[rd] = (uint64_t)(int64_t)simi_imm28(w); set_tag(fr, rd, 0); pc++; break;
        case OP_LOADI64: {
            uint32_t idx = simi_rb_raw(w);
            if (idx >= obj.num_literals) die("LOADI64 literal pool index out of range", pc);
            fr->regs[rd] = obj.literals[idx];
            set_tag(fr, rd, 0);
            pc++; break;
        }

        case OP_CMP: {
            uint64_t a = fr->regs[ra], b = fr->regs[simi_rb_reg(w)];
            int rel = simi_flags(w);
            int result = 0;
            /* Gap Remediation SIMI Phase 10: T_F32/T_F64 route through
             * real IEEE-754 comparisons instead of the integer path.
             * C's native double/float comparison operators already
             * implement IEEE-754 unordered-comparison semantics exactly
             * per the language standard -- LT/LE/GT/GE are all false when
             * either operand is NaN, EQ is false for NaN==NaN, NE is true
             * for it -- so no special NaN-branching logic is needed here,
             * just routing through the right C operators. Implemented as
             * specified, not "corrected" to be more intuitive: any future
             * LLVM front end (per the scoping doc) will expect real IEEE
             * semantics. REL_*U (unsigned) relations have no float
             * meaning and are not specially handled here (assembler-level
             * concern, same as SAR already rejecting unsigned types). */
            if (type == T_F64) {
                double fa = f64_of_bits(a), fb = f64_of_bits(b);
                switch (rel) {
                    case REL_EQ: result = (fa == fb); break;
                    case REL_NE: result = (fa != fb); break;
                    case REL_LT: result = (fa <  fb); break;
                    case REL_LE: result = (fa <= fb); break;
                    case REL_GT: result = (fa >  fb); break;
                    case REL_GE: result = (fa >= fb); break;
                    default: die("bad CMP relation code for a float type", pc);
                }
            } else if (type == T_F32) {
                float fa = f32_of_bits(a), fb = f32_of_bits(b);
                switch (rel) {
                    case REL_EQ: result = (fa == fb); break;
                    case REL_NE: result = (fa != fb); break;
                    case REL_LT: result = (fa <  fb); break;
                    case REL_LE: result = (fa <= fb); break;
                    case REL_GT: result = (fa >  fb); break;
                    case REL_GE: result = (fa >= fb); break;
                    default: die("bad CMP relation code for a float type", pc);
                }
            } else {
                switch (rel) {
                    case REL_EQ:  result = (a == b); break;
                    case REL_NE:  result = (a != b); break;
                    case REL_LT:  result = ((int64_t)a <  (int64_t)b); break;
                    case REL_LE:  result = ((int64_t)a <= (int64_t)b); break;
                    case REL_GT:  result = ((int64_t)a >  (int64_t)b); break;
                    case REL_GE:  result = ((int64_t)a >= (int64_t)b); break;
                    case REL_LTU: result = (a <  b); break;
                    case REL_LEU: result = (a <= b); break;
                    case REL_GTU: result = (a >  b); break;
                    case REL_GEU: result = (a >= b); break;
                    default: die("bad CMP relation code", pc);
                }
            }
            fr->regs[rd] = result ? 1 : 0;
            set_tag(fr, rd, 0);
            pc++; break;
        }

        case OP_BR: {
            int32_t disp = simi_imm28(w);
            pc = (uint32_t)((int64_t)pc + 1 + disp);
            break;
        }
        case OP_BC: {
            uint64_t cond = fr->regs[ra];
            int invert = simi_flags(w) & FLAG_INVERT;
            int taken = invert ? (cond == 0) : (cond != 0);
            int32_t disp = simi_imm28(w);
            pc = taken ? (uint32_t)((int64_t)pc + 1 + disp) : pc + 1;
            break;
        }
        case OP_CALL: {
            int32_t disp = simi_imm28(w);
            uint32_t target = (uint32_t)((int64_t)pc + 1 + disp);
            if (frame_top + 1 >= MAX_FRAMES) die("call stack overflow", pc);
            Frame *nf = &frames[frame_top + 1];
            memset(nf->regs, 0, sizeof(nf->regs));
            memset(nf->cap_tag, 0, sizeof(nf->cap_tag));
            for (int i = 0; i < 8; i++) nf->regs[i] = fr->regs[i]; /* r0-r7 argument convention, §4.8 */
            /* Gap Remediation SIMI Phase 12: r0-r7 argument tags now ride
             * along with the value copy above, exactly like MOV already
             * does within one frame -- this is a plain, uncomputed copy
             * crossing a frame boundary via CALL instead of staying within
             * one, so it preserves Phase 7's unforgeability invariant
             * (a capability can only be a real RESOLVE result or a chain of
             * plain copies from one) rather than weakening it. */
            for (int i = 0; i < 8; i++) set_tag(nf, i, fr->cap_tag[i]);
            nf->caller_frame = frame_top;
            nf->return_pc = pc + 1;
            frame_top++;
            pc = target;
            break;
        }
        case OP_JMPR: {
            /* Gap Remediation SIMI Phase 14: rA holds a target as an
             * abstract SIMI instruction index (pc) -- the interpreter's
             * own dispatch is already pc-indexed, so honoring it is a
             * direct pc assignment, exactly like BR/CALL above. The
             * mandatory bounds check is what makes this safe: a bogus or
             * attacker-controlled value must kill the process here, not
             * silently wrap or land mid-nowhere (there's no "mid-
             * instruction" hazard for the interpreter the way there is
             * for real machine code, but an out-of-range index is still a
             * real bug/attack surface worth trapping on, not tolerating). */
            uint64_t target = fr->regs[ra];
            if (target >= obj.num_instr) die("JMPR target out of range (bad indirect jump)", pc);
            pc = (uint32_t)target;
            break;
        }
        case OP_RET: {
            if (frame_top == 0) {
                printf("%lld\n", (long long)(int64_t)frames[0].regs[0]);
                simi_obj_free(&obj);
                return 0;
            }
            uint64_t retval = fr->regs[0];
            uint8_t  rettag = fr->cap_tag[0];
            uint32_t resume = fr->return_pc;
            int caller = fr->caller_frame;
            frame_top = caller;
            frames[frame_top].regs[0] = retval; /* return value convention, §4.8 */
            /* Gap Remediation SIMI Phase 12: propagate the callee's r0 tag
             * into the caller's r0, same plain-copy rule as CALL's argument
             * marshaling above. */
            set_tag(&frames[frame_top], 0, rettag);
            pc = resume;
            break;
        }

        case OP_LOAD: {
            uint64_t addr = fr->regs[ra] + (uint64_t)(int64_t)simi_imm28(w);
            int wdt = width_of((SimiType)type);
            if (addr + wdt > MEM_SIZE) die("LOAD out of simulated memory bounds", pc);
            uint64_t v = 0;
            memcpy(&v, &mem[addr], wdt);
            if (simi_type_signed((SimiType)type) && wdt < 8) {
                int shift = 64 - wdt * 8;
                v = (uint64_t)(((int64_t)(v << shift)) >> shift);
            }
            fr->regs[rd] = v;
            set_tag(fr, rd, 0);
            pc++; break;
        }
        case OP_STORE: {
            uint64_t addr = fr->regs[ra] + (uint64_t)(int64_t)simi_imm28(w);
            int wdt = width_of((SimiType)type);
            if (addr + wdt > MEM_SIZE) die("STORE out of simulated memory bounds", pc);
            memcpy(&mem[addr], &fr->regs[rd], wdt);
            pc++; break;
        }
        case OP_LEA: {
            fr->regs[rd] = fr->regs[ra] + (uint64_t)(int64_t)simi_imm28(w);
            set_tag(fr, rd, 0);
            pc++; break;
        }
        case OP_PTRADD: {
            uint64_t idx = fetch_operand_b(w, fr);
            int scale = width_of((SimiType)type);
            fr->regs[rd] = fr->regs[ra] + idx * (uint64_t)scale;
            set_tag(fr, rd, 0);   /* Phase 7: pointer arithmetic never yields a capability */
            pc++; break;
        }

        case OP_RESOLVE: {
            uint32_t idx = simi_rb_raw(w);
            if (idx >= obj.num_names) die("RESOLVE name-pool index out of range", pc);
            uint64_t result = mock_resolve(obj.names[idx].name);
            fr->regs[rd] = result;
            set_tag(fr, rd, result != 0);  /* Phase 7: tag only a successful resolve */
            pc++; break;
        }
        case OP_OBJSIZE: {
            if (!fr->cap_tag[ra]) { fr->regs[rd] = 0; set_tag(fr, rd, 0); pc++; break; }
            const MockCatalogEntry *e = mock_lookup_vaddr(fr->regs[ra]);
            fr->regs[rd] = e ? e->byte_size : 0;
            set_tag(fr, rd, 0);
            pc++; break;
        }
        case OP_OBJTYPE: {
            if (!fr->cap_tag[ra]) { fr->regs[rd] = 0xFFFFFFFFu; set_tag(fr, rd, 0); pc++; break; }
            const MockCatalogEntry *e = mock_lookup_vaddr(fr->regs[ra]);
            fr->regs[rd] = e ? e->obj_type : 0xFFFFFFFFu;
            set_tag(fr, rd, 0);
            pc++; break;
        }

        default:
            die("unimplemented/unknown opcode", pc);
        }
    }
}
