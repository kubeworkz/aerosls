/*
 * simi_interp.c — in-kernel SIMI interpreter (Persistent Execution
 * Contexts, Phase 1). See simi_interp.h for the design rationale and the
 * full list of deliberate differences from the reference interpreter.
 *
 * Ported from tools/simi/simi_interp.c. The opcode semantics are a
 * faithful transcription -- that is the point, since the reference is
 * cross-validation ground truth for the x86 JIT and RV64 backend, and this
 * joins that agreement. What changed is only the four things a kernel
 * requires: no libc, no exit(), a bounded run loop, and real catalog
 * bindings.
 *
 * Freestanding: no stdio, no stdlib, no string.h. The three helpers below
 * follow this codebase's established per-file convention (p_memcpy in
 * persist.c, tn_streq in tenant.c, db_* in database.c, mk_streq in
 * microkernel.c) rather than introducing a shared mini-libc, which no
 * other file here assumes exists.
 */
#include "simi_interp.h"
#include "simi_runtime.h"   /* real catalog bindings: simi_rt_resolve/objsize/objtype */
#include "kernel_io.h"

/* ─── Local helpers (no libc) ───────────────────────────────────────── */
static void si_memzero(void* d, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = 0;
}
static void si_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static int si_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

const char* simi_status_name(SimiStatus s) {
    switch (s) {
        case SIMI_STATUS_OK:                     return "OK";
        case SIMI_STATUS_HALTED:                 return "HALTED";
        case SIMI_STATUS_TRAP_PC_RANGE:          return "TRAP:pc-out-of-range";
        case SIMI_STATUS_TRAP_BAD_OPCODE:        return "TRAP:bad-opcode";
        case SIMI_STATUS_TRAP_DIV_ZERO:          return "TRAP:div-by-zero";
        case SIMI_STATUS_TRAP_DIV_OVERFLOW:      return "TRAP:div-overflow";
        case SIMI_STATUS_TRAP_MOD_ZERO:          return "TRAP:mod-by-zero";
        case SIMI_STATUS_TRAP_MEM_BOUNDS:        return "TRAP:memory-bounds";
        case SIMI_STATUS_TRAP_CALL_OVERFLOW:     return "TRAP:call-stack-overflow";
        case SIMI_STATUS_TRAP_LITERAL_RANGE:     return "TRAP:literal-index";
        case SIMI_STATUS_TRAP_NAME_RANGE:        return "TRAP:name-index";
        case SIMI_STATUS_TRAP_JMPR_RANGE:        return "TRAP:jmpr-target";
        case SIMI_STATUS_TRAP_BAD_RELATION:      return "TRAP:bad-cmp-relation";
        case SIMI_STATUS_TRAP_SAR_UNSIGNED:      return "TRAP:sar-on-unsigned";
        case SIMI_STATUS_TRAP_MOD_FLOAT:         return "TRAP:float-mod-unimplemented-by-design";
        case SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED: return "TRAP:float-unsupported-under-mno-sse";
        default:                                 return "TRAP:unknown";
    }
}

/* See simi_interp.h: lets a test compiled against the real simi_isa.h
 * detect drift in this file's mirrored copy of the ISA numbering. */
uint64_t simi_interp_isa_fingerprint(void) {
    return ((uint64_t)OP_COUNT      << 56)
         | ((uint64_t)OP_JMPR       << 48)
         | ((uint64_t)OP_RESOLVE    << 40)
         | ((uint64_t)OP_RET        << 32)
         | ((uint64_t)T_OBJREF      << 24)
         | ((uint64_t)REL_GEU       << 16)
         | ((uint64_t)SIMI_MAX_NAME <<  8)
         | ((uint64_t)FLAG_IMM);
}

static int si_width_of(SimiType t) {
    switch (t) {
        case T_I8: case T_U8: case T_BOOL: return 1;
        case T_I16: case T_U16: return 2;
        case T_I32: case T_U32: case T_F32: return 4;
        default: return 8;
    }
}

