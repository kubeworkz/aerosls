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

/* ─── F0 (Phase 10 float): IEEE-754 bit-reinterpretation ────────────────
 * T_F64 = a double in the low 64 bits of vN, T_F32 = a single in the low
 * 32 bits (high 32 zeroed on s-form writes) — the same union trick as
 * simi_interp.c's f64_of_bits/bits_of_f64, so the executor and the
 * reference interpreter compute the identical bit patterns: same C
 * `double`/`float` types, same -std=c11 -O2 flags (no -ffast-math),
 * default IEEE-754 rounding. That sameness IS the F0 bit-identity
 * contract — a64_exec.c is host-only tooling like simi_interp.c, never
 * compiled into the kernel's -mno-sse builds. */
static double f64_of_bits(uint64_t b) { union { uint64_t u; double d; } c; c.u = b; return c.d; }
static uint64_t bits_of_f64(double d) { union { uint64_t u; double d; } c; c.d = d; return c.u; }
static float f32_of_bits(uint64_t b) { union { uint32_t u; float f; } c; c.u = (uint32_t)b; return c.f; }
static uint64_t bits_of_f32(float f) { union { uint32_t u; float f; } c; c.f = f; return (uint64_t)c.u; }

/* F0/M3: real A64 FCMP flag model, pinned EMPIRICALLY on real A64
 * (aarch64-linux-gnu-gcc + qemu-aarch64, M3's fcmp_probe4.c): an
 * unordered (NaN) FCMP sets NZCV = N=0, Z=0, C=1, V=1, and ordered
 * ones C = (a >= b), Z = (a == b), N = (a < b), V = 0. From those
 * flags the condition codes give
 *   EQ (Z)          : equal → 1, NaN → 0            — IEEE EQ ✓
 *   NE (!Z)         : NaN → 1                        — IEEE NE ✓
 *   LT (N != V)     : NaN → 1 !!                     — IEEE LT ✗
 *   LE (Z || N!=V)  : NaN → 1 !!                     — IEEE LE ✗
 *   GT (!Z && N==V) : unordered → 0                  — IEEE GT ✓
 *   GE (N == V)     : unordered → 0                  — IEEE GE ✓
 * So LT and LE cannot be a single cset lt/le — the classic A64 NaN
 * gotcha (N=0, V=1 for unordered, so N!=V and Z||N!=V are both
 * true) — and the F1 codegen emits cset mi (N) for LT and cset ls
 * (!C || Z) for LE, each 0 on unordered; EQ/NE/GT/GE are the naive
 * cset eq/ne/gt/ge, all IEEE-correct with NO operand swap. The
 * pre-M3 model (N=1, Z=0) was wrong and the swapped-operand GT/GE
 * trick was tuned to it — encoder and decoder AGREED, so four-way
 * parity passed while real A64 disagreed (float_ops = 16, not 15);
 * M3's real-execution leg caught the agreeing pair. The three NaN
 * checks in float_ops.simi + the NaN LT/LE/GT/GE pins in
 * a64_f0_test.c are the discriminators. */
