/*
 * a64_f0_test.c — F0 gate for the A64 float decode+execute work
 * (docs/AeroSLS-SIMI-Float-Atomics-Plan-v0.1.md §3 F0).
 *
 * simi_arm.c cannot yet emit float code — that is F1 — so this harness
 * hand-assembles the EXACT semantics of tests/float_ops.simi into A64
 * words using precisely the encodings F0 added to a64_exec.c, executes
 * them on the purpose-built executor, and pins three things:
 *
 *   1. The program returns AR_EXEC_OK and lands 15 in x0 — the same 15
 *      the reference interpreter produces running the real
 *      float_ops.simi (verified separately with `simi-run`, and on real
 *      hardware with `simi-jit-test`, whose x86 Phase 10 codegen already
 *      executes the same program to 15).
 *
 *   2. Every one of the 10 arithmetic results is bit-identical to the
 *      value the interpreter computes: the expected bit patterns are
 *      derived HERE from C `double`/`float` arithmetic with the same
 *      union trick (same platform, same -std=c11 -O2, no -ffast-math —
 *      that sameness is the contract), then compared against the
 *      executor's f-register bits.
 *
 *   3. Every one of the 17 CMP check values (9 f64 + 5 f32 + 3 NaN) is
 *      the expected 0/1 — pinning the fcmp NZCV model including the
 *      unordered-NaN case — plus two extra NaN pins for GT and GE
 *      (float_ops only NaN-tests EQ/NE/LT; the plan's D5 mandate was to
 *      verify the full unordered mapping, and GT/GE are exactly the two
 *      relations a single cset gets WRONG, which is why F1 compares
 *      them via swapped operands).
 *
 * Every word below is annotated with the QEMU a64.decode pattern it was
 * derived from — the same authoritative source the executor's other
 * families cite (FADD_s/FSUB_s/FMUL_s/FDIV_s 0001 1110 ..1 ..... <opc6>,
 * FMOV_s 0001 1110 ..1 00000 010000, FCMP 00011110 .. 1 rm 001000 rn
 * e z 000, FMOV_xd/ws/dx/sw 0/1 0011110 <type> 1 100110/100111 000000).
 * A transcription error in either direction (word or decoder) shows up
 * as a wrong value or a fault, not a silent pass.
 */
#include "a64_exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── word encoders (mirror a64_enc_check.py's bit layout) ─────────────── */
static uint32_t movz(int rd, uint16_t imm16, int hw) { return 0xD2800000u | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | (rd & 0x1F); }
static uint32_t movk(int rd, uint16_t imm16, int hw) { return 0xF2800000u | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | (rd & 0x1F); }
static uint32_t eor_sh(int rd, int rn, int rm)       { return 0xCA000000u | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F); }
static uint32_t add_sh(int rd, int rn, int rm)       { return 0x8B000000u | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F); }
static uint32_t orr_sh(int rd, int rn, int rm)       { return 0xAA000000u | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F); }
static uint32_t cset(int rd, int cond)               { return 0x9A9F07E0u | ((cond ^ 1) << 12) | (rd & 0x1F); }
static uint32_t ret_w(void)                          { return 0xD65F03C0u; }
/* scalar FP 3-same: 0001 1110 0 <sz> 1 Rm <opc6> Rn Rd (bit 22 = sz) */
static uint32_t fadd(int rd, int rn, int rm, int d)  { return (d ? 0x1E600000u : 0x1E200000u) | ((rm & 0x1F) << 16) | (0x0Au << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F); }
static uint32_t fsub(int rd, int rn, int rm, int d)  { return (d ? 0x1E600000u : 0x1E200000u) | ((rm & 0x1F) << 16) | (0x0Eu << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F); }
static uint32_t fmul(int rd, int rn, int rm, int d)  { return (d ? 0x1E600000u : 0x1E200000u) | ((rm & 0x1F) << 16) | (0x02u << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F); }
static uint32_t fdiv(int rd, int rn, int rm, int d)  { return (d ? 0x1E600000u : 0x1E200000u) | ((rm & 0x1F) << 16) | (0x06u << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F); }
/* FMOV 1-src scalar copy: 0001 1110 0 <sz> 1 00000 010000 Rn Rd */
static uint32_t fmov_ss(int rd, int rn, int d)       { return (d ? 0x1E600000u : 0x1E200000u) | (0x10u << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F); }
/* FCMP: 0001 1110 0 <sz> 1 Rm 001000 Rn 0 <z> 000 */
static uint32_t fcmp(int rn, int rm, int d)          { return (d ? 0x1E600000u : 0x1E200000u) | ((rm & 0x1F) << 16) | (0x08u << 10) | ((rn & 0x1F) << 5); }
/* FMOV general: sf 0011110 <type> 1 <opc5> 000000 Rn Rd (opc 0x06 FP→GP, 0x07 GP→FP) */
static uint32_t fmov_ws(int rd, int rn)              { return 0x1E260000u | ((rn & 0x1F) << 5) | (rd & 0x1F); }   /* w0 ← s1 */
static uint32_t fmov_sw(int rd, int rn)              { return 0x1E270000u | ((rn & 0x1F) << 5) | (rd & 0x1F); }   /* s0 ← w1 */
static uint32_t fmov_xd(int rd, int rn)              { return 0x9E660000u | ((rn & 0x1F) << 5) | (rd & 0x1F); }   /* x0 ← d1 */
static uint32_t fmov_dx(int rd, int rn)              { return 0x9E670000u | ((rn & 0x1F) << 5) | (rd & 0x1F); }   /* d0 ← x1 */