static uint64_t si_operand_b(uint64_t w, const struct SimiFrame* fr) {
    if (simi_flags(w) & FLAG_IMM) return (uint64_t)(int64_t)simi_imm28(w);
    return fr->regs[simi_rb_reg(w)];
}

static void si_set_tag(struct SimiFrame* fr, uint16_t reg, int val) {
    fr->cap_tag[reg] = (uint8_t)(val ? 1 : 0);
}

/* Records a trap and returns it, so call sites read `return si_trap(...)`.
 * The reference calls die()+exit() here; a kernel cannot, and must leave
 * the context inspectable instead of unwinding. */
static SimiStatus si_trap(struct SimiContext* ctx, SimiStatus s, uint32_t pc) {
    ctx->status  = s;
    ctx->trap_pc = pc;
    return s;
}

/* Is this type a float? Used to route float ops to the deliberate trap
 * rather than computing them -- see simi_interp.h's long note on why. */
static int si_is_float(uint8_t type) { return type == T_F32 || type == T_F64; }

int simi_interp_init(struct SimiContext* ctx, const SimiObject* obj, const char* entry_name) {
    si_memzero(ctx, sizeof(*ctx));
    ctx->obj    = obj;
    ctx->status = SIMI_STATUS_OK;

    if (!obj || !entry_name) {
        ctx->status = SIMI_STATUS_TRAP_PC_RANGE;
        return 1;
    }

    uint32_t entry_pc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < obj->num_entries; i++) {
        if (si_streq(obj->entries[i].name, entry_name)) { entry_pc = obj->entries[i].offset; break; }
    }
    if (entry_pc == 0xFFFFFFFFu) {
        kernel_serial_printf("[SIMI] no such entry point '%s'\n", entry_name);
        ctx->status = SIMI_STATUS_TRAP_PC_RANGE;
        return 1;
    }

    ctx->pc                       = entry_pc;
    ctx->frame_top                = 0;
    ctx->frames[0].caller_frame   = -1;
    ctx->frames[0].return_pc      = 0;
    return 0;
}

uint32_t simi_interp_live_bytes(const struct SimiContext* ctx) {
    uint32_t hdr = (uint32_t)(sizeof(struct SimiContext)
                              - sizeof(ctx->frames)
                              - sizeof(ctx->mem));
    uint32_t live = (ctx->frame_top < 0) ? 0
                  : (uint32_t)(ctx->frame_top + 1) * (uint32_t)sizeof(struct SimiFrame);
    return hdr + live + (uint32_t)sizeof(ctx->mem);
}

