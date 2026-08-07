/*
 * a64_exec.c — see a64_exec.h for what this is and why it exists.
 *
 * Decode strategy: every family simi_arm.c emits has a distinct fixed
 * bit pattern in the top bytes, verified bit-for-bit against QEMU's
 * authoritative decoder (target/arm/tcg/a64.decode) and against canonical
 * constants (ldr x0,[x1] == 0xF9400020, add x0,x1,#0 == 0x91000020,
 * cset x0,eq == 0x9A9F17E0, cbz == 0xB4000000, br == 0xD61F0000,
 * mvn x0,x1 == 0xAA213E0 (orn x0,xzr,x1)). The checks below are ordered so overlapping
 * masks can't misfire (e.g. the 0x9A000000 csel/variable-shift/div split
 * uses the finer 0xFFE00000 mask, and the load/store family is fenced by
 * the 0x3F000000 mask that excludes LDUR/STUR and register-offset forms).
 */
#include "a64_exec.h"

static int64_t sext(int64_t val, int bits) {
    uint64_t mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1ull);
    uint64_t m = 1ull << (bits - 1);
    val &= (int64_t)mask;
    /* (val^m)-m computed in uint64_t arithmetic wraps to a huge unsigned
     * value whenever the result is "negative" — casting THAT straight to
     * int64_t zero-extends the wrapped bit pattern instead of sign-
     * extending the intended negative number. Must land in int32_t first
     * so the wrapped bit pattern is reinterpreted as signed, THEN widen
     * to int64_t, which sign-extends correctly from there. Same trap as
     * rv64_exec.c's sext(). */
    return (int64_t)(int32_t)((uint32_t)((uint64_t)(val ^ (int64_t)m) - m));
}

static int fetch32(struct A64Cpu* cpu, uint64_t addr, uint32_t* out) {
    if (addr + 4 > cpu->mem_size || (addr & 3)) return 0;
    *out = (uint32_t)cpu->mem[addr] | ((uint32_t)cpu->mem[addr+1]<<8) |
           ((uint32_t)cpu->mem[addr+2]<<16) | ((uint32_t)cpu->mem[addr+3]<<24);
    return 1;
}
static int load_mem(struct A64Cpu* cpu, uint64_t addr, int width, int is_signed, uint64_t* out) {
    if (addr + (uint64_t)width > cpu->mem_size) return 0;
    uint64_t v = 0;
    for (int i = 0; i < width; i++) v |= ((uint64_t)cpu->mem[addr+i]) << (8*i);
    if (is_signed && width < 8) {
        uint64_t signbit = 1ull << (8*width - 1);
        v = (v ^ signbit) - signbit;
    }
    *out = v;
    return 1;
}
static int store_mem(struct A64Cpu* cpu, uint64_t addr, int width, uint64_t val) {
    if (addr + (uint64_t)width > cpu->mem_size) return 0;
    for (int i = 0; i < width; i++) cpu->mem[addr+i] = (uint8_t)((val >> (8*i)) & 0xFF);
    return 1;
}

/* ─── NZCV flag model ───────────────────────────────────────────────────
 * add_flags/sub_flags compute the 64-bit result AND the N/Z/C/V flags
 * exactly as real A64 SUBS/ADDS do. The formulas:
 *   add: N = bit63(r), Z = (r==0), C = carry-out (r < a unsigned),
 *        V = ((~(a^b)) & (a^r)) >> 63   (operands same sign, result differs)
 *   sub: N = bit63(r), Z = (r==0), C = no-borrow (a >= b unsigned),
 *        V = ((a^b) & (a^r)) >> 63      (operands differ, result sign != a)
 * Only the subs-derived `cmp` path is emitted by simi_arm.c; adds is
 * implemented anyway for symmetry at negligible cost. */