static void fcmp_flags(struct A64Cpu* cpu, int is_d, uint64_t abits, uint64_t bbits) {
    int nan, lt, eq, ge;
    if (is_d) {
        double a = f64_of_bits(abits), b = f64_of_bits(bbits);
        nan = (a != a) || (b != b);
        lt = (a < b); eq = (a == b); ge = (a >= b);
    } else {
        float a = f32_of_bits(abits), b = f32_of_bits(bbits);
        nan = (a != a) || (b != b);
        lt = (a < b); eq = (a == b); ge = (a >= b);
    }
    cpu->n = (uint8_t)(!nan && lt);
    cpu->z = (uint8_t)(!nan && eq);
    cpu->c = (uint8_t)(nan || ge);
    cpu->v = (uint8_t)nan;
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
        cpu->steps++;   /* M2.74: one executed instruction (bench_exec.c's exact guard) */

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

        /* ─── ADR (PC-relative address) — 0 00 10000 immlo:1 immhi:19 Rd ─
         * M3: the dynamic-JMPR dispatch loads its table base with adr
         * (PC + signed 21-bit byte offset), which is how the table is
         * reachable on REAL A64 — the pre-M3 movz+movk base baked a bare
         * byte OFFSET into out_buf that only a64_exec's guest convention
         * could branch to (a real `br` to 0xNNN faults). imm =
         * (immhi << 2) | immlo, sign-extended from bit 20 (bytes). PC
         * here is cpu->pc — the byte offset of this instruction — which
         * is exactly the guest value real A64 produces by adding the
         * real PC, so the table lands at the same place in both
         * conventions and the signed RELATIVE entries (offset −
         * table_off, M3) resolve to absolute targets in both. ADRP (bit
         * 31 set) is never emitted. */
        if ((w & 0x9F000000u) == 0x10000000u) {
            uint32_t immlo = (w >> 23) & 1;
            int32_t imm = (int32_t)((((w >> 5) & 0x7FFFFu) << 2) | immlo);
            if (imm & (1 << 20)) imm |= (int32_t)0xFFE00000u;  /* sign-extend 21 bits */
            set_x(cpu, (int)(w & 0x1F), cpu->pc + (int64_t)imm);
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

        /* ─── 32-bit ORR (register) — A3 (Phase 15): emitted only as
         * `mov w12, w12` (enc_orr_32, the i32 CAS expected-value mask —
         * orr wd, wzr, wm). The W-form write zero-extends into the
         * upper 32 bits, like every other 32-bit register write here.
         * Only the shift=0, N=0 form is emitted; anything else in the
         * 32-bit logical family faults. */
        if ((w & 0xFFE00000u) == 0x2A000000u) {
            int nbit = (int)((w >> 21) & 1);
            int rm = (int)((w >> 16) & 0x1F);
            int shift = (int)((w >> 22) & 3);
            uint64_t imm6 = (w >> 10) & 0x3F;
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            if (nbit || shift != 0 || imm6 != 0) return AR_EXEC_BAD_INSTR;
            uint64_t a = rx(cpu, rn);
            uint64_t b = rx(cpu, rm);
            set_x(cpu, rd, (a | b) & 0xFFFFFFFFull);
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

        /* ─── Scalar floating point, the 0x1E family (F0, Phase 10) —──
         * sf=0 (bit 31 = 0), u=0 (bit 30 = 0), bits 28:24 = 11110:
         * the fenced mask (w & 0x1F000000) == 0x1E000000 and bit 21 =
         * 1 selects exactly {FADD/FSUB/FMUL/FDIV 3-same, FMOV 1-src
         * scalar copy, FCMP, FMOV general 32-bit}. Bit 22 = sz (0 = S,
         * 1 = D); bit 23 = 1 is fp16 (0x3E family — never emitted,
         * BAD_INSTR). Split by bits 15:10, verified bit-for-bit
         * against QEMU's a64.decode:
         *   FMUL 000010, FDIV 000110, FADD 001010, FSUB 001110
         *     (Rm@20:16, Rn@9:5, Rd@4:0)
         *   1-src: 010000 (opcode@20:16 — 00000 = FMOV copy only)
         *   FCMP:  001000 (Rm@20:16, Rn@9:5, e@4, z@3)
         *   FMOV general 32: 000000 (opcode@20:16 — 00110 = w←s,
         *     00111 = s←w; sf=0 forces type 00, bit 22 = 0)
         * The s-form arithmetic/fmov writes zero the high 32 bits of
         * the destination f-register, per real A64. */
        if ((w & 0x1F000000u) == 0x1E000000u && (w & 0x200000u) && !(w & 0x80000000u)) {
            int sz = (int)((w >> 22) & 1);          /* 0 = S, 1 = D */
            uint32_t mid = (w >> 10) & 0x3F;        /* bits 15:10 */
            int rm = (int)((w >> 16) & 0x1F);
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            if (sz) {                               /* 64-bit d-form */
                switch (mid) {
                    case 0x02: cpu->f[rd] = bits_of_f64(f64_of_bits(cpu->f[rn]) * f64_of_bits(cpu->f[rm])); break;
                    case 0x06: cpu->f[rd] = bits_of_f64(f64_of_bits(cpu->f[rn]) / f64_of_bits(cpu->f[rm])); break;
                    case 0x0A: cpu->f[rd] = bits_of_f64(f64_of_bits(cpu->f[rn]) + f64_of_bits(cpu->f[rm])); break;
                    case 0x0E: cpu->f[rd] = bits_of_f64(f64_of_bits(cpu->f[rn]) - f64_of_bits(cpu->f[rm])); break;
                    case 0x10: {                    /* 1-src: FMOV Dd, Dn */
                        if (((w >> 16) & 0x1F) != 0) return AR_EXEC_BAD_INSTR;  /* FABS/FNEG/FSQRT/FRINT not emitted */
                        cpu->f[rd] = cpu->f[rn];
                        break;
                    }
                    case 0x08: {                    /* FCMP Dn, Dm / Dn, #0.0 */
                        int with_zero = (int)((w >> 3) & 1);
                        fcmp_flags(cpu, 1, cpu->f[rn], with_zero ? 0ull : cpu->f[rm]);
                        break;
                    }
                    default: return AR_EXEC_BAD_INSTR;   /* 0x00 + sz=1, FP-imm, FCVT, fp16 — not emitted */
                }
            } else {                                /* 32-bit s-form */
                switch (mid) {
                    case 0x02: cpu->f[rd] = bits_of_f32(f32_of_bits(cpu->f[rn]) * f32_of_bits(cpu->f[rm])); break;
                    case 0x06: cpu->f[rd] = bits_of_f32(f32_of_bits(cpu->f[rn]) / f32_of_bits(cpu->f[rm])); break;
                    case 0x0A: cpu->f[rd] = bits_of_f32(f32_of_bits(cpu->f[rn]) + f32_of_bits(cpu->f[rm])); break;
                    case 0x0E: cpu->f[rd] = bits_of_f32(f32_of_bits(cpu->f[rn]) - f32_of_bits(cpu->f[rm])); break;
                    case 0x10: {                    /* 1-src: FMOV Sd, Sn */
                        if (((w >> 16) & 0x1F) != 0) return AR_EXEC_BAD_INSTR;
                        cpu->f[rd] = cpu->f[rn] & 0xFFFFFFFFull;   /* zero the high 32 */
                        break;
                    }
                    case 0x08: {                    /* FCMP Sn, Sm / Sn, #0.0 */
                        int with_zero = (int)((w >> 3) & 1);
                        fcmp_flags(cpu, 0, cpu->f[rn], with_zero ? 0ull : cpu->f[rm]);
                        break;
                    }
                    case 0x00: {                    /* FMOV general 32-bit: fmov w0,s1 / fmov s0,w1 */
                        uint32_t opc = (w >> 16) & 0x1F;
                        if (opc == 0x06)      cpu->x[rd] = cpu->f[rn] & 0xFFFFFFFFull;   /* FP → GP, zero-extend */
                        else if (opc == 0x07) cpu->f[rd] = cpu->x[rn] & 0xFFFFFFFFull;   /* GP → FP, zero the high 32 */
                        else return AR_EXEC_BAD_INSTR;   /* FCVT/SCVTF/etc. not emitted */
                        break;
                    }
                    default: return AR_EXEC_BAD_INSTR;
                }
            }
            cpu->pc = next_pc;
            continue;
        }

        /* ─── FMOV general 64-bit (F0): fmov x0,d1 / fmov d0,x1 —──
         * sf=1 (bit 31) family: 1001 1110 01 1 <opc> 000000 Rn Rd,
         * i.e. (w & 0xFF000000) == 0x9E000000 with type (bits 23:22)
         * = 01 (D); type 10/11 are the fp16/Q forms, never emitted.
         * opcode@20:16: 00110 = FP → GP (Xd, Dn), 00111 = GP → FP
         * (Dd, Xn). Verified against QEMU's a64.decode FMOV_xd /
         * FMOV_dx patterns. */
        if ((w & 0xFF000000u) == 0x9E000000u) {
            if ((w & 0xC00000u) != 0x400000u) return AR_EXEC_BAD_INSTR;  /* type != 01 */
            if (((w >> 10) & 0x3F) != 0)       return AR_EXEC_BAD_INSTR;
            uint32_t opc = (w >> 16) & 0x1F;
            int rn = (int)((w >> 5) & 0x1F);
            int rd = (int)(w & 0x1F);
            if (opc == 0x06)            cpu->x[rd] = cpu->f[rn];
            else if (opc == 0x07)       cpu->f[rd] = cpu->x[rn];
            else return AR_EXEC_BAD_INSTR;
            cpu->pc = next_pc;
            continue;
        }

        /* ─── Load/store register, unscaled immediate (ldur/stur): ────
         * size:2 111 0 00 opc 0 imm9:9 00 Rn Rt. M2.10: simi_arm.c
         * emits these for ANY displacement in A64's signed 9-bit imm9
         * ([-256, 255]) — one word, with NO writeback and NO alignment
         * requirement, superseding the M2.9 pre-indexed form (which
         * required alignment to the access width and writeback).
         * Fenced by the 0x3B000000 mask (bits 29:27 = 111, 26 = 0,
         * 25:24 = 00, 21 = 0 — the 9-bit-immediate load/store family;
         * the scaled class has 25:24 = 01 and register-offset has
         * bit 21 = 1) and bits 11:10 = 00 (unscaled: 01 = post-indexed
         * and 11 = pre-indexed are never emitted — the M2.9 pre-indexed
         * fold was replaced by this form — and both fall through to
         * BAD_INSTR below). No writeback: X[n] is never modified, so
         * rt==rn reads the base then overwrites it with the loaded
         * value exactly as the scaled form does. Verified against
         * QEMU's a64.decode @ldst_imm9 and the canonical
         * stur x29, [sp, #-16] == 0xF81F03FD. */
        if ((w & 0x3B000000u) == 0x38000000u && (w & 0xC00u) == 0x0000u) {
            int size = (int)(w >> 30);
            int opc = (int)((w >> 22) & 3);
            int64_t imm9 = sext((w >> 12) & 0x1FF, 9);
            int rn = (int)((w >> 5) & 0x1F);
            int rt = (int)(w & 0x1F);
            int width = 1 << size;
            uint64_t addr = cpu->x[rn] + imm9;          /* no writeback — X[n] untouched */
            if (opc == 0) {                             /* STR */
                uint64_t sv = (rt == 31) ? 0 : cpu->x[rt];
                if (!store_mem(cpu, addr, width, sv)) return AR_EXEC_MEM_FAULT;
            } else if (opc == 1) {                      /* LDR */
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

        /* ─── Load/store register, REGISTER offset (ldr/str xt,[xb,xm]) ─
         * size:2 111 0 00 opc 1 Rm opt:3 s:1 10 Rn Rt. M2.11: simi_arm.c
         * emits these for displacements past the imm12 range (the
         * materialize path) — the displacement lives in a register (X_DR
         * = x12 on a run of same-displacement accesses, a scratch
         * otherwise) and the access itself is one word. Fenced by bit 21
         * = 1 and bits 11:10 = 10 (the imm9 class has bit 21 = 0, the
         * scaled class 25:24 = 01), and only two options are emitted,
         * pinned against QEMU's a64.decode @ldst: opt = 011 (LSL #0,
         * Xm used in full) with size = 11 — a 64-bit two's-complement
         * displacement wraps the address add correctly — and opt = 110
         * (SXTW, the low 32 bits sign-extended) with the narrower sizes;
         * any other option, or a narrow LSL (which would zero-extend and
         * is never emitted), faults. X[n] is untouched; Rm = 31 reads
         * XZR (zero). */
        if (((w & 0x3F20FC00u) == 0x38206800u || (w & 0x3F20FC00u) == 0x3820C800u)) {
            int size = (int)(w >> 30);
            int opc = (int)((w >> 22) & 3);
            int opt = (int)((w >> 13) & 7);
            int rm = (int)((w >> 16) & 0x1F);
            int rn = (int)((w >> 5) & 0x1F);
            int rt = (int)(w & 0x1F);
            int width = 1 << size;
            int64_t off;
            if (opt == 3) {                              /* LSL #0 — Xm in full */
                if (size != 3) return AR_EXEC_BAD_INSTR; /* narrow LSL not emitted (would zero-extend) */
                off = (int64_t)rx(cpu, rm);
            } else if (opt == 6) {                       /* SXTW — sign-extend the low 32 bits */
                off = (int64_t)(int32_t)rx(cpu, rm);
            } else {
                return AR_EXEC_BAD_INSTR;                /* options 000-010/100-101/111 not emitted */
            }
            uint64_t addr = cpu->x[rn] + (uint64_t)off;  /* 64-bit wrap — negative offsets just work */
            if (opc == 0) {                             /* STR */
                if (cpu->excl_valid && addr == cpu->excl_addr) cpu->excl_valid = 0;  /* A3: a store to the watched address clears the monitor */
                uint64_t sv = (rt == 31) ? 0 : cpu->x[rt];
                if (!store_mem(cpu, addr, width, sv)) return AR_EXEC_MEM_FAULT;
            } else if (opc == 1) {                      /* LDR */
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
                if (cpu->excl_valid && addr == cpu->excl_addr) cpu->excl_valid = 0;  /* A3: a store to the watched address clears the monitor */
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

        /* ─── B.cond (conditional branch on NZCV flags) ──────────────────
         * 0101 0100 0 imm19:19 0 cond:4 00000 == 0x54000000 (bits 31:24
         * and bit 4 are fixed). M2.25's inline JMPR chain emits
         * cmp (subs xzr) + b.eq pairs; cond_true() is the same condition
         * table cset uses. */
        if ((w & 0xFF000010u) == 0x54000000u) {
            int cond = (int)(w & 0xF);   /* cond is bits 3:0 — reading 15:12
                                          * grabbed imm19's top nibble and
                                          * misdecoded every b.eq whose offset
                                          * exceeded 512 bytes as NE instead of
                                          * EQ (M2.30's 15-candidate chain
                                          * exposed it; M2.25-M2.29's shorter
                                          * chains stayed under the threshold) */
            int64_t off = sext(((w >> 5) & 0x7FFFFu) << 2, 21);
            if (cond_true(cpu, cond)) cpu->pc = (uint64_t)((int64_t)cpu->pc + off);
            else                      cpu->pc = next_pc;
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

        /* ─── Exclusive-access family (ldxr/ldaxr/stxr/stlxr) — ────────
         * Gap Remediation SIMI Phase 15 (A3, plan D2): the atomics loops
         * emit ldaxr + stlxr. The family shares bits 29:24 = 001000
         * (mask 0x3F000000 == 0x08000000), with bit 22 separating load
         * (1) from store (0), bit 15 the acquire/release bit (1 =
         * ldaxr/stlxr — the only forms emitted), and size (bits 31:30)
         * the width (11 = 64-bit X, 10 = 32-bit W — W forms
         * zero-extend the loaded value / store the low 32 bits, exactly
         * the interpreter's width-masked semantics). The exclusive-
         * monitor model (see the A64Cpu struct): LDAXR sets the
         * monitor; STLXR succeeds iff it is valid and matches, then
         * clears it; any other store to the watched address clears it
         * too (the STR paths above). The single-threaded guest's
         * atomics loops emit no store between the pair, so stlxr
         * succeeds first try here — the retry path needs real
         * hardware (documented in the plan). */
        if ((w & 0x3F000000u) == 0x08000000u) {
            int size = (int)(w >> 30);
            int is_load = (int)((w >> 22) & 1);
            int rn = (int)((w >> 5) & 0x1F);
            int rt = (int)(w & 0x1F);
            int width = 1 << size;
            if (width != 4 && width != 8) return AR_EXEC_BAD_INSTR;
            uint64_t addr = cpu->x[rn];
            if (is_load) {
                uint64_t v;
                if (!load_mem(cpu, addr, width, 0, &v)) return AR_EXEC_MEM_FAULT;
                set_x(cpu, rt, v);
                cpu->excl_valid = 1;                 /* monitor armed on the loaded address */
                cpu->excl_addr = addr;
            } else {
                int rs = (int)((w >> 16) & 0x1F);    /* status: 0 = success, 1 = failure */
                uint64_t sv = (rt == 31) ? 0 : cpu->x[rt];
                int ok = cpu->excl_valid && cpu->excl_addr == addr;
                if (ok) {
                    if (!store_mem(cpu, addr, width, sv)) return AR_EXEC_MEM_FAULT;
                    set_x(cpu, rs, 0);
                } else {
                    set_x(cpu, rs, 1);
                }
                cpu->excl_valid = 0;                 /* the monitor is always cleared by STXR */
            }
            cpu->pc = next_pc;
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