SimiStatus simi_interp_run(struct SimiContext* ctx, uint64_t budget) {
    /* A finished context stays finished. Re-running a halted or trapped
     * context must not resurrect it -- a scheduler polling every runnable
     * context would otherwise re-execute past the end. */
    if (ctx->status != SIMI_STATUS_OK) return ctx->status;
    if (ctx->frame_top < 0)            return si_trap(ctx, SIMI_STATUS_TRAP_PC_RANGE, ctx->pc);

    const SimiObject* obj = ctx->obj;

    for (uint64_t budget_left = budget; budget_left > 0; budget_left--) {
        if (ctx->pc >= obj->num_instr) return si_trap(ctx, SIMI_STATUS_TRAP_PC_RANGE, ctx->pc);

        uint64_t w    = obj->instr[ctx->pc];
        uint8_t  op   = simi_op(w);
        uint8_t  type = simi_type(w);
        struct SimiFrame* fr = &ctx->frames[ctx->frame_top];
        uint16_t rd = simi_rd(w), ra = simi_ra(w);

        ctx->steps++;

        switch (op) {
        case OP_ENTER:
            fr->declared_nregs = (uint32_t)simi_rb_raw(w);
            ctx->pc++; break;
        case OP_LEAVE:
            ctx->pc++; break;

        /* ── Arithmetic. Float variants trap; see simi_interp.h. ──────── */
        case OP_ADD:
            if (si_is_float(type)) return si_trap(ctx, SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED, ctx->pc);
            fr->regs[rd] = fr->regs[ra] + si_operand_b(w, fr);
            si_set_tag(fr, rd, 0); ctx->pc++; break;

        case OP_SUB:
            if (si_is_float(type)) return si_trap(ctx, SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED, ctx->pc);
            fr->regs[rd] = fr->regs[ra] - si_operand_b(w, fr);
            si_set_tag(fr, rd, 0); ctx->pc++; break;

        case OP_MUL:
            if (si_is_float(type)) return si_trap(ctx, SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED, ctx->pc);
            fr->regs[rd] = fr->regs[ra] * si_operand_b(w, fr);
            si_set_tag(fr, rd, 0); ctx->pc++; break;

        case OP_DIV: {
            if (si_is_float(type)) return si_trap(ctx, SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED, ctx->pc);
            uint64_t b = si_operand_b(w, fr);
            if (b == 0) return si_trap(ctx, SIMI_STATUS_TRAP_DIV_ZERO, ctx->pc);
            if (simi_type_signed((SimiType)type)) {
                int64_t a = (int64_t)fr->regs[ra], bs = (int64_t)b;
                if (a == INT64_MIN && bs == -1)
                    return si_trap(ctx, SIMI_STATUS_TRAP_DIV_OVERFLOW, ctx->pc);
                fr->regs[rd] = (uint64_t)(a / bs);
            } else {
                fr->regs[rd] = fr->regs[ra] / b;
            }
            si_set_tag(fr, rd, 0); ctx->pc++; break;
        }
        case OP_MOD: {
            /* Float MOD is permanently unimplemented by ISA design (no
             * single hardware instruction on any target this project
             * ships for) -- a distinct status from the -mno-sse trap
             * above, because this one will never be implemented and that
             * one is waiting on SIMD enablement. */
            if (si_is_float(type)) return si_trap(ctx, SIMI_STATUS_TRAP_MOD_FLOAT, ctx->pc);
            uint64_t b = si_operand_b(w, fr);
            if (b == 0) return si_trap(ctx, SIMI_STATUS_TRAP_MOD_ZERO, ctx->pc);
            if (simi_type_signed((SimiType)type)) {
                int64_t a = (int64_t)fr->regs[ra], bs = (int64_t)b;
                if (a == INT64_MIN && bs == -1) fr->regs[rd] = 0;
                else fr->regs[rd] = (uint64_t)(a % bs);
            } else {
                fr->regs[rd] = fr->regs[ra] % b;
            }
            si_set_tag(fr, rd, 0); ctx->pc++; break;
        }

        case OP_AND: fr->regs[rd] = fr->regs[ra] & si_operand_b(w, fr); si_set_tag(fr, rd, 0); ctx->pc++; break;
        case OP_OR:  fr->regs[rd] = fr->regs[ra] | si_operand_b(w, fr); si_set_tag(fr, rd, 0); ctx->pc++; break;
        case OP_XOR: fr->regs[rd] = fr->regs[ra] ^ si_operand_b(w, fr); si_set_tag(fr, rd, 0); ctx->pc++; break;
        case OP_NOT: fr->regs[rd] = ~fr->regs[ra];                      si_set_tag(fr, rd, 0); ctx->pc++; break;

        case OP_NEG:
            if (si_is_float(type)) return si_trap(ctx, SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED, ctx->pc);
            fr->regs[rd] = (uint64_t)(-(int64_t)fr->regs[ra]);
            si_set_tag(fr, rd, 0); ctx->pc++; break;

        case OP_SHL: { uint64_t a = si_operand_b(w, fr) & 0x3F; fr->regs[rd] = fr->regs[ra] << a; si_set_tag(fr, rd, 0); ctx->pc++; break; }
        case OP_SHR: { uint64_t a = si_operand_b(w, fr) & 0x3F; fr->regs[rd] = fr->regs[ra] >> a; si_set_tag(fr, rd, 0); ctx->pc++; break; }
        case OP_SAR: {
            if (!simi_type_signed((SimiType)type))
                return si_trap(ctx, SIMI_STATUS_TRAP_SAR_UNSIGNED, ctx->pc);
            uint64_t a = si_operand_b(w, fr) & 0x3F;
            fr->regs[rd] = (uint64_t)((int64_t)fr->regs[ra] >> a);
            si_set_tag(fr, rd, 0); ctx->pc++; break;
        }

        /* MOV is the one opcode that PROPAGATES a capability tag rather
         * than clearing it -- copying a capability is legitimate, that is
         * how it reaches where it is needed. Every other register-writing
         * opcode clears, which is what makes capabilities unforgeable
         * (ISA §Phase 7). */
        case OP_MOV:   fr->regs[rd] = fr->regs[ra]; si_set_tag(fr, rd, fr->cap_tag[ra]); ctx->pc++; break;
        case OP_LOADI: fr->regs[rd] = (uint64_t)(int64_t)simi_imm28(w); si_set_tag(fr, rd, 0); ctx->pc++; break;
        case OP_LOADI64: {
            uint32_t idx = simi_rb_raw(w);
            if (idx >= obj->num_literals) return si_trap(ctx, SIMI_STATUS_TRAP_LITERAL_RANGE, ctx->pc);
            fr->regs[rd] = obj->literals[idx];
            si_set_tag(fr, rd, 0); ctx->pc++; break;
        }

        case OP_CMP: {
            if (si_is_float(type)) return si_trap(ctx, SIMI_STATUS_TRAP_FLOAT_UNSUPPORTED, ctx->pc);
            uint64_t a = fr->regs[ra], b = fr->regs[simi_rb_reg(w)];
            int result = 0;
            switch (simi_flags(w)) {
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
                default: return si_trap(ctx, SIMI_STATUS_TRAP_BAD_RELATION, ctx->pc);
            }
            fr->regs[rd] = result ? 1u : 0u;
            si_set_tag(fr, rd, 0); ctx->pc++; break;
        }

        case OP_BR:
            ctx->pc = (uint32_t)((int64_t)ctx->pc + 1 + simi_imm28(w));
            break;
        case OP_BC: {
            int invert = simi_flags(w) & FLAG_INVERT;
            int taken  = invert ? (fr->regs[ra] == 0) : (fr->regs[ra] != 0);
            ctx->pc = taken ? (uint32_t)((int64_t)ctx->pc + 1 + simi_imm28(w))
                            : ctx->pc + 1;
            break;
        }
        case OP_CALL: {
            uint32_t target = (uint32_t)((int64_t)ctx->pc + 1 + simi_imm28(w));
            if (ctx->frame_top + 1 >= SIMI_MAX_FRAMES)
                return si_trap(ctx, SIMI_STATUS_TRAP_CALL_OVERFLOW, ctx->pc);
            struct SimiFrame* nf = &ctx->frames[ctx->frame_top + 1];
            si_memzero(nf->regs, sizeof(nf->regs));
            si_memzero(nf->cap_tag, sizeof(nf->cap_tag));
            /* r0-r7 argument convention (§4.8); tags ride along with the
             * values, same plain-copy rule as MOV, just crossing a frame
             * boundary (ISA §Phase 12). */
            for (int i = 0; i < 8; i++) { nf->regs[i] = fr->regs[i]; si_set_tag(nf, (uint16_t)i, fr->cap_tag[i]); }
            nf->caller_frame = ctx->frame_top;
            nf->return_pc    = ctx->pc + 1;
            ctx->frame_top++;
            ctx->pc = target;
            break;
        }
        case OP_JMPR: {
            uint64_t target = fr->regs[ra];
            if (target >= obj->num_instr) return si_trap(ctx, SIMI_STATUS_TRAP_JMPR_RANGE, ctx->pc);
            ctx->pc = (uint32_t)target;
            break;
        }
        case OP_RET: {
            if (ctx->frame_top == 0) {
                ctx->result = ctx->frames[0].regs[0];
                ctx->status = SIMI_STATUS_HALTED;
                return SIMI_STATUS_HALTED;
            }
            uint64_t retval = fr->regs[0];
            uint8_t  rettag = fr->cap_tag[0];
            uint32_t resume = fr->return_pc;
            ctx->frame_top  = fr->caller_frame;
            ctx->frames[ctx->frame_top].regs[0] = retval;   /* §4.8 return convention */
            si_set_tag(&ctx->frames[ctx->frame_top], 0, rettag);
            ctx->pc = resume;
            break;
        }

        case OP_LOAD: {
            uint64_t addr = fr->regs[ra] + (uint64_t)(int64_t)simi_imm28(w);
            int wdt = si_width_of((SimiType)type);
            if (addr + (uint64_t)wdt > SIMI_MEM_SIZE)
                return si_trap(ctx, SIMI_STATUS_TRAP_MEM_BOUNDS, ctx->pc);
            uint64_t v = 0;
            si_memcpy(&v, &ctx->mem[addr], (uint32_t)wdt);
            if (simi_type_signed((SimiType)type) && wdt < 8) {
                int shift = 64 - wdt * 8;
                v = (uint64_t)(((int64_t)(v << shift)) >> shift);
            }
            fr->regs[rd] = v;
            si_set_tag(fr, rd, 0); ctx->pc++; break;
        }
        case OP_STORE: {
            uint64_t addr = fr->regs[ra] + (uint64_t)(int64_t)simi_imm28(w);
            int wdt = si_width_of((SimiType)type);
            if (addr + (uint64_t)wdt > SIMI_MEM_SIZE)
                return si_trap(ctx, SIMI_STATUS_TRAP_MEM_BOUNDS, ctx->pc);
            si_memcpy(&ctx->mem[addr], &fr->regs[rd], (uint32_t)wdt);
            ctx->pc++; break;
        }
        case OP_LEA:
            fr->regs[rd] = fr->regs[ra] + (uint64_t)(int64_t)simi_imm28(w);
            si_set_tag(fr, rd, 0); ctx->pc++; break;
        case OP_PTRADD: {
            uint64_t idx = si_operand_b(w, fr);
            fr->regs[rd] = fr->regs[ra] + idx * (uint64_t)si_width_of((SimiType)type);
            si_set_tag(fr, rd, 0);   /* pointer arithmetic never yields a capability */
            ctx->pc++; break;
        }

        /* ── Object ops: bound to the REAL catalog, not a mock ────────
         * This is where the kernel interpreter is strictly better than the
         * reference, which has no catalog to consult. Same three functions
         * the translated path calls (kernel/simi_runtime.c), so both
         * execution modes observe identical object state. */
        case OP_RESOLVE: {
            uint32_t idx = simi_rb_raw(w);
            if (idx >= obj->num_names) return si_trap(ctx, SIMI_STATUS_TRAP_NAME_RANGE, ctx->pc);
            uint64_t result = simi_rt_resolve(obj->names[idx].name);
            fr->regs[rd] = result;
            si_set_tag(fr, rd, result != 0);   /* tag ONLY a successful resolve */
            ctx->pc++; break;
        }
        case OP_OBJSIZE:
            /* An untagged operand is refused without consulting the
             * catalog at all -- that refusal IS the forgery defence, not
             * the lookup result. */
            fr->regs[rd] = fr->cap_tag[ra] ? simi_rt_objsize(fr->regs[ra]) : 0;
            si_set_tag(fr, rd, 0); ctx->pc++; break;
        case OP_OBJTYPE:
            fr->regs[rd] = fr->cap_tag[ra] ? simi_rt_objtype(fr->regs[ra]) : 0xFFFFFFFFu;
            si_set_tag(fr, rd, 0); ctx->pc++; break;

        default:
            return si_trap(ctx, SIMI_STATUS_TRAP_BAD_OPCODE, ctx->pc);
        }
    }

    /* Budget spent mid-program: resumable, and this is precisely the point
     * at which a checkpoint can be taken. */
    return SIMI_STATUS_OK;
}