static uint64_t add_flags(struct A64Cpu* cpu, uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    cpu->n = (uint8_t)((r >> 63) & 1);
    cpu->z = (uint8_t)(r == 0);
    cpu->c = (uint8_t)(r < a);
    cpu->v = (uint8_t)(((~(a ^ b)) & (a ^ r)) >> 63);
    return r;
}
static uint64_t sub_flags(struct A64Cpu* cpu, uint64_t a, uint64_t b) {
    uint64_t r = a - b;
    cpu->n = (uint8_t)((r >> 63) & 1);
    cpu->z = (uint8_t)(r == 0);
    cpu->c = (uint8_t)(a >= b);
    cpu->v = (uint8_t)(((a ^ b) & (a ^ r)) >> 63);
    return r;
}

/* A64 condition code evaluation, from the current NZCV. */
static int cond_true(struct A64Cpu* cpu, int cond) {
    switch (cond & 0xF) {
        case 0x0: return cpu->z;                          /* EQ */
        case 0x1: return !cpu->z;                         /* NE */
        case 0x2: return cpu->c;                          /* CS/HS */
        case 0x3: return !cpu->c;                         /* CC/LO */
        case 0x4: return cpu->n;                          /* MI */
        case 0x5: return !cpu->n;                         /* PL */
        case 0x6: return cpu->v;                          /* VS */
        case 0x7: return !cpu->v;                         /* VC */
        case 0x8: return cpu->c && !cpu->z;               /* HI */
        case 0x9: return !cpu->c || cpu->z;               /* LS */
        case 0xA: return cpu->n == cpu->v;                /* GE */
        case 0xB: return cpu->n != cpu->v;                /* LT */
        case 0xC: return !cpu->z && (cpu->n == cpu->v);   /* GT */
        case 0xD: return cpu->z || (cpu->n != cpu->v);    /* LE */
        default:  return 1;                               /* AL */
    }
}

/* XZR-context register write: Rd==31 discards (XZR), every other number
 * writes the register. SP-context writes (add/sub-immediate with Rd==31)
 * go through cpu->x[31] directly at the call site instead. */
static void set_x(struct A64Cpu* cpu, int rd, uint64_t v) {
    if (rd != 31) cpu->x[rd] = v;
}

/* XZR-context register read: 31 reads as zero. In the A64 base data-
 * processing families (shifted ALU, logical, csel, mul/div, variable
 * shift) register 31 is the zero register, NOT SP — SP only appears in
 * add/sub-immediate and in load/store base registers. Getting this wrong
 * silently reads the stack pointer as an operand (caught by a64_exec
 * actually executing: cset x9,gt read SP and produced a nonzero "GT",
 * sending loop_sum straight to its done label). */
static uint64_t rx(struct A64Cpu* cpu, int r) {
    return (r == 31) ? 0 : cpu->x[r];
}

/* v0.3 (Phase 6): host-callback table — see a64_exec.h's top comment. */
static ArHostFn g_hostfns[AR_EXEC_MAX_HOSTFNS];

void a64_exec_set_hostfn(int idx, ArHostFn fn) {
    if (idx >= 0 && idx < AR_EXEC_MAX_HOSTFNS) g_hostfns[idx] = fn;
}

const char* a64_exec_strerror(int code) {
    switch (code) {
        case AR_EXEC_OK: return "ok";
        case AR_EXEC_STEP_LIMIT: return "exceeded step budget (likely infinite loop or decode bug)";
        case AR_EXEC_BAD_INSTR: return "unimplemented or malformed instruction";
        case AR_EXEC_MEM_FAULT: return "fetch/load/store outside guest memory buffer";
        default: return "unknown error";
    }
}