/* li64: movz + movk for the nonzero 16-bit halves (0 → single movz). */
static void li64(uint32_t* w, int* n, int rd, uint64_t v) {
    int first = 1;
    for (int hw = 0; hw < 4; hw++) {
        uint16_t half = (uint16_t)(v >> (16 * hw));
        if (half != 0) { w[(*n)++] = first ? movz(rd, half, hw) : movk(rd, half, hw); first = 0; }
    }
    if (first) w[(*n)++] = movz(rd, 0, 0);
}

/* The same bit-reinterpretation helpers a64_exec.c uses — the harness
 * derives the expected bit patterns with C double/float arithmetic,
 * which IS the interpreter's contract. */
static uint64_t bits_of_f64(double d) { union { uint64_t u; double d; } c; c.d = d; return c.u; }
static uint64_t bits_of_f32(float f) { union { uint32_t u; float f; } c; c.f = f; return (uint64_t)c.u; }

#define CODE_WORDS 256
#define GUEST_MEM_SIZE (CODE_WORDS * 4 + 4096)

static int g_failures = 0;
static void check(const char* what, int ok) {
    if (!ok) { printf("  FAIL: %s\n", what); g_failures++; }
}

int main(void) {
    uint32_t w[CODE_WORDS];
    int n = 0;

    /* ── f64: a = 3.5 (d0), b = 2.0 (d1) ───────────────────────────── */
    li64(w, &n, 0, 0x400C000000000000ull); w[n++] = fmov_dx(0, 0);      /* 3.5 */
    li64(w, &n, 0, 0x4000000000000000ull); w[n++] = fmov_dx(1, 0);      /* 2.0 */
    w[n++] = fadd(2, 0, 1, 1);    /* d2 = 5.5   */
    w[n++] = fsub(3, 0, 1, 1);    /* d3 = 1.5   */
    w[n++] = fmul(4, 0, 1, 1);    /* d4 = 7.0   */
    w[n++] = fdiv(5, 0, 1, 1);    /* d5 = 1.75  */
    /* NEG via sign-bit XOR (plan D3 — no float instruction): */
    w[n++] = fmov_xd(0, 0);       /* x0 = bits(3.5) */
    li64(w, &n, 1, 0x8000000000000000ull);
    w[n++] = eor_sh(0, 0, 1);     /* x0 ^= 0x8000... */
    w[n++] = fmov_dx(6, 0);       /* d6 = -3.5 */
    li64(w, &n, 0, 0x4016000000000000ull); w[n++] = fmov_dx(7, 0);   /* 5.5  */
    li64(w, &n, 0, 0x3FF8000000000000ull); w[n++] = fmov_dx(8, 0);   /* 1.5  */
    li64(w, &n, 0, 0x401C000000000000ull); w[n++] = fmov_dx(9, 0);   /* 7.0  */
    li64(w, &n, 0, 0x3FFC000000000000ull); w[n++] = fmov_dx(10, 0);  /* 1.75 */
    li64(w, &n, 0, 0xC00C000000000000ull); w[n++] = fmov_dx(11, 0);  /* -3.5 */

    /* ── f64 checks → x12..x20 ──────────────────────────────────────── */
    w[n++] = fcmp(2, 7, 1);  w[n++] = cset(12, 0x0);   /* add eq    → 1 */
    w[n++] = fcmp(3, 8, 1);  w[n++] = cset(13, 0x0);   /* sub eq    → 1 */
    w[n++] = fcmp(4, 9, 1);  w[n++] = cset(14, 0x0);   /* mul eq    → 1 */
    w[n++] = fcmp(5, 10, 1); w[n++] = cset(15, 0x0);   /* div eq    → 1 */
    w[n++] = fcmp(6, 11, 1); w[n++] = cset(16, 0x0);   /* neg eq    → 1 */
    w[n++] = fcmp(1, 0, 1);  w[n++] = cset(17, 0xB);   /* 3.5>2.0 GT → swapped fcmp(b,a), lt → 1 */
    w[n++] = fcmp(1, 0, 1);  w[n++] = cset(18, 0xB);   /* 2.0<3.5 LT → lt → 1 */
    w[n++] = fcmp(0, 0, 1);  w[n++] = cset(19, 0xD);   /* 3.5>=3.5 GE reflexive → le → 1 */
    w[n++] = fcmp(0, 1, 1);  w[n++] = cset(20, 0x1);   /* 3.5!=2.0 NE → 1 */

    /* ── f32: a = 3.5f (s12), b = 2.0f (s13) ────────────────────────── */
    li64(w, &n, 0, 0x40600000ull); w[n++] = fmov_sw(12, 0);  /* 3.5f */
    li64(w, &n, 0, 0x40000000ull); w[n++] = fmov_sw(13, 0);  /* 2.0f */
    w[n++] = fadd(14, 12, 13, 0);   /* s14 = 5.5f  */
    w[n++] = fsub(15, 12, 13, 0);   /* s15 = 1.5f  */
    w[n++] = fmul(16, 12, 13, 0);   /* s16 = 7.0f  */
    w[n++] = fdiv(17, 12, 13, 0);   /* s17 = 1.75f */
    w[n++] = fmov_ws(0, 12);        /* x0 = bits(3.5f) */
    li64(w, &n, 1, 0x80000000ull);
    w[n++] = eor_sh(0, 0, 1);
    w[n++] = fmov_sw(18, 0);        /* s18 = -3.5f */
    li64(w, &n, 0, 0x40B00000ull); w[n++] = fmov_sw(19, 0);  /* 5.5f  */
    li64(w, &n, 0, 0x3FC00000ull); w[n++] = fmov_sw(20, 0);  /* 1.5f  */
    li64(w, &n, 0, 0x40E00000ull); w[n++] = fmov_sw(21, 0);  /* 7.0f  */
    li64(w, &n, 0, 0x3FE00000ull); w[n++] = fmov_sw(22, 0);  /* 1.75f */
    li64(w, &n, 0, 0xC0600000ull); w[n++] = fmov_sw(23, 0);  /* -3.5f */

    /* ── f32 checks → x21..x25 (s26 = fmov scalar copy of s14) ─────── */
    w[n++] = fmov_ss(26, 14, 0); /* FMOV Sd, Sn — exercises the 1-src copy */
    w[n++] = fcmp(26, 19, 0); w[n++] = cset(21, 0x0);
    w[n++] = fcmp(15, 20, 0); w[n++] = cset(22, 0x0);
    w[n++] = fcmp(16, 21, 0); w[n++] = cset(23, 0x0);
    w[n++] = fcmp(17, 22, 0); w[n++] = cset(24, 0x0);
    w[n++] = fcmp(18, 23, 0); w[n++] = cset(25, 0x0);

    /* ── NaN unordered semantics (f64) ──────────────────────────────── */
    li64(w, &n, 0, 0x7FF8000000000000ull); w[n++] = fmov_dx(24, 0);  /* quiet NaN */
    w[n++] = fmov_ss(25, 0, 1);            /* d25 = fmov copy of d0 (3.5) */
    w[n++] = fcmp(24, 25, 1); w[n++] = cset(26, 0x0);   /* NaN == 3.5 → 0 */
    w[n++] = fcmp(24, 25, 1); w[n++] = cset(27, 0x1);   /* NaN != 3.5 → 1 */
    w[n++] = fcmp(24, 25, 1); w[n++] = cset(28, 0xB);   /* NaN <  3.5 → 0 */
    /* plan D5 pins: NaN GT/GE must be 0 — the two relations a single
     * cset gets wrong, closed via the swapped-operand path F1 will emit */
    w[n++] = fcmp(25, 24, 1); w[n++] = cset(3, 0xB);    /* GT(NaN,3.5) → 0 */
    w[n++] = fcmp(25, 24, 1); w[n++] = cset(4, 0xD);    /* GE(NaN,3.5) → 0 */

    /* ── sum the 17 checks → x29; result in x0 ──────────────────────── */
    w[n++] = add_sh(29, 12, 13);   /* i32 sum — small values, 64-bit add identical */
    w[n++] = add_sh(29, 29, 14);
    w[n++] = add_sh(29, 29, 15);
    w[n++] = add_sh(29, 29, 16);
    w[n++] = add_sh(29, 29, 17);
    w[n++] = add_sh(29, 29, 18);
    w[n++] = add_sh(29, 29, 19);
    w[n++] = add_sh(29, 29, 20);
    w[n++] = add_sh(29, 29, 21);
    w[n++] = add_sh(29, 29, 22);
    w[n++] = add_sh(29, 29, 23);
    w[n++] = add_sh(29, 29, 24);
    w[n++] = add_sh(29, 29, 25);
    w[n++] = add_sh(29, 29, 26);
    w[n++] = add_sh(29, 29, 27);
    w[n++] = add_sh(29, 29, 28);
    w[n++] = orr_sh(0, 31, 29);    /* mov x0, x29 (orr x0, xzr, x29) */
    w[n++] = ret_w();

    /* ── set up and run ────────────────────────────────────────────── */
    uint8_t* guest_mem = calloc(1, GUEST_MEM_SIZE);
    if (!guest_mem) { perror("calloc"); return 1; }
    for (int i = 0; i < n; i++) {
        guest_mem[4*i+0] = (uint8_t)(w[i]);
        guest_mem[4*i+1] = (uint8_t)(w[i] >> 8);
        guest_mem[4*i+2] = (uint8_t)(w[i] >> 16);
        guest_mem[4*i+3] = (uint8_t)(w[i] >> 24);
    }

    struct A64Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.mem = guest_mem;
    cpu.mem_size = GUEST_MEM_SIZE;
    cpu.pc = 0;
    cpu.x[30] = AR_EXEC_SENTINEL_LR;
    cpu.x[31] = GUEST_MEM_SIZE - 16;   /* sp — unused by this program, set anyway */

    int erc = a64_exec_run(&cpu, 10000000ull);
    check("executor returns AR_EXEC_OK", erc == AR_EXEC_OK);
    if (erc != AR_EXEC_OK) {
        printf("F0 FAIL — execution error %s (pc=0x%llx)\n", a64_exec_strerror(erc), (unsigned long long)cpu.pc);
        return 1;
    }

    /* ── 1. final result ────────────────────────────────────────────── */
    check("final result x0 == 15 (the interpreter's answer on float_ops.simi)", cpu.x[0] == 15);

    /* ── 2. arithmetic bit-identity (expected patterns from C IEEE) ──── */
    check("d2 == 5.5  (0x4016000000000000)", cpu.f[2]  == bits_of_f64(3.5 + 2.0));
    check("d3 == 1.5  (0x3FF8000000000000)", cpu.f[3]  == bits_of_f64(3.5 - 2.0));
    check("d4 == 7.0  (0x401C000000000000)", cpu.f[4]  == bits_of_f64(3.5 * 2.0));
    check("d5 == 1.75 (0x3FFC000000000000)", cpu.f[5]  == bits_of_f64(3.5 / 2.0));
    check("d6 == -3.5 (0xC00C000000000000)", cpu.f[6]  == bits_of_f64(-3.5));
    check("s14 == 5.5f  (0x40B00000)", cpu.f[14] == bits_of_f32(3.5f + 2.0f));
    check("s15 == 1.5f  (0x3FC00000)", cpu.f[15] == bits_of_f32(3.5f - 2.0f));
    check("s16 == 7.0f  (0x40E00000)", cpu.f[16] == bits_of_f32(3.5f * 2.0f));
    check("s17 == 1.75f (0x3FE00000)", cpu.f[17] == bits_of_f32(3.5f / 2.0f));
    check("s18 == -3.5f (0xC0600000)", cpu.f[18] == bits_of_f32(-3.5f));
    check("d25 == fmov copy of d0", cpu.f[25] == cpu.f[0]);
    check("s26 == fmov copy of s14 (upper 32 zeroed)", cpu.f[26] == (cpu.f[14] & 0xFFFFFFFFull));

    /* ── 3. the 17 check values + the two D5 NaN pins ───────────────── */
    int exp17[17] = { 1,1,1,1,1, 1,1,1,1, 1,1,1,1,1, 0,1,0 };
    int reg17[17]  = { 12,13,14,15,16, 17,18,19,20, 21,22,23,24,25, 26,27,28 };
    for (int i = 0; i < 17; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "check %d == %d (x%d)", i + 1, exp17[i], reg17[i]);
        check(msg, cpu.x[reg17[i]] == (uint64_t)exp17[i]);
    }
    check("NaN GT pin == 0 (swapped fcmp + lt)", cpu.x[3] == 0);
    check("NaN GE pin == 0 (swapped fcmp + le)", cpu.x[4] == 0);

    if (g_failures == 0) {
        printf("F0 PASS — float_ops.simi semantics on the A64 executor: %d checks, final 15, all bits identical\n", 17);
        printf("         (%u words emitted)\n", (unsigned)n);
        free(guest_mem);
        return 0;
    }
    printf("F0 FAIL — %d assertion(s) failed\n", g_failures);
    free(guest_mem);
    return 1;
}