int a64_exec_run(struct A64Cpu* cpu, uint64_t max_steps) {
    for (uint64_t step = 0; step < max_steps; step++) {
        if (cpu->pc == AR_EXEC_SENTINEL_LR) return AR_EXEC_OK;

        uint32_t w;
        if (!fetch32(cpu, cpu->pc, &w)) return AR_EXEC_MEM_FAULT;

        uint64_t next_pc = cpu->pc + 4;

        /* ─── MOVZ / MOVK (move wide immediate) ───────────────────────── */
        /* 64-bit: 1 10/11 100101 hw:2 imm16 rd — 0xD28/0xF28 top; 32-bit
         * forms share the same 0x7F800000 mask (0x52/0x72 after masking). */
        if ((w & 0x7F800000u) == 0x52800000u) {
            int sf = (int)((w >> 31) & 1);
            uint32_t hw = (w >> 21) & (sf ? 3u : 1u);
            uint64_t imm16 = (w >> 5) & 0xFFFF;
            int rd = (int)(w & 0x1F);
            uint64_t v = imm16 << (16 * hw);
            if (sf) { if (rd != 31) cpu->x[rd] = v; }
            else    { if (rd != 31) cpu->x[rd] = v & 0xFFFFFFFFull; }
            cpu->pc = next_pc;
            continue;
        }
        if ((w & 0x7F800000u) == 0x72800000u) {
            int sf = (int)((w >> 31) & 1);
            uint32_t hw = (w >> 21) & (sf ? 3u : 1u);
            uint64_t imm16 = (w >> 5) & 0xFFFF;
            int rd = (int)(w & 0x1F);
            if (rd != 31) {
                uint64_t mask = ~(0xFFFFull << (16 * hw));
                cpu->x[rd] = (cpu->x[rd] & mask) | (imm16 << (16 * hw));
                if (!sf) cpu->x[rd] &= 0xFFFFFFFFull;
            }
            cpu->pc = next_pc;
            continue;
        }

        /* ─── ADD/SUB/ADDS/SUBS (immediate) — 1 00/10/01/11 100010 sh ───
         * sh=1 (bit 22) scales imm12 by 12; simi_arm.c emits sh=1 for
         * the M2.7 shifted-immediate fold (multiples of 4096) and sh=0
         * elsewhere. 31 == SP here (the one place the subset uses the SP
         * interpretation: sub sp,sp,#16 and friends). */
        if ((w & 0xFF000000u) == 0x91000000u ||
            (w & 0xFF000000u) == 0xD1000000u ||
            (w & 0xFF000000u) == 0xF1000000u) {
            int s = (int)((w >> 29) & 1);
            int is_sub = (int)((w >> 30) & 1);
            uint64_t imm12 = (w >> 10) & 0xFFF;
            if (w & 0x400000u) imm12 <<= 12;            /* sh bit */
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            uint64_t a = cpu->x[rn];                    /* rn==31 reads SP */
            uint64_t r;
            if (is_sub) r = sub_flags(cpu, a, imm12);
            else        r = add_flags(cpu, a, imm12);
            if (s) set_x(cpu, rd, r);                   /* ADDS/SUBS: Rd==31 = XZR */
            else  cpu->x[rd] = r;                       /* ADD/SUB: Rd==31 = SP */
            cpu->pc = next_pc;
            continue;
        }

        /* ─── ADD/ADDS/SUB/SUBS (shifted register) ───────────────────────
         * 1 00/01/10/11 01011 shift:2 0 Rm imm6 Rn Rd. 31 is XZR in these
         * forms (sub x9, xzr, x9 for NEG; cmp = SUBS xzr). */
        if ((w & 0xFF000000u) == 0x8B000000u ||
            (w & 0xFF000000u) == 0xCB000000u ||
            (w & 0xFF000000u) == 0xEB000000u ||
            (w & 0xFF000000u) == 0xAB000000u) {
            int is_sub = (int)((w >> 30) & 1);
            int rm = (int)((w >> 16) & 0x1F);
            int shift = (int)((w >> 22) & 3);
            uint64_t imm6 = (w >> 10) & 0x3F;
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            uint64_t b = rx(cpu, rm);                   /* rm==31 = XZR */
            switch (shift) {
                case 0: b <<= imm6; break;              /* LSL */
                case 1: b >>= imm6; break;              /* LSR */
                default: b = (uint64_t)((int64_t)b >> imm6); break; /* ASR */
            }
            uint64_t a = rx(cpu, rn);                   /* rn==31 = XZR */
            uint64_t r;
            if (is_sub) r = sub_flags(cpu, a, b);
            else        r = add_flags(cpu, a, b);
            set_x(cpu, rd, r);                          /* shifted forms never use SP */
            cpu->pc = next_pc;
            continue;
        }

        /* ─── AND/ORR/EOR (shifted register) + ORN (N bit) ──────────────
         * 1 00/01/10 01010 shift N Rm imm6 Rn Rd. MVN = ORN xd, xzr, xm. */
        if ((w & 0xFF000000u) == 0x8A000000u ||
            (w & 0xFF000000u) == 0xAA000000u ||
            (w & 0xFF000000u) == 0xCA000000u) {
            int kind = (int)((w >> 29) & 3);            /* 0=AND,1=ORR,2=EOR */
            int nbit = (int)((w >> 21) & 1);
            int rm = (int)((w >> 16) & 0x1F);
            int shift = (int)((w >> 22) & 3);
            uint64_t imm6 = (w >> 10) & 0x3F;
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            uint64_t b = rx(cpu, rm);
            switch (shift) {
                case 0: b <<= imm6; break;
                case 1: b >>= imm6; break;
                default: b = (uint64_t)((int64_t)b >> imm6); break;
            }
            uint64_t a = rx(cpu, rn);
            uint64_t r;
            if (nbit) r = a | ~b;                       /* ORN (used by MVN) */
            else if (kind == 0) r = a & b;
            else if (kind == 1) r = a | b;
            else                r = a ^ b;
            set_x(cpu, rd, r);
            cpu->pc = next_pc;
            continue;
        }

        /* ─── Data-processing register families: csel/csinc/csinv/csneg,
         * variable shifts, mul/div/msub — all share the 1 00 1101/0110
         * top region; split by the finer 0xFFE00000 mask. */
        if ((w & 0xFFE00000u) == 0x9A000000u ||
            (w & 0xFFE00000u) == 0x9A800000u) {         /* CSEL family */
            int else_inv = (int)((w >> 30) & 1);
            int else_inc = (int)((w >> 10) & 1);
            int rm = (int)((w >> 16) & 0x1F);
            int cond = (int)((w >> 12) & 0xF);
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            uint64_t a = rx(cpu, rn), b = rx(cpu, rm);  /* cset reads XZR here */
            int take = cond_true(cpu, cond);
            uint64_t r;
            if (!else_inv && !else_inc) r = take ? a : b;           /* CSEL */
            else if (!else_inv && else_inc) r = take ? a : b + 1;   /* CSINC */
            else if (else_inv && !else_inc) r = take ? ~a : ~b;     /* CSINV */
            else                            r = take ? ~a + 1 : b;  /* CSNEG */
            set_x(cpu, rd, r);
            cpu->pc = next_pc;
            continue;
        }
        if ((w & 0xFFE00000u) == 0x9AC00000u) {         /* LSLV/LSRV/ASRV/SDIV/UDIV */
            int rm = (int)((w >> 16) & 0x1F);
            uint32_t funct = (w >> 10) & 0x3F;
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            uint64_t a = rx(cpu, rn), b = rx(cpu, rm);
            uint64_t r;
            switch (funct) {
                case 0x08: r = a << (b & 0x3F); break;              /* LSLV */
                case 0x09: r = a >> (b & 0x3F); break;              /* LSRV */
                case 0x0A: r = (uint64_t)((int64_t)a >> (b & 0x3F)); break; /* ASRV */
                case 0x02: r = b == 0 ? 0 : a / b; break;           /* UDIV */
                case 0x03: r = b == 0 ? 0 : (uint64_t)((int64_t)a / (int64_t)b); break; /* SDIV */
                default: return AR_EXEC_BAD_INSTR;
            }
            set_x(cpu, rd, r);
            cpu->pc = next_pc;
            continue;
        }
        if ((w & 0xFFE00000u) == 0x9B000000u) {         /* MADD/MSUB */
            int sub = (int)((w >> 15) & 1);
            int rm = (int)((w >> 16) & 0x1F);
            int ra = (int)((w >> 10) & 0x1F);
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            uint64_t prod = rx(cpu, rn) * rx(cpu, rm);
            uint64_t r = sub ? rx(cpu, ra) - prod : rx(cpu, ra) + prod;  /* MUL: ra==31 = XZR */
            set_x(cpu, rd, r);
            cpu->pc = next_pc;
            continue;
        }

        /* ─── Load/store register, pre-indexed (9-bit signed imm, ─────
         * writeback): size:2 111 0 00 opc 0 imm9:9 11 Rn Rt. M2.9:
         * simi_arm.c emits these for negative displacements that are
         * aligned to the access width and fit imm9 (-256..255) — one
         * word vs the add/sub-imm address math + zero-offset access.
         * Fenced by the 0x3B000000 mask (bits 29:27 = 111, 26 = 0,
         * 25:24 = 00, 21 = 0 — the 9-bit-immediate load/store family;
         * the scaled class has 25:24 = 01 and register-offset has
         * bit 21 = 1) and bits 11:10 = 11 (pre-indexed: 00 = the
         * unscaled ldur/stur form, 01 = post-indexed — neither is
         * emitted, both fall through to BAD_INSTR as before).
         * Writeback order follows the ARM pseudocode: X[n] is updated
         * to the effective address BEFORE Rt is read/stored, so
         * ldr xt,[xn,#imm]! with rt==rn yields the loaded value and
         * str xt,[xn,#imm]! with rt==rn stores the address. Verified
         * against QEMU's a64.decode @ldst_imm_pre and the canonical
         * str x29, [sp, #-16]! == 0xF81F0FFD. */
        if ((w & 0x3B000000u) == 0x38000000u && (w & 0xC00u) == 0xC00u) {
            int size = (int)(w >> 30);
            int opc = (int)((w >> 22) & 3);
            int64_t imm9 = sext((w >> 12) & 0x1FF, 9);
            int rn = (int)((w >> 5) & 0x1F);
            int rt = (int)(w & 0x1F);
            int width = 1 << size;
            uint64_t addr = cpu->x[rn] + imm9;
            cpu->x[rn] = addr;                          /* writeback (rn==31 = SP, per A64) */
            if (opc == 0) {                             /* STR — Rt read AFTER writeback */
                uint64_t sv = (rt == 31) ? 0 : cpu->x[rt];
                if (!store_mem(cpu, addr, width, sv)) return AR_EXEC_MEM_FAULT;
            } else if (opc == 1) {                      /* LDR — result overwrites the writeback when rt==rn */
                uint64_t v;
                if (!load_mem(cpu, addr, width, 0, &v)) return AR_EXEC_MEM_FAULT;
                set_x(cpu, rt, v);
            } else if (opc == 2) {                      /* LDRS (sign-extend) */
                uint64_t v;
                if (!load_mem(cpu, addr, width, 1, &v)) return AR_EXEC_MEM_FAULT;
                set_x(cpu, rt, v);
            } else {
                return AR_EXEC_BAD_INSTR;               /* opc 11 not emitted */
            }
            cpu->pc = next_pc;
            continue;
        }

        /* ─── Load/store register (unsigned scaled 12-bit immediate) ────
         * size:2 111 0 01 opc:2 imm12 Rn Rt. opc 00=STR, 01=LDR
         * (zero-extend), 10=LDRS (sign-extend), 11=pre/post (unused).
         * Rn==31 reads SP; Rt==31 is XZR. Verified against canonical
         * ldr x0,[x1] == 0xF9400020 and str x0,[x1] == 0xF9000020. */
        if ((w & 0x3F000000u) == 0x39000000u) {
            int size = (int)(w >> 30);
            int opc = (int)((w >> 22) & 3);
            uint64_t imm12 = (w >> 10) & 0xFFF;
            int rn = (int)((w >> 5) & 0x1F);
            int rt = (int)(w & 0x1F);
            int width = 1 << size;
            uint64_t addr = cpu->x[rn] + (imm12 << size);
            if (opc == 0) {                             /* STR — Rt==31 is XZR (stores zero), only Rn can be SP */
                uint64_t sv = (rt == 31) ? 0 : cpu->x[rt];
                if (!store_mem(cpu, addr, width, sv)) return AR_EXEC_MEM_FAULT;
            } else if (opc == 1) {                      /* LDR */
                uint64_t v;
                if (!load_mem(cpu, addr, width, 0, &v)) return AR_EXEC_MEM_FAULT;
                set_x(cpu, rt, v);
            } else if (opc == 2) {                      /* LDRS */
                uint64_t v;
                if (!load_mem(cpu, addr, width, 1, &v)) return AR_EXEC_MEM_FAULT;
                set_x(cpu, rt, v);
            } else {
                return AR_EXEC_BAD_INSTR;
            }
            cpu->pc = next_pc;
            continue;
        }

        /* ─── Unconditional branch (B) and branch-and-link (BL) ──────── */
        if ((w & 0xFC000000u) == 0x14000000u) {
            int64_t off = sext((w & 0x03FFFFFFu) << 2, 28);
            cpu->pc = (uint64_t)((int64_t)cpu->pc + off);
            continue;
        }
        if ((w & 0xFC000000u) == 0x94000000u) {
            int64_t off = sext((w & 0x03FFFFFFu) << 2, 28);
            cpu->x[30] = next_pc;
            cpu->pc = (uint64_t)((int64_t)cpu->pc + off);
            continue;
        }

        /* ─── CBZ / CBNZ (compare and branch on zero) ─────────────────── */
        if ((w & 0x7E000000u) == 0x34000000u) {
            int sf = (int)((w >> 31) & 1);
            int nz = (int)((w >> 24) & 1);
            int rt = (int)(w & 0x1F);
            int64_t off = sext(((w >> 5) & 0x7FFFFu) << 2, 21);
            uint64_t v = (rt == 31) ? 0 : (sf ? cpu->x[rt] : (cpu->x[rt] & 0xFFFFFFFFull));
            int taken = nz ? (v != 0) : (v == 0);
            if (taken) cpu->pc = (uint64_t)((int64_t)cpu->pc + off);
            else       cpu->pc = next_pc;
            continue;
        }

        /* ─── BR / BLR / RET (branch to register) ───────────────────────
         * 1101011 0opc 11111 000000 Rn 00000. BR = 0xD61F0000, BLR =
         * 0xD63F0000, RET = 0xD65F0000 (bits 20:16 == 11111 for all). */
        if ((w & 0xFFFFFC00u) == 0xD61F0000u) {
            int rn = (int)((w >> 5) & 0x1F);
            uint64_t target = cpu->x[rn];
            cpu->pc = target;
            continue;
        }
        if ((w & 0xFFFFFC00u) == 0xD63F0000u) {
            int rn = (int)((w >> 5) & 0x1F);
            uint64_t target = cpu->x[rn];
            if ((target & AR_EXEC_HOSTFN_MASK) == AR_EXEC_HOSTFN_BASE) {
                /* v0.3 (Phase 6): host-callback sentinel — call the
                 * registered host function instead of fetching guest
                 * instructions at a fake address; x0 is both the argument
                 * and, per real A64 ABI convention, where the result
                 * lands. Resume at `link` (pc+4), exactly as if this had
                 * been a real call that returned immediately. */
                uint32_t idx = (uint32_t)(target & 0xFFFFFFFFull) / AR_EXEC_HOSTFN_STRIDE;
                if (idx >= AR_EXEC_MAX_HOSTFNS || !g_hostfns[idx]) return AR_EXEC_BAD_INSTR;
                cpu->x[0] = g_hostfns[idx](cpu->x[0]);
                cpu->pc = next_pc;
            } else {
                cpu->x[30] = next_pc;
                cpu->pc = target;
            }
            continue;
        }
        if ((w & 0xFFFFFC00u) == 0xD65F0000u) {
            int rn = (int)((w >> 5) & 0x1F);
            cpu->pc = cpu->x[rn];                       /* rn==30 == LR for plain `ret` */
            continue;
        }

        /* Anything else — including UDF #0 (0x00000000), simi_arm.c's
         * illegal-word for JMPR out-of-bounds: A64's all-zeros word is
         * permanently reserved as an unconditional undefined instruction
         * (UDF #0), the direct analog of RV64's reserved all-zeros word
         * and x86's ud2. This executor has no trap model, so it reports
         * the fault instead — exactly the "must NOT transfer control"
         * property jmpr_oob.simi asserts. */
        return AR_EXEC_BAD_INSTR;
    }
    return AR_EXEC_STEP_LIMIT;
}
