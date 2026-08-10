/*
 * simi_arm.c — SIMI-to-AArch64 (A64) native translator (kernel copy).
 *
 * This file is a direct, unmodified copy of the host toolchain's
 * simi_arm.c (tools/simi/simi_arm.c) below this comment — see that copy
 * for the full "A64 design decisions, stated up front" block (SP-relative
 * frame addressing with the positive scaled-immediate property, movz/movk
 * wide immediates replacing RV64's auipc+ld literal pool, CBZ/CBNZ +
 * subs/cset covering BC/CMP, the M1 register cache in x9/x10/x11, the
 * M2-series code-size folds and chain analysis, and the F1 float
 * GP-bounce). No libc dependency, same freestanding discipline as
 * kernel/simi_riscv.c.
 *
 * Gap Remediation SIMI M3 (kernel half): the kernel-side copy of the
 * AArch64 translator, staged exactly like the Phase 5 RV64 kernel copy
 * (kernel/simi_riscv.c). Unlike that copy — which is built into the
 * RISC-V kernel (RV_C_SRC) because a RISC-V kernel exists — there is
 * deliberately NO arm64 kernel build in this tree, so this file is
 * compiled nowhere and linked into nothing. It earns its place by the
 * two properties that make it trustworthy the day an arm64 kernel target
 * lands: (1) byte-identity with the host copy below this header
 * (re-diffed at commit time, and re-verified on every push by the
 * tests/arm_kernel_copy_rediff_check.sh CI tripwire — §10.182), whose
 * correctness is established by the qemu-aarch64 REAL-execution leg of
 * M3 (plan doc §10.178/10.179) — same encoder, same bugs or lack
 * thereof; and (2) a clean compile under
 * aarch64 freestanding flags with zero warnings, enforced by the
 * arm64-guards CI job. The no-libc contract is precise: the object's
 * only undefined symbols may be the freestanding {memcpy, memset} pair
 * (GCC 13 on AArch64 synthesizes exactly those two calls from the M2
 * chain-analysis struct copies — the RV64/x86 kernel copies compile to
 * zero undefined symbols on their toolchains — and a real kernel
 * provides them, as Linux arm64 does); ANY other symbol fails the gate
 * (§10.181). What's NOT here: kernel/simi_translate_arm.c-
 * equivalent glue, an arm64 activation/spawn path, user-mode paging, and
 * the object-catalog/syscall-dispatch/exit-stub story — all of it needs
 * an arm64 kernel target the roadmap does not grow; plan doc §6 keeps
 * that half of M3 honest and undone.
 */
#include "simi_arm.h"

/* ─── Local no-libc helpers ────────────────────────────────────────────── */
static int ar_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void ar_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}

/* Mirrors simi_isa.h's opcode/type/relation numbering exactly (Phase 1) —
 * same enums as simi_x86.c/simi_riscv.c, kept as an independent copy per
 * this file's zero-shared-header policy (see simi_arm.h's top comment). */
enum {
    OP_ADD=0, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_SHL, OP_SHR, OP_SAR, OP_NEG, OP_MOV, OP_LOADI, OP_LOADI64, OP_CMP,
    OP_BR, OP_BC, OP_CALL, OP_RET, OP_LOAD, OP_STORE, OP_LEA, OP_PTRADD,
    OP_ENTER, OP_LEAVE,
    OP_RESOLVE, OP_OBJSIZE, OP_OBJTYPE, /* v0.3 (Phase 6) */
    OP_JMPR, /* Gap Remediation SIMI Phase 14 */
    OP_CAS, OP_ATOMIC_ADD, /* Gap Remediation SIMI Phase 15 (shared-memory atomics) */
    OP_COUNT
};
enum { T_I8=0,T_I16,T_I32,T_I64,T_U8,T_U16,T_U32,T_U64,T_F32,T_F64,T_PTR,T_BOOL,T_OBJREF };
enum { REL_EQ=0,REL_NE,REL_LT,REL_LE,REL_GT,REL_GE,REL_LTU,REL_LEU,REL_GTU,REL_GEU };

#define FLAG_IMM    0x1u
#define FLAG_INVERT 0x1u

static int ar_type_signed(int t) { return t==T_I8||t==T_I16||t==T_I32||t==T_I64; }

/* ─── Instruction word decode (mirrors simi_isa.h bit layout) ───────────── */
static uint8_t  w_op(uint64_t w)    { return (uint8_t)((w>>56)&0xFFu); }
static uint8_t  w_type(uint64_t w)  { return (uint8_t)((w>>52)&0xFu); }
static uint16_t w_rd(uint64_t w)    { return (uint16_t)((w>>42)&0x3FFu); }
static uint16_t w_ra(uint64_t w)    { return (uint16_t)((w>>32)&0x3FFu); }
static uint32_t w_rb_raw(uint64_t w){ return (uint32_t)((w>>4)&0xFFFFFFFu); }
static uint8_t  w_flags(uint64_t w) { return (uint8_t)(w&0xFu); }
static uint16_t w_rb_reg(uint64_t w){ return (uint16_t)(w_rb_raw(w)&0x3FFu); }
static int32_t  w_imm28(uint64_t w) {
    uint32_t raw = w_rb_raw(w);
    if (raw & 0x8000000u) raw |= 0xF0000000u;
    return (int32_t)raw;
}

/* ─── A64 registers used by this translator (ABI names / numbers) ──────── */
#define X_ARG   0    /* x0: real A64 ABI arg0/return register — used only
                      * around RESOLVE/OBJSIZE/OBJTYPE runtime calls */
#define X_T0    9    /* t0: primary working register; carries the RET result
                      * (see the OP_RET design note in simi_arm_verify.c) */
#define X_T1    10   /* t1: secondary working register */
#define X_T2    11   /* t2: tertiary working register */
#define X_DR    12   /* x12: M2.11 run-reuse displacement register — deliberately
                      * OUTSIDE the x9/x10/x11 cache (slot 3), so cache evictions and
                      * operand fetches never touch it; holds a LOAD/STORE displacement
                      * shared across a run of accesses. Never a hostfn/trampoline
                      * register (x0-x5) nor frame (x29/x30/sp). */
#define X_FP    29   /* frame pointer (entry sp), saved/restored like RV64's s0 */
#define X_LR    30   /* link register */
#define X_SP    31   /* sp */

/* ─── Code buffer ─────────────────────────────────────────────────────── */
struct CodeBuf { uint8_t* buf; uint32_t cap; uint32_t len; int overflow; };

static void e8(struct CodeBuf* cb, uint8_t b) {
    if (cb->len < cb->cap) cb->buf[cb->len] = b;
    else cb->overflow = 1;
    cb->len++;
}
static void e32(struct CodeBuf* cb, uint32_t v) {
    e8(cb, (uint8_t)(v & 0xFF));       e8(cb, (uint8_t)((v>>8)&0xFF));
    e8(cb, (uint8_t)((v>>16)&0xFF));   e8(cb, (uint8_t)((v>>24)&0xFF));
}
/* Overwrite an already-emitted 4-byte instruction word (patch passes). */
static void patch32(uint8_t* out_buf, uint32_t pos, uint32_t v) {
    out_buf[pos+0] = (uint8_t)(v & 0xFF);
    out_buf[pos+1] = (uint8_t)((v>>8) & 0xFF);
    out_buf[pos+2] = (uint8_t)((v>>16) & 0xFF);
    out_buf[pos+3] = (uint8_t)((v>>24) & 0xFF);
}

/* ─── A64 instruction word encoders ──────────────────────────────────────
 * Every constant below was verified bit-for-bit against QEMU's
 * authoritative A64 decoder (target/arm/tcg/a64.decode) and canonical
 * constants: add x0,x1,#0 == 0x91000020, ldr x0,[x1] == 0xF9400020,
 * str x0,[x1] == 0xF9000020, mvn x0,x1 == 0xAA213E0 (orn x0,xzr,x1),
 * cset x0,eq == 0x9A9F17E0, mul x0,x1,x2 == 0x9B027C20, cbz == 0xB4000000,
 * br == 0xD61F0000. See docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md §4. */

/* Move wide: movz xd,#imm16,lsl #(16*hw); movk merges. */
static uint32_t enc_movz(uint8_t rd, uint16_t imm16, uint8_t hw) {
    return 0xD2800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm16 << 5) | rd;
}

static uint32_t enc_movk(uint8_t rd, uint16_t imm16, uint8_t hw) {
    return 0xF2800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm16 << 5) | rd;
}

/* ADR (PC-relative address): 0 00 10000 immlo:1 immhi:19 Rd — imm is a
 * SIGNED 21-bit byte offset from the adr's own address. M3: the dynamic
 * JMPR dispatch loads its table base this way so the table is reachable
 * on real A64 (a movz+movk base baked a bare out_buf byte offset, which
 * only a64_exec's guest convention could branch to). ±1MB range; the
 * whole blob is capped at 256 KiB (CODE_CAP), so any adr-to-table
 * distance is structurally in range. */
static uint32_t enc_adr(uint8_t rd, int32_t imm) {
    uint32_t u = (uint32_t)imm & 0x1FFFFFu;
    return 0x10000000u | ((u & 1u) << 23) | ((u >> 2) << 5) | rd;
}
/* Add/subtract immediate, sh=0: 1 00/10/11 100010 0 imm12 Rn Rd. */
static uint32_t enc_add_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
    return 0x91000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_sub_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
    return 0xD1000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | rd;
}
/* The same forms with the sh bit (bit 22): value = imm12 << 12. M2.7's
 * shifted-immediate fold emits these for magnitudes that are multiples
 * of 4096 (4096..0xFFFFFF); a64_exec.c already applies the shift on
 * decode ("if (w & 0x400000u) imm12 <<= 12"). */
static uint32_t enc_add_imm_sh(uint8_t rd, uint8_t rn, uint32_t imm12, uint8_t sh) {
    return 0x91000000u | ((uint32_t)(sh & 1) << 22) | ((imm12 & 0xFFFu) << 10) |
           ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_sub_imm_sh(uint8_t rd, uint8_t rn, uint32_t imm12, uint8_t sh) {
    return 0xD1000000u | ((uint32_t)(sh & 1) << 22) | ((imm12 & 0xFFFu) << 10) |
           ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_subs_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
    return 0xF1000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | rd;
}
/* Add/subtract shifted register: 1 00/10/11 01011 shift 0 Rm imm6 Rn Rd. */
static uint32_t enc_add_shift(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t shift, uint8_t imm6) {
    return 0x8B000000u | ((uint32_t)(shift & 3) << 22) | ((uint32_t)rm << 16) |
           ((uint32_t)(imm6 & 0x3F) << 10) | ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_sub_shift(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t shift, uint8_t imm6) {
    return 0xCB000000u | ((uint32_t)(shift & 3) << 22) | ((uint32_t)rm << 16) |
           ((uint32_t)(imm6 & 0x3F) << 10) | ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_subs_shift(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t shift, uint8_t imm6) {
    return 0xEB000000u | ((uint32_t)(shift & 3) << 22) | ((uint32_t)rm << 16) |
           ((uint32_t)(imm6 & 0x3F) << 10) | ((uint32_t)rn << 5) | rd;
}
/* Logical shifted register: 1 00/01/10 01010 shift N Rm imm6 Rn Rd. */
static uint32_t enc_and_shift(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t shift, uint8_t imm6) {
    return 0x8A000000u | ((uint32_t)(shift & 3) << 22) | ((uint32_t)rm << 16) |
           ((uint32_t)(imm6 & 0x3F) << 10) | ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_orr_shift(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t shift, uint8_t imm6) {
    return 0xAA000000u | ((uint32_t)(shift & 3) << 22) | ((uint32_t)rm << 16) |
           ((uint32_t)(imm6 & 0x3F) << 10) | ((uint32_t)rn << 5) | rd;
}
/* Phase 15 (A3): `orr wd, wzr, wm` — the 32-bit MOV (register), whose
 * W-form write ZERO-EXTENDS m into d. Used to mask an i32 CAS's expected
 * value to its low 32 bits (the interpreter's width-masked compare —
 * garbage high bits in rB must not affect the match, the A1 tooth's
 * shape). sf=0 makes the whole encoding the 32-bit ORR. */
static uint32_t enc_orr_32(uint8_t rd, uint8_t rm) {
    return 0x2A0003E0u | ((uint32_t)rm << 16) | rd;
}
static uint32_t enc_eor_shift(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t shift, uint8_t imm6) {
    return 0xCA000000u | ((uint32_t)(shift & 3) << 22) | ((uint32_t)rm << 16) |
           ((uint32_t)(imm6 & 0x3F) << 10) | ((uint32_t)rn << 5) | rd;
}
/* MVN xd, xm == ORN xd, xzr, xm (N bit set, Rn==31). Rn MUST be 31, not
 * 0: on real A64, `orn xd, x0, xm` computes x0 | ~xm and x0 is the
 * caller's register (and this translator's hostfn arg/result register) —
 * the emitted word only produced ~xm while a64_exec kept host x0 at
 * zero. M2/M3 (real execution) would catch this divergence immediately;
 * caught in M0 review instead. */
static uint32_t enc_orn(uint8_t rd, uint8_t rm) {
    return 0xAA200000u | ((uint32_t)rm << 16) | (31u << 5) | rd;
}
/* Variable shifts: 1 00 11010110 Rm funct Rn Rd. */
static uint32_t enc_lslv(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x9AC02000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_lsrv(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x9AC02400u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_asrv(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x9AC02800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}
/* MADD/MSUB: 1 00 11011000 Rm op Ra Rn Rd. MUL == MADD with Ra=31. */
static uint32_t enc_madd(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra) {
    return 0x9B000000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) |
           ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_msub(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra) {
    return 0x9B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) |
           ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_sdiv(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x9AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_udiv(uint8_t rd, uint8_t rn, uint8_t rm) {
    return 0x9AC00800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}
/* CSET xd, cond == CSINC xd, xzr, xzr, cond^1. cond is the *set* condition. */
static uint32_t enc_cset(uint8_t rd, uint8_t cond) {
    return 0x9A9F07E0u | ((uint32_t)(cond ^ 1) << 12) | rd;
}
/* ─── F1 (Gap Remediation SIMI Phase 10): scalar floating point —──────
 * Every word verified against QEMU's a64.decode in F0 (see the
 * scalar-FP section of a64_exec.c for the patterns and the canonical
 * S/D constants):
 *   3-same FADD/FSUB/FMUL/FDIV: 0001 1110 0 sz 1 Rm <opc6> Rn Rd,
 *     bit 22 = sz (0 = S, 1 = D), opc6 FMUL 000010 FDIV 000110
 *     FADD 001010 FSUB 001110.
 *   FCMP: 0001 1110 0 sz 1 Rm 001000 Rn 0 0 000 (register form,
 *     quiet — e=0, z=0).
 *   FMOV general: sf 0011110 <type> 1 <opc5> 000000 Rn Rd, type 00 =
 *     S / 01 = D, opc5 00110 = FP→GP (Xd,Dn / Wd,Sn), 00111 = GP→FP
 *     (Dd,Xn / Sd,Wn).
 * The F1 codegen (GP-bounce per plan D4) uses d0/d1 as pure compute
 * scratch and moves bits through the x9/x10/x11 integer cache, so
 * these are the ONLY FP encoders the integer-allocation machinery
 * needs to know about — LOAD/STORE under float types already reuse
 * the integer encoders (D3: rt is a plain register number). */
static uint32_t enc_fadd_d(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1E600000u | ((uint32_t)rm << 16) | (0x0Au << 10) | ((uint32_t)rn << 5) | rd; }
static uint32_t enc_fadd_s(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1E200000u | ((uint32_t)rm << 16) | (0x0Au << 10) | ((uint32_t)rn << 5) | rd; }
static uint32_t enc_fsub_d(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1E600000u | ((uint32_t)rm << 16) | (0x0Eu << 10) | ((uint32_t)rn << 5) | rd; }
static uint32_t enc_fsub_s(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1E200000u | ((uint32_t)rm << 16) | (0x0Eu << 10) | ((uint32_t)rn << 5) | rd; }
static uint32_t enc_fmul_d(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1E600000u | ((uint32_t)rm << 16) | (0x02u << 10) | ((uint32_t)rn << 5) | rd; }
static uint32_t enc_fmul_s(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1E200000u | ((uint32_t)rm << 16) | (0x02u << 10) | ((uint32_t)rn << 5) | rd; }
static uint32_t enc_fdiv_d(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1E600000u | ((uint32_t)rm << 16) | (0x06u << 10) | ((uint32_t)rn << 5) | rd; }
static uint32_t enc_fdiv_s(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1E200000u | ((uint32_t)rm << 16) | (0x06u << 10) | ((uint32_t)rn << 5) | rd; }
static uint32_t enc_fcmp_d(uint8_t rn, uint8_t rm) { return 0x1E600000u | ((uint32_t)rm << 16) | (0x08u << 10) | ((uint32_t)rn << 5); }
static uint32_t enc_fcmp_s(uint8_t rn, uint8_t rm) { return 0x1E200000u | ((uint32_t)rm << 16) | (0x08u << 10) | ((uint32_t)rn << 5); }
static uint32_t enc_fmov_xd(uint8_t rd, uint8_t rn) { return 0x9E660000u | ((uint32_t)rn << 5) | rd; }   /* FMOV Xd, Dn  — FP → GP, 64-bit */
static uint32_t enc_fmov_dx(uint8_t rd, uint8_t rn) { return 0x9E670000u | ((uint32_t)rn << 5) | rd; }   /* FMOV Dd, Xn  — GP → FP, 64-bit */
static uint32_t enc_fmov_ws(uint8_t rd, uint8_t rn) { return 0x1E260000u | ((uint32_t)rn << 5) | rd; }   /* FMOV Wd, Sn  — FP → GP, 32-bit (zero-extend) */
static uint32_t enc_fmov_sw(uint8_t rd, uint8_t rn) { return 0x1E270000u | ((uint32_t)rn << 5) | rd; }   /* FMOV Sd, Wn  — GP → FP, 32-bit (zero high half) */
/* Load/store register, unsigned scaled immediate: size:2 111 0 01 opc imm12. */
static uint32_t enc_ldr (uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xF9400000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_str (uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xF9000000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrb(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x39400000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
/* Gap Remediation SIMI Phase 15 (A3, plan D2): the exclusive-access
 * encodings — ldaxr/stlxr, the acquire/release pair the atomics loops
 * emit (aq=1/rl=1 baked into the opcode). LDXR/LDAXR: size 00 111000
 * 01/10 1 11111 Rn Rt (bits 20:16 all-ones = load, bit 15 = 0 for
 * ldxr, 1 for ldaxr). STXR/STLXR: size 00 111000 00 0 Rs 11111 Rn Rt
 * (bit 15 = 0 for stxr, 1 for stlxr; Rs = status dest, always a W
 * register). The 32-bit forms differ only in size = 10 (sf bit 30).
 * Bit 22 separates load (1) from store (0), bit 15 the acquire/
 * release bit — the decoder's discriminator below. */
static uint32_t enc_ldaxr (uint8_t rt, uint8_t rn) { return 0xC85FFC00u | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldaxr_w(uint8_t rt, uint8_t rn) { return 0x885FFC00u | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_stlxr (uint8_t rs, uint8_t rt, uint8_t rn) { return 0xC800FC00u | ((uint32_t)rs << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_stlxr_w(uint8_t rs, uint8_t rt, uint8_t rn) { return 0x8800FC00u | ((uint32_t)rs << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_strb(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x39000000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrh(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x79400000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_strh(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x79000000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldr_w(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xB9400000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_str_w(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xB9000000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsb(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x39800000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsh(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x79800000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsw(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xB9800000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
/* Load/store register, UNSCALED 9-bit signed immediate (ldur/stur):
 * size:2 111 0 00 opc 0 imm9:9 00 Rn Rt. The imm9 is UNSCALED (not
 * shifted by size), signed -256..255, and bits 11:10 = 00 select the
 * unscaled form — the access has NO writeback and NO alignment
 * requirement (that is exactly what ldur/stur exist for; bits 11:10
 * = 01 is post-indexed and 11 is pre-indexed). M2.10: this form
 * SUPERSEDES the M2.9 pre-indexed fold (writeback): same single word
 * for the foldable range, no writeback to poison the base host (so a
 * resident base stays resident), and it extends the fold to POSITIVE
 * and UNALIGNED displacements the scaled path rejects (e.g. [x, #5]
 * i32, which M2.8 paid add #5 + access for). Verified against QEMU's
 * a64.decode @ldst_imm and the canonical stur x29, [sp, #-16] ==
 * 0xF81F03FD. */
static uint32_t enc_ldur  (uint8_t rt, uint8_t rn, int16_t imm9) { return 0xF8400000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_stur  (uint8_t rt, uint8_t rn, int16_t imm9) { return 0xF8000000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldurb (uint8_t rt, uint8_t rn, int16_t imm9) { return 0x38400000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_sturb (uint8_t rt, uint8_t rn, int16_t imm9) { return 0x38000000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldurh (uint8_t rt, uint8_t rn, int16_t imm9) { return 0x78400000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_sturh (uint8_t rt, uint8_t rn, int16_t imm9) { return 0x78000000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldur_w(uint8_t rt, uint8_t rn, int16_t imm9) { return 0xB8400000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_stur_w(uint8_t rt, uint8_t rn, int16_t imm9) { return 0xB8000000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldursb(uint8_t rt, uint8_t rn, int16_t imm9) { return 0x38800000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldursh(uint8_t rt, uint8_t rn, int16_t imm9) { return 0x78800000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldursw(uint8_t rt, uint8_t rn, int16_t imm9) { return 0xB8800000u | (((uint32_t)(imm9 & 0x1FF)) << 12) | ((uint32_t)rn << 5) | rt; }
/* Load/store register, REGISTER offset: size:2 111 0 00 opc 1 Rm opt 0 10
 * Rn Rt — bit 21 = 1 (vs the imm9 forms' 0) and bits 11:10 = 10. M2.11:
 * the materialize path (|disp| past the imm12 range) puts the displacement
 * in a register and the access is ONE word — the M2.8 li64 + add_shift +
 * zero-offset access was two words of address math on top of the
 * materialize. Two options, pinned against QEMU's a64.decode @ldst
 * (register offset): opt = 011 (LSL #0, Xm used in full) for the 64-bit
 * forms, and opt = 110 (SXTW, sign-extend the low 32 bits of Xm) for the
 * 8/16/32-bit forms — the narrow forms need SXTW because LSL #0 would
 * ZERO-extend Wm and turn a negative two's-complement displacement into a
 * huge positive offset. The translator materializes the full 64-bit
 * displacement; its low 32 bits sign-extend back to itself, so SXTW is
 * exact for both signs (imm28 range is well inside int32). Verified:
 * ldr x0, [x1, x2] == 0xF8626820. */
static uint32_t enc_ldr_reg (uint8_t rt, uint8_t rn, uint8_t rm) { return 0xF8606800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_str_reg (uint8_t rt, uint8_t rn, uint8_t rm) { return 0xF8206800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrb_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0x3860C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_strb_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0x3820C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrh_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0x7860C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_strh_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0x7820C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldr_w_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0xB860C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_str_w_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0xB820C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsb_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0x38A0C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsh_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0x78A0C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsw_reg(uint8_t rt, uint8_t rn, uint8_t rm) { return 0xB8A0C800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt; }
/* Branches. imm26/imm19 are in units of 4 bytes (word offsets). */
static uint32_t enc_b   (int32_t imm26) { return 0x14000000u | ((uint32_t)imm26 & 0x03FFFFFFu); }
static uint32_t enc_bl  (int32_t imm26) { return 0x94000000u | ((uint32_t)imm26 & 0x03FFFFFFu); }
static uint32_t enc_cbz (uint8_t rt, int32_t imm19) { return 0xB4000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | rt; }
static uint32_t enc_cbnz(uint8_t rt, int32_t imm19) { return 0xB5000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | rt; }
/* B.cond — conditional branch on NZCV: 0101 0100 0 imm19:19 0 cond:4
 * 00000 (0x54000000 | cond | (imm19 << 5)). M2.25's inline JMPR
 * chain emits cmp (subs xzr) + b.eq pairs instead of the runtime table.
 *
 * A3 (Phase 15) bug-fix: the cond MUST land in bits 3:0, not bits 12:15.
 * Every pre-A3 caller used cond = 0 (b.eq), where the two layouts agree
 * (both write zeros there), so the wrong placement was invisible for
 * months; the atomics' b.ne (cond = 1) exposed it — the word decoded as
 * b.eq on the executor, the CAS mismatch path never fired, and the
 * compare fell through into the stlxr. a64_exec.c's B.cond decode reads
 * w & 0xF (bits 3:0), the ARM-canonical position. */
static uint32_t enc_b_cond(uint8_t cond, int32_t imm19) {
    return 0x54000000u | ((uint32_t)(cond & 0xF)) | (((uint32_t)imm19 & 0x7FFFFu) << 5);
}
static uint32_t enc_br  (uint8_t rn) { return 0xD61F0000u | ((uint32_t)rn << 5); }
static uint32_t enc_blr (uint8_t rn) { return 0xD63F0000u | ((uint32_t)rn << 5); }

/* ─── Symbolic register slot helpers: SP-relative, all positive offsets ──
 * See the FRAME design note in the file's top comment. slot i lives at
 * sp + (568 - 8i) — imm12 field (71 - i) — and tag i at sp + i. These
 * are byte-identical guest addresses to RV64's layout (entry-24-8i and
 * entry-592+i) since sp == entry-592 throughout a procedure body. */
static void ld_slot(struct CodeBuf* cb, uint8_t rd, int i) { e32(cb, enc_ldr(rd, X_SP, 71 - i)); }
static void st_slot(struct CodeBuf* cb, uint8_t rs, int i) { e32(cb, enc_str(rs, X_SP, 71 - i)); }
static void ld_tag(struct CodeBuf* cb, uint8_t rd, int i)  { e32(cb, enc_ldrb(rd, X_SP, i)); }
static void st_tag(struct CodeBuf* cb, uint8_t rs, int i)  { e32(cb, enc_strb(rs, X_SP, i)); }
/* clear the tag byte directly — STRB WZR, no scratch register needed. */
static void st_untag(struct CodeBuf* cb, int i)            { e32(cb, enc_strb(31, X_SP, i)); }
/* Every opcode that writes a plain (non-capability) value goes through
 * this instead of the raw st_slot, so "clear the destination's tag" can
 * never be forgotten at a new call site — mirrors st_slot_untag in
 * simi_x86.c/simi_riscv.c. */
static void st_slot_untag(struct CodeBuf* cb, uint8_t rs, int i) { st_slot(cb, rs, i); st_untag(cb, i); }

static void emit_li64(struct CodeBuf* cb, uint8_t reg, uint64_t imm);   /* fwd: defined below */

/* ─── M1: three-entry symbolic-register cache (x9/x10/x11) ────────────────
 * The M0 codegen load-operate-stored every operand and result — five or
 * six words per arithmetic instruction even when the operands were just
 * produced two instructions earlier. M1 keeps up to three guest registers
 * resident in the working registers x9/x10/x11 across SIMI instruction
 * boundaries, keyed by a tiny compile-time directory:
 *
 *   - g_cache_guest[i] = guest reg resident in x9+i, or -1.
 *   - cache_reserve() claims a host register for the current result with
 *     round-robin eviction; the evicted value is spilled to its slot.
 *   - get_operand() reads a guest reg through the cache (a mov when it is
 *     resident in another working register, a slot load otherwise).
 *   - store_result() keeps the result resident (writing only the tag byte,
 *     so memory's tag bytes stay authoritative), or falls back to the M0
 *     store+untag sequence.
 *
 * Correctness rules, all enforced at emit time:
 *   1. MEMORY IS ALWAYS THE SOURCE OF TRUTH AT BLOCK BOUNDARIES. Every
 *      BR/BC/CALL spills the whole cache first, and every instruction
 *      whose next pc is a branch target ends with a spill, so a jump
 *      always lands on code that re-fetches from slots. The branch path
 *      and the fall-through path therefore see identical, correct memory
 *      — the fall-through into a mid-chain target cannot rely on cached
 *      values, because the target is shared with paths that never cached
 *      them (or that arrive with different registers clobbered).
 *   2. JMPR DISABLES THE CACHE FOR THE WHOLE PROGRAM. The JMPR table can
 *      land on any pc, so every pc would have to be a block head — which
 *      is no cache at all. Programs containing JMPR translate with
 *      g_alloc=0 and get byte-identical M0 codegen.
 *   3. TAG BYTES ARE WRITTEN TO MEMORY IMMEDIATELY. A cached value's tag
 *      byte is stored the moment the value is cached (store_result's
 *      st_untag, MOV's st_tag, the call site's st_tag), so a spill only
 *      ever needs to write the value — the tag byte in memory is always
 *      authoritative. This is what lets OBJSIZE/OBJTYPE/RET/CALL read
 *      tags straight from memory without knowing the cache state.
 *   4. SCRATCH USE EVICTS FIRST. X_T1/X_T2 are also cache hosts; any use
 *      of them as scratch (constants, the MOV tag load) spills the
 *      occupant first, and the reserved result slot is never spilled by
 *      the in-flight instruction's own operand fetches (g_resv_slot).
 *      A64 reads all operands before writing the result, so the result
 *      register may alias an operand register.
 *   5. RESERVATION PREFERS NON-SOURCE VICTIMS (M2.4). cache_reserve is
 *      told the instruction's source registers and evicts a slot that
 *      holds neither source when one exists — which it always does, with
 *      3 slots and at most 2 sources — so chains that re-read the same
 *      register (ADD r4, r1, r2; ADD r5, r1, r2) keep the source
 *      resident instead of spilling it now and reloading it when
 *      get_operand fetches it a few words later.
 *   6. FETCH TARGETS ARE CHOSEN TO MATCH RESIDENCY (M2.5). get_operand's
 *      default host assignment (operand1→x9, operand2→x10) is overridden
 *      per instruction by cache_fetch_hosts/cache_single_host: when an
 *      operand is resident at slot 1 (or the second operand at slot 0),
 *      the fetch targets swap so each operand fetches into its own
 *      resident slot — otherwise the first fetch spills the second
 *      operand, then the second fetch spills the first and reloads it,
 *      two spills and a reload for operands that were both resident
 *      (e.g. ADD r5, r2, r1 after ADD r4, r1, r2: r1@slot0, r2@slot1
 *      makes the swapped-order ADD crossed).
 * The naive (g_alloc=0) path degrades these helpers to exactly the M0
 * sequences, so the JMPR fallback is exercised by jmpr_basic/jmpr_oob.
 */
#define AR_CACHE_N 3
static int g_alloc;                    /* 0 when any JMPR cannot be folded to a direct branch */
/* M2.71: the safety net's trip count (default 16 — see the M2.18/M2.70
 * net comment in the fixpoint loop) and the fixpoint's scan count for
 * the last translate. Exposed (non-static, declared in simi_arm.h) so
 * bench_net.c can measure the M2.70 translate-time win — 16 vs the
 * M2.69-era 512 — from one binary: g_ar_net_scans is reset by the
 * bench, incremented once per fixpoint scan, and read back after. */
int g_ar_net_trip = 16;
int g_ar_net_scans = 0;
static int g_cache_guest[AR_CACHE_N];  /* guest reg resident in x9+i, or -1 */
static int g_cache_round;              /* round-robin eviction cursor */
static int g_resv_slot;                /* slot reserved for the in-flight result */
static int g_resv_reuse;               /* reserved slot held rd's old value (in-place result) */
static int g_resv_guest;               /* the guest reg the reservation is for (== rd) */
static uint8_t g_pc_target[4096];      /* branch/call/fold-target pcs (block heads) */
/* M2: constant-index JMPR folding. A JMPR's index is usually a constant
 * loaded shortly before the dispatch (LOADI/MOV chains), and a JMPR with
 * a provably-constant in-range index is just a direct branch — which the
 * normal block-head discipline makes cache-safe. The pre-pass tracks
 * per-register compile-time constants (g_const_known/g_const_val, reset
 * at every block head so the analysis is sound across joins) and records
 * a fold target per JMPR pc in g_jmpr_fold (-1 = keep the dynamic
 * runtime-table path). g_alloc is 1 only when every JMPR folds; a single
 * non-constant (or out-of-range) JMPR still forces the whole program
 * back to the naive path, because that JMPR's runtime targets can be any
 * pc, and any pc reachable without a compile-time discipline makes every
 * pc a potential block head. */
static uint64_t g_const_val[TX_AR_MAX_REGS];
static uint8_t  g_const_known[TX_AR_MAX_REGS];
/* M2.18: targeted fixpoint relaxation — a block head whose ONLY incoming
 * path is a single forward BR/BC (linear predecessor is a terminal, so no
 * fall-through; not an entry; not a fold target) is NOT a join, so the
 * constant map as it was at that branch survives into the head.
 * g_relax_ok is the static part (computed once); g_relax_snap_* hold the
 * per-head snapshot taken at the branch during each scan; the fold-target
 * exclusion is per-scan (g_fold_tgt_prev, live-updated as the scan
 * discovers folds). This is what lets a TWO-COMPUTED epi-merge dead
 * region fold: T+2 is the fold-source BR target (a block head), and
 * without the relaxation the reset there kills the seed constants from
 * before the BR, so the computed index chain cannot fold. */
static uint8_t  g_relax_ok[4096];
static uint8_t  g_relax_snap_known[4096][TX_AR_MAX_REGS];
static uint64_t g_relax_snap_val[4096][TX_AR_MAX_REGS];
static uint8_t  g_relax_snap_active[4096];
static uint8_t  g_fold_tgt_prev[4096];
static int      g_jmpr_fold[4096];      /* per-JMPR-pc folded target, or -1 */
static int      g_jmpr_fold_prev[4096]; /* fixpoint convergence snapshot (file-scope like the other arrays) */
/* M2.25: inline JMPR dispatch chain. In naive mode (g_alloc=0) with
 * EXACTLY one dynamic (non-folding) JMPR, the runtime table + bounds
 * check is replaced by an inline compare-and-branch chain over the index
 * register's provable candidate set — one cmp + b.eq per candidate, UDF
 * fall-through. g_chain_cand is the sorted in-range subset (a candidate
 * >= num_instr has no code and falls through to the UDF, matching the
 * table's bounds-check fault — jmpr_oob's index 999 becomes a bare UDF).
 * g_chain_ncand == 0 is legal (the degenerate always-fault chain).
 * Soundness: the set is the union of the constant sets arriving at the
 * dispatch along every incoming edge — the same join discipline as the
 * fold fixpoint, but a UNION, because the chain must cover every path's
 * value, not fold a single one. M2.26: the walk tracks ra plus its
 * TRANSITIVE FEEDERS (the closure in translate()), so an index built by
 * a register-form ADD/SUB — LOADI rb; ADD ra, ra, rb — chains too. */
/* M2.29: the WALK's flat-set capacity is 12 — larger than the
 * M2.25-M2.28 hard cap of 8, so 9-11-candidate sets can be collected
 * AND emitted when the adaptive cost check says the chain beats the
 * table. The emission is no longer a fixed cap: it fires when 8*n + 4
 * < 40 + 4*num_instr (see the activation below). M2.30: a register-form
 * pair product whose flat set would EXCEED 12 is not collapsed to
 * UNKNOWN — it is DEFERRED (the def_* fields record the op and the two
 * source slots) and re-computed at the dispatch into the BIG candidate
 * set (TX_AR_CHAIN_BIG), so a 13-64-candidate dispatch can still chain
 * when the cost gate says it wins. M2.40 raises TX_AR_CHAIN_BIG to 64
 * (the walk's big-set cap, the union record's materialization cap, and
 * the chain's candidate cap): a 33-64-candidate dispatch — a union of
 * records whose merged true set exceeds 32 — now also chains when the
 * gate says it beats the table. The LINEAR chain is the right emission
 * shape at any n: each candidate needs its own cmp + b.eq pair
 * (2 words) regardless, so "two chained segments" would add a selector
 * without reducing comparisons (8n + 8 vs 8n + 4) and a compare-tree
 * costs ~4 words per internal node — both strictly worse on bytes; the
 * honest 8*n + 4 < 40 + 4*num_instr gate is what decides. M2.41 raised
 * TX_AR_CHAIN_MAX from 12 to 20: a 13-20-value product now stays FLAT
 * instead of deferring — and a flat set SURVIVES a source-register
 * write (chain_prewrite only flattens DEFERRED forms; a flat set in
 * rd's slot is untouched by writes to its feeders), while a deferred
 * record of > 12 values was flattened to UNKNOWN by the same write
 * (the eager flatten caps at the walk bound). The raise needed two
 * companions: (a) a flat+flat head-union whose merged set exceeds the
 * cap freezes as a PRE-MERGED union record instead of collapsing
 * (chain14/15's 30-value joins must not regress), and (b) an in-place
 * product over a FLAT source whose image overflows freezes the aliased
 * source as a CD_FLAT operand so it still defers (chain13's second
 * product must not regress). M2.42 raises TX_AR_CHAIN_MAX to 64 to
 * MATCH TX_AR_CHAIN_BIG: a 21-64-value product now stays flat too —
 * and since a set of > 96 values can never chain (the emission gate
 * rejects ncand > TX_AR_CHAIN_BIG), the deferred-record machinery
 * beyond the record-pool cap is UNREACHABLE for chaining: every
 * reachable chain materializes from a flat set or a record whose true
 * set fits the BIG store, and the M2.30-M2.41 record paths (defers,
 * union records, the flat+flat fallback, the in-place freeze) are
 * vestigial above that — retained as the recorded history and as a
 * safety net should the caps ever diverge again. Deferral is sound
 * only while the sources are untouched: every write or head-union of a
 * source slot flattens (and caps) the deferred form eagerly, and the
 * dispatch materializes over the provably-unchanged sources. */
/* M2.61: the EQUAL-CAPS regression. The cap series (M2.54/M2.55) found
 * the discipline's two failure modes when the caps DIVERGE — the
 * flatten overflow (MAX > BIG) and the freeze overflow (BIG > MAX) —
 * and every pin since has guarded the equal-caps turn-over at 64
 * (chain31) or the collapse side (chain33/chain30). This milestone
 * permanently bumps BOTH caps together 64 -> 96, proving the caps are
 * parameterized (nothing hardcodes 64): jmpr_chain37 pins the new
 * turn-over at exactly 96, chain33's 80-value root now materializes
 * and chains (the exact behavior M2.57's Control A proved sound under
 * divergence — the 64/64 collapse was cap-specific, not structural),
 * and chain30's 100-value index still collapses conservatively (100 >
 * 96). The bump is monotone in capability: sets <= 64 behave
 * byte-identically (all shared gate rows unchanged), and 65-96-value
 * sets gain the chain. The static footprint scales: g_chain_arr is
 * REGS x 4096 ChainSets. */
#define TX_AR_CHAIN_MAX 96
#define TX_AR_CHAIN_BIG 96
/* M2.43: the tracked-register closure cap. M2.26 sized it at 4 — the
 * index register plus its feeders. M2.43 raises it to 8 so a 5-8-
 * feeder index chain (r1 = (r4 + r5) + r3 over joins — jmpr_chain19)
 * still analyzes: under the old cap the closure truncated silently
 * (the missing feeder's set stayed UNKNOWN, poisoning the index set to
 * UNKNOWN and falling to the table). M2.62 raises it to 12 (the
 * EQUAL-CAPS regression's register dimension): a 12-feeder index chain
 * (jmpr_chain38 — the closure {r1..r12} = exactly the cap, the M2.56
 * turn-over at the new value) now analyzes, and a 13th feeder still
 * truncates conservatively to the table. g_chain_arr is REGS x 4096
 * ChainSets, so the static footprint scales with the cap (8 -> 12
 * adds ~6 MB of BSS at MAX = 96); 12 is a generous bound for
 * realistic register chains while staying well under TX_AR_MAX_REGS. */
#define TX_AR_CHAIN_REGS 12
/* M2.64: the deferred-record pool cap raised 64 -> 96 to MATCH MAX and
 * BIG (the EQUAL-CAPS regression's third dimension — M2.61 did the set
 * caps, M2.62 the register closure). The M2.57-era 64-record boundary
 * was shifted by M2.61 itself: with MAX = BIG = 96 a root product whose
 * true set exceeds 96 DEFERS (record 0) and its materialization
 * collapses conservatively at the BIG store (chain_merge_big's
 * overflow -> UNKNOWN — the exact-or-conservative discipline), while a
 * <= 96-value root stays FLAT and never touches the pool, so the pool
 * is a pure CEILING on DAG depth: nothing below the cap moves. The
 * M2.64 pin (jmpr_chain40) is a DAG needing EXACTLY 96 records — the
 * root's 100-value product plus 95 in-place ADDs — that fits the pool
 * at the new cap (instrumented: ndef = 96, r1's def = 95 at the
 * dispatch; the runtime 289 = the 100th candidate would UDF-trap any
 * truncated materialization, so the conservative table is what PASSes)
 * and exhausts it at DEFS = 95 (the ad-hoc control: the 96th
 * allocation returns -1 -> r1 falls to flat UNKNOWN at the walk — the
 * same table, a different mechanism, proven by the walk state). The
 * static footprint scales: g_chain_defs and its companion stores are
 * DEFS entries each. */
#define TX_AR_CHAIN_DEFS 96
#define CD_SLOT 0   /* M2.32: deferred-product operand kind — a tracked slot */
#define CD_REC  1   /* M2.32: deferred-product operand kind — an older pool record */
#define CD_IMM  2   /* M2.33: deferred-product operand kind — an immediate constant */
#define CD_FLAT 3   /* M2.37: deferred-union operand kind — an embedded FROZEN flat set
                     * (stored in g_chain_def_flat[rec]; immune to writes and unions) */
#define CHAIN_OP_UNION 0xFE  /* M2.37: record op sentinel — a union record op(rec(R), flat(F))
                              * materializes to the MERGED true set (R's set union F, capped at
                              * TX_AR_CHAIN_BIG), so a >12 union that is not a single product can
                              * still feed the chain. Not an instruction opcode. */
/* M2.32/M2.33/M2.37: an immutable deferred-product record. An operand is
 * a tracked slot (its value as it stands when the record is materialized,
 * sound because any write to the slot invalidates every record that
 * transitively reads it), an OLDER pool record (the indirection that
 * makes in-place products sound: ADD r1, r1, r4 defers as op(rec(R1),
 * slot(r4)) — the left operand is the immutable record for r1's OLD
 * value, not cur[r1] itself, so no self-cycle), an immediate constant
 * (CD_IMM — an imm-form product over a deferred source defers as
 * op(rec(R1), #32) instead of collapsing to UNKNOWN), or a FROZEN flat
 * set (CD_FLAT — the union record's small side, stored out-of-line and
 * never invalidated). CHAIN_OP_UNION records carry the record side in
 * operand a (CD_REC) and the second side in operand b — CD_FLAT (M2.37)
 * or CD_REC (M2.38, the record-vs-record join). Records only reference
 * older records, so the DAG is acyclic. */
struct ChainDef {
    uint8_t op;       /* ADD/SUB/MUL/AND/OR/XOR */
    uint8_t ka, kb;   /* CD_SLOT / CD_REC / CD_IMM for operand a / operand b */
    int32_t a, b;     /* slot index (CD_SLOT), record index (CD_REC), or imm (CD_IMM) */
};
struct ChainSet {
    uint8_t n, unk;
    uint32_t v[TX_AR_CHAIN_MAX];
    int16_t def;      /* M2.30/M2.32: -1 = flat set (n/unk live); else g_chain_defs index */
};
struct ChainBig { uint8_t n, unk; uint32_t v[TX_AR_CHAIN_BIG]; };
static struct ChainDef g_chain_defs[TX_AR_CHAIN_DEFS];
static int      g_chain_ndef;      /* M2.32: pool cursor, reset each walk iteration */
static uint32_t g_chain_def_pc[TX_AR_CHAIN_DEFS];   /* M2.36: the walk pc where each record was created */
static struct ChainBig g_chain_def_flat[TX_AR_CHAIN_DEFS]; /* M2.37/M2.41: the per-record FROZEN set store — a CD_FLAT product
                                                     * operand (the in-place source, M2.41) or a union record's PRE-MERGED
                                                     * true set (M2.37/M2.38/M2.41). Immutable: never invalidated by writes
                                                     * or unions (chain_def_touches/live ignore non-CDSLOT operands). */
static int32_t  g_chain_last_write[TX_AR_CHAIN_REGS]; /* M2.36: last WRITE pc per tracked slot (per iteration, -1 = never) */
static uint32_t g_chain_cand[TX_AR_CHAIN_BIG];
static struct ChainBig g_chain_bigsnap;
static int      g_chain_ncand;
static int      g_chain_active;
/* M2.26: which registers the walk tracks (ra + feeders) and their slot */
static uint8_t  g_chain_tracked[TX_AR_MAX_REGS];
static int8_t   g_chain_slot[TX_AR_MAX_REGS];
/* per-head arrival accumulators, one set per tracked slot */
static struct ChainSet g_chain_arr[TX_AR_CHAIN_REGS][4096];
static uint8_t  g_fold_fall[4096];      /* M2.12: folded JMPR whose target is pc+1 — a dead
                                         * branch (control falls through to it anyway). The main
                                         * loop drops the branch entirely; when the target also
                                         * has NO other incoming edge, the coalescing pre-pass
                                         * clears its g_pc_target mark so the end-of-loop flush
                                         * is skipped too and the cache survives the join. */
/* M2.15: epilogue/prologue merge flags per pc. For a single-edge
 * BACKWARD fold (target T < fold pc), the head block at T reloads its
 * operand registers from the frame — and the source block's tail may
 * have reloaded the SAME registers a few instructions earlier, leaving
 * the values in x9/x10 (transient fetches the fold's flush does not
 * store). The head's matching reloads then re-read the same slot values
 * into the same hosts — dead code. g_epi_merge[T] bit 1 = drop the
 * first source's fetch (g1@x9), bit 2 = drop the second (g2@x10); the
 * pre-pass computes which from the cache round-robin cursor; see the
 * M2.15 pre-pass block in translate() below. */
static uint8_t  g_epi_merge[4096];
/* M2.15: the 2-register-source ALU family whose codegen has the canonical
 * x9/x10 fetch pattern (cache_fetch_hosts + two get_operands) — the
 * pattern/tail opcodes of an epilogue/prologue merge. */
static int ar_is_alu2(uint8_t op) {
    return op == OP_ADD || op == OP_SUB || op == OP_AND || op == OP_OR ||
           op == OP_XOR || op == OP_MUL || op == OP_SHL || op == OP_SHR ||
           op == OP_SAR;
}
/* M2.15: ops whose codegen calls cache_reserve (each advances the cache
 * round-robin cursor exactly once when g_alloc=1). Mirrors the reserve
 * call sites in translate()/emit_instr/emit_call_site — the count mod 3
 * is the cursor value at a pc ONLY when no reserve reuses a resident
 * slot, which epi_merge_pass guarantees with its distinct-registers
 * check. */
static int ar_is_result_op(uint8_t op) {
    switch (op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:
    case OP_SAR: case OP_NOT: case OP_NEG: case OP_MOV: case OP_LOADI:
    case OP_LOADI64: case OP_CMP: case OP_LEA: case OP_PTRADD:
    case OP_LOAD: case OP_CALL:
        return 1;
    default:
        return 0;
    }
}
/* M2.17: the dead-region intermediates (T+2/T+3) of an epi-merge shape
 * may be ANY plain result-op whose emission is "reserve + transient
 * fetches + compute + store" — the merge invariants depend only on the
 * CLAIM COUNT (two fresh claims between the terminal and the tail net
 * +3 ≡ 0 mod 3, so the tail's result host stays the pre-T cursor value
 * cnt%3) and on the destinations being distinct and not the pattern's
 * sources; NOT on what computes the values. So a COMPUTED fold index —
 * an arithmetic chain in the dead region (M2.1-style constant folding
 * makes it fold), or a CMP/LEA/PTRADD result — merges exactly like a
 * LOADI did. Excluded: OP_CALL (no plain claim; clobbers the call-site
 * scratch) and OP_LOAD (its address math's clobber_scratch paths and
 * the M2.8-M2.10 folds aren't covered by the invariant argument). */
static int ar_is_interm_op(uint8_t op) {
    switch (op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:
    case OP_SAR: case OP_NOT: case OP_NEG: case OP_MOV: case OP_LOADI:
    case OP_LOADI64: case OP_CMP: case OP_LEA: case OP_PTRADD:
        return 1;
    default:
        return 0;
    }
}
/* M2.19: per-instruction constant propagation, factored out of the
 * fixpoint scan so a leaf callee can be analyzed against a scratch map.
 * Applies one instruction's effect to known[]/val[]: LOADI/LOADI64/MOV
 * establish or copy, the ALU family folds when its sources are known
 * (plain 64-bit ops, never fault; shifts mask the amount mod 64; SAR
 * sign-fills), and every other register-writing opcode breaks the
 * constant. ENTER/RET/JMPR/CALL/BR/BC are handled by the callers. */
static void ar_const_step(uint64_t w, uint32_t num_literals, const uint64_t* literals,
                          uint8_t* known, uint64_t* val) {
    uint8_t op = w_op(w);
    uint16_t rd = w_rd(w), ra = w_ra(w);
    if (op == OP_LOADI) {
        if (rd < TX_AR_MAX_REGS) { known[rd] = 1; val[rd] = (uint64_t)(int64_t)w_imm28(w); }
    } else if (op == OP_LOADI64) {
        uint32_t idx = w_rb_raw(w);
        if (idx < num_literals && rd < TX_AR_MAX_REGS) { known[rd] = 1; val[rd] = literals[idx]; }
    } else if (op == OP_MOV) {
        if (rd < TX_AR_MAX_REGS) {
            known[rd] = (ra < TX_AR_MAX_REGS) ? known[ra] : 0;
            val[rd] = (ra < TX_AR_MAX_REGS) ? val[ra] : 0;
        }
    } else if (op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_AND ||
               op == OP_OR || op == OP_XOR || op == OP_SHL || op == OP_SHR ||
               op == OP_SAR) {
        /* M2.3: fold arithmetic constants through the whole ALU
         * family. The emitted ALU is a plain 64-bit op for every
         * SIMI type (no width or signedness rounding — the
         * translator never truncates), the imm28 is sign-extended
         * exactly as materialize_imm does, and the binary ops are
         * enc_and/orr/eor_shift while the shifts are
         * enc_lslv/lsrv/asrv — all plain 64-bit, never fault. The
         * shift AMOUNT is masked mod 64 (& 0x3F) in hardware
         * (lslv/lsrv/asrv) and identically in the interpreter
         * (fetch_operand_b & 0x3F) and RV64 (v2 & 0x3F), so the
         * fold masks too; SAR is arithmetic (sign-filling) like
         * the interpreter's (int64)>>. Deliberately limited:
         * DIV/MOD would change behavior on a translate-time
         * division by zero, and NOT (a foldable ~a, plain 64-bit)
         * is left out as a conservative omission — unary, never
         * a dispatch index in practice. */
        if (rd < TX_AR_MAX_REGS) {
            int k = (ra < TX_AR_MAX_REGS) ? known[ra] : 0;
            uint64_t a = (ra < TX_AR_MAX_REGS) ? val[ra] : 0;
            int fold = 0;
            uint64_t b = 0;
            if (w_flags(w) & FLAG_IMM) {
                b = (uint64_t)(int64_t)w_imm28(w);
                fold = k;
            } else {
                uint16_t rb = w_rb_reg(w);
                fold = k && rb < TX_AR_MAX_REGS && known[rb];
                if (fold) b = val[rb];
            }
            if (fold) {
                known[rd] = 1;
                uint64_t amt = b & 0x3F;   /* shifts mask the amount mod 64 */
                switch (op) {
                    case OP_ADD: val[rd] = a + b; break;
                    case OP_SUB: val[rd] = a - b; break;
                    case OP_MUL: val[rd] = a * b; break;
                    case OP_AND: val[rd] = a & b; break;
                    case OP_OR:  val[rd] = a | b; break;
                    case OP_XOR: val[rd] = a ^ b; break;
                    case OP_SHL: val[rd] = a << amt; break;
                    case OP_SHR: val[rd] = a >> amt; break;
                    case OP_SAR: val[rd] = (uint64_t)((int64_t)a >> amt); break;
                }
            } else {
                known[rd] = 0;
            }
        }
    } else if (rd < TX_AR_MAX_REGS) {
        known[rd] = 0;   /* every other register-writing opcode breaks the constant */
    }
}
/* M2.20: is the function at T an analyzable straight-line leaf? Starts
 * with ENTER at T (the emitted prologue zeroes the frame, matching the
 * interpreter's fresh-frame CALL — a callee without ENTER reads the
 * caller's stale frame in the emitted code and is not analyzable),
 * contains no BR/BC/JMPR/CALL and no mid-body ENTER (M2.19 reviewer
 * hardening: a mid-body ENTER would mean the region overlaps another
 * function's prologue, and the fixpoint's own OP_ENTER semantics is a
 * full constant-map reset) before its FIRST RET. Returns that RET's pc,
 * or -1 when the callee is not analyzable. The per-call-site BODY
 * analysis (what r0 is at the RET) lives in the fixpoint scan, which
 * seeds a scratch map from the CALLER's argument constants at the call
 * (M2.20) — or leaves them unknown (M2.19's per-callee analysis, the
 * args-unknown special case). */
static int ar_leaf_ret_pc(const uint64_t* instrs, uint32_t num_instr, uint32_t T) {
    if (w_op(instrs[T]) != OP_ENTER) return -1;
    for (uint32_t pc = T + 1; pc < num_instr; pc++) {
        uint8_t op = w_op(instrs[pc]);
        if (op == OP_ENTER) return -1;
        if (op == OP_RET) return (int)pc;
        if (op == OP_BR || op == OP_BC || op == OP_JMPR || op == OP_CALL) return -1;
    }
    return -1;   /* no RET before the end of the stream */
}
/* M2.20: per-callee-ENTRY straight-line leaf marker (the RET pc, or -1
 * when the callee at that pc is not analyzable). Precomputed before the
 * fixpoint; the scan's CALL branch runs the body analysis with a scratch
 * map seeded from the CALLER's constants. The scratch map is file-scope
 * (like g_relax_snap_*) — transient per-CALL-per-pass state, hoisted to
 * avoid stack churn on every CALL in every scan pass. */
static int g_call_leaf_ret[4096];
static uint8_t  g_call_arg_known[TX_AR_MAX_REGS];
static uint64_t g_call_arg_val[TX_AR_MAX_REGS];
/* M2.21: static reachability from the entries. The BFS follows the
 * non-JMPR control edges (BR/BC targets + fall-through, CALL target +
 * fall-through; RET is a terminal) AND, because it runs AFTER the fold
 * fixpoint, the fold edges: a FOLDED JMPR's target is a runtime edge
 * (the fold is a direct branch — the M2.12 fall-through fold's target
 * pc+1 is reached by falling through), while an UNFOLDED JMPR is a
 * terminal whose runtime target is data-dependent. g_alloc gates on
 * REACHABLE JMPRs only: a JMPR whose pc can never dispatch cannot make
 * every pc a potential block head, so an unreachable function's
 * non-folding JMPR no longer throws the whole program back to the naive
 * path. The fold edges are the soundness-critical part: a reachable
 * fold can dispatch into a statically-unreachable-looking region, and
 * a non-folding JMPR THERE would still dispatch to arbitrary pcs at
 * runtime — it must gate (M2.21 reviewer finding). */
static uint8_t  g_reach[4096];
/* M2.22: per-pc LIVE-incoming-edge count from the same BFS. A pc with
 * count 0 is never entered by any runtime path (a dead-region start —
 * entries are roots, marked reachable with count 0, so the count alone
 * does not distinguish them; see the emission's region-start reset). */
static uint16_t g_npred[4096];
/* M2.11: run-reuse tables. g_run_first/g_run_cont mark the head pc and the
 * continuation pcs of a maximal straight-line run of LOAD/STORE that share
 * ONE displacement past the imm12 range — the displacement is materialized
 * once into X_DR (x12) at the run head and every access in the run is a
 * single register-offset word. g_cur_pc is the main loop's current pc,
 * visible to emit_instr (which takes no pc parameter). */
static uint8_t  g_run_first[4096];
static uint8_t  g_run_cont[4096];
static uint32_t g_cur_pc;
/* M2.14: tail-reuse offset. The first RET in the function emits the
 * shared return-sequence tail in place and records the tail's first
 * word here; every later RET emits `cache_flush; b tail` instead of its
 * own copy (0xFFFFFFFF = no owner yet / naive mode). See OP_RET. */
static uint32_t g_tail_ret_off;

static uint8_t cache_host(int i) { return (uint8_t)(X_T0 + i); }
static int cache_find(int g) {
    for (int i = 0; i < AR_CACHE_N; i++)
        if (g_cache_guest[i] == g) return i;
    return -1;
}
/* Write one resident value back to its slot and drop the directory entry.
 * The tag byte is already authoritative in memory (rule 3). */
static void cache_spill_one(struct CodeBuf* cb, int i) {
    if (!g_alloc || g_cache_guest[i] < 0) return;
    st_slot(cb, cache_host(i), g_cache_guest[i]);
    g_cache_guest[i] = -1;
}
static void cache_flush(struct CodeBuf* cb) {
    if (!g_alloc) return;
    for (int i = 0; i < AR_CACHE_N; i++) cache_spill_one(cb, i);
}
/* Claim a host register for rd's result; returns the cache slot (result
 * register = x9+slot), or -1 in naive mode (result in x9, stored by
 * store_result). When rd is already resident its slot is REUSED and the
 * result is computed in place (g_resv_reuse=1): the register still holds
 * the OLD value, which an operand read of rd must see — and a new slot
 * claim would strand that value in the old slot's register while the
 * directory pointed at a fresh one. Only when rd is not resident does a
 * victim get evicted (spilled) and claimed.
 *
 * M2.4: src1/src2 are the instruction's source operands (guest regs it
 * reads, or -1 when there is no second source / the operand is an
 * immediate). The victim is chosen to PREFER a slot that is neither
 * source: evicting a source forces a spill now and a memory reload when
 * get_operand fetches it a few words later, on exactly the chains that
 * re-read the same register (e.g. ADD r5, r1, r2 after ADD r4, r1, r2).
 * With 3 slots and at most 2 distinct sources, a non-source slot always
 * exists (a reused rd is handled by the reuse path above), so the scan
 * finds one; the plain round-robin victim remains only as a defensive
 * fallback. The round-robin cursor still advances every call, so no
 * slot starves. */
static int cache_reserve(struct CodeBuf* cb, int rd, int src1, int src2) {
    if (!g_alloc) { g_resv_slot = -1; g_resv_reuse = 0; return -1; }
    int v = cache_find(rd);
    if (v >= 0) {
        g_resv_reuse = 1;
    } else {
        /* Two-pass preference: an EMPTY slot (never costs a spill) beats
         * a non-source occupant, which beats the plain round-robin
         * fallback. Empty slots occur transiently — clobber_scratch and
         * cache_spill_one leave -1 without reclaiming — so scanning for
         * them first avoids spilling a dead-but-occupied slot when a
         * free one exists. With rd not resident, at most 2 of the 3
         * slots hold the ≤2 distinct sources, so pass 2 always finds a
         * non-source slot; the fallback is defensive only. */
        int i = 0;
        for (i = 0; i < AR_CACHE_N; i++)
            if (g_cache_guest[(g_cache_round + i) % AR_CACHE_N] < 0) break;
        if (i >= AR_CACHE_N)
            for (i = 0; i < AR_CACHE_N; i++) {
                int cand = (g_cache_round + i) % AR_CACHE_N;
                if (g_cache_guest[cand] != src1 && g_cache_guest[cand] != src2) break;
            }
        v = (i < AR_CACHE_N) ? (g_cache_round + i) % AR_CACHE_N : g_cache_round;
        g_cache_round = (g_cache_round + 1) % AR_CACHE_N;
        if (g_cache_guest[v] >= 0) cache_spill_one(cb, v);
        g_resv_reuse = 0;
    }
    g_cache_guest[v] = rd;
    g_resv_slot = v;
    g_resv_guest = rd;
    return v;
}
static uint8_t result_host(int v) { return (uint8_t)(X_T0 + (v >= 0 ? v : 0)); }
/* Commit a plain result: resident in alloc mode (tag byte stored now),
 * M0 store+untag in naive mode. */
static void store_result(struct CodeBuf* cb, int rd) {
    if (g_alloc) st_untag(cb, rd);   /* value already resident in the reserved register */
    else         st_slot_untag(cb, X_T0, rd);
}
/* Evict (spill) the occupant of cache slot hosting `h` unless it is the
 * in-flight result slot, before `h` is clobbered as scratch or as a
 * base/address register (the occupant's value has been consumed). The
 * reserved slot is never spilled here — if it was reused, its old value
 * is dead (being replaced); if freshly claimed, the register is garbage
 * until the result lands. */
static void clobber_scratch(struct CodeBuf* cb, uint8_t h) {
    int slot = (int)(h - X_T0);
    if (g_alloc && slot >= 0 && slot < AR_CACHE_N &&
        g_cache_guest[slot] >= 0 && slot != g_resv_slot)
        cache_spill_one(cb, slot);
}
/* M2.5: FETCH TARGET SELECTION AWARE OF BOTH OPERANDS. get_operand's
 * default host assignment is g1→x9, g2→x10. When the operands are
 * resident CROSSED (g1 at slot 1, g2 at slot 0), the default fetch of
 * g1 spills g2, then the fetch of g2 spills g1 and reloads g2 from
 * memory — two spills and a reload for operands that were BOTH resident
 * (e.g. ADD r5, r2, r1 after ADD r4, r1, r2 leaves r1@slot0 and
 * r2@slot1, so a swapped-order ADD crosses them). Swapping the
 * assignment (g1→x10, g2→x9) makes each fetch a no-op into its own
 * resident slot. The rule swaps whenever g1 is resident at slot 1 or g2
 * at slot 0, because the swap is never larger than the default and
 * usually smaller (g1@1 avoids spilling slot 1's occupant AND a wasted
 * mov; g2@0 is the symmetric case); the cases where the default is
 * already optimal (both at slots 0/2 or 2/1) do not match the rule.
 * Naive mode keeps the canonical x9/x10 order — M0 codegen must stay
 * byte-identical for the gate baselines. */
static void cache_fetch_hosts(int g1, int g2, uint8_t* h1, uint8_t* h2) {
    *h1 = X_T0; *h2 = X_T1;
    if (!g_alloc) return;
    if (cache_find(g1) == 1 || cache_find(g2) == 0) { *h1 = X_T1; *h2 = X_T0; }
}
/* M2.5: fetch host for an instruction's ONE register operand when the
 * other operand is an immediate needing a scratch register (x9 or x10).
 * Default is operand→x9, imm→x10. When the operand is resident at slot
 * 1, that default would spill it on the imm materialization (and waste
 * the mov that just fetched it into x9), so swap: operand→x10, imm→x9.
 * The swap also defuses the only dangerous aliasing for the imm target:
 * a REUSED result register holding rd's live old value sits at
 * g_resv_slot, and rd==ra there means cache_find(ra)==g_resv_slot — so
 * when the operand takes X_T1 (g_resv_slot==1), the imm goes to X_T0
 * and never touches it; when the imm goes to X_T1, the operand did not
 * take X_T1, so g_resv_slot==1 there implies the reused value is dead
 * (rd != ra) or the slot is a fresh-claim phantom (garbage until the
 * result lands). */
static uint8_t cache_single_host(int g) {
    if (g_alloc && cache_find(g) == 1) return X_T1;
    return X_T0;
}
/* M2.7/M2.8: split a magnitude into A64's add/sub immediate form —
 * (sh, imm12) with value = imm12 << (12*sh), sh=0 for 0..4095 and
 * sh=1 for multiples of 4096 up to 0xFFFFFF — returning 0 when it does
 * not fit. Shared by the ALU immediate fold and the LOAD/STORE
 * displacement address math. */
static int imm12_split(uint64_t mag, int* sh, uint32_t* u) {
    if (mag <= 4095) { *sh = 0; *u = (uint32_t)mag; return 1; }
    if ((mag & 0xFFFu) == 0 && mag <= 0xFFFFFFu) { *sh = 1; *u = (uint32_t)(mag >> 12); return 1; }
    return 0;
}
/* Materialize `imm` into the given scratch host, spilling its occupant
 * first. Callers pick the host with cache_single_host/cache_fetch_hosts
 * so it is never the first-operand host (which holds a live operand this
 * instruction reads). The reserved-result aliasing cases are all
 * dead-or-phantom: h is always x9 or x10 (never the slot-2 host), and
 * the one live-reuse slot that could collide — slot 1 with rd==ra — is
 * exactly the case cache_single_host's slot-1 check swaps away, so the
 * imm never overwrites a reused result register still holding rd's live
 * old value (this is why the M1-era X_T2 g_resv_slot==1 fallback is
 * gone). */
static uint8_t materialize_imm(struct CodeBuf* cb, uint64_t imm, uint8_t h) {
    clobber_scratch(cb, h);
    emit_li64(cb, h, imm);
    return h;
}
/* Read guest register g into host h (x9/x10/x11), through the cache.
 * Loading or moving into h destroys any occupant of h's slot, so that
 * occupant is spilled first — except the in-flight result slot. The one
 * subtlety is the fresh-claim phantom: cache_reserve may have just
 * claimed a slot for rd whose register does NOT yet hold rd's value (the
 * result lands there at the end of the instruction), so a read of rd as
 * an operand when rd was not resident must go to memory, not to the
 * phantom register. When the slot was REUSED the old value IS in the
 * register, so the read uses it.
 *
 * rd_live says whether this instruction reads rd's OLD value as a source
 * operand (rd aliases an operand register). When an operand fetch must
 * clobber a REUSED result register, the old value is spilled only when
 * rd_live — a later fetch of rd must reload it from memory; when rd is
 * not an operand (pure ADD rd, ra, rb with rd resident) the old value is
 * dead, so the spill is skipped and the result overwrites it in place,
 * saving one store on the common path. The reservation is downgraded to
 * a fresh claim either way: the register is garbage until the result
 * lands, so reads of rd must hit memory (the phantom rule below). */
static void get_operand(struct CodeBuf* cb, int g, uint8_t h, int rd_live) {
    if (!g_alloc) { ld_slot(cb, h, g); return; }
    int slot = (int)(h - X_T0);
    if (g_cache_guest[slot] >= 0 && g_cache_guest[slot] != g) {
        if (slot == g_resv_slot && g_resv_reuse) {
            /* The register h is about to clobber is the in-place RESULT
             * register, still holding rd's OLD value. If rd is an operand
             * of this instruction (e.g. ADD rd, ra, rd) that value is
             * live and must be spilled so a later fetch of rd reloads it
             * from memory; if rd is not an operand, the value is dead and
             * the spill is skipped (the result replaces it in place).
             * Downgrade to a fresh claim either way: the register is
             * garbage until the result lands, so reads of rd must hit
             * memory (the phantom rule below). */
            if (rd_live) cache_spill_one(cb, slot);
            g_cache_guest[slot] = g_resv_guest;
            g_resv_reuse = 0;
        } else if (slot != g_resv_slot) {
            cache_spill_one(cb, slot);
        }
    }
    int i = cache_find(g);
    if (i >= 0 && !(i == g_resv_slot && !g_resv_reuse)) {
        if (i != slot) e32(cb, enc_orr_shift(h, 31, cache_host(i), 0, 0));  /* mov h, x{i} */
    } else {
        ld_slot(cb, h, g);   /* not resident, or the claimed slot is a phantom */
    }
}
/* ─── 64-bit constants: movz + up to 3 movk, no literal pool ─────────────
 * RV64 needed a whole literal-pool machinery (auipc+ld pairs, a second
 * patch pass, pool emission) because RV64 has no single-instruction 64-bit
 * load. A64's move-wide family does it in 1-4 fixed instructions, so all
 * of that machinery simply does not exist here. Zero high halves are
 * skipped (movz already zeroed the register). */
static void emit_li64(struct CodeBuf* cb, uint8_t reg, uint64_t imm) {
    e32(cb, enc_movz(reg, (uint16_t)(imm & 0xFFFF), 0));
    for (int hw = 1; hw < 4; hw++) {
        uint16_t half = (uint16_t)((imm >> (16 * hw)) & 0xFFFF);
        if (half) e32(cb, enc_movk(reg, half, (uint8_t)hw));
    }
}
/* ─── Local (intra-instruction) conditional branch helpers ───────────────
 * OBJSIZE/OBJTYPE and JMPR need runtime branches whose targets are a few
 * instructions further into this same emitted SIMI instruction's code,
 * not another SIMI pc — same rationale as RV64's emit_beqz_placeholder:
 * they don't go through the global g_fixups table (keyed by target_pc,
 * resolved in a separate pass) and are patched immediately. */
static uint32_t emit_cbz_placeholder(struct CodeBuf* cb) {
    uint32_t pos = cb->len;
    e32(cb, 0);   /* placeholder, patched below once the branch target is known */
    return pos;
}
static void patch_local_cbz(struct CodeBuf* cb, uint32_t pos, uint8_t rt) {
    int32_t off = (int32_t)(cb->len - pos);
    patch32(cb->buf, pos, enc_cbz(rt, off / 4));
}
/* Phase 15 (A3): local B.cond and backward-CBNZ variants of the same
 * pattern — the CAS lr/sc-style loop needs a forward b.ne (mismatch ->
 * .done) and a backward cbnz (stlxr failure -> .retry), both
 * intra-instruction. enc_b_cond's imm19 is in words, like the other
 * branch encoders here. */
static uint32_t emit_bcond_placeholder(struct CodeBuf* cb) {
    uint32_t pos = cb->len;
    e32(cb, 0);
    return pos;
}
static void patch_local_bcond(struct CodeBuf* cb, uint32_t pos, uint8_t cond) {
    int32_t off = (int32_t)(cb->len - pos);
    patch32(cb->buf, pos, enc_b_cond(cond, off / 4));
}
static uint32_t emit_cbnz_placeholder(struct CodeBuf* cb) {
    uint32_t pos = cb->len;
    e32(cb, 0);
    return pos;
}
static void patch_cbnz_back(struct CodeBuf* cb, uint32_t branch_pos, uint32_t target_pos, uint8_t rt) {
    int32_t off = (int32_t)(target_pos - branch_pos);
    patch32(cb->buf, branch_pos, enc_cbnz(rt, off / 4));
}
static uint32_t emit_b_placeholder(struct CodeBuf* cb) {
    uint32_t pos = cb->len;
    e32(cb, 0);
    return pos;
}
static void patch_local_b(struct CodeBuf* cb, uint32_t pos) {
    int32_t off = (int32_t)(cb->len - pos);
    patch32(cb->buf, pos, enc_b(off / 4));
}
/* The all-zero 32-bit word is permanently reserved by the base A64 ISA as
 * UDF #0, an unconditional undefined instruction (A64 has no 16-bit
 * compressed encoding to reserve instead) — the direct analog of RV64's
 * reserved all-zeros word and x86's ud2, used for the JMPR OOB hazard. */
static void op_illegal(struct CodeBuf* cb) { e32(cb, 0); }

/* ─── Branch/call fixups (B / BL / CBZ / CBNZ targets) ───────────────────
 * Same two-pass shape as simi_x86.c's g_fixups: emit a placeholder word
 * during the main pass, rewrite the whole word once every instruction's
 * offset is known (the imm26/imm19 fields are not contiguous sub-fields
 * that survive patching in place — same reasoning as RV64's J/B-type
 * re-encode). */
enum { FIX_B, FIX_BL, FIX_CBZ, FIX_CBNZ, FIX_B_COND };
#define TX_AR_MAX_FIXUPS 4096
struct Fixup { uint32_t instr_pos; uint32_t target_pc; uint8_t kind; uint8_t rt; };
static struct Fixup g_fixups[TX_AR_MAX_FIXUPS];
static uint32_t g_nfixups;
static uint32_t g_instr_off[4096];   /* TX_MAX_INSTR, mirrors simi_x86.c */

/* v0.3 (Phase 6): translate-time context for RESOLVE/OBJSIZE/OBJTYPE. */
static uint64_t g_rt_resolve_fn, g_rt_objsize_fn, g_rt_objtype_fn;
static uint32_t g_num_names;

/* Gap Remediation SIMI Phase 14: JMPR support.
 * g_num_instr mirrors simi_x86.c's global of the same name. The jump
 * table itself holds plain byte offsets into out_buf (g_instr_off[pc]),
 * NOT host addresses — a64_exec.c's guest CPU addresses its own code
 * purely by offset into a bounded guest memory buffer, exactly like every
 * B/BL/CBZ target this file computes (see simi_riscv.c's Phase 14
 * correction note for the identical trap, already learned once on RV64).
 * Each JMPR site emits a fixed 2-word movz+movk placeholder for the
 * table base offset (the only constant not known at emit time); the
 * position of each placeholder is recorded here and rewritten in the
 * final pass once the table's offset is known. The base is a byte
 * offset into out_buf — cb.len is uint32_t, so 32 bits always suffice
 * (M2.24; M2.23-era used a 4-word li64 for an 8-byte-entry table). */
static uint32_t g_num_instr;
static uint32_t g_jmpr_li_pos[TX_AR_MAX_FIXUPS];
static uint32_t g_njmpr_li_pos;

static int add_fixup(struct CodeBuf* cb, uint32_t target_pc, uint8_t kind, uint8_t rt) {
    if (g_nfixups >= TX_AR_MAX_FIXUPS) return 0;
    g_fixups[g_nfixups].instr_pos = cb->len;
    g_fixups[g_nfixups].target_pc = target_pc;
    g_fixups[g_nfixups].kind = kind;
    g_fixups[g_nfixups].rt = rt;
    g_nfixups++;
    e32(cb, 0);   /* placeholder word, patched later */
    return 1;
}
static void op_b(struct CodeBuf* cb, uint32_t target_pc) { add_fixup(cb, target_pc, FIX_B, 0); }
static void op_bl(struct CodeBuf* cb, uint32_t target_pc) { add_fixup(cb, target_pc, FIX_BL, 0); }
/* BC tests a single register for zero/nonzero — CBZ/CBNZ fit SIMI's BC
 * exactly (A64 has no flag-conditioned branch on a register value, but it
 * has the compare-and-branch-on-zero pair built in). */
static void op_cbz(struct CodeBuf* cb, uint32_t target_pc, uint8_t rt) { add_fixup(cb, target_pc, FIX_CBZ, rt); }
static void op_cbnz(struct CodeBuf* cb, uint32_t target_pc, uint8_t rt) { add_fixup(cb, target_pc, FIX_CBNZ, rt); }
static void op_b_cond(struct CodeBuf* cb, uint32_t target_pc, uint8_t cond) { add_fixup(cb, target_pc, FIX_B_COND, cond); }

/* M2.25: merge constant set b into set a, keeping sorted order with
 * dedup. A union that would exceed TX_AR_CHAIN_MAX collapses to UNKNOWN
 * — the sound over-approximation that disables the chain (the walk is
 * monotone, so the iteration converges). */
static void chain_merge(struct ChainSet* a, const struct ChainSet* b) {
    if (b->unk || a->unk) { a->unk = 1; a->n = 0; a->def = -1; return; }
    uint32_t tmp[TX_AR_CHAIN_MAX];
    uint8_t i = 0, j = 0, k = 0;
    while (i < a->n && j < b->n && k < TX_AR_CHAIN_MAX) {
        if (a->v[i] < b->v[j]) tmp[k++] = a->v[i++];
        else if (b->v[j] < a->v[i]) tmp[k++] = b->v[j++];
        else { tmp[k++] = a->v[i++]; j++; }
    }
    while (i < a->n && k < TX_AR_CHAIN_MAX) tmp[k++] = a->v[i++];
    while (j < b->n && k < TX_AR_CHAIN_MAX) tmp[k++] = b->v[j++];
    if (i < a->n || j < b->n) { a->unk = 1; a->n = 0; a->def = -1; return; }   /* union overflowed the cap */
    for (uint8_t t = 0; t < k; t++) a->v[t] = tmp[t];
    a->n = k;
    a->def = -1;   /* every merge output is flat (M2.30) */
}
/* M2.26: set constructors and the tracked-register operand tests. Every
 * constructor and every merge OUTPUT is a flat set (def = -1); only the
 * walk's product branch records a deferred form. */
static void chain_singleton(struct ChainSet* s, uint32_t v) { s->n = 1; s->unk = 0; s->v[0] = v; s->def = -1; }
static void chain_unknown(struct ChainSet* s) { s->n = 0; s->unk = 1; s->def = -1; }
/* M2.27: the ALU image evaluator — plain 64-bit, never faults,
 * type-agnostic (the M2.2 constant-fold argument). M2.47: the shifts
 * join the family — SHL/SHR/SAR are also plain 64-bit and never fault;
 * the AMOUNT is masked mod 64 (& 0x3F) exactly as the hardware does
 * (lslv/lsrv/asrv), the interpreter (fetch_operand_b & 0x3F) and
 * ar_const_step — so a constant-amount shift of a known set re-derives
 * (the M2.3 argument). SHR is LOGICAL (unsigned >>), SAR arithmetic
 * (sign-filling, like the interpreter's (int64)>>). M2.48: the unary
 * NOT (~a) and NEG (-a) join too — plain 64-bit, never fault,
 * type-agnostic; bv is ignored for them. */
static int64_t chain_alu_eval(uint8_t op, int64_t av, int64_t bv) {
    switch (op) {
    case OP_ADD: return av + bv;
    case OP_SUB: return av - bv;
    case OP_MUL: return av * bv;
    case OP_AND: return av & bv;
    case OP_OR:  return av | bv;
    case OP_XOR: return av ^ bv;
    case OP_SHL: return av << (bv & 0x3F);
    case OP_SHR: return (int64_t)((uint64_t)av >> (bv & 0x3F));
    case OP_SAR: return (int64_t)((int64_t)av >> (bv & 0x3F));
    case OP_NOT: return ~av;
    case OP_NEG: return -av;
    default:     return 0;   /* caller gates the op set */
    }
}
/* M2.32: allocate a deferred-product record referencing operand a and
 * operand b (each a tracked slot, an older pool record, an immediate,
 * or — M2.41 — a FROZEN flat set). A CD_FLAT operand is the record's
 * OWN store index (ri — the freeze set is copied into g_chain_def_flat
 * before the record is published), so an in-place product over a flat
 * source (ADD r1, r1, r4) can defer soundly: the aliased source's set
 * is frozen as it stands at THIS instruction — exactly the runtime
 * value r1 holds — instead of self-referencing cur[r1]. `freeze` is
 * the flat set to store (NULL when neither operand is CD_FLAT).
 * Returns -1 on pool exhaustion — the caller falls back to UNKNOWN
 * (conservative). The pool is reset at the top of every walk
 * iteration, so the indices stored in cur[].def are valid only within
 * one iteration. */
static int chain_def_alloc(uint8_t op, uint8_t ka, int32_t a, uint8_t kb, int32_t b,
                           const struct ChainSet* freeze, uint32_t pc) {
    if (g_chain_ndef >= TX_AR_CHAIN_DEFS) return -1;
    /* M2.55: the freeze copy below writes freeze->n values into the
     * record's BIG-shaped store (g_chain_def_flat[ri].v[TX_AR_CHAIN_BIG]),
     * but freeze is a flat ChainSet (stored at TX_AR_CHAIN_MAX width).
     * The shipped caps are equal, so freeze->n <= MAX = BIG and the copy
     * is safe; a cap raise that makes MAX > BIG (the M2.54-direction
     * control — an in-place product over a >64-value flat source) would
     * OVERFLOW the store — refuse the record instead (the caller falls
     * back to UNKNOWN, the exact-or-conservative discipline, matching
     * chain_flatten_big's M2.54 guard). */
    if (freeze && (freeze->unk || freeze->n > TX_AR_CHAIN_BIG)) return -1;
    int ri = g_chain_ndef++;
    struct ChainDef* d = &g_chain_defs[ri];
    d->op = op;
    d->ka = ka; d->a = (ka == CD_FLAT) ? ri : a;
    d->kb = kb; d->b = (kb == CD_FLAT) ? ri : b;
    if (freeze) {
        g_chain_def_flat[ri].n = freeze->n;
        g_chain_def_flat[ri].unk = freeze->unk;
        for (uint8_t i = 0; i < freeze->n; i++) g_chain_def_flat[ri].v[i] = freeze->v[i];
    }
    g_chain_def_pc[ri] = pc;   /* M2.36: the record's value is fixed at this instruction */
    return ri;
}
/* M2.32: does the record DAG rooted at rec transitively reference any
 * tracked slot marked dirty? Records reference slots and older records
 * only, so this terminates. */
static int chain_def_touches(int16_t rec, const uint8_t* dirty) {
    const struct ChainDef* d = &g_chain_defs[rec];
    if ((d->ka == CD_SLOT && dirty[d->a]) || (d->kb == CD_SLOT && dirty[d->b])) return 1;
    if (d->ka == CD_REC && chain_def_touches(d->a, dirty)) return 1;
    if (d->kb == CD_REC && chain_def_touches(d->b, dirty)) return 1;
    return 0;
}
/* M2.30/M2.32: materialize a deferred product RECORD into a flat set,
 * capped at TX_AR_CHAIN_MAX (overflow -> UNKNOWN — the conservative
 * walk-state bound). A CD_SLOT operand reads cur[] (recursing if that
 * slot is itself deferred); a CD_REC operand recurses into the older
 * record. Called wherever a deferred form must become flat: source
 * writes, head unions, deliveries, and as an input to a later image. */
static void chain_flatten(struct ChainSet* out, const struct ChainSet* cur, int16_t slot);
static void chain_flatten_big(struct ChainBig* out, const struct ChainSet* cur, int16_t slot);
static void chain_flatten_rec(struct ChainSet* out, const struct ChainSet* cur, int16_t rec);
static void chain_flatten_big_rec(struct ChainBig* out, const struct ChainSet* cur, int16_t rec);
/* M2.33/M2.37: resolve one record operand into a capped flat set: an
 * immediate (a singleton), a tracked slot (recursing if that slot is
 * itself deferred), an older record, or a FROZEN flat set (CD_FLAT —
 * copied out of the record's own store). */
static void chain_flatten_op(struct ChainSet* out, const struct ChainSet* cur, uint8_t k, int32_t v) {
    if (k == CD_IMM) { out->n = 1; out->unk = 0; out->v[0] = (uint32_t)v; out->def = -1; }
    else if (k == CD_SLOT) { if (cur[v].def >= 0) chain_flatten(out, cur, (int16_t)v); else *out = cur[v]; }
    else if (k == CD_FLAT) {
        /* M2.41: the frozen store is BIG-capable (a pre-merged union can
         * exceed the walk bound) — cap-copy into the small flat set,
         * collapsing to UNKNOWN on overflow (the walk's flat bound). */
        const struct ChainBig* st = &g_chain_def_flat[v];
        if (st->unk || st->n > TX_AR_CHAIN_MAX) { chain_unknown(out); return; }
        out->n = st->n; out->unk = 0; out->def = -1;
        for (uint8_t i = 0; i < st->n; i++) out->v[i] = st->v[i];
    }
    else chain_flatten_rec(out, cur, (int16_t)v);
}
static void chain_flatten_rec(struct ChainSet* out, const struct ChainSet* cur, int16_t rec) {
    const struct ChainDef* d = &g_chain_defs[rec];
    if (d->op == CHAIN_OP_UNION) {
        /* M2.37/M2.41: the union record's true set is PRE-MERGED and
         * frozen in its store at creation — exact-or-conservative, both
         * sides are materialized over the current cur[] at the head (the
         * carry record is live by construction, the arrival record under
         * chain_def_live, flat sides frozen) and later head-unions can
         * only widen. Flattening into the walk's flat set copies the
         * store, capped at TX_AR_CHAIN_MAX — overflow collapses to
         * UNKNOWN, the walk's flat bound. */
        const struct ChainBig* st = &g_chain_def_flat[rec];
        if (st->unk || st->n > TX_AR_CHAIN_MAX) { chain_unknown(out); return; }
        out->n = st->n; out->unk = 0; out->def = -1;
        for (uint8_t i = 0; i < st->n; i++) out->v[i] = st->v[i];
        return;
    }
    struct ChainSet va, vb;
    chain_flatten_op(&va, cur, d->ka, d->a);
    chain_flatten_op(&vb, cur, d->kb, d->b);
    if (va.unk || vb.unk) { chain_unknown(out); return; }
    struct ChainSet tmp = { .n = 0, .unk = 0 };
    for (uint8_t i = 0; i < va.n && !tmp.unk; i++)
        for (uint8_t j = 0; j < vb.n && !tmp.unk; j++) {
            struct ChainSet one;
            chain_singleton(&one, (uint32_t)chain_alu_eval(d->op, (int64_t)va.v[i], (int64_t)vb.v[j]));
            chain_merge(&tmp, &one);
        }
    *out = tmp;
}
static void chain_flatten(struct ChainSet* out, const struct ChainSet* cur, int16_t slot) {
    const struct ChainSet* s = &cur[slot];
    if (s->def < 0) { *out = *s; return; }
    chain_flatten_rec(out, cur, s->def);
}
static void chain_unknown_big(struct ChainBig* s) { s->n = 0; s->unk = 1; }
static void chain_merge_big(struct ChainBig* a, const struct ChainSet* b) {
    if (b->unk || a->unk) { a->unk = 1; a->n = 0; return; }
    uint32_t tmp[TX_AR_CHAIN_BIG];
    uint8_t i = 0, j = 0, k = 0;
    while (i < a->n && j < b->n && k < TX_AR_CHAIN_BIG) {
        if (a->v[i] < b->v[j]) tmp[k++] = a->v[i++];
        else if (b->v[j] < a->v[i]) tmp[k++] = b->v[j++];
        else { tmp[k++] = a->v[i++]; j++; }
    }
    while (i < a->n && k < TX_AR_CHAIN_BIG) tmp[k++] = a->v[i++];
    while (j < b->n && k < TX_AR_CHAIN_BIG) tmp[k++] = b->v[j++];
    if (i < a->n || j < b->n) { a->unk = 1; a->n = 0; return; }
    for (uint8_t t = 0; t < k; t++) a->v[t] = tmp[t];
    a->n = k;
}
/* M2.37/M2.38/M2.41: allocate a UNION record whose true set is the
 * PRE-MERGED union of two sides, each a deferred record (CD_REC) or a
 * FROZEN flat set (CD_FLAT), capped at TX_AR_CHAIN_BIG. M2.37: a
 * deferred record meets a non-contained flat set — op(rec(R), flat(F));
 * M2.38: two different deferred records — op(rec(R), rec(R2)); M2.41:
 * two flat sets whose merged walk set exceeds TX_AR_CHAIN_MAX —
 * op(flat(F1), flat(F2)). The merged true set is stored in the
 * record's own g_chain_def_flat[ri] (a CD_FLAT self-reference: d->ka =
 * d->kb = CD_FLAT, d->a = d->b = ri), so materialization is a single
 * store copy and the DAG never grows. The eager cap-check is
 * exact-or-conservative: both sides are materialized over the CURRENT
 * cur[] (record sides are live — the carry record by construction, the
 * arrival record under chain_def_live; flat sides are frozen) and any
 * later head-union can only widen (a sound superset), never shrink, so
 * a union that fits here is provably representable. Returns -1 when
 * the union exceeds the cap or the pool is full — the caller falls
 * back to UNKNOWN. */
static int chain_def_alloc_union(const struct ChainSet* cur, uint8_t ka, int16_t a, const struct ChainSet* fa,
                                 uint8_t kb, int32_t b, const struct ChainSet* fb, uint32_t pc) {
    struct ChainBig rb = { .n = 0, .unk = 0 };
    if (ka == CD_REC) {
        chain_flatten_big_rec(&rb, cur, a);
        if (rb.unk) return -1;
    } else {
        if (fa->unk) return -1;
        for (uint8_t i = 0; i < fa->n && !rb.unk; i++) {
            struct ChainSet one;
            chain_singleton(&one, fa->v[i]);
            chain_merge_big(&rb, &one);
        }
    }
    if (kb == CD_REC) {
        struct ChainBig r2;
        chain_flatten_big_rec(&r2, cur, (int16_t)b);
        if (r2.unk) return -1;
        for (uint8_t i = 0; i < r2.n && !rb.unk; i++) {
            struct ChainSet one;
            chain_singleton(&one, r2.v[i]);
            chain_merge_big(&rb, &one);
        }
    } else {
        if (fb->unk) return -1;
        for (uint8_t i = 0; i < fb->n && !rb.unk; i++) {
            struct ChainSet one;
            chain_singleton(&one, fb->v[i]);
            chain_merge_big(&rb, &one);
        }
    }
    if (rb.unk) return -1;
    if (g_chain_ndef >= TX_AR_CHAIN_DEFS) return -1;
    int ri = g_chain_ndef++;
    struct ChainDef* d = &g_chain_defs[ri];
    d->op = CHAIN_OP_UNION; d->ka = CD_FLAT; d->a = ri; d->kb = CD_FLAT; d->b = ri;
    g_chain_def_flat[ri] = rb;
    g_chain_def_pc[ri] = pc;   /* M2.36: the union's value is fixed at this instruction */
    return ri;
}
/* M2.30/M2.32: the dispatch-side materialization — a deferred product
 * is re-computed over its record DAG into the BIG candidate set (capped
 * at TX_AR_CHAIN_BIG), because the walk's flat sets cannot hold > 12
 * and the emission gate may still want a 13-64-candidate chain (M2.40
 * raised the cap from 32). Sound only because every write or union of
 * a source slot flattened every
 * record that transitively reads it eagerly, so the slots referenced
 * here are exactly what each product saw at its instruction. */
/* M2.33: resolve one record operand into a BIG flat set: an immediate
 * (a singleton), a tracked slot (recursing if that slot is itself
 * deferred), or an older record. */
static void chain_flatten_big_op(struct ChainBig* out, const struct ChainSet* cur, uint8_t k, int32_t v) {
    if (k == CD_IMM) { out->n = 1; out->unk = 0; out->v[0] = (uint32_t)v; }
    else if (k == CD_SLOT) {
        if (cur[v].def >= 0) chain_flatten_big(out, cur, (int16_t)v);
        else { out->n = cur[v].n; out->unk = cur[v].unk; for (uint8_t i = 0; i < out->n; i++) out->v[i] = cur[v].v[i]; }
    } else if (k == CD_FLAT) {
        *out = g_chain_def_flat[v];   /* M2.41: the frozen store is already BIG-shaped */
    } else chain_flatten_big_rec(out, cur, (int16_t)v);
}
static void chain_flatten_big_rec(struct ChainBig* out, const struct ChainSet* cur, int16_t rec) {
    const struct ChainDef* d = &g_chain_defs[rec];
    if (d->op == CHAIN_OP_UNION) {
        /* M2.37/M2.41: the union record's true set is PRE-MERGED and
         * frozen in its store — materializing is a single store copy. */
        *out = g_chain_def_flat[rec];
        return;
    }
    struct ChainBig tmp = { .n = 0, .unk = 0 };
    struct ChainBig va, vb;
    chain_flatten_big_op(&va, cur, d->ka, d->a);
    chain_flatten_big_op(&vb, cur, d->kb, d->b);
    if (va.unk || vb.unk) { chain_unknown_big(&tmp); *out = tmp; return; }
    for (uint8_t i = 0; i < va.n && !tmp.unk; i++)
        for (uint8_t j = 0; j < vb.n && !tmp.unk; j++) {
            struct ChainSet one;
            chain_singleton(&one, (uint32_t)chain_alu_eval(d->op, (int64_t)va.v[i], (int64_t)vb.v[j]));
            chain_merge_big(&tmp, &one);
        }
    *out = tmp;
}
static void chain_flatten_big(struct ChainBig* out, const struct ChainSet* cur, int16_t slot) {
    const struct ChainSet* s = &cur[slot];
    struct ChainBig tmp = { .n = 0, .unk = 0 };
    if (s->def < 0) {
        /* M2.54: the flat set is stored at TX_AR_CHAIN_MAX width but the
         * BIG store (and this tmp) is TX_AR_CHAIN_BIG. The shipped caps
         * are equal, so n <= BIG holds and this is a plain copy; a cap
         * raise that makes MAX > BIG (the M2.54 control) would OVERFLOW
         * tmp.v — collapse to UNKNOWN instead, the exact-or-conservative
         * discipline (a truncated candidate set could UDF-fault at
         * runtime; UNKNOWN always falls to the table). */
        if (s->unk || s->n > TX_AR_CHAIN_BIG) { chain_unknown_big(&tmp); *out = tmp; return; }
        tmp.n = s->n; tmp.unk = s->unk;
        for (uint8_t i = 0; i < s->n; i++) tmp.v[i] = s->v[i];
        *out = tmp; return;
    }
    chain_flatten_big_rec(out, cur, s->def);
}
/* M2.35: is every value of the flat set f contained in the true set of
 * the deferred form on slot (materialized BIG over cur)? Sound because
 * the caller's record is LIVE — chain_prewrite flattens a deferred
 * form on any write to its DAG — so its CD_SLOT operands still read
 * exactly the values the product saw. Both sets are sorted; walk them
 * together. An unknown or EMPTY f returns 0: unknown cannot be
 * tested, and an empty arrival is a no-op union that must keep the
 * M2.34 behavior (the deferred carry still collapses on the first,
 * delivery-less iteration, then the branch deliveries re-establish
 * it). */
static int chain_flat_in_def(const struct ChainSet* cur, int16_t slot, const struct ChainSet* f) {
    if (f->unk || f->n == 0) return 0;
    struct ChainBig rb;
    chain_flatten_big(&rb, cur, slot);
    if (rb.unk) return 0;
    uint8_t i = 0, j = 0;
    while (i < f->n && j < rb.n) {
        if (f->v[i] == rb.v[j]) { i++; j++; }
        else if (f->v[i] < rb.v[j]) return 0;
        else j++;
    }
    return i == f->n;
}
/* M2.36: is the deferred record `rec` still LIVE at the current head —
 * was no tracked slot in its transitive DAG WRITTEN after the record's
 * creation pc? This is what makes materializing a deferred ARRIVAL
 * sound: a write REPLACES a slot's set, so a written DAG leaf would
 * materialize a WRONG set (missing true values -> the chain could
 * UDF-fault). Head-unions are deliberately NOT writes: a union only
 * ever widens a slot to a SUPERSET of what the record saw (it includes
 * the arrival path's own delivery), so materializing over a widened
 * leaf is a sound over-approximation. */
static int chain_def_live(int16_t rec) {
    const struct ChainDef* d = &g_chain_defs[rec];
    if ((d->ka == CD_SLOT && g_chain_last_write[d->a] > (int32_t)g_chain_def_pc[rec]) ||
        (d->kb == CD_SLOT && g_chain_last_write[d->b] > (int32_t)g_chain_def_pc[rec])) return 0;
    if (d->ka == CD_REC && !chain_def_live(d->a)) return 0;
    if (d->kb == CD_REC && !chain_def_live(d->b)) return 0;
    return 1;
}
/* M2.36: is the flat set f contained in the true set of the deferred
 * record `rec` — a def INDEX, not necessarily in cur[] (the
 * deferred-ARRIVAL case)? Materialize the record BIG directly over cur
 * (only sound when chain_def_live(rec) held) and check containment. */
static int chain_flat_in_def_idx(const struct ChainSet* cur, int16_t rec, const struct ChainSet* f) {
    if (f->unk || f->n == 0) return 0;
    struct ChainBig rb;
    chain_flatten_big_rec(&rb, cur, rec);
    if (rb.unk) return 0;
    uint8_t i = 0, j = 0;
    while (i < f->n && j < rb.n) {
        if (f->v[i] == rb.v[j]) { i++; j++; }
        else if (f->v[i] < rb.v[j]) return 0;
        else j++;
    }
    return i == f->n;
}
/* M2.30/M2.34: deliver cur[s] into a per-head accumulator. A DEFERRED
 * arrival is never flattened into the union — every record's true set
 * exceeds the flat cap (it was created by an overflow), so its
 * contribution is UNKNOWN unless the accumulator already holds the SAME
 * record, in which case the union is a no-op (every path carries the
 * same product — the join can keep a deferred source). A flat arrival
 * into a deferred accumulator also collapses to UNKNOWN (the record's
 * > 12 values dominate). Never flattening a delivered record sidesteps
 * the stale-slot trap: a record delivered early and invalidated by a
 * later slot write can no longer be materialized correctly, so it is
 * conservatively UNKNOWN instead of a wrong flat set. */
static void chain_deliver(struct ChainSet* dst, const struct ChainSet* cur, int s) {
    if (cur[s].def >= 0) {
        if (dst->def == cur[s].def) return;               /* same record: union is a no-op */
        if (dst->n == 0 && !dst->unk && dst->def < 0) { *dst = *cur; return; }   /* first arrival keeps the deferred form */
        chain_unknown(dst);                               /* record vs anything else: > 12, UNKNOWN */
    } else {
        if (dst->def >= 0) chain_unknown(dst);            /* flat arrival into a deferred accumulator: UNKNOWN */
        chain_merge(dst, &cur[s]);
    }
}
/* M2.30/M2.32: a deferred product's sources must not change — its value
 * is fixed at its instruction. Before slot s is written or unioned,
 * flatten every deferred form whose record DAG TRANSITIVELY reads s — a
 * nested deferral reads its source's source, so a write to a leaf would
 * otherwise leave the nested form deferring over a RECOMPUTED (new)
 * leaf value while its own runtime value is fixed at its instruction.
 * Each flatten reads the OLD cur[] (the write has not happened yet). */
static void chain_prewrite(struct ChainSet* cur, int ntr, int s) {
    uint8_t dirty[TX_AR_CHAIN_REGS];
    for (int t = 0; t < ntr; t++) dirty[t] = 0;
    dirty[s] = 1;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int t = 0; t < ntr; t++)
            if (!dirty[t] && cur[t].def >= 0 && chain_def_touches(cur[t].def, dirty)) { dirty[t] = 1; changed = 1; }
    }
    for (int t = 0; t < ntr; t++)
        if (dirty[t] && t != s && cur[t].def >= 0)
            chain_flatten(&cur[t], cur, t);
}
/* M2.27: the ALU set image — S(out) = { f(a, k) : a in S(a) } for the
 * immediate form, or { f(a, b) : a in S(a), b in S(b) } for the register
 * form. f is ADD/SUB/MUL (M2.27), AND/OR/XOR (M2.28), SHL/SHR/SAR
 * (M2.47, amount masked mod 64) and — via the immediate path with a
 * dummy imm — the unary NOT/NEG (M2.48, bv ignored). All plain 64-bit
 * ops that never fault and ignore the declared type (the same argument
 * as the M2.2 constant fold), so the image is bit-identical to
 * runtime. The merge caps and collapses to UNKNOWN exactly like the
 * unions. */
static void chain_img_alu(struct ChainSet* out, uint8_t op, int use_imm, int32_t imm,
                          const struct ChainSet* a, const struct ChainSet* b) {
    struct ChainSet tmp = { .n = 0, .unk = 0 };
    if (a->unk || (!use_imm && b->unk)) { chain_unknown(&tmp); *out = tmp; return; }
    for (uint8_t i = 0; i < a->n && !tmp.unk; i++) {
        int64_t av = (int64_t)a->v[i];
        if (use_imm) {
            int64_t r = chain_alu_eval(op, av, (int64_t)imm);
            struct ChainSet one;
            chain_singleton(&one, (uint32_t)r);
            chain_merge(&tmp, &one);
        } else for (uint8_t j = 0; j < b->n && !tmp.unk; j++) {
            int64_t r = chain_alu_eval(op, av, (int64_t)b->v[j]);
            struct ChainSet one;
            chain_singleton(&one, (uint32_t)r);
            chain_merge(&tmp, &one);
        }
    }
    *out = tmp;
}
/* w_ra is a register source operand (feeds rd when rd is tracked). */
static int ar_has_reg_ra(uint8_t op) {
    switch (op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR: case OP_SAR:
    case OP_NOT: case OP_NEG: case OP_MOV: case OP_CMP:
    case OP_LEA: case OP_PTRADD: case OP_LOAD: case OP_STORE:
    case OP_BC: case OP_JMPR:
        return 1;
    default: return 0;
    }
}
/* w_rb_reg is a register source (register-form binary ALU, no FLAG_IMM). */
static int ar_has_reg_rb(uint8_t op) {
    switch (op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR: case OP_SAR:
    case OP_CMP:
        return 1;
    default: return 0;
    }
}
/* rd is a WRITTEN destination (STORE's rd is the value source, not a
 * destination; the transfers write nothing). */
static int ar_writes_rd(uint8_t op) {
    switch (op) {
    case OP_STORE: case OP_BR: case OP_BC: case OP_JMPR:
    case OP_RET: case OP_ENTER: case OP_CALL:
        return 0;
    default: return 1;
    }
}

/* ─── CMP: synthesize all 10 relations from cmp+cset (§4) ────────────────
 * Operands in rn (lhs), rm (rhs); result (0/1) ends up in `rd` (M1: the
 * reserved result register, which may alias an operand register — A64
 * reads all operands before writing). `cmp xN, xM` (subs xzr, xN, xM)
 * writes NZCV; `cset xd, cond` reads it. The A64 condition code for each
 * SIMI relation: EQ=0 NE=1 HS=2 LO=3 HI=8 LS=9 GE=10 LT=11 GT=12 LE=13.
 * M2.5: rn/rm are the operand hosts chosen by cache_fetch_hosts, so a
 * crossed residency (ra@1, rb@0) swaps them without disturbing the
 * relation's operand order — subs xzr, x10, x9 with the same semantics
 * as subs xzr, x9, x10. */
static int emit_cmp(struct CodeBuf* cb, int rel, uint8_t rd, uint8_t rn, uint8_t rm) {
    e32(cb, enc_subs_shift(31, rn, rm, 0, 0));   /* cmp rn, rm */
    switch (rel) {
        case REL_EQ:  e32(cb, enc_cset(rd, 0)); break;   /* eq */
        case REL_NE:  e32(cb, enc_cset(rd, 1)); break;   /* ne */
        case REL_LT:  e32(cb, enc_cset(rd, 11)); break;  /* lt */
        case REL_GT:  e32(cb, enc_cset(rd, 12)); break;  /* gt */
        case REL_LE:  e32(cb, enc_cset(rd, 13)); break;  /* le */
        case REL_GE:  e32(cb, enc_cset(rd, 10)); break;  /* ge */
        case REL_LTU: e32(cb, enc_cset(rd, 3)); break;   /* lo (cc) */
        case REL_GTU: e32(cb, enc_cset(rd, 8)); break;   /* hi */
        case REL_LEU: e32(cb, enc_cset(rd, 9)); break;   /* ls */
        case REL_GEU: e32(cb, enc_cset(rd, 2)); break;   /* hs (cs) */
        default: return 0;
    }
    return 1;
}

/* ─── LOAD/STORE width+signedness (mirrors type_width() in simi_x86.c) —
 * A64 has a native load/store opcode for every SIMI width/signedness
 * combination (opc 01 zero-extends, opc 10 sign-extends, 32-bit rt
 * zeroes the upper half), so like RV64 the mapping is 1:1 with no x86
 * movsx/movzx zoo. */
/* M1: imm12 is the scaled unsigned offset — 0 in naive mode, the folded
 * displacement when it fits (an M1 size win for pointer-heavy code). */
static void load_typed(struct CodeBuf* cb, int type, uint8_t rd, uint8_t rn, uint16_t imm12) {
    switch (type) {
        case T_I8:  e32(cb, enc_ldrsb(rd, rn, imm12)); break;
        case T_U8:  case T_BOOL: e32(cb, enc_ldrb(rd, rn, imm12)); break;
        case T_I16: e32(cb, enc_ldrsh(rd, rn, imm12)); break;
        case T_U16: e32(cb, enc_ldrh(rd, rn, imm12)); break;
        case T_I32: e32(cb, enc_ldrsw(rd, rn, imm12)); break;
        case T_F32: e32(cb, enc_ldr_w(rd, rn, imm12)); break;  /* F4: f32 loads zero-extend (the interpreter's raw-bits convention) — ldrsw would sign-extend negative floats */
        case T_U32: e32(cb, enc_ldr_w(rd, rn, imm12)); break;
        default:    e32(cb, enc_ldr(rd, rn, imm12)); break;   /* i64/u64/f64/ptr */
    }
}
static void store_typed(struct CodeBuf* cb, int type, uint8_t rs, uint8_t rn, uint16_t imm12) {
    switch (type) {
        case T_I8: case T_U8: case T_BOOL: e32(cb, enc_strb(rs, rn, imm12)); break;
        case T_I16: case T_U16:            e32(cb, enc_strh(rs, rn, imm12)); break;
        case T_I32: case T_U32: case T_F32: e32(cb, enc_str_w(rs, rn, imm12)); break;
        default: e32(cb, enc_str(rs, rn, imm12)); break;
    }
}
/* M2.10: the same width/signedness table as load_typed/store_typed, in
 * the UNSCALED form — the access's imm9 IS the displacement (checked
 * by the caller to fit -256..255), so there is no separate address-math
 * word and no separate zero-offset access: the whole thing is one
 * word. No writeback, so the base host register is untouched. */
static void load_unscaled(struct CodeBuf* cb, int type, uint8_t rd, uint8_t rn, int16_t imm9) {
    switch (type) {
        case T_I8:  e32(cb, enc_ldursb(rd, rn, imm9)); break;
        case T_U8:  case T_BOOL: e32(cb, enc_ldurb(rd, rn, imm9)); break;
        case T_I16: e32(cb, enc_ldursh(rd, rn, imm9)); break;
        case T_U16: e32(cb, enc_ldurh(rd, rn, imm9)); break;
        case T_I32: e32(cb, enc_ldursw(rd, rn, imm9)); break;
        case T_F32: e32(cb, enc_ldur_w(rd, rn, imm9)); break;  /* F4: f32 loads zero-extend (see load_typed) */
        case T_U32: e32(cb, enc_ldur_w(rd, rn, imm9)); break;
        default:    e32(cb, enc_ldur(rd, rn, imm9)); break;
    }
}
static void store_unscaled(struct CodeBuf* cb, int type, uint8_t rs, uint8_t rn, int16_t imm9) {
    switch (type) {
        case T_I8: case T_U8: case T_BOOL: e32(cb, enc_sturb(rs, rn, imm9)); break;
        case T_I16: case T_U16:            e32(cb, enc_sturh(rs, rn, imm9)); break;
        case T_I32: case T_U32: case T_F32: e32(cb, enc_stur_w(rs, rn, imm9)); break;
        default: e32(cb, enc_stur(rs, rn, imm9)); break;
    }
}
/* M2.11: the same width/signedness table in the REGISTER-offset form
 * (bit 21 = 1, opt/s as below). The 64-bit forms use LSL #0 (Xm in
 * full — a two's-complement 64-bit displacement wraps the address add
 * correctly); every narrower form uses SXTW (the low 32 bits sign-
 * extended), which is how a negative displacement stays negative — LSL
 * #0 would zero-extend Wm into a huge positive offset. The caller
 * guarantees the displacement fits int32 (imm28 range). The register
 * rm holds the displacement (X_DR on a marked run, a scratch otherwise)
 * and is never modified by the access. */
static void load_typed_reg(struct CodeBuf* cb, int type, uint8_t rd, uint8_t rn, uint8_t rm) {
    switch (type) {
        case T_I8:  e32(cb, enc_ldrsb_reg(rd, rn, rm)); break;
        case T_U8:  case T_BOOL: e32(cb, enc_ldrb_reg(rd, rn, rm)); break;
        case T_I16: e32(cb, enc_ldrsh_reg(rd, rn, rm)); break;
        case T_U16: e32(cb, enc_ldrh_reg(rd, rn, rm)); break;
        case T_I32: e32(cb, enc_ldrsw_reg(rd, rn, rm)); break;
        case T_F32: e32(cb, enc_ldr_w_reg(rd, rn, rm)); break;  /* F4: f32 loads zero-extend (see load_typed) */
        case T_U32: e32(cb, enc_ldr_w_reg(rd, rn, rm)); break;
        default:    e32(cb, enc_ldr_reg(rd, rn, rm)); break;
    }
}
static void store_typed_reg(struct CodeBuf* cb, int type, uint8_t rs, uint8_t rn, uint8_t rm) {
    switch (type) {
        case T_I8: case T_U8: case T_BOOL: e32(cb, enc_strb_reg(rs, rn, rm)); break;
        case T_I16: case T_U16:            e32(cb, enc_strh_reg(rs, rn, rm)); break;
        case T_I32: case T_U32: case T_F32: e32(cb, enc_str_w_reg(rs, rn, rm)); break;
        default: e32(cb, enc_str_reg(rs, rn, rm)); break;
    }
}
static int type_shift(int t) {   /* log2(byte width) — used for PTRADD scaling */
    switch (t) {
        case T_I8: case T_U8: case T_BOOL: return 0;
        case T_I16: case T_U16: return 1;
        case T_I32: case T_U32: case T_F32: return 2;
        default: return 3;
    }
}
/* M2.11: does this LOAD/STORE land in the materialize path — i.e. NOT the
 * scaled-fast (1 word), NOT the unscaled imm9 (1 word), NOT the imm12
 * add/sub (2 words)? Only such displacements (> 4095 and not a multiple
 * of 4096, either sign) are worth the register-offset/run-reuse
 * machinery; the class is purely a function of (disp, type) and is
 * identical for every access sharing a displacement, since the scaled
 * and imm12 checks depend on the same magnitude. Consulted by the run
 * pre-pass, which runs only when g_alloc (the codegen's imm9/imm12 folds
 * are g_alloc-gated too, so the classes match exactly). */
static int mem_materialize_class(int32_t disp, int type) {
    int sh = type_shift(type);
    if (disp >= 0 && (disp & ((1 << sh) - 1)) == 0 && (disp >> sh) <= 0xFFF) return 0;
    if (disp >= -256 && disp <= 255) return 0;
    uint64_t mag = (disp < 0) ? (uint64_t)(-(int64_t)disp) : (uint64_t)disp;
    int sh2; uint32_t u;
    if (imm12_split(mag, &sh2, &u)) return 0;
    return 1;
}

/* ─── Procedure prologue/epilogue ─────────────────────────────────────────
 * sub sp,sp,#16; str x30,[sp,#8]; str x29,[sp,#0]; add x29,sp,#16 —
 * standard A64 non-leaf prologue opening, the exact RV64 shape with A64
 * instruction names. x29 (X_FP) now equals the sp value at entry, and sp
 * is reserved down by 576 (512 bytes of register slots + 64 tag bytes).
 * Incoming r0..r7 args are copied in from entry_sp + 8*i = sp + 592 + 8*i
 * — see emit_call_site for why that's where the caller left them. */
#define TX_AR_FRAME_BYTES (TX_AR_MAX_REGS * 8)   /* 512 */
#define TX_AR_TOTAL_FRAME_BYTES (TX_AR_FRAME_BYTES + TX_AR_MAX_REGS)  /* 576 */
/* Arg slots live above entry_sp (the caller's outgoing area): [sp + 592 + 8i]. */
#define TX_AR_ARG_BASE_DISP 592
/* The argument-tag mask byte rides at entry_sp + 64, like RV64 (see the
 * Phase 12 design notes in simi_riscv.c). From the body, sp + 592 + 64. */
#define TX_AR_ARG_MASK_DISP (TX_AR_ARG_BASE_DISP + 64)
#define TX_AR_OUTGOING_BYTES 80   /* call-site/trampoline arg area, 16-aligned (5 x 16) */

static void emit_prologue(struct CodeBuf* cb) {
    e32(cb, enc_sub_imm(X_SP, X_SP, 16));
    e32(cb, enc_str(X_LR, X_SP, 1));
    e32(cb, enc_str(X_FP, X_SP, 0));
    e32(cb, enc_add_imm(X_FP, X_SP, 16));
    e32(cb, enc_sub_imm(X_SP, X_SP, (uint32_t)TX_AR_TOTAL_FRAME_BYTES));
    for (int i = 0; i < TX_AR_MAX_REGS; i++) st_slot(cb, 31, i);  /* str xzr */
    for (int i = 0; i < TX_AR_MAX_REGS; i++) st_untag(cb, i);     /* strb wzr */
    int nargs = TX_AR_MAX_REGS < 8 ? TX_AR_MAX_REGS : 8;
    for (int i = 0; i < nargs; i++) {
        e32(cb, enc_ldr(X_T0, X_SP, TX_AR_ARG_BASE_DISP / 8 + i)); /* 74+i */
        st_slot(cb, X_T0, i);
    }
    /* Apply the caller's argument-tag mask — branch-free per-argument bit
     * test, one reload per arg: t1 = mask; t2 = (mask & (1<<i)) != 0. */
    for (int i = 0; i < nargs; i++) {
        e32(cb, enc_ldrb(X_T1, X_SP, (uint16_t)TX_AR_ARG_MASK_DISP));
        e32(cb, enc_movz(X_T2, (uint16_t)(1u << i), 0));
        e32(cb, enc_and_shift(X_T2, X_T1, X_T2, 0, 0));
        e32(cb, enc_subs_imm(31, X_T2, 0));        /* cmp x11, #0 */
        e32(cb, enc_cset(X_T2, 1));                /* cset x11, ne */
        st_tag(cb, X_T2, i);
    }
}

/* ─── Call-site codegen: marshal r0..r7 into the outgoing-argument area
 * (entry_sp + 8*i), call, then read the result back from t0 (see OP_RET
 * for why it's still live there) into rd's slot. Mirrors simi_x86.c's
 * emit_call_site. Gap Remediation SIMI Phase 12: the outgoing-arg area is
 * 80 bytes (5 x 16, keeping SP 16-aligned where RV64's 72 was not — A64
 * requires alignment) with the 9th byte at [sp+64] carrying the
 * argument-tag mask, and the result propagates a real tag (read from t1,
 * set by the callee's RET) instead of an unconditional untag.
 *
 * SP-relative wrinkle (caught by a64_exec actually executing the output,
 * not by review): the slot/tag helpers address the frame from the BODY sp
 * (entry - 592), but this sequence decrements sp by 80 to open the
 * outgoing-arg area — the loads below must therefore be re-anchored to
 * the decremented sp (entry - 672): slot i sits at sp' + (648 - 8i) and
 * tag i at sp' + (80 + i). The RV64 call site has no such wrinkle because
 * its slots are s0-relative and s0 never moves. The mask is built with a
 * single shifted-ORR per arg (orr x10, x10, x11, lsl #i) — A64's shifted
 * register operands fold the tag's shift into the OR, one instruction
 * where RV64 needs slli+or. */
static void emit_call_site(struct CodeBuf* cb, uint32_t target_pc, int rd) {
    e32(cb, enc_sub_imm(X_SP, X_SP, TX_AR_OUTGOING_BYTES));
    int nargs = TX_AR_MAX_REGS < 8 ? TX_AR_MAX_REGS : 8;
    for (int i = 0; i < nargs; i++) {
        /* slot i from the decremented sp: 648 - 8i -> imm12 81 - i */
        e32(cb, enc_ldr(X_T0, X_SP, 81 - i));
        e32(cb, enc_str(X_T0, X_SP, i));
    }
    for (int i = nargs; i < 8; i++) e32(cb, enc_str(31, X_SP, i));  /* zero pad */
    e32(cb, enc_movz(X_T1, 0, 0));                    /* t1 = mask accumulator */
    for (int i = 0; i < nargs; i++) {
        /* tag i from the decremented sp: 80 + i */
        e32(cb, enc_ldrb(X_T2, X_SP, 80 + i));
        e32(cb, enc_orr_shift(X_T1, X_T1, X_T2, 0, (uint8_t)i));
    }
    e32(cb, enc_strb(X_T1, X_SP, 64));
    op_bl(cb, target_pc);
    e32(cb, enc_add_imm(X_SP, X_SP, TX_AR_OUTGOING_BYTES));
    /* Gap Remediation SIMI Phase 12: propagate r0's tag. M1: the callee's
     * RET left the value in x9 and the tag in x10 — keep the value
     * resident (rule 3: write the tag byte to memory now so it stays
     * authoritative), M0's store+tag in naive mode. */
    if (g_alloc) {
        int v = cache_reserve(cb, rd, -1, -1);   /* call result: no SIMI sources to keep resident */
        clobber_scratch(cb, X_T1);      /* x10 is about to be used for the tag store */
        st_tag(cb, X_T1, rd);           /* tag byte from x10, before the value move */
        uint8_t rh = result_host(v);
        if (rh != X_T0) e32(cb, enc_orr_shift(rh, 31, X_T0, 0, 0));
    } else {
        st_slot(cb, X_T0, rd);
        st_tag(cb, X_T1, rd);
    }
}

/* ─── Trampoline: zero r0..r5, r6=namepool_ptr, r7=scratch_ptr, call
 * entry, return. The `bl` clobbers x30, so the incoming x30 (the
 * verifier's sentinel) is explicitly saved/restored around it — mirrors
 * RV64's trampoline, which must do the same around its `jal`. Ends in
 * `br x30` (the A64 `ret` pseudo-op is exactly that) so this trampoline
 * is a normal callable subroutine. */
static int emit_trampoline(struct CodeBuf* cb, uint32_t target_pc, uint64_t scratch_ptr,
                            uint64_t namepool_ptr) {
    e32(cb, enc_sub_imm(X_SP, X_SP, 16));
    e32(cb, enc_str(X_LR, X_SP, 1));
    e32(cb, enc_str(X_FP, X_SP, 0));
    e32(cb, enc_add_imm(X_FP, X_SP, 16));
    e32(cb, enc_sub_imm(X_SP, X_SP, TX_AR_OUTGOING_BYTES));
    for (int i = 0; i < 6; i++) e32(cb, enc_str(31, X_SP, i));   /* zero r0..r5 */
    emit_li64(cb, X_T0, namepool_ptr);
    e32(cb, enc_str(X_T0, X_SP, 6));
    emit_li64(cb, X_T0, scratch_ptr);
    e32(cb, enc_str(X_T0, X_SP, 7));
    /* Gap Remediation SIMI Phase 12: zero the argument-tag mask too —
     * nothing before "main" starts can hold a real capability. */
    e32(cb, enc_strb(31, X_SP, 64));
    op_bl(cb, target_pc);
    e32(cb, enc_add_imm(X_SP, X_SP, TX_AR_OUTGOING_BYTES));
    e32(cb, enc_ldr(X_LR, X_SP, 1));
    e32(cb, enc_ldr(X_FP, X_SP, 0));
    e32(cb, enc_add_imm(X_SP, X_SP, 16));
    e32(cb, enc_br(X_LR));
    return 1;
}

/* ─── Main per-instruction codegen ────────────────────────────────────── */
static int emit_instr(struct CodeBuf* cb, uint64_t w) {
    uint8_t op = w_op(w), type = w_type(w), flags = w_flags(w);
    uint16_t rd = w_rd(w), ra = w_ra(w);

    if (rd >= TX_AR_MAX_REGS || ra >= TX_AR_MAX_REGS) return TX_AR_ERR_REG_OUT_OF_RANGE;
    if ((op==OP_ADD||op==OP_SUB||op==OP_MUL||op==OP_DIV||op==OP_MOD||op==OP_AND||
         op==OP_OR||op==OP_XOR||op==OP_SHL||op==OP_SHR||op==OP_SAR||op==OP_CMP||
         op==OP_PTRADD||op==OP_CAS||op==OP_ATOMIC_ADD) && !(flags & FLAG_IMM) && w_rb_reg(w) >= TX_AR_MAX_REGS)
        return TX_AR_ERR_REG_OUT_OF_RANGE;
    /* Gap Remediation SIMI Phase 10 (F1): ADD/SUB/MUL/DIV/NEG/CMP now
     * have real IEEE-754 float codegen below (GP-bounce per plan D4,
     * fcmp+cset per D5). Two float shapes stay rejected, matching the
     * interpreter's permanent boundaries and x86's codegen:
     *   - MOD has no float instruction on any target this project ships
     *     (compose DIV+MUL+SUB instead) — permanent, not a temporary gap.
     *   - a float op with an immediate operand has no float meaning
     *     (you cannot add a float to a 28-bit integer immediate); x86
     *     rejects the same combination (simi_x86.c). AND/OR/XOR/SHL/SHR/
     *     SAR are deliberately NOT rejected: they operate on the raw
     *     bits of the slot exactly like the reference interpreter's
     *     plain integer path. */
    if ((type==T_F64 || type==T_F32) && op == OP_MOD)
        return TX_AR_ERR_FLOAT_UNSUPPORTED;
    if ((type==T_F64 || type==T_F32) && (flags & FLAG_IMM) &&
        (op==OP_ADD||op==OP_SUB||op==OP_MUL||op==OP_DIV))
        return TX_AR_ERR_FLOAT_UNSUPPORTED;

    switch (op) {
    case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR:
    case OP_MUL: case OP_SHL: case OP_SHR: case OP_SAR: {
        int v = cache_reserve(cb, rd, ra, (flags & FLAG_IMM) ? -1 : w_rb_reg(w));
        uint8_t rh = result_host(v);
        int rd_live = (rd == ra) || (!(flags & FLAG_IMM) && rd == w_rb_reg(w));
        uint8_t h_a, h_b;
        if (flags & FLAG_IMM) {
            h_a = cache_single_host(ra);
            h_b = (h_a == X_T0) ? X_T1 : X_T0;
            get_operand(cb, ra, h_a, rd_live);
            /* M2.6/M2.7: fold ADD/SUB immediates into A64's add-imm /
             * sub-imm forms — 1 word instead of the movz(+movk)+add_shift
             * materialization, exactly what LEA/PTRADD/LOAD already do
             * for their displacements. The magnitude folds into the
             * plain imm12 form (0..4095) or the imm12<<12 shifted form
             * (multiples of 4096 up to 0xFFFFFF); the direction flips
             * with the sign — ADD #-v == SUB #v and SUB #-v == ADD #v —
             * and the negated-magnitude encodings are bit-identical to
             * the 64-bit wrap materialization (ra + (2^64 - |v|) ==
             * ra - |v|). Gated on g_alloc: the naive (JMPR) path must
             * stay byte-identical to M0 for the gate's jmpr baselines
             * and its "0 saved, the honest floor" rows. Values outside
             * both forms (|imm| past 0xFFFFFF, or a non-multiple of 4096
             * past 4095) keep the materialized path; the non-add/sub ops
             * are untouched (A64's AND/OR/XOR immediates are bitmask
             * encodings, not plain 12-bit). */
            if (g_alloc && (op == OP_ADD || op == OP_SUB)) {
                int64_t imm = w_imm28(w);
                uint64_t mag = (imm < 0) ? (uint64_t)(-imm) : (uint64_t)imm;
                int add_dir = (op == OP_ADD) ^ (imm < 0);   /* 1 = add-imm, 0 = sub-imm */
                int sh; uint32_t u;
                if (imm12_split(mag, &sh, &u)) {
                    e32(cb, add_dir ? enc_add_imm_sh(rh, h_a, u, (uint8_t)sh)
                                    : enc_sub_imm_sh(rh, h_a, u, (uint8_t)sh));
                    store_result(cb, rd);
                    break;   /* fold consumed this instruction — skip the inner switch */
                }
            }
            materialize_imm(cb, (uint64_t)(int64_t)w_imm28(w), h_b);
        } else {
            cache_fetch_hosts(ra, w_rb_reg(w), &h_a, &h_b);
            /* M2.15: at a merged single-edge backward-fold head, the
             * pattern's operand fetches are dead — the source block's
             * tail left the SAME guests in these hosts and the fold's
             * flush does not store transient values (see
             * epi_merge_pass). Skipping the fetch makes the instruction
             * read the runtime register directly; A64 reads all sources
             * before writing the result, so the result host may alias a
             * dropped operand host. */
            uint8_t epi = g_epi_merge[g_cur_pc];
            if (!(epi & 1)) get_operand(cb, ra, h_a, rd_live);
            if (!(epi & 2)) get_operand(cb, w_rb_reg(w), h_b, rd_live);
        }
        /* F1: float ADD/SUB/MUL — the GP-bounce (plan D4). The value
         * never leaves the x9/x10/x11 integer cache: the operand bits
         * move into the FP scratch registers d0/s0 and d1/s1, the
         * IEEE-754 op computes there, and the result bits move back
         * into the reserved cache host. Four words per float op (the
         * documented "two extra moves" cost — actually three), and
         * d0/d1 are never used by the integer codegen, so there is no
         * cross-instruction FP state to manage. The s-form fmovs zero
         * the high 32 bits exactly like the interpreter's bits_of_f32. */
        if ((type == T_F64 || type == T_F32) &&
            (op == OP_ADD || op == OP_SUB || op == OP_MUL)) {
            int d = (type == T_F64);
            e32(cb, d ? enc_fmov_dx(0, h_a) : enc_fmov_sw(0, h_a));
            e32(cb, d ? enc_fmov_dx(1, h_b) : enc_fmov_sw(1, h_b));
            switch (op) {
                case OP_ADD: e32(cb, d ? enc_fadd_d(0, 0, 1) : enc_fadd_s(0, 0, 1)); break;
                case OP_SUB: e32(cb, d ? enc_fsub_d(0, 0, 1) : enc_fsub_s(0, 0, 1)); break;
                default:     e32(cb, d ? enc_fmul_d(0, 0, 1) : enc_fmul_s(0, 0, 1)); break;
            }
            e32(cb, d ? enc_fmov_xd(rh, 0) : enc_fmov_ws(rh, 0));
            store_result(cb, rd);
            break;   /* float consumed this instruction — skip the integer inner switch */
        }
        switch (op) {
            case OP_ADD: e32(cb, enc_add_shift(rh, h_a, h_b, 0, 0)); break;
            case OP_SUB: e32(cb, enc_sub_shift(rh, h_a, h_b, 0, 0)); break;
            case OP_AND: e32(cb, enc_and_shift(rh, h_a, h_b, 0, 0)); break;
            case OP_OR:  e32(cb, enc_orr_shift(rh, h_a, h_b, 0, 0)); break;
            case OP_XOR: e32(cb, enc_eor_shift(rh, h_a, h_b, 0, 0)); break;
            case OP_MUL: e32(cb, enc_madd(rh, h_a, h_b, 31)); break;  /* MUL */
            case OP_SHL: e32(cb, enc_lslv(rh, h_a, h_b)); break;
            case OP_SHR: e32(cb, enc_lsrv(rh, h_a, h_b)); break;
            case OP_SAR: e32(cb, enc_asrv(rh, h_a, h_b)); break;
        }
        store_result(cb, rd);
        break;
    }
    case OP_DIV: case OP_MOD: {
        int v = cache_reserve(cb, rd, ra, (flags & FLAG_IMM) ? -1 : w_rb_reg(w));
        uint8_t rh = result_host(v);
        int rd_live = (rd == ra) || (!(flags & FLAG_IMM) && rd == w_rb_reg(w));
        uint8_t h_a, h_b;
        if (flags & FLAG_IMM) {
            h_a = cache_single_host(ra);
            h_b = (h_a == X_T0) ? X_T1 : X_T0;
            get_operand(cb, ra, h_a, rd_live);
            materialize_imm(cb, (uint64_t)(int64_t)w_imm28(w), h_b);
        } else {
            cache_fetch_hosts(ra, w_rb_reg(w), &h_a, &h_b);
            get_operand(cb, ra, h_a, rd_live);
            get_operand(cb, w_rb_reg(w), h_b, rd_live);
        }
        if (type == T_F64 || type == T_F32) {
            /* F1: float DIV GP-bounce (float MOD was rejected up front). */
            int d = (type == T_F64);
            e32(cb, d ? enc_fmov_dx(0, h_a) : enc_fmov_sw(0, h_a));
            e32(cb, d ? enc_fmov_dx(1, h_b) : enc_fmov_sw(1, h_b));
            e32(cb, d ? enc_fdiv_d(0, 0, 1) : enc_fdiv_s(0, 0, 1));
            e32(cb, d ? enc_fmov_xd(rh, 0) : enc_fmov_ws(rh, 0));
            store_result(cb, rd);
            break;
        }
        int sgn = ar_type_signed(type);
        /* A64 has no remainder instruction: sdiv/udiv then msub folds the
         * quotient back (t = t - (t/d)*d), yielding the dividend-sign
         * remainder RV64's rem/remu produce. M1 keeps the dividend and
         * divisor live in the working registers, so the quotient uses x0
         * (dead between hostfn calls — every such call flushes first)
         * instead of x11, which may be the reserved result register. */
        if (op == OP_DIV) {
            e32(cb, sgn ? enc_sdiv(rh, h_a, h_b) : enc_udiv(rh, h_a, h_b));
        } else {
            e32(cb, sgn ? enc_sdiv(X_ARG, h_a, h_b) : enc_udiv(X_ARG, h_a, h_b));
            e32(cb, enc_msub(rh, X_ARG, h_b, h_a));
        }
        store_result(cb, rd);
        break;
    }
    case OP_NOT:
    case OP_NEG: {
        /* F1: float NEG is a sign-bit flip through the integer cache
         * (plan D3 — deliberately NO float instruction; F0 proved this
         * exact XOR path bit-for-bit). The mask always lands in X_T1:
         * the operand sits in X_T0, and when X_T1 IS the reserved
         * result register the eor reads both sources before writing,
         * so mask-then-eor is correct there too. The 64-bit mask
         * flips bit 63; the 32-bit mask flips bit 31 and leaves the
         * zero-extended upper half zero, matching bits_of_f32. */
        if (type == T_F64 || type == T_F32) {
            int v = cache_reserve(cb, rd, ra, -1);
            uint8_t rh = result_host(v);
            get_operand(cb, ra, X_T0, rd == ra);
            uint64_t sign = (type == T_F64) ? 0x8000000000000000ull : 0x80000000ull;
            materialize_imm(cb, sign, X_T1);
            e32(cb, enc_eor_shift(rh, X_T0, X_T1, 0, 0));
            store_result(cb, rd);
            break;
        }
        int v = cache_reserve(cb, rd, ra, -1);
        uint8_t rh = result_host(v);
        get_operand(cb, ra, X_T0, rd == ra);
        if (op == OP_NOT) e32(cb, enc_orn(rh, X_T0));              /* mvn rh, x9 */
        else              e32(cb, enc_sub_shift(rh, 31, X_T0, 0, 0));  /* sub rh, xzr, x9 */
        store_result(cb, rd);
        break;
    }
    case OP_MOV: {
        /* v0.3 (Phase 7): the one opcode besides RESOLVE that can produce
         * a tagged register — propagating an existing capability is still
         * a valid capability. The tag byte is written to memory
         * immediately (rule 3), so the propagated value's tag is
         * authoritative wherever it came from. */
        if (g_alloc) {
            int i0 = cache_find(ra);        /* source residency BEFORE reserve */
            int v = cache_reserve(cb, rd, ra, -1);
            uint8_t rh = result_host(v);
            /* Subtle: since M2.4 the reserve scan never picks ra's slot
             * as a victim, so i0 == v now arises only for MOV rd,rd via
             * the reuse path — the copy below is skipped, and the
             * register still physically holds ra (a store cannot modify
             * a register). */
            if (i0 >= 0) {
                if (cache_host(i0) != rh) e32(cb, enc_orr_shift(rh, 31, cache_host(i0), 0, 0));
            } else {
                ld_slot(cb, rh, ra);
            }
            uint8_t sc = (g_resv_slot == 2) ? X_T1 : X_T2;
            clobber_scratch(cb, sc);
            ld_tag(cb, sc, ra);
            st_tag(cb, sc, rd);
        } else {
            ld_slot(cb, X_T0, ra);
            st_slot(cb, X_T0, rd);
            ld_tag(cb, X_T1, ra);
            st_tag(cb, X_T1, rd);
        }
        break;
    }
    case OP_LOADI: {
        int v = cache_reserve(cb, rd, -1, -1);
        emit_li64(cb, result_host(v), (uint64_t)(int64_t)w_imm28(w));
        store_result(cb, rd);
        break;
    }
    case OP_LOADI64:
        return TX_AR_ERR_BAD_OPCODE; /* unreachable: handled specially in translate(), needs literal pool value */
    case OP_CMP: {
        int v = cache_reserve(cb, rd, ra, w_rb_reg(w));   /* CMP is always register form */
        uint8_t rh = result_host(v);
        int rd_live = (rd == ra) || (rd == w_rb_reg(w));
        uint8_t h_a, h_b;
        cache_fetch_hosts(ra, w_rb_reg(w), &h_a, &h_b);
        get_operand(cb, ra, h_a, rd_live);
        get_operand(cb, w_rb_reg(w), h_b, rd_live);
        if (type == T_F64 || type == T_F32) {
            /* F1/M3: float CMP — fcmp + cset under the REAL unordered
             * model (N=0, Z=0, C=1, V=1, pinned empirically on real A64
             * by M3): EQ/NE/GT/GE are the naive cset eq/ne/gt/ge, all
             * IEEE-correct with NO operand swap (Z and N==V are both
             * false on unordered); LT/LE need cset mi (N) and cset ls
             * (!C || Z) — the naive lt/le read N!=V / Z||N!=V, both TRUE
             * on unordered, the classic A64 NaN gotcha. The pre-M3 D5
             * swapped-operand trick (GT/GE via fcmp (b,a) + cset lt/le)
             * was tuned to the wrong model and is gone. The unsigned
             * relations have no float meaning (assembler-level, same as
             * the interpreter). */
            int d = (type == T_F64);
            int cond;
            switch (flags) {
                case REL_EQ:       cond = 0;  break;   /* eq */
                case REL_NE:       cond = 1;  break;   /* ne */
                case REL_LT:       cond = 4;  break;   /* mi (N) — lt is N!=V, true on unordered */
                case REL_LE:       cond = 9;  break;   /* ls (!C||Z) — le is Z||N!=V, true on unordered */
                case REL_GT:       cond = 12; break;   /* gt */
                case REL_GE:       cond = 10; break;   /* ge */
                default: return TX_AR_ERR_BAD_OPCODE;  /* LTU..GEU: no float meaning */
            }
            e32(cb, d ? enc_fmov_dx(0, h_a) : enc_fmov_sw(0, h_a));
            e32(cb, d ? enc_fmov_dx(1, h_b) : enc_fmov_sw(1, h_b));
            e32(cb, d ? enc_fcmp_d(0, 1) : enc_fcmp_s(0, 1));
            e32(cb, enc_cset(rh, cond));
            store_result(cb, rd);
            break;
        }
        if (flags >= 10 || !emit_cmp(cb, flags, rh, h_a, h_b)) return TX_AR_ERR_BAD_OPCODE;
        store_result(cb, rd);
        break;
    }
    case OP_BR: return TX_AR_ERR_BAD_OPCODE;  /* handled specially in translate() (needs pc) */
    case OP_LEA: {
        int v = cache_reserve(cb, rd, ra, -1);
        uint8_t rh = result_host(v);
        uint8_t h_a = cache_single_host(ra);
        uint8_t h_imm = (h_a == X_T0) ? X_T1 : X_T0;
        get_operand(cb, ra, h_a, rd == ra);
        int32_t disp = w_imm28(w);
        if (disp >= 0 && disp <= 4095) {
            e32(cb, enc_add_imm(rh, h_a, (uint32_t)disp));
        } else {
            uint8_t sc = materialize_imm(cb, (uint64_t)(int64_t)disp, h_imm);
            e32(cb, enc_add_shift(rh, h_a, sc, 0, 0));
        }
        store_result(cb, rd);
        break;
    }
    case OP_PTRADD: {
        /* v0.3 (Phase 7): always untagged — pointer arithmetic must never
         * yield a capability even when rA is currently tagged. */
        int v = cache_reserve(cb, rd, ra, (flags & FLAG_IMM) ? -1 : w_rb_reg(w));
        uint8_t rh = result_host(v);
        int rd_live = (rd == ra) || (!(flags & FLAG_IMM) && rd == w_rb_reg(w));
        int shift = type_shift(type);
        if (flags & FLAG_IMM) {
            uint8_t h_a = cache_single_host(ra);
            uint8_t h_imm = (h_a == X_T0) ? X_T1 : X_T0;
            get_operand(cb, ra, h_a, rd_live);   /* base */
            int64_t scaled = (int64_t)w_imm28(w) << shift;
            if (scaled >= 0 && scaled <= 4095) {
                e32(cb, enc_add_imm(rh, h_a, (uint32_t)scaled));
            } else {
                uint8_t sc = materialize_imm(cb, (uint64_t)scaled, h_imm);
                e32(cb, enc_add_shift(rh, h_a, sc, 0, 0));
            }
        } else {
            uint8_t h_a, h_b;
            cache_fetch_hosts(ra, w_rb_reg(w), &h_a, &h_b);
            get_operand(cb, ra, h_a, rd_live);   /* base */
            get_operand(cb, w_rb_reg(w), h_b, rd_live);
            /* add xd, xA, xB, lsl #shift — A64's shifted register operand
             * folds the scale into the add, one instruction where RV64
             * needs a separate slli. */
            e32(cb, enc_add_shift(rh, h_a, h_b, 0, (uint8_t)shift));
        }
        store_result(cb, rd);
        break;
    }
    case OP_LOAD: {
        int v = cache_reserve(cb, rd, ra, -1);
        uint8_t rh = result_host(v);
        uint8_t h_a = cache_single_host(ra);
        uint8_t h_imm = (h_a == X_T0) ? X_T1 : X_T0;
        get_operand(cb, ra, h_a, rd == ra);   /* base */
        int32_t disp = w_imm28(w);
        int sh = type_shift(type);
        if (disp >= 0 && (disp & ((1 << sh) - 1)) == 0 && (disp >> sh) <= 0xFFF) {
            /* the offset folds into the scaled immediate — no address math */
            load_typed(cb, type, rh, h_a, (uint16_t)(disp >> sh));
        } else {
            /* M2.8: address math. Fold |disp| into a single add/sub-imm
             * (plain or shifted) when it fits — negative displacements
             * previously fell to the materialize path even at
             * |disp| <= 4095 — else materialize. Gated on g_alloc like
             * the ALU fold: the naive JMPR path stays byte-identical to
             * M0. */
            uint64_t mag = (disp < 0) ? (uint64_t)(-(int64_t)disp) : (uint64_t)disp;
            int sh2; uint32_t u;
            /* M2.10: unscaled ldur/stur fold — ANY displacement that
             * fits A64's signed 9-bit imm9 ([-256, 255]) folds into a
             * single ldr/str xt, [xb, #imm] word, with NO writeback and
             * NO alignment requirement (the M2.9 pre-indexed form needed
             * both — unaligned displacements like #-45 fell to the M2.8
             * address math). Nothing modifies h_a here, so this branch
             * skips the clobber the add/sub paths need: if the base is
             * resident at this slot its directory entry stays valid and
             * a later fetch of ra can reuse the register. The unscaled
             * word IS the access, so the trailing load_typed is
             * skipped. */
            if (g_alloc && disp >= -256 && disp <= 255) {
                load_unscaled(cb, type, rh, h_a, (int16_t)disp);
            } else {
                if (g_alloc && imm12_split(mag, &sh2, &u)) {
                    /* The add/sub below modifies h_a IN PLACE — the clobber
                     * is load-bearing (M2.8): without it, a base resident at
                     * this slot would leave a stale directory entry claiming
                     * a register that now holds base±disp, poisoning later
                     * fetches of ra. */
                    clobber_scratch(cb, h_a);
                    e32(cb, (disp >= 0) ? enc_add_imm_sh(h_a, h_a, u, (uint8_t)sh2)
                                        : enc_sub_imm_sh(h_a, h_a, u, (uint8_t)sh2));
                    load_typed(cb, type, rh, h_a, 0);
                } else {
                    /* M2.11: register-offset access. The displacement lives
                     * in a register and the access is ONE word — the M2.8
                     * materialize + add_shift + zero-offset access was two
                     * words of address math on top of the materialize. The
                     * register is X_DR (x12) when this pc is inside a marked
                     * run (the materialize happened once at the run head);
                     * otherwise a scratch (h_imm, a cache host, which
                     * materialize_imm's clobber makes safe). Nothing
                     * modifies h_a, so no clobber here and the base's
                     * directory entry stays valid (a resident base stays
                     * resident). */
                    uint8_t sc;
                    if (g_alloc && g_run_cont[g_cur_pc]) sc = X_DR;
                    else if (g_alloc && g_run_first[g_cur_pc]) sc = materialize_imm(cb, (uint64_t)(int64_t)disp, X_DR);
                    else sc = materialize_imm(cb, (uint64_t)(int64_t)disp, h_imm);
                    load_typed_reg(cb, type, rh, h_a, sc);
                }
            }
        }
        store_result(cb, rd);
        break;
    }
    case OP_STORE: {
        /* No reservation in STORE (g_resv_slot == -1), so the rd_live
         * hint is inert — passed as 0 for the signature. M2.5: the base
         * and value registers are two sources like any ALU's, so the
         * fetch hosts swap when they are crossed (base@1, value@0). */
        uint8_t h_base, h_val;
        cache_fetch_hosts(ra, rd, &h_base, &h_val);
        get_operand(cb, ra, h_base, 0);   /* base */
        get_operand(cb, rd, h_val, 0);    /* rd holds the *source* value register, same convention as x86 */
        int32_t disp = w_imm28(w);
        int sh = type_shift(type);
        if (disp >= 0 && (disp & ((1 << sh) - 1)) == 0 && (disp >> sh) <= 0xFFF) {
            store_typed(cb, type, h_val, h_base, (uint16_t)(disp >> sh));
        } else {
            /* M2.10: unscaled ldur/stur fold — same imm9 window as
             * LOAD's, on the base register, and equally writeback-free:
             * the emitted rt (h_val) and rn (h_base) are always distinct
             * hosts (cache_fetch_hosts returns x9/x10), so the store
             * value register is untouched even when the guest value
             * register aliases the base (STORE r, r, #disp). The
             * unscaled word IS the access, so the trailing store_typed
             * is skipped. */
            uint64_t mag = (disp < 0) ? (uint64_t)(-(int64_t)disp) : (uint64_t)disp;
            int sh2; uint32_t u;
            if (g_alloc && disp >= -256 && disp <= 255) {
                store_unscaled(cb, type, h_val, h_base, (int16_t)disp);
            } else {
                if (g_alloc && imm12_split(mag, &sh2, &u)) {
                    /* The add/sub below modifies h_base IN PLACE — the
                     * clobber_scratch(h_base) is load-bearing exactly as in
                     * LOAD's address math (M2.8): the in-place add/sub would
                     * leave a stale resident directory entry claiming a
                     * register that now holds base±disp. */
                    clobber_scratch(cb, h_base);
                    e32(cb, (disp >= 0) ? enc_add_imm_sh(h_base, h_base, u, (uint8_t)sh2)
                                        : enc_sub_imm_sh(h_base, h_base, u, (uint8_t)sh2));
                    store_typed(cb, type, h_val, h_base, 0);
                } else {
                    /* M2.11: register-offset access — same shape as LOAD's,
                     * on the base register. The store value (h_val) and base
                     * (h_base) hosts are distinct (x9/x10); the displacement
                     * register is X_DR on a marked run or X_T2 otherwise
                     * (clobbered first — X_T2's slot may hold a resident
                     * guest value). Nothing modifies h_base. */
                    uint8_t sc;
                    if (g_alloc && g_run_cont[g_cur_pc]) sc = X_DR;
                    else if (g_alloc && g_run_first[g_cur_pc]) sc = materialize_imm(cb, (uint64_t)(int64_t)disp, X_DR);
                    else { clobber_scratch(cb, X_T2); sc = materialize_imm(cb, (uint64_t)(int64_t)disp, X_T2); }
                    store_typed_reg(cb, type, h_val, h_base, sc);
                }
            }
        }
        break;
    }
    case OP_ENTER: cache_flush(cb); emit_prologue(cb); break;
    case OP_LEAVE: /* no-op directive, matches Phase 1 interpreter */ break;
    case OP_RESOLVE: {
        /* M1: the runtime call clobbers x0 and reads the name-pool arg from
         * its slot — the cache must be empty first (rule 1). */
        cache_flush(cb);
        /* rb_raw holds the name-pool index (FMT_RESOLVE, simi_isa.h). x0 =
         * namepool_ptr(r6) + idx*TX_AR_NAME_SIZE, a raw pointer straight
         * into the object's own name-pool bytes — no copy, mirrors x86.
         *
         * v0.3 (Phase 7): tag the destination iff the resolve succeeded
         * (result != 0). `cmp x0, #0; cset x10, ne` is A64's analog of
         * RV64's sltu-based "a0 != 0" — no branch needed. */
        uint32_t idx = w_rb_raw(w);
        if (idx >= g_num_names) return TX_AR_ERR_NAME_OUT_OF_RANGE;
        ld_slot(cb, X_ARG, TX_AR_NAMEPOOL_ARG_IDX);
        int32_t disp = (int32_t)(idx * TX_AR_NAME_SIZE);
        if (disp >= 0 && disp <= 4095) e32(cb, enc_add_imm(X_ARG, X_ARG, (uint32_t)disp));
        else { emit_li64(cb, X_T2, (uint64_t)(int64_t)disp); e32(cb, enc_add_shift(X_ARG, X_ARG, X_T2, 0, 0)); }
        emit_li64(cb, X_T2, g_rt_resolve_fn);
        e32(cb, enc_blr(X_T2));
        e32(cb, enc_subs_imm(31, X_ARG, 0));   /* cmp x0, #0 */
        e32(cb, enc_cset(X_T1, 1));            /* cset x10, ne — tag = (x0 != 0) */
        st_slot(cb, X_ARG, rd);                /* tagged store */
        st_tag(cb, X_T1, rd);
        break;
    }
    case OP_OBJSIZE: case OP_OBJTYPE: {
        /* M1: the runtime call clobbers x0 and the tag/value reads go
         * straight to memory, which must be current first (rule 1/3). */
        cache_flush(cb);
        /* v0.3 (Phase 7): require rA to currently carry a valid capability
         * tag before ever consulting the runtime catalog — an untagged
         * operand is rejected with the same sentinel used for "no such
         * object," and the runtime function is never called at all. This
         * is what closes the forgery hole (see tests/cap_forge.simi). */
        uint64_t fn = (op == OP_OBJSIZE) ? g_rt_objsize_fn : g_rt_objtype_fn;
        uint64_t invalid_sentinel = (op == OP_OBJSIZE) ? 0ull : 0xFFFFFFFFull;
        ld_tag(cb, X_T1, ra);
        uint32_t cbz_pos = emit_cbz_placeholder(cb);   /* cbz t1, .invalid */
        ld_slot(cb, X_ARG, ra);                              /* T_OBJREF value */
        emit_li64(cb, X_T2, fn);
        e32(cb, enc_blr(X_T2));
        uint32_t b_pos = emit_b_placeholder(cb);             /* b .done */
        patch_local_cbz(cb, cbz_pos, X_T1);                  /* .invalid: */
        emit_li64(cb, X_ARG, invalid_sentinel);
        patch_local_b(cb, b_pos);                            /* .done: */
        st_slot_untag(cb, X_ARG, rd);
        break;
    }
    case OP_RET:
        cache_flush(cb);         /* M1: r0 (and everything else) must be in slots — the call site reads them */
        /* M2.14 TAIL REUSE. The sequence below (ld_slot t0,0; ld_tag
         * t1,0; add sp,#frame; ldr x30; ldr x29; add sp,#16; br x30) is
         * byte-identical for EVERY RET in the function — the flush above
         * emptied x9/x10/x11 (so the tail's use of t0/t1 is
         * cache-independent) and none of it depends on the pc or the
         * resident set. The first RET emits it in place and records the
         * tail's first word in g_tail_ret_off; every later RET emits
         * just `b tail` (a backward branch — the owner is earlier in
         * layout), so N RETs share ONE copy of the return sequence and
         * N-1 copies are deleted (6 words each). Gated on g_alloc like
         * every fold: the naive path stays byte-identical to M0 (and
         * jmpr_dyn keeps its documented 0-saved honest floor). The tail
         * spans every RET in the object — correct even if a .tmo ever
         * carries multiple exported entries, since it is sp-relative and
         * the frame layout (TX_AR_TOTAL_FRAME_BYTES) is uniform. Soundness:
         * each RET's own flush writes ITS resident set to slots before
         * branching, and the shared tail then reads r0's slot+tag and
         * returns — the branch crosses no SIMI block boundary (the cache
         * is empty and the tail touches only t0/t1), so every arrival
         * sees exactly the state its own RET left. */
        if (g_alloc && g_tail_ret_off != 0xFFFFFFFFu) {
            e32(cb, enc_b((int)((int64_t)g_tail_ret_off - (int64_t)cb->len) / 4));
            break;
        }
        if (g_alloc) g_tail_ret_off = cb->len;   /* owner: record the tail's first word */
        ld_slot(cb, X_T0, 0);    /* r0 is the return-value register, §4.8; t0 survives the epilogue below untouched */
        /* Gap Remediation SIMI Phase 12: r0's tag rides in t1, alongside
         * t0 — the epilogue below (add sp; ldr x30; ldr x29; add sp; br
         * x30) touches neither, so both survive intact to the call site's
         * post-call code (see emit_call_site()). */
        ld_tag(cb, X_T1, 0);
        e32(cb, enc_add_imm(X_SP, X_SP, (uint32_t)TX_AR_TOTAL_FRAME_BYTES));
        e32(cb, enc_ldr(X_LR, X_SP, 1));
        e32(cb, enc_ldr(X_FP, X_SP, 0));
        e32(cb, enc_add_imm(X_SP, X_SP, 16));
        e32(cb, enc_br(X_LR));
        break;
    case OP_CALL: return TX_AR_ERR_BAD_OPCODE; /* handled specially in translate() (needs pc) */
    case OP_JMPR: return TX_AR_ERR_BAD_OPCODE; /* handled specially in translate() (needs the jump table; folded in M2) */
    case OP_CAS: case OP_ATOMIC_ADD: {
        /* Gap Remediation SIMI Phase 15 (A3, plan D2): the exclusive-
         * monitor loops — CAS is ldaxr/cmp/b.ne/stlxr/cbnz retry,
         * ATOMIC_ADD is ldaxr/add/stlxr/cbnz retry, exactly as the
         * plan's D2 design specified (ARMv8.0, no LSE dependency).
         * v1 scope: 4/8-byte cells, register operands only (FLAG_IMM
         * rejected), SC ordering. rA = base, rB = expected (CAS) /
         * addend (ATOMIC_ADD), rD = new-in/old-out (CAS, the cmpxchg
         * shape) / old-out only (ATOMIC_ADD).
         *
         * Register plan: this instruction needs FOUR persistent hosts
         * (base, new/addend, expected, and the result register), one
         * more than the 3-slot x9/x10/x11 cache — so the cache is
         * flushed first (empty, like RESOLVE's runtime-call flush), the
         * result host rh is reserved for rd, the two non-rh slots hold
         * base and new/addend, and the expected (CAS only) lives in
         * X_DR (x12, outside the cache — no run-reuse displacement is
         * live at an atomics pc, since runs are marked only on
         * LOAD/STORE). The loop reuses rh for old (the ldaxr target —
         * the mismatch path exits with old already in the result
         * register, zero extra words) and the stlxr status doubles rh
         * (on success old == expected, recovered from x12 with one
         * mov; on the retry path the next ldaxr overwrites it). For
         * ATOMIC_ADD the sum is a transient in x12 and old stays in rh
         * untouched — no recovery mov at all.
         *
         * Width masking: ldaxr w (32-bit) zero-extends the loaded old;
         * the 32-bit expected is masked with `mov w12, w12` (orr-32),
         * so the cmp compares the width-masked pair exactly like the
         * interpreter (garbage high bits in rB are ignored — the A1
         * i32 tooth's shape). The 32-bit ATOMIC_ADD computes the sum
         * in 64 bits and the stlxr w store truncates to low 32,
         * matching the interpreter's memcpy of wdt bytes of the full
         * sum. */
        int wdt = 1 << type_shift(type);
        if (flags & FLAG_IMM) return TX_AR_ERR_BAD_OPCODE;
        if (wdt != 4 && wdt != 8) return TX_AR_ERR_BAD_OPCODE;
        cache_flush(cb);
        int v = cache_reserve(cb, rd, -1, -1);
        uint8_t rh = result_host(v);
        uint8_t h_base, h_other;
        if (!g_alloc) { h_base = X_T1; h_other = X_T2; }
        else { h_base = (uint8_t)(X_T0 + ((v + 1) % AR_CACHE_N)); h_other = (uint8_t)(X_T0 + ((v + 2) % AR_CACHE_N)); }
        get_operand(cb, ra, h_base, 0);                 /* base */
        if (op == OP_CAS) {
            get_operand(cb, rd, h_other, 0);            /* new value (rD input) */
            ld_slot(cb, X_DR, w_rb_reg(w));             /* expected (rB) — cache is empty, the slot is valid */
            if (wdt == 4) e32(cb, enc_orr_32(X_DR, X_DR));   /* mask expected to low 32 */
            uint32_t retry_pos = cb->len;               /* .retry: */
            e32(cb, (wdt == 8) ? enc_ldaxr(rh, h_base) : enc_ldaxr_w(rh, h_base));   /* old -> rh */
            e32(cb, enc_subs_shift(31, rh, X_DR, 0, 0));    /* cmp rh, x12 (both masked for wdt==4) */
            uint32_t mismatch_pos = emit_bcond_placeholder(cb); /* b.ne .done */
            e32(cb, (wdt == 8) ? enc_stlxr(rh, h_other, h_base) : enc_stlxr_w(rh, h_other, h_base)); /* status -> w{rh}; store new */
            uint32_t retry_branch = emit_cbnz_placeholder(cb); /* cbnz rh, .retry */
            patch_cbnz_back(cb, retry_branch, retry_pos, rh);
            e32(cb, enc_orr_shift(rh, 31, X_DR, 0, 0));       /* success: old == expected — mov rh, x12 */
            patch_local_bcond(cb, mismatch_pos, 1);     /* .done: b.ne (NE = 1) */
        } else {
            get_operand(cb, w_rb_reg(w), h_other, 0);   /* addend (rB) */
            uint32_t retry_pos = cb->len;               /* .retry: */
            e32(cb, (wdt == 8) ? enc_ldaxr(rh, h_base) : enc_ldaxr_w(rh, h_base));   /* old -> rh (the result host) */
            e32(cb, enc_add_shift(X_DR, rh, h_other, 0, 0)); /* x12 = old + addend */
            e32(cb, (wdt == 8) ? enc_stlxr(X_DR, X_DR, h_base) : enc_stlxr_w(X_DR, X_DR, h_base)); /* status -> w12; store the sum */
            uint32_t retry_branch = emit_cbnz_placeholder(cb); /* cbnz x12, .retry */
            patch_cbnz_back(cb, retry_branch, retry_pos, X_DR);
        }
        store_result(cb, rd);   /* rh = old: CAS's rh holds it on both paths; ATOMIC_ADD's rh never left it */
        break;
    }
    default: return TX_AR_ERR_BAD_OPCODE;
    }
    return TX_AR_OK;
}

/* Rewrite a 1-word adr placeholder (JMPR table base) in place. M3: the
 * table base is reached PC-RELATIVELY — adr x11, #(table_off − pos) — so
 * the same blob executes under a64_exec's guest convention (PC = byte
 * offset into out_buf) and on real A64 (PC = actual address): both land
 * on the table, and the table's signed relative entries resolve to
 * absolute targets in both. The pre-M3 movz+movk base baked a bare byte
 * offset that ONLY the guest convention could branch to. imm = target −
 * pos fits ±1MB by construction (CODE_CAP = 256 KiB, table inside). */
static void patch_adr(uint8_t* out_buf, uint32_t pos, uint32_t target_off) {
    int64_t imm = (int64_t)target_off - (int64_t)pos;
    if (imm < -(1ll << 20) || imm > ((1ll << 20) - 1)) return;  /* unreachable (256 KiB cap) */
    patch32(out_buf, pos, enc_adr(X_T2, (int32_t)imm));
}

/* ─── Top-level translate: two passes (emit + patch fixups) ─────────────
 * No literal pool pass — movz/movk made it unnecessary; the only post-emit
 * rewrite besides branch/call fixups is the JMPR table-base adr (see
 * g_jmpr_li_pos) and the JMPR table's own contents, both of which happen
 * once every instruction's offset is known. */
int simi_arm_translate(const uint8_t* obj_data, uint32_t obj_size,
                        uint8_t* out_buf, uint32_t out_cap,
                        const char* entry_name, uint64_t scratch_ptr,
                        uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                        uint64_t rt_objtype_fn,
                        uint32_t* out_len, uint32_t* entry_off) {
    if (!obj_data || obj_size < sizeof(struct SimiObjHdrAR)) return TX_AR_ERR_BAD_HEADER;
    struct SimiObjHdrAR hdr;
    ar_memcpy(&hdr, obj_data, sizeof(hdr));
    if (hdr.magic != SIMI_AR_MAGIC) return TX_AR_ERR_BAD_HEADER;

    uint64_t expect = sizeof(struct SimiObjHdrAR);
    expect += (uint64_t)hdr.num_instr * 8;
    expect += (uint64_t)hdr.num_literals * 8;
    expect += (uint64_t)hdr.num_entries * sizeof(struct TxEntryRecAR);
    expect += (uint64_t)hdr.num_names * sizeof(struct TxNameRecAR);
    if (expect != (uint64_t)obj_size) return TX_AR_ERR_BAD_HEADER;
    if (hdr.num_instr == 0 || hdr.num_instr > 4096) return TX_AR_ERR_TOO_MANY_INSTR;

    const uint64_t* instrs = (const uint64_t*)(obj_data + sizeof(struct SimiObjHdrAR));
    const uint64_t* literals = (const uint64_t*)((const uint8_t*)instrs + hdr.num_instr * 8);
    const struct TxEntryRecAR* entries = (const struct TxEntryRecAR*)
        ((const uint8_t*)literals + hdr.num_literals * 8);
    const struct TxNameRecAR* names = (const struct TxNameRecAR*)
        ((const uint8_t*)entries + hdr.num_entries * sizeof(struct TxEntryRecAR));
    /* v0.3: namepool_ptr is baked as a raw pointer straight into obj_data's
     * own name-pool bytes — mirrors simi_x86.c / simi_riscv.c exactly. */
    uint64_t namepool_ptr = (uint64_t)(uintptr_t)names;

    g_rt_resolve_fn = rt_resolve_fn;
    g_rt_objsize_fn = rt_objsize_fn;
    g_rt_objtype_fn = rt_objtype_fn;
    g_num_names = hdr.num_names;
    g_num_instr = hdr.num_instr;
    g_njmpr_li_pos = 0;
    g_tail_ret_off = 0xFFFFFFFFu;   /* M2.14: no tail owner yet */

    struct CodeBuf cb; cb.buf = out_buf; cb.cap = out_cap; cb.len = 0; cb.overflow = 0;
    g_nfixups = 0;

    /* M1/M2 pre-pass. Pass A: mark branch/call target pcs (block heads).
     * The targets must be cleared ONCE, before the loop — zeroing
     * g_pc_target[pc] inside the loop would wipe marks that earlier
     * branches set (branch_cmp caught this exact bug). */
    for (uint32_t q = 0; q < 4096; q++) g_pc_target[q] = 0;
    for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
        uint64_t w = instrs[pc];
        uint8_t op = w_op(w);
        if (op == OP_BR || op == OP_BC || op == OP_CALL) {
            int64_t tgt = (int64_t)pc + 1 + w_imm28(w);
            if (tgt >= 0 && tgt < (int64_t)hdr.num_instr) g_pc_target[(uint32_t)tgt] = 1;
        }
    }
    /* M2.13: entry pcs are block heads too — the trampoline branches to
     * them, so constants cannot survive into an entry pc. This hardens
     * the corner where an entry pc is ALSO a fall-through fold target:
     * the fold fixpoint's rule-1 merge skips fall-through targets, so
     * without this the entry would be unmarked and a JMPR after it could
     * fold on a constant that only holds on the fall-through path (the
     * trampoline arrival marshals the arg slots and guarantees nothing).
     * Byte-neutral for the corpus: entries sit at pc 0, where the scan
     * starts with an empty constant map anyway. */
    for (uint32_t i = 0; i < hdr.num_entries; i++)
        if (entries[i].offset < hdr.num_instr) g_pc_target[entries[i].offset] = 1;
    /* M2.18: the static part of the fixpoint relaxation. A head pc X is
     * eligible iff its ONLY incoming path is a single FORWARD BR/BC edge
     * (X-1 is a terminal — RET or BR — so the fall-through is dead, no
     * CALL targets X, X is not an entry, and no backward branch targets
     * it). The fold-target exclusion is deliberately NOT here: the fold
     * set is what the fixpoint is converging, so it is applied per-scan
     * (g_fold_tgt_prev). */
    for (uint32_t q = 0; q < 4096; q++) { g_relax_ok[q] = 0; g_relax_snap_active[q] = 0; }
    for (uint32_t X = 1; X < hdr.num_instr; X++) {
        int nbr = 0, ncall = 0, src = -1;
        for (uint32_t P = 0; P < hdr.num_instr; P++) {
            uint64_t wP = instrs[P];
            uint8_t opP = w_op(wP);
            if (opP != OP_BR && opP != OP_BC && opP != OP_CALL) continue;
            int64_t tgt = (int64_t)P + 1 + w_imm28(wP);
            if (tgt != (int64_t)X) continue;
            if (opP == OP_CALL) ncall++; else { nbr++; src = (int)P; }
        }
        if (nbr != 1 || ncall != 0 || src >= (int)X) continue;  /* unique FORWARD edge */
        uint8_t opPrev = w_op(instrs[X - 1]);
        if (!(opPrev == OP_RET || opPrev == OP_BR)) continue;   /* terminal: no fall-through */
        int is_entry = 0;
        for (uint32_t i = 0; i < hdr.num_entries; i++)
            if (entries[i].offset == X) { is_entry = 1; break; }
        if (is_entry) continue;   /* the trampoline edge guarantees nothing */
        g_relax_ok[X] = 1;
    }
    /* M2.20: leaf-callee marker, precomputed per CALL TARGET. Whether the
     * callee is an analyzable straight-line leaf is a property of the
     * callee alone (M2.19 computed the args-unknown constant per call
     * SITE; M2.20 moves the body analysis into the scan so each site can
     * seed the callee's r0-r7 with ITS OWN caller-side constants). */
    for (uint32_t q = 0; q < 4096; q++) g_call_leaf_ret[q] = -1;
    for (uint32_t P = 0; P < hdr.num_instr; P++) {
        uint64_t wP = instrs[P];
        if (w_op(wP) != OP_CALL) continue;
        int64_t tgt = (int64_t)P + 1 + w_imm28(wP);
        if (tgt < 0 || tgt >= (int64_t)hdr.num_instr) continue;
        g_call_leaf_ret[(uint32_t)tgt] = ar_leaf_ret_pc(instrs, hdr.num_instr, (uint32_t)tgt);
    }
    /* M2: constant-index JMPR folding, by fixpoint. Each scan walks the
     * linear stream with a per-register constant map that resets at every
     * pc in g_pc_target (pass-A targets plus any fold targets discovered
     * so far). The reset makes the analysis sound across joins: a chain
     * between block heads is executed identically on every path that
     * enters it, so a constant attributed to the JMPR's index there holds
     * on every arrival. A JMPR whose index is a known in-range constant
     * records its fold target; the target is merged into g_pc_target so
     * rule 1 flushes its block head in the main loop. The fixpoint
     * matters because a fold target can sit inside another JMPR's chain
     * (a backward fold retroactively splits it, potentially un-folding
     * that JMPR) — the fold set is monotone decreasing, so this
     * terminates in at most the number of JMPRs. */
    for (int i = 0; i < 4096; i++) g_jmpr_fold[i] = -1;
    int relax_enabled = 1;   /* M2.18; the safety fallback below disables it */
    int passes = 0;
    for (;;) {
        g_ar_net_scans++;    /* M2.71: fixpoint scan count (bench_net.c reads it) */
        for (int i = 0; i < 4096; i++) g_jmpr_fold_prev[i] = g_jmpr_fold[i];
        /* M2.18: per-scan fold-target exclusion for the relaxation — the
         * previous scan's COMPLETE fold set (a fold edge is another
         * incoming path into the head; live-updated below as the scan
         * discovers folds, so a forward fold into a relaxed head is
         * caught within the same scan). */
        for (int i = 0; i < 4096; i++) g_fold_tgt_prev[i] = 0;
        for (uint32_t q = 0; q < hdr.num_instr; q++)
            if (g_jmpr_fold_prev[q] >= 0 && g_jmpr_fold_prev[q] != (int)(q + 1))
                g_fold_tgt_prev[(uint32_t)g_jmpr_fold_prev[q]] = 1;
        for (int i = 0; i < TX_AR_MAX_REGS; i++) g_const_known[i] = 0;
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            if (g_pc_target[pc]) {   /* block head: constants do not survive a join */
                if (relax_enabled && g_relax_ok[pc] && g_relax_snap_active[pc] &&
                    !g_fold_tgt_prev[pc]) {
                    /* M2.18: the unique incoming path is a single forward
                     * BR/BC from a pc whose fall-through cannot reach here
                     * (X-1 is a terminal) — restore the map as it was at
                     * that branch; there is no join to merge. */
                    for (int i = 0; i < TX_AR_MAX_REGS; i++) {
                        g_const_known[i] = g_relax_snap_known[pc][i];
                        g_const_val[i] = g_relax_snap_val[pc][i];
                    }
                } else {
                    for (int i = 0; i < TX_AR_MAX_REGS; i++) g_const_known[i] = 0;
                }
            }
            uint64_t w = instrs[pc];
            uint8_t op = w_op(w);
            uint16_t ra = w_ra(w);
            if (op == OP_BR || op == OP_BC) {
                /* M2.18: snapshot the current map at the branch, for an
                 * eligible (and not-fold-target) target head. Taken before
                 * the else-clause below, which may clear rd's constant. */
                int64_t tgt = (int64_t)pc + 1 + w_imm28(w);
                if (relax_enabled && tgt >= 0 && tgt < (int64_t)hdr.num_instr &&
                    g_relax_ok[(uint32_t)tgt] && !g_fold_tgt_prev[(uint32_t)tgt]) {
                    for (int i = 0; i < TX_AR_MAX_REGS; i++) {
                        g_relax_snap_known[(uint32_t)tgt][i] = g_const_known[i];
                        g_relax_snap_val[(uint32_t)tgt][i] = g_const_val[i];
                    }
                    g_relax_snap_active[(uint32_t)tgt] = 1;
                }
            }
            if (op == OP_ENTER || op == OP_RET) {
                for (int i = 0; i < TX_AR_MAX_REGS; i++) g_const_known[i] = 0;  /* prologue/terminal: no constant survives */
            } else if (op == OP_JMPR) {
                g_jmpr_fold[pc] = (ra < TX_AR_MAX_REGS && g_const_known[ra] &&
                                   g_const_val[ra] < hdr.num_instr) ? (int)g_const_val[ra] : -1;
                if (g_jmpr_fold[pc] >= 0 && g_jmpr_fold[pc] != (int)(pc + 1))
                    g_fold_tgt_prev[(uint32_t)g_jmpr_fold[pc]] = 1;   /* M2.18: live fold-target exclusion */
            } else if (op == OP_CALL) {
                /* M2.19/M2.20: the return value. The fresh-frame model
                 * (the interpreter zeroes the callee's frame, copies r0-r7
                 * as args, and on RET only r0 propagates back) means the
                 * caller's other registers are structurally preserved by
                 * the call — r0 is the callee's return value, unknown
                 * unless the callee is an analyzable leaf. M2.20 seeds the
                 * callee's ARGS from the CALLER's constant map at the call
                 * site: the emitted call site marshals r0-r7 from the
                 * caller's frame (and the interpreter copies them), so a
                 * known constant argument is a known constant inside the
                 * leaf, and a leaf that returns a function of its constant
                 * arguments folds. The scratch map's r0-r7 come from the
                 * caller (known constants or unknown); r8+ = 0, zeroed by
                 * the leaf's ENTER prologue. The walk is re-run every scan
                 * pass — deterministic, and cheap: the leaf is rejected
                 * upfront unless it is straight-line. */
                int64_t tgt = (int64_t)pc + 1 + w_imm28(w);
                if (tgt >= 0 && tgt < (int64_t)hdr.num_instr) {
                    int ret = g_call_leaf_ret[(uint32_t)tgt];
                    /* A leaf's RET pc is always >= T+1 >= 1, so > 0
                     * distinguishes the -1 sentinel (not a leaf). */
                    if (ret > 0) {
                        /* Snapshot the caller's map for the args BEFORE
                         * clearing r0 below: arg0 is the caller's r0 at the
                         * call, and the clear models the return value — the
                         * M2.18 lesson (snapshot before the clear that
                         * overwrites it) applies to the argument seeding. */
                        for (int i = 0; i < 8; i++) {
                            g_call_arg_known[i] = g_const_known[i];
                            g_call_arg_val[i] = g_const_val[i];
                        }
                        for (int i = 8; i < TX_AR_MAX_REGS; i++) { g_call_arg_known[i] = 1; g_call_arg_val[i] = 0; }
                        for (uint32_t q = (uint32_t)tgt + 1; q < (uint32_t)ret; q++)
                            ar_const_step(instrs[q], hdr.num_literals, literals, g_call_arg_known, g_call_arg_val);
                        if (g_call_arg_known[0]) { g_const_known[0] = 1; g_const_val[0] = g_call_arg_val[0]; }
                        else                    { g_const_known[0] = 0; }
                    } else {
                        g_const_known[0] = 0;   /* not a leaf: the return value is unknown */
                    }
                } else {
                    g_const_known[0] = 0;   /* out-of-range call target: unknown */
                }
            } else {
                ar_const_step(w, hdr.num_literals, literals, g_const_known, g_const_val);
            }
        }
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            /* rule 1: fold targets are block heads — EXCEPT a fall-through
             * fold (target == pc + 1): its branch is dead (M2.12 drops it), so
             * it marks nothing here, and the M2.12 coalescing pre-pass below
             * is the authority on whether the target keeps a mark (its OTHER
             * incoming edges — pass-A branches, real folds, entries). Skipping
             * the mark is what lets a chain of fall-through folds CASCADE:
             * the target's constant map is not reset, the next JMPR in the
             * chain still sees its index constant, folds, and so on down the
             * chain — instead of the first fold marking the next JMPR as a
             * block head, which resets its index constant and un-folds it
             * (turning it into a DYNAMIC JMPR and killing g_alloc entirely). */
            if (g_jmpr_fold[pc] >= 0 && g_jmpr_fold[pc] != (int)(pc + 1))
                g_pc_target[(uint32_t)g_jmpr_fold[pc]] = 1;
        }
        int same = 1;
        for (int i = 0; i < 4096; i++)
            if (g_jmpr_fold[i] != g_jmpr_fold_prev[i]) { same = 0; break; }
        if (same) break;
        if (++passes > g_ar_net_trip) {
            /* M2.18 safety net: a fold ENABLED BY the relaxation can target
             * the relaxed head itself (a backward fold re-entering the head
             * makes it a loop head the relaxation must not apply to) — that
             * shape 2-cycles the fixpoint (relax -> fold -> reset ->
             * un-fold). M2.69/M2.70 bound the LEGITIMATE convergence:
             * rule-1-driven retroactive splits settle in <= 3 scans
             * (M2.69, chain43), and the relaxation-flip re-fold (M2.70,
             * chain44 — an un-fold clearing the per-pass exclusion lets
             * the head's JMPR re-fold once) adds a 4th scan (passes=3);
             * the corpus max is passes=2 (instrumented). The 2-cycle is
             * the only divergence, so the trip count just needs to sit
             * above the legit max with margin: 512 was a 170x
             * overestimate; the shipped trip count is the tunable
             * g_ar_net_trip = 16 (5x over passes=3), which still never
             * fires on legitimate input while cutting the 2-cycle's
             * scan count 514 -> 18 (28.6x) and chain36's end-to-end
             * translate ~8-9x — measured as committed numbers by
             * bench_net (M2.71). The restart is sound regardless: fall
             * back to the un-relaxed fixpoint, which is
             * monotone-decreasing in the fold set and provably
             * terminates. */
            relax_enabled = 0;
            for (int i = 0; i < 4096; i++) g_jmpr_fold[i] = -1;
            passes = 0;
            continue;
        }
        /* The retroactive-split case — a fold target landing inside
         * another JMPR's chain, before that JMPR's constant source — is
         * covered by this re-scan (the added reset un-folds that JMPR)
         * but has no dedicated test: constructing it requires two JMPRs
         * tangled with branches in a way the corpus does not contain.
         * The reset argument holds regardless; see plan doc §10. */
    }
    /* M2.21: static reachability pre-pass — a worklist BFS from the
     * entries. RET is a terminal; BR adds its target (never falls
     * through); BC/CALL add their target AND fall through (the call
     * returns); everything else flows to pc+1; a FOLDED JMPR adds its
     * fold target (a direct branch — for the M2.12 fall-through shape
     * that is pc+1, reached by falling through), while an UNFOLDED
     * JMPR is a terminal (its runtime target is data-dependent; its own
     * pc being reachable or not is all the gate below needs). The fold
     * edges are what make the gate SOUND: without them, a reachable
     * fold into a statically-unreachable-looking region could hide a
     * runtime-reachable non-folding JMPR there, which would still
     * dispatch to arbitrary pcs (M2.21 reviewer finding). */
    for (uint32_t q = 0; q < 4096; q++) { g_reach[q] = 0; g_npred[q] = 0; }
    {
        uint32_t wl[4096], wh = 0, wr = 0;
        for (uint32_t i = 0; i < hdr.num_entries; i++)
            if (entries[i].offset < hdr.num_instr && !g_reach[entries[i].offset]) {
                g_reach[entries[i].offset] = 1;
                wl[wr++] = entries[i].offset;
            }
        while (wh < wr) {
            uint32_t p = wl[wh++];
            uint8_t op = w_op(instrs[p]);
            if (op == OP_RET) continue;                    /* terminal */
            if (op == OP_JMPR) {
                if (g_jmpr_fold[p] >= 0) {
                    uint32_t t = (uint32_t)g_jmpr_fold[p];
                    if (t < hdr.num_instr) { g_npred[t]++; if (!g_reach[t]) { g_reach[t] = 1; wl[wr++] = t; } }
                }
                continue;                                  /* unfolded: data-dependent target */
            }
            if (op == OP_BR) {
                int64_t t = (int64_t)p + 1 + w_imm28(instrs[p]);
                if (t >= 0 && t < (int64_t)hdr.num_instr) { g_npred[(uint32_t)t]++; if (!g_reach[(uint32_t)t]) { g_reach[(uint32_t)t] = 1; wl[wr++] = (uint32_t)t; } }
                continue;                                  /* BR never falls through */
            }
            if (op == OP_BC || op == OP_CALL) {
                int64_t t = (int64_t)p + 1 + w_imm28(instrs[p]);
                if (t >= 0 && t < (int64_t)hdr.num_instr) { g_npred[(uint32_t)t]++; if (!g_reach[(uint32_t)t]) { g_reach[(uint32_t)t] = 1; wl[wr++] = (uint32_t)t; } }
            }
            if (p + 1 < hdr.num_instr) { g_npred[p + 1]++; if (!g_reach[p + 1]) { g_reach[p + 1] = 1; wl[wr++] = p + 1; } }
        }
    }
    /* g_alloc = cache enabled iff every REACHABLE JMPR folds. One
     * reachable non-foldable JMPR can dispatch to any pc, which makes
     * every pc a block head — no cache. Folded JMPRs are direct branches
     * either way (they need no cache), and unreachable JMPRs never
     * dispatch (M2.21) — both are inert for the gate. */
    g_alloc = 1;
    for (uint32_t pc = 0; pc < hdr.num_instr; pc++)
        if (w_op(instrs[pc]) == OP_JMPR && g_jmpr_fold[pc] < 0 && g_reach[pc]) {
            g_alloc = 0; break;
        }
    for (int i = 0; i < AR_CACHE_N; i++) g_cache_guest[i] = -1;
    g_cache_round = 0;
    g_resv_slot = -1;

    /* M2.12: block-coalescing pre-pass. A folded JMPR whose target is the
     * VERY NEXT pc (g_jmpr_fold[pc] == pc + 1) emits `cache_flush; b
     * next` — a branch to the immediately-following instruction, which
     * control reaches by falling through anyway. The branch is dead: mark
     * the pc so the main loop emits NOTHING for the JMPR (rule 1's
     * cache_flush is still owed on the fall-through side — see below).
     * When the target also has NO other incoming edge — no BR/BC/CALL,
     * no other folded JMPR, no entry trampoline — the two blocks FUSE:
     * the target's block-head mark is cleared, so the end-of-loop flush
     * that rule 1 would otherwise emit at the boundary is skipped too,
     * and the x9/x10/x11 cache survives the join: the register frame is
     * loaded once, not once per block. Soundness rests on the incoming-
     * edge check: with no other way in, the only path to the target is
     * the fall-through, which carries exactly the cache state the
     * previous instruction left — the same invariant straight-line
     * code already relies on. M2.13: the fixpoint's rule-1 merge no
     * longer marks fall-through fold targets at all (a fall-through
     * fold's own edge is the one being dropped, so it owes no mark),
     * which lets CHAINS cascade: three JMPRs each folding to their own
     * next pc fold in sequence, and this pass fuses all three joins.
     * When the target keeps other edges, the pass re-marks it (the
     * merge skipped it), so the boundary flush and the analysis-side
     * constant reset are exactly what they were before the merge
     * change. Gated on g_alloc: a dynamic JMPR can land on any pc
     * (every pc is a block head), so no fusion — and the naive path
     * stays byte-identical to M0. */
    for (uint32_t q = 0; q < 4096; q++) g_fold_fall[q] = 0;
    if (g_alloc) {
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            if (g_jmpr_fold[pc] != (int)(pc + 1)) continue;
            g_fold_fall[pc] = 1;
            /* Count other incoming edges to pc+1. The fold at pc itself
             * IS an edge (it's the one being dropped); the g_pc_target
             * pass-A marks were seeded for BR/BC/CALL, so re-derive from
             * the instructions plus the entry trampolines. */
            uint32_t t = pc + 1;
            int other = 0;
            for (uint32_t q = 0; q < hdr.num_instr && !other; q++) {
                if (q == pc) continue;   /* the fold at pc is the edge being dropped, not an "other" */
                uint64_t wq = instrs[q];
                uint8_t opq = w_op(wq);
                if (opq == OP_BR || opq == OP_BC || opq == OP_CALL) {
                    int64_t tgt = (int64_t)q + 1 + w_imm28(wq);
                    if (tgt >= 0 && tgt < (int64_t)hdr.num_instr &&
                        (uint32_t)tgt == t) other = 1;
                } else if (opq == OP_JMPR && g_jmpr_fold[q] >= 0 &&
                           (uint32_t)g_jmpr_fold[q] == t) {
                    other = 1;
                }
            }
            if (!other)
                for (uint32_t i = 0; i < hdr.num_entries; i++)
                    if (entries[i].offset == t) { other = 1; break; }
            if (!other) g_pc_target[t] = 0;   /* fuse: the fold is the only way in */
            else g_pc_target[t] = 1;          /* M2.13: re-mark — the fixpoint's rule-1
                                               * merge skips fall-through targets, so the
                                               * target's mark now owes to its OTHER edges;
                                               * this restores the boundary flush (and the
                                               * analysis-side constant reset) exactly as
                                               * before the merge change. */
        }
    }

    /* M2.15: epilogue/prologue merging for single-edge BACKWARD folds.
     * A folded JMPR whose target is an EARLIER pc emits
     * `cache_flush; b target`. The target block's head reloads its
     * operand registers from the frame (rule 1 gives it an empty cache) —
     * and the source block's tail may have RELOADED THE SAME REGISTERS a
     * few instructions earlier. The fold's flush stores only the
     * directory's resident RESULTS, so the tail's fetched values (which
     * are transient — never claimed by the directory) are STILL IN x9/x10
     * when control lands at the head: the head's matching reloads re-read
     * the same slot values into the same hosts — dead code. The merge
     * DROPS them: the frame is loaded once (at the tail), not once per
     * block — §10.29's "epilogue/prologue merging across backward folds".
     *
     * Soundness requires the head to be reached ONLY via the fold — any
     * other edge arrives with its own, unknown x9/x10 state, and the
     * head's code is shared. So the target must have no BR/BC/CALL edge,
     * no other folded JMPR, no entry trampoline, and NO fall-through:
     * its predecessor T-1 must be an unconditional terminal (RET, BR, or
     * a JMPR that does not fold to T). This makes the shape inherently
     * dead-path code (a live loop's head is also reached by its own
     * entry), which is exactly what the parity set measures — emission.
     *
     * The runtime state at the head is determined by the emitted words
     * between the head and the fold, so the analysis must know them
     * statically. The layout is rigid: the head block is exactly
     * [ALU-2src pattern @ T][terminal @ T+1]; then exactly two
     * result-only plain instructions at T+2/T+3 (M2.17: any
     * ar_is_interm_op — LOADI/LOADI64 as before, or a COMPUTED result
     * like an arithmetic chain — the invariants below depend only on
     * the claim count, not on what computes the values; their operand
     * fetches are transient and the tail's fetches re-establish the
     * transient state); then the tail [ALU-2src @ T+4] reading the
     * SAME guests in the SAME order as the pattern; then the fold
     * [JMPR @ T+5] back to T. The tail's reserve pick (its result
     * host slot v) is the cache round-robin cursor slot, because the
     * pattern's claim plus the two intermediates' claims net +3 ≡ 0
     * mod 3 — the two intermediates occupy cursor+1 and cursor+2; the
     * fetches go to x9 (g1) and x10 (g2), so g1's value survives iff
     * v != 0 and g2's iff v != 1 — the drop flags. The cursor at the
     * tail is (result-instruction count mod 3), which equals the count
     * WITHOUT simulation only when no reserve reuses a resident slot;
     * the pre-T result registers must therefore be pairwise distinct
     * (the directory only ever holds result registers, so a register
     * never before written as a result cannot be resident). Gated on
     * g_alloc like every fold: the naive JMPR path stays byte-identical
     * to M0. */
    for (uint32_t q = 0; q < 4096; q++) g_epi_merge[q] = 0;
    if (g_alloc) {
        for (uint32_t T = 1; T + 5 < hdr.num_instr; T++) {
            if (w_op(instrs[T + 5]) != OP_JMPR || g_jmpr_fold[T + 5] != (int)T) continue;
            /* single-edge target T: no other fold, no BR/BC/CALL, no entry */
            int other = 0;
            for (uint32_t q = 0; q < hdr.num_instr && !other; q++) {
                if (q != T + 5 && g_jmpr_fold[q] >= 0 && (uint32_t)g_jmpr_fold[q] == T) other = 1;
                if (!other && (w_op(instrs[q]) == OP_BR || w_op(instrs[q]) == OP_BC ||
                               w_op(instrs[q]) == OP_CALL)) {
                    int64_t tgt = (int64_t)q + 1 + w_imm28(instrs[q]);
                    if (tgt >= 0 && tgt < (int64_t)hdr.num_instr && (uint32_t)tgt == T) other = 1;
                }
            }
            if (!other)
                for (uint32_t i = 0; i < hdr.num_entries; i++)
                    if (entries[i].offset == T) { other = 1; break; }
            if (other) continue;
            uint8_t op_prev = w_op(instrs[T - 1]);
            if (!(op_prev == OP_RET || op_prev == OP_BR ||
                  (op_prev == OP_JMPR && g_jmpr_fold[T - 1] != (int)T))) continue;
            /* the pattern (head block's first instruction) + rigid layout */
            if (!ar_is_alu2(w_op(instrs[T])) || (w_flags(instrs[T]) & FLAG_IMM)) continue;
            uint16_t a1 = w_ra(instrs[T]), b1 = w_rb_reg(instrs[T]);
            if (!(w_op(instrs[T + 1]) == OP_RET || w_op(instrs[T + 1]) == OP_BR)) continue;
            if (!ar_is_interm_op(w_op(instrs[T + 2]))) continue;  /* M2.17: computed results allowed */
            if (!ar_is_interm_op(w_op(instrs[T + 3]))) continue;
            if (!ar_is_alu2(w_op(instrs[T + 4])) || (w_flags(instrs[T + 4]) & FLAG_IMM)) continue;
            if (w_ra(instrs[T + 4]) != a1 || w_rb_reg(instrs[T + 4]) != b1) continue;
            /* the two dead-region intermediates' destinations must be
             * distinct and not the pattern's sources (the tail's fetches
             * are then real reloads, misses against a cache holding
             * exactly those two). */
            uint16_t d1 = w_rd(instrs[T + 2]), d2 = w_rd(instrs[T + 3]);
            if (d1 == d2 || d1 == a1 || d1 == b1 || d2 == a1 || d2 == b1) continue;
            /* the pre-T result registers must be pairwise distinct (no
             * reserve reuse — the cursor is then exactly the count). */
            uint64_t seen = 0; int cnt = 0; int ok = 1;
            for (uint32_t pc = 0; pc < T; pc++) {
                uint64_t w = instrs[pc];
                uint8_t op = w_op(w);
                if (!ar_is_result_op(op)) continue;
                uint16_t rd = (op == OP_CALL) ? 0 : w_rd(w);
                if (rd >= 64 || (seen & (1ull << rd))) { ok = 0; break; }
                seen |= (1ull << rd);
                cnt++;
            }
            if (!ok) continue;
            int c = cnt % 3;   /* the tail's result host slot */
            g_epi_merge[T] = (uint8_t)(((c != 0) ? 1 : 0) | ((c != 1) ? 2 : 0));
        }
    }

    /* M2.11: run-reuse pre-pass. A maximal straight-line run of LOAD/STORE
     * with the SAME displacement that lands in the materialize path (past
     * the imm12 range — the imm9/imm12 folds already take those in one or
     * two words, so a run would only be bigger) shares ONE materialization
     * of that displacement into X_DR (x12, outside the cache): the run
     * head emits the li64 once and every access in the run is a single
     * register-offset word, instead of a per-access materialize +
     * add_shift + zero-offset access. The run must not cross a block head
     * (a branch could arrive with x12 holding garbage) nor a non-
     * LOAD/STORE instruction (CALL/trampoline clobber x12), and it is
     * gated on g_alloc like every fold — the naive JMPR path stays
     * byte-identical to M0. Runs of one are left to the per-instruction
     * register-offset fold below. */
    for (uint32_t q = 0; q < 4096; q++) { g_run_first[q] = 0; g_run_cont[q] = 0; }
    if (g_alloc) {
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            if (g_pc_target[pc]) continue;   /* a branch could land here with x12 stale */
            uint64_t w0 = instrs[pc];
            uint8_t op0 = w_op(w0);
            if ((op0 != OP_LOAD && op0 != OP_STORE) ||
                !mem_materialize_class(w_imm28(w0), w_type(w0)))
                continue;
            int32_t disp0 = w_imm28(w0);
            uint32_t j = pc + 1;
            while (j < hdr.num_instr && !g_pc_target[j] &&
                   (w_op(instrs[j]) == OP_LOAD || w_op(instrs[j]) == OP_STORE) &&
                   w_imm28(instrs[j]) == disp0 &&
                   mem_materialize_class(w_imm28(instrs[j]), w_type(instrs[j])))
                j++;
            if (j - pc >= 2) {               /* a run is >= 2 accesses or it isn't worth it */
                g_run_first[pc] = 1;
                for (uint32_t k = pc + 1; k < j; k++) g_run_cont[k] = 1;
                pc = j - 1;                  /* the loop's pc++ lands on the access after the run */
            }
        }
    }

    /* M2.25/M2.26: inline-dispatch chain analysis. Naive mode
     * (g_alloc=0) with exactly ONE dynamic JMPR: replace the runtime
     * table + bounds check with an inline compare-and-branch chain over
     * the index register's PROVABLE candidate set. The set is computed
     * by a forward walk over the linear stream with the same join
     * discipline as the fold fixpoint (constants die at block heads),
     * but a head's set is the UNION of its incoming edges' sets — the
     * chain must cover every path's value, not fold a single one.
     * M2.26: the walk tracks ra plus its TRANSITIVE FEEDERS (the
     * closure below), so an index built by a register-form ADD/SUB —
     * LOADI rb; ADD ra, ra, rb — chains too (jmpr_chain2); a writer
     * that is not a known constant (LOAD, CALL result, MUL/DIV/AND/
     * OR/XOR, shifts...) still marks its set UNKNOWN and no chain
     * fires (jmpr_dyn/mix/foldreach keep the table). The walk iterates
     * to a fixpoint because a backward branch delivers its arrival set
     * to a head already processed; sets only grow and collapse to
     * UNKNOWN at the cap, so it converges. Soundness: the chain's
     * fall-through UDF fires exactly for indices the analysis proves
     * impossible; candidates >= num_instr get no branch and land on the
     * UDF, matching the table's bounds-check fault (jmpr_oob's constant
     * 999 collapses to a bare UDF). The walk's linear state is built
     * only from real edges — a non-head pc after a terminal is either a
     * branch target (its union resets it) or unreachable, and the
     * single dynamic JMPR is reachable here (it is what forced
     * g_alloc=0), so its set is never tainted by a dead region. */
    g_chain_active = 0;
    g_chain_ncand = 0;
    if (!g_alloc) {
        uint32_t dyn_pc = 0, n_dyn = 0;
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++)
            if (w_op(instrs[pc]) == OP_JMPR && g_jmpr_fold[pc] < 0) { n_dyn++; dyn_pc = pc; }
        if (n_dyn == 1) {
            uint16_t ra = w_ra(instrs[dyn_pc]);
            if (ra < TX_AR_MAX_REGS) {
                /* M2.26/M2.43: the tracked-register closure — ra plus
                 * every register that can transitively feed it (operands
                 * of instructions whose rd is tracked). Capped at
                 * TX_AR_CHAIN_REGS; beyond that the analysis declines
                 * (the table path). M2.43 raised the cap 4 -> 8: a
                 * closure needing more than 8 feeders still declines,
                 * but 5-8-feeder index chains (jmpr_chain19) now
                 * analyze instead of silently truncating to UNKNOWN.
                 * The closure is over the whole stream, so a feeder on
                 * any path is tracked everywhere — over-tracking is
                 * harmless (bounded by the cap), under-tracking is what
                 * would be unsound. */
                int ntr = 1;
                for (int i = 0; i < TX_AR_MAX_REGS; i++) { g_chain_tracked[i] = 0; g_chain_slot[i] = -1; }
                g_chain_tracked[ra] = 1;
                int grew = 1;
                while (grew && ntr <= TX_AR_CHAIN_REGS) {
                    grew = 0;
                    for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
                        uint64_t w = instrs[pc];
                        uint16_t rd = w_rd(w);
                        if (rd >= TX_AR_MAX_REGS || !g_chain_tracked[rd]) continue;
                        uint8_t op = w_op(w);
                        /* the ntr < cap guard on each addition keeps the
                         * count bounded even when one round would add
                         * several feeders at once — g_chain_arr and the
                         * walk state are sized TX_AR_CHAIN_REGS */
                        if (ar_has_reg_ra(op)) {
                            uint16_t ra2 = w_ra(w);
                            if (ra2 < TX_AR_MAX_REGS && !g_chain_tracked[ra2] && ntr < TX_AR_CHAIN_REGS) { g_chain_tracked[ra2] = 1; ntr++; grew = 1; }
                        }
                        if (!(w_flags(w) & FLAG_IMM) && ar_has_reg_rb(op)) {
                            uint16_t rb = w_rb_reg(w);
                            if (rb < TX_AR_MAX_REGS && !g_chain_tracked[rb] && ntr < TX_AR_CHAIN_REGS) { g_chain_tracked[rb] = 1; ntr++; grew = 1; }
                        }
                    }
                }
                if (ntr <= TX_AR_CHAIN_REGS) {
                    int slot = 0;
                    for (int i = 0; i < TX_AR_MAX_REGS; i++)
                        if (g_chain_tracked[i]) g_chain_slot[i] = (int8_t)(slot++);
                    for (int s = 0; s < ntr; s++)
                        for (uint32_t q = 0; q < 4096; q++) { g_chain_arr[s][q].n = 0; g_chain_arr[s][q].unk = 0; g_chain_arr[s][q].def = -1; }
                    g_chain_bigsnap.n = 0; g_chain_bigsnap.unk = 1;   /* M2.30: the big convergence snapshot */
                    int changed = 1;
                    for (int iter = 0; changed && iter < 64; iter++) {
                        changed = 0;
                        g_chain_ndef = 0;   /* M2.32: the def-record pool is per-iteration */
                        for (int s = 0; s < ntr; s++) g_chain_last_write[s] = -1;   /* M2.36 */
                        struct ChainSet cur[TX_AR_CHAIN_REGS];
                        for (int s = 0; s < ntr; s++) { cur[s].n = 0; cur[s].unk = 1; cur[s].def = -1; }  /* entry: opaque */
                        int carry = 1;                  /* pc 0 is reached from the trampoline */
                        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
                            uint64_t w = instrs[pc];
                            uint8_t op = w_op(w);
                            uint16_t rd = w_rd(w);
                            if (g_pc_target[pc] && pc != 0) {
                                /* block head: union of the fall-through
                                 * carry (only if pc-1 falls through) and
                                 * the branch deliveries, per tracked slot.
                                 * M2.30/M2.34/M2.35/M2.36: a DEFERRED
                                 * carry's true set always exceeds the flat
                                 * cap, so the union is UNKNOWN unless the
                                 * arrival is the SAME record — every path
                                 * carries the same product, the union is a
                                 * no-op, and the deferred form crosses the
                                 * join (the head-union no longer flattens
                                 * what it preserves). The same-record
                                 * check is sound: the carry record is
                                 * live (any write to its DAG would have
                                 * invalidated cur[]), so an equal-index
                                 * delivery — made after the product — is
                                 * live too. M2.35 adds the FLAT-arrival
                                 * case against a deferred carry: when the
                                 * flat path contributes only values the
                                 * record already has, the union is still
                                 * exactly the record (the containment
                                 * test materializes the LIVE carry record
                                 * — sound for the same reason). M2.36
                                 * adds the mirror — a deferred ARRIVAL
                                 * against a flat carry — gated on the
                                 * record-LIVENESS check (no write to its
                                 * transitive DAG since creation; only
                                 * writes replace a set, head-unions only
                                 * widen, a sound superset) plus the same
                                 * containment. M2.37: when containment
                                 * FAILS in either orientation, the union
                                 * is represented as a UNION record —
                                 * op(rec(R), flat(F)) materializing to
                                 * the merged true set, capped at 32 —
                                 * so a non-contained flat join still
                                 * feeds the chain instead of collapsing.
                                 * M2.38: the arrival may also be a
                                 * DIFFERENT deferred record — the union
                                 * is then op(rec(R), rec(R2)) over BOTH
                                 * records (arrival liveness required),
                                 * and the first loop's different-record
                                 * unknowning is gone: every record-vs-
                                 * record case is decided here, where the
                                 * union can fire (the carry record is
                                 * live by construction, so the merged
                                 * materialization is exact-or-superset).
                                 * Anything else with a record on either
                                 * side collapses to UNKNOWN. */
                                for (int s = 0; s < ntr; s++) {
                                    if (!carry) { cur[s].n = 0; cur[s].unk = 0; cur[s].def = -1; }
                                    if (cur[s].def >= 0 && g_chain_arr[s][pc].def == cur[s].def) continue;   /* same record: keep deferred */
                                    if (cur[s].def >= 0) {
                                        /* M2.35: a deferred carry meets a
                                         * FLAT arrival — the union is
                                         * still the record exactly when
                                         * the flat path contributes only
                                         * values the record already has
                                         * (containment); otherwise the
                                         * union exceeds it (still > 12)
                                         * and cannot be a flat walk set.
                                         * Materializing the CARRY record
                                         * is sound: it is live, so its
                                         * DAG slots still read the values
                                         * the product saw. */
                                        if (g_chain_arr[s][pc].def < 0 &&
                                            chain_flat_in_def(cur, s, &g_chain_arr[s][pc]))
                                            continue;
                                        /* M2.37: the flat arrival is NOT
                                         * contained — the union exceeds
                                         * the record. Represent it as a
                                         * UNION record when the merged
                                         * true set still fits the 32-cap,
                                         * so the chain survives the join
                                         * instead of collapsing (an empty
                                         * arrival still collapses, keeping
                                         * the M2.34 first-iteration
                                         * behavior byte-identical). */
                                        if (g_chain_arr[s][pc].def < 0 &&
                                            g_chain_arr[s][pc].n > 0 && !g_chain_arr[s][pc].unk) {
                                            int ri = chain_def_alloc_union(cur, CD_REC, cur[s].def, NULL, CD_FLAT, 0, &g_chain_arr[s][pc], pc);
                                            if (ri >= 0) { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; continue; }
                                        }
                                        /* M2.38: the arrival is a DIFFERENT
                                         * deferred record — the union of
                                         * two >12-value records. Represent
                                         * it as a UNION record over BOTH
                                         * records when the merged true
                                         * set fits the 32-cap: the carry
                                         * record is live by construction,
                                         * and the arrival record must
                                         * pass the M2.36 liveness check
                                         * (no write to its transitive DAG
                                         * since creation) to be
                                         * materializable at the dispatch. */
                                        if (g_chain_arr[s][pc].def >= 0 &&
                                            chain_def_live(g_chain_arr[s][pc].def)) {
                                            int ri = chain_def_alloc_union(cur, CD_REC, cur[s].def, NULL, CD_REC, (int32_t)g_chain_arr[s][pc].def, NULL, pc);
                                            if (ri >= 0) { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; continue; }
                                        }
                                        chain_unknown(&cur[s]);
                                        continue;
                                    }
                                    if (g_chain_arr[s][pc].def >= 0) {
                                        /* M2.36: a deferred ARRIVAL meets a
                                         * flat carry — the mirror of M2.35.
                                         * The union is still the record
                                         * when (a) the record is LIVE — no
                                         * write to its transitive DAG since
                                         * its creation (a write REPLACES a
                                         * leaf, so materializing over it
                                         * would be a wrong set; head-unions
                                         * only widen, a sound superset) —
                                         * and (b) the flat carry is
                                         * contained in the record's true
                                         * set (the carry path contributes
                                         * nothing new). */
                                        if (cur[s].def < 0 && !cur[s].unk) {
                                            int16_t rec = g_chain_arr[s][pc].def;
                                            if (chain_def_live(rec) &&
                                                chain_flat_in_def_idx(cur, rec, &cur[s])) {
                                                cur[s] = g_chain_arr[s][pc];
                                                continue;
                                            }
                                            /* M2.37: the flat carry is NOT
                                             * contained in the LIVE arrival
                                             * record — the mirror of the
                                             * M2.35 fallback: a UNION record
                                             * when it fits the 32-cap (an
                                             * empty carry still collapses,
                                             * byte-identical to M2.36). */
                                            if (chain_def_live(rec) && cur[s].n > 0) {
                                                int ri = chain_def_alloc_union(cur, CD_REC, rec, NULL, CD_FLAT, 0, &cur[s], pc);
                                                if (ri >= 0) { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; continue; }
                                            }
                                        }
                                        chain_unknown(&cur[s]);
                                        continue;
                                    }
                                    {
                                        /* M2.41: both sides FLAT — a plain
                                         * merge, capped at TX_AR_CHAIN_MAX.
                                         * When the union exceeds the cap
                                         * (sets that the old cap would
                                         * have deferred, now flat), freeze
                                         * it as a PRE-MERGED union record
                                         * op(flat(F1), flat(F2)) so a
                                         * >cap flat join still feeds the
                                         * chain instead of collapsing to
                                         * UNKNOWN. Both sides are frozen
                                         * as they stand at this head —
                                         * the carry's and the arrival's
                                         * sets are exact for their paths —
                                         * so the merged store is the
                                         * join's true set. An empty or
                                         * unknown side keeps the old
                                         * collapse (the M2.34
                                         * first-iteration behavior).
                                         * M2.42: with TX_AR_CHAIN_MAX ==
                                         * TX_AR_CHAIN_BIG this branch is
                                         * UNREACHABLE — a >64 union can
                                         * never chain (the emission gate
                                         * rejects ncand > 64), and the
                                         * allocator's cap-check returns
                                         * -1, so it collapses exactly as
                                         * the plain merge would. */
                                        struct ChainSet carry = cur[s];
                                        const struct ChainSet* arr = &g_chain_arr[s][pc];
                                        chain_merge(&cur[s], arr);
                                        if (cur[s].unk && carry.n > 0 && !carry.unk &&
                                            arr->n > 0 && !arr->unk) {
                                            int ri = chain_def_alloc_union(cur, CD_FLAT, 0, &carry,
                                                                           CD_FLAT, 0, arr, pc);
                                            if (ri >= 0) { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; }
                                        }
                                    }
                                }
                            }
                            if (pc == dyn_pc) {
                                /* capture the candidate set at the dispatch.
                                 * M2.30: a deferred pair product is
                                 * materialized BIG here — the sources are
                                 * provably unchanged (every write or union
                                 * of a source flattened it eagerly), so
                                 * this is exactly the product's set. */
                                int sr = g_chain_slot[ra];
                                struct ChainBig flat;
                                chain_flatten_big(&flat, cur, sr);
                                struct ChainBig* c = &flat;
                                if (c->n != g_chain_bigsnap.n || c->unk != g_chain_bigsnap.unk) changed = 1;
                                else for (uint8_t i = 0; i < c->n; i++)
                                    if (c->v[i] != g_chain_bigsnap.v[i]) { changed = 1; break; }
                                g_chain_bigsnap = *c;
                            }
                            if (op == OP_ENTER) {
                                for (int s = 0; s < ntr; s++) { chain_unknown(&cur[s]); g_chain_last_write[s] = (int32_t)pc; }  /* fresh frame */
                                carry = 1;
                            } else if (op == OP_RET) {
                                carry = 0;                /* terminal */
                            } else if (op == OP_BR) {
                                int64_t t = (int64_t)pc + 1 + w_imm28(w);
                                if (t >= 0 && t < (int64_t)hdr.num_instr)
                                    for (int s = 0; s < ntr; s++) chain_deliver(&g_chain_arr[s][(uint32_t)t], cur, s);
                                carry = 0;
                            } else if (op == OP_BC) {
                                int64_t t = (int64_t)pc + 1 + w_imm28(w);
                                if (t >= 0 && t < (int64_t)hdr.num_instr)
                                    for (int s = 0; s < ntr; s++) chain_deliver(&g_chain_arr[s][(uint32_t)t], cur, s);
                                carry = 1;
                            } else if (op == OP_CALL) {
                                /* callee entry is opaque (args + fresh frame);
                                 * the return clobbers r0 only. M2.30: the
                                 * r0 clobber is a WRITE — flatten deferred
                                 * forms reading r0's slot first. */
                                int64_t t = (int64_t)pc + 1 + w_imm28(w);
                                if (t >= 0 && t < (int64_t)hdr.num_instr)
                                    for (int s = 0; s < ntr; s++) {
                                        struct ChainSet unk = { .n = 0, .unk = 1 };
                                        chain_merge(&g_chain_arr[s][(uint32_t)t], &unk);
                                    }
                                if (g_chain_slot[0] >= 0) {
                                    chain_prewrite(cur, ntr, g_chain_slot[0]);
                                    chain_unknown(&cur[g_chain_slot[0]]);
                                    g_chain_last_write[g_chain_slot[0]] = (int32_t)pc;   /* M2.36 */
                                }
                                carry = 1;
                            } else if (op == OP_JMPR) {
                                if (g_jmpr_fold[pc] >= 0) {
                                    uint32_t t = (uint32_t)g_jmpr_fold[pc];
                                    if (t < hdr.num_instr)
                                        for (int s = 0; s < ntr; s++) chain_deliver(&g_chain_arr[s][t], cur, s);
                                    carry = (t == pc + 1);    /* fall-through fold continues linearly */
                                } else {
                                    carry = 0;                /* dynamic dispatch: terminal */
                                }
                            } else if (ar_writes_rd(op) && rd < TX_AR_MAX_REGS &&
                                       g_chain_slot[rd] >= 0) {
                                int s = g_chain_slot[rd];
                                chain_prewrite(cur, ntr, s);   /* M2.30: flatten deferred forms that read this slot */
                                g_chain_last_write[s] = (int32_t)pc;   /* M2.36: this pc is the slot's last write */
                                if (op == OP_LOADI) {
                                    chain_singleton(&cur[s], (uint32_t)w_imm28(w));
                                } else if (op == OP_LOADI64) {
                                    uint32_t idx = w_rb_raw(w);
                                    if (idx < hdr.num_literals) chain_singleton(&cur[s], (uint32_t)literals[idx]);
                                    else chain_unknown(&cur[s]);
                                } else if (op == OP_MOV) {
                                    int ss = (w_ra(w) < TX_AR_MAX_REGS) ? g_chain_slot[w_ra(w)] : -1;
                                    if (ss >= 0) cur[s] = cur[ss];
                                    else chain_unknown(&cur[s]);
                                } else if (op == OP_ADD || op == OP_SUB || op == OP_MUL ||
                                           op == OP_AND || op == OP_OR || op == OP_XOR ||
                                           op == OP_SHL || op == OP_SHR || op == OP_SAR) {
                                    /* M2.27/M2.28: S(rd) = image of S(ra)
                                     * under the op and the immediate (imm
                                     * form) or S(rb) (register form) —
                                     * plain 64-bit, never faults,
                                     * type-agnostic. M2.47: the shifts
                                     * join — a constant-amount shift of a
                                     * known set re-derives (the M2.3
                                     * argument); the amount masks mod 64,
                                     * SHR logical, SAR arithmetic. */
                                    int s_a = (w_ra(w) < TX_AR_MAX_REGS) ? g_chain_slot[w_ra(w)] : -1;
                                    struct ChainSet unk = { .n = 0, .unk = 1 };
                                    const struct ChainSet* sa = (s_a >= 0) ? &cur[s_a] : &unk;
                                    const struct ChainSet* sb = &unk;
                                    int use_imm = (w_flags(w) & FLAG_IMM) != 0;
                                    int s_b = -1;
                                    if (!use_imm) {
                                        s_b = (w_rb_reg(w) < TX_AR_MAX_REGS) ? g_chain_slot[w_rb_reg(w)] : -1;
                                        if (s_b >= 0) sb = &cur[s_b];
                                    }
                                    /* M2.32: a DEFERRED source is not
                                     * flattened for the deferral decision —
                                     * its true set exceeds the flat cap, so
                                     * the capped flatten would collapse to
                                     * UNKNOWN and kill the chain. A product
                                     * over a deferred source always exceeds
                                     * 12 (the source alone does), so it is
                                     * DEFERRED directly: the record operands
                                     * reference the source slots — or, when
                                     * rd aliases a source (ADD r1, r1, r4),
                                     * the OLD record of that slot, which is
                                     * what makes the in-place form sound
                                     * (the immutable record, not cur[r1]
                                     * itself). M2.41: an in-place FLAT
                                     * source (against a DEFERRED other
                                     * source) freezes as a CD_FLAT operand —
                                     * the aliased source's set as it stands
                                     * at this instruction — instead of
                                     * falling to UNKNOWN; the flat-cap
                                     * overflow path (M2.30) got the same
                                     * freeze, so an in-place product over
                                     * a >cap flat set defers soundly.
                                     * M2.42: with equal caps this is
                                     * UNREACHABLE — an in-place product
                                     * whose image exceeds 64 materializes
                                     * > 64 candidates, which the emission
                                     * gate rejects. */
                                    int da = (s_a >= 0 && cur[s_a].def >= 0);
                                    int db = (!use_imm && s_b >= 0 && cur[s_b].def >= 0);
                                    if (!use_imm && (da || db) && !sa->unk && !sb->unk) {
                                        int rd_is_a = (rd == w_ra(w));
                                        int rd_is_b = (!use_imm && rd == w_rb_reg(w));
                                        if ((rd_is_a && !da) || (rd_is_b && !db)) {
                                            /* M2.41: an in-place FLAT source cannot be a CD_SLOT
                                             * operand (a self-cycle), but it CAN be frozen as a
                                             * CD_FLAT operand — the aliased source's flat set as
                                             * it stands at THIS instruction, exactly the runtime
                                             * value rd holds — so an in-place product against a
                                             * flat source (with the other source deferred) defers
                                             * soundly instead of collapsing to UNKNOWN. The other
                                             * operand keeps the normal encoding (CD_REC when
                                             * deferred, CD_SLOT when flat). */
                                            uint8_t ka = (rd_is_a && !da) ? CD_FLAT : (da ? CD_REC : CD_SLOT);
                                            int32_t oa = (rd_is_a && !da) ? 0 : (da ? (int32_t)cur[s_a].def : (int32_t)s_a);
                                            uint8_t kb = (rd_is_b && !db) ? CD_FLAT : (db ? CD_REC : CD_SLOT);
                                            int32_t ob = (rd_is_b && !db) ? 0 : (db ? (int32_t)cur[s_b].def : (int32_t)s_b);
                                            int ri = chain_def_alloc(op, ka, oa, kb, ob,
                                                                     (rd_is_a && !da) ? sa : sb, pc);
                                            if (ri < 0) chain_unknown(&cur[s]);
                                            else { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; }
                                        } else {
                                            uint8_t ka = da ? CD_REC : CD_SLOT;
                                            int32_t oa = da ? (int32_t)cur[s_a].def : (int32_t)s_a;
                                            uint8_t kb = db ? CD_REC : CD_SLOT;
                                            int32_t ob = db ? (int32_t)cur[s_b].def : (int32_t)s_b;
                                            int ri = chain_def_alloc(op, ka, oa, kb, ob, NULL, pc);
                                            if (ri < 0) chain_unknown(&cur[s]);
                                            else { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; }
                                        }
                                    } else if (use_imm && da && !sa->unk) {
                                        /* M2.33: an IMM-form product over a
                                         * deferred source. The image has the
                                         * source's cardinality (> 12 — a
                                         * flattened source would collapse to
                                         * UNKNOWN), so it DEFERS directly
                                         * with the imm as a CD_IMM operand.
                                         * The source is referenced as its
                                         * immutable record (in-place or not —
                                         * a record ref is safe either way). */
                                        int ri = chain_def_alloc(op, CD_REC, (int32_t)cur[s_a].def, CD_IMM, (int32_t)w_imm28(w), NULL, pc);
                                        if (ri < 0) chain_unknown(&cur[s]);
                                        else { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; }
                                    } else {
                                        struct ChainSet fa, fb;
                                        if (sa->def >= 0) { chain_flatten(&fa, cur, s_a); sa = &fa; }
                                        if (!use_imm && sb->def >= 0) { chain_flatten(&fb, cur, s_b); sb = &fb; }
                                        /* M2.41: snapshot the OLD flat set of any source that rd
                                         * ALIASES (the image write below clobbers the slot) — the
                                         * freeze for the in-place overflow defer below. */
                                        struct ChainSet old_a = { .n = 0, .unk = 0 }, old_b = { .n = 0, .unk = 0 };
                                        int keep_a = 0, keep_b = 0;
                                        if (rd == w_ra(w) && s_a >= 0 && sa->def < 0 && !sa->unk) { old_a = *sa; keep_a = 1; }
                                        if (!use_imm && rd == w_rb_reg(w) && s_b >= 0 && sb->def < 0 && !sb->unk) { old_b = *sb; keep_b = 1; }
                                        chain_img_alu(&cur[s], op, use_imm, w_imm28(w), sa, sb);
                                        /* M2.30: a register-form product that
                                         * OVERFLOWED the flat cap with known
                                         * FLAT sources is DEFERRED — the
                                         * walk records the op + source
                                         * slots, and the dispatch re-
                                         * computes the product into the BIG
                                         * candidate set. Sound only while
                                         * the sources are untouched (every
                                         * write/union flattens eagerly) and
                                         * rd != ra/rb — M2.41 relaxes the
                                         * in-place exclusion: an ALIASED flat
                                         * source is frozen as a CD_FLAT
                                         * operand (snapshotted above, before
                                         * the image write), so an in-place
                                         * product over a >cap flat set
                                         * (chain13's second product) defers
                                         * soundly instead of collapsing.
                                         * M2.42: with equal caps this is
                                         * UNREACHABLE — the image overflow
                                         * means > 64 candidates, which can
                                         * never chain. */
                                        if (!use_imm && cur[s].unk && s_a >= 0 && s_b >= 0) {
                                            int rd_is_a = (rd == w_ra(w));
                                            int rd_is_b = (!use_imm && rd == w_rb_reg(w));
                                            int a_ok = rd_is_a ? keep_a : (!sa->unk && sa->def < 0);
                                            int b_ok = rd_is_b ? keep_b : (!sb->unk && sb->def < 0);
                                            if (a_ok && b_ok) {
                                                if (rd_is_a || rd_is_b) {
                                                    uint8_t ka = rd_is_a ? CD_FLAT : CD_SLOT;
                                                    int32_t oa = rd_is_a ? 0 : (int32_t)s_a;
                                                    uint8_t kb = rd_is_b ? CD_FLAT : CD_SLOT;
                                                    int32_t ob = rd_is_b ? 0 : (int32_t)s_b;
                                                    int ri = chain_def_alloc(op, ka, oa, kb, ob,
                                                                             rd_is_a ? &old_a : &old_b, pc);
                                                    if (ri < 0) chain_unknown(&cur[s]);
                                                    else { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; }
                                                } else {
                                                    int ri = chain_def_alloc(op, CD_SLOT, (int16_t)s_a, CD_SLOT, (int16_t)s_b, NULL, pc);
                                                    if (ri < 0) chain_unknown(&cur[s]);
                                                    else { cur[s].def = (int16_t)ri; cur[s].n = 0; cur[s].unk = 0; }
                                                }
                                            }
                                        }
                                    }
                                } else if (op == OP_NOT || op == OP_NEG) {
                                    /* M2.48: UNARY image — { f(a) : a in
                                     * S(a) }, f = NOT (~a) or NEG (-a),
                                     * plain 64-bit, never faults,
                                     * type-agnostic (the M2.2 argument).
                                     * Reuse the image's immediate path
                                     * with a dummy imm (the unary eval
                                     * ignores bv). A deferred source is
                                     * unreachable under the M2.42
                                     * equal-caps proof (no record is ever
                                     * created); fall to UNKNOWN there to
                                     * stay conservative. */
                                    int s_a = (w_ra(w) < TX_AR_MAX_REGS) ? g_chain_slot[w_ra(w)] : -1;
                                    struct ChainSet unk = { .n = 0, .unk = 1 };
                                    const struct ChainSet* sa = (s_a >= 0) ? &cur[s_a] : &unk;
                                    if (sa->def >= 0) chain_unknown(&cur[s]);
                                    else chain_img_alu(&cur[s], op, 1, 0, sa, &unk);
                                } else if (op == OP_CMP) {
                                    cur[s].n = 2; cur[s].unk = 0; cur[s].def = -1;
                                    cur[s].v[0] = 0; cur[s].v[1] = 1;
                                } else {
                                    chain_unknown(&cur[s]);   /* opaque writer: no chain */
                                }
                                carry = 1;
                            } else {
                                carry = 1;
                            }
                        }
                    }
                    /* M2.58: the 64-iteration cap can STOP a still-growing
                     * walk. A BACKWARD-branch lattice (a loop whose
                     * back-edge delivers the loop-carried set to its own
                     * head) is path-insensitive in this walk — the head
                     * set grows by one value per iteration and the
                     * fixpoint of an accumulator loop is UNBOUNDED, so
                     * the walk never converges and the cap truncates it.
                     * The truncated snapshot is UNSOUND as a candidate
                     * set: a runtime index outside the truncated tail is
                     * legitimate (the loop can iterate more times) yet
                     * the chain would UDF-trap it (jmpr_chain34's probe
                     * proved it: runtime 165, pre-fix snapshot {101..164},
                     * the chain missed and faulted). Unconverged ->
                     * UNKNOWN, the exact-or-conservative discipline: the
                     * table dispatches every index. `changed` is only
                     * ever set by the bigsnap comparator, so a still-1
                     * flag here means the snapshot was still moving at
                     * the last iteration. Byte-invisible for every
                     * CONVERGED program (the existing corpus: forward
                     * join lattices settle within a handful of
                     * iterations) — the size gate proves it. */
                    if (changed) chain_unknown_big(&g_chain_bigsnap);
                    /* the converged snapshot at dyn_pc is the candidate set */
                    if (!g_chain_bigsnap.unk && g_chain_bigsnap.n > 0 &&
                        g_chain_bigsnap.n <= TX_AR_CHAIN_BIG) {
                        for (uint8_t i = 0; i < g_chain_bigsnap.n; i++)
                            if (g_chain_bigsnap.v[i] < hdr.num_instr && g_chain_ncand < TX_AR_CHAIN_BIG)
                                g_chain_cand[g_chain_ncand++] = g_chain_bigsnap.v[i];
                        /* M2.29: ADAPTIVE chain. Emit the chain only when
                         * it actually beats the runtime table on bytes.
                         * Chain: one subs+b.eq pair per in-range candidate
                         * (2 words) + the UDF fall-through (1 word); the
                         * index load and the pre-dispatch flush are shared
                         * with the table path, so they cancel. Table: the
                         * 10-word dispatch (movz num_instr, subs cmp, cset,
                         * cbz, 2-word li32 base, add-shift, ldr W, br, UDF)
                         * + num_instr 4-byte entries. Chain wins iff
                         * 8*n + 4 < 40 + 4*num_instr. ncand==0 (jmpr_oob's
                         * constant 999) always wins — a bare UDF. Every
                         * in-range candidate is < num_instr <= 4096, so
                         * all fit the imm12 of subs — no movz+subs form is
                         * ever needed. M2.29 raised the walk cap 8 -> 12
                         * and added the cost gate; M2.30 lets a deferred
                         * pair product deliver 13-64 candidates here (the
                         * BIG cap, raised from 32 by M2.40) when the same
                         * gate says the chain wins. */
                        if (g_chain_ncand <= TX_AR_CHAIN_BIG &&
                            8*g_chain_ncand + 4 < 40 + 4*(int)hdr.num_instr)
                            g_chain_active = 1;
                    }
                }
            }
        }
    }

    for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
        /* M2.23: DEAD-CODE ELIMINATION. A pc with zero live predecessors
         * — g_npred[pc]==0 per the fold-aware BFS (entries are roots,
         * reachable with count 0, excluded here by !g_reach) — never
         * executes under g_alloc=1 (the closure of §10.52: every
         * reachable JMPR folds, so the reachable set is exact), so its
         * ENTIRE body — ENTER frame load, arithmetic, branches, CALL
         * sites, dynamic JMPR table paths, RETs — is dead code and is
         * not emitted at all (M2.22 dropped only the frame load and the
         * flushes; this supersedes that). g_instr_off[pc] still records
         * a byte position (the next live code) so the JMPR-table and
         * patch passes stay well-defined; nothing ever dispatches to a
         * dead pc. Naive mode is untouched: a dynamic dispatch can land
         * anywhere, so no pc is provably dead there. */
        if (g_alloc && !g_reach[pc] && g_npred[pc] == 0) {
            g_instr_off[pc] = cb.len;
            continue;
        }
        /* rule 1 (fall-through half), hoisted to the block head: a head
         * reached by fall-through must arrive with current memory and no
         * cache. Emitting the spill HERE (before pc's first word, i.e.
         * directly after the previous instruction's code) is the same
         * byte position the end-of-loop flush occupied, and the M2.22
         * dead-head skip needs no guard: a dead head never reaches this
         * line (eliminated above), and a live head always owes the
         * flush (g_reach is exact under the closure). */
        if (pc >= 1 && g_pc_target[pc]) cache_flush(&cb);
        g_instr_off[pc] = cb.len;
        /* The in-flight result reservation is per-instruction state: a
         * stale g_resv_slot from the previous instruction would make
         * get_operand() skip the eviction of the register it clobbers as
         * an operand base (mem_ops_native: STORE's base load destroyed
         * x9 while the directory still claimed r1 was resident there,
         * and the source read then went to memory and stored 0). */
        g_resv_slot = -1;
        g_resv_reuse = 0;
        g_resv_guest = -1;
        g_cur_pc = pc;
        uint64_t w = instrs[pc];
        uint8_t op = w_op(w);

        if (op == OP_LOADI64) {
            uint32_t idx = w_rb_raw(w);
            if (idx >= hdr.num_literals) return TX_AR_ERR_LITERAL_OUT_OF_RANGE;
            uint16_t rd = w_rd(w);
            if (rd >= TX_AR_MAX_REGS) return TX_AR_ERR_REG_OUT_OF_RANGE;
            int v = cache_reserve(&cb, rd, -1, -1);
            emit_li64(&cb, result_host(v), literals[idx]);
            store_result(&cb, rd);
        } else if (op == OP_BR) {
            /* rule 1: the branch path and the fall-through both land on
             * code that must see current memory and an empty cache. */
            cache_flush(&cb);
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            op_b(&cb, target);
        } else if (op == OP_BC) {
            uint16_t ra = w_ra(w);
            if (ra >= TX_AR_MAX_REGS) return TX_AR_ERR_REG_OUT_OF_RANGE;
            cache_flush(&cb);
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            get_operand(&cb, ra, X_T0, 0);   /* no reservation in BC, hint inert */
            if (w_flags(w) & FLAG_INVERT) op_cbz(&cb, target, X_T0);
            else                          op_cbnz(&cb, target, X_T0);
        } else if (op == OP_CALL) {
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            /* CALL has no rD in the ISA — the call site always stores the
             * result into the CALLER's r0, same as x86/RV64. M1: flush
             * first — the argument marshaling reads slots 0..7 and the
             * arg-tag mask from memory, which must be current. */
            cache_flush(&cb);
            emit_call_site(&cb, target, 0);
        } else if (op == OP_JMPR) {
            /* Gap Remediation SIMI Phase 14 / M2. Two shapes:
             *  - FOLDED (g_jmpr_fold[pc] >= 0): the index is provably a
             *    constant in [0, num_instr) — a plain direct branch. The
             *    target pc is a block head (the pre-pass merged it into
             *    g_pc_target), so rule 1 gives it an empty cache, and no
             *    bounds check is needed because the range is proved. This
             *    is what lets JMPR-bearing programs keep the cache.
             *  - DYNAMIC: the runtime table + bounds-check path below,
             *    moved here from emit_instr (it needs the pc table). The
             *    bounds check (t0 < num_instr) is the non-negotiable CFI
             *    requirement of ISA §16: cmp+cset lo then cbz to UDF #0. */
            uint16_t ra = w_ra(w);
            if (ra >= TX_AR_MAX_REGS) return TX_AR_ERR_REG_OUT_OF_RANGE;
            if (g_jmpr_fold[pc] >= 0) {
                /* M2.12: a fall-through fold (target == pc+1) is a dead
                 * branch — the target is the next instruction, reached by
                 * falling through. Emit NOTHING for it (M2.13: the fold
                 * fixpoint now folds whole CHAINS of these — each target
                 * is unmarked by the merge, so every link folds). The
                 * hoisted block-head flush at the target (loop top) still
                 * fires when the target kept its mark (it has other
                 * incoming edges — those land with an empty cache exactly
                 * as before), and is SKIPPED when the coalescing pre-pass
                 * fused the target (the fold was the only way in), so the
                 * x9/x10/x11 cache survives the join and the register
                 * frame is loaded once, not once per block. */
                if (!(g_alloc && g_fold_fall[pc])) {
                    cache_flush(&cb);
                    op_b(&cb, (uint32_t)g_jmpr_fold[pc]);
                }
            } else if (g_chain_active) {
                /* M2.25: inline compare-and-branch chain — the single
                 * dynamic JMPR's index is provably one of g_chain_cand.
                 * Each in-range candidate is a cmp + b.eq pair straight
                 * to its code; the fall-through is UDF #0 — the same
                 * non-negotiable CFI word the bounds check targets. An
                 * index outside the provable set contradicts the
                 * analysis (dead in practice, still a fault if reached),
                 * and an out-of-range constant like jmpr_oob's 999 has
                 * no branch and lands here, exactly as the table's
                 * bounds check would fault. No table is emitted at all
                 * (g_njmpr_li_pos stays 0 — the chain needs no base or
                 * indirect load). */
                cache_flush(&cb);
                if (g_chain_ncand > 0) {
                    ld_slot(&cb, X_T0, ra);                         /* t0 = index */
                    for (int i = 0; i < g_chain_ncand; i++) {
                        e32(&cb, enc_subs_imm(31, X_T0, g_chain_cand[i]));  /* cmp t0, #c */
                        op_b_cond(&cb, g_chain_cand[i], 0);                /* b.eq target (EQ=0) */
                    }
                }
                op_illegal(&cb);
            } else {
                cache_flush(&cb);
                ld_slot(&cb, X_T0, ra);                             /* t0 = target abstract pc */
                e32(&cb, enc_movz(X_T2, (uint16_t)g_num_instr, 0)); /* num_instr <= 4096 fits one movz */
                e32(&cb, enc_subs_shift(31, X_T0, X_T2, 0, 0));     /* cmp x9, x11 */
                e32(&cb, enc_cset(X_T1, 3));                        /* cset x10, lo — unsigned less */
                uint32_t cbz_pos = emit_cbz_placeholder(&cb);       /* cbz t1, .oob */
                if (g_njmpr_li_pos >= TX_AR_MAX_FIXUPS) return TX_AR_ERR_TOO_MANY_FIXUPS;
                g_jmpr_li_pos[g_njmpr_li_pos++] = cb.len;           /* 1-word PC-relative adr table-base placeholder (M3) */
                e32(&cb, 0);
                e32(&cb, enc_add_shift(X_T1, X_T2, X_T0, 0, 2));    /* t1 = base + (target << 2) — entry address */
                e32(&cb, enc_ldrsw(X_T1, X_T1, 0));                 /* t1 = s32 table[target] — (offset − table_off), sign-extended */
                e32(&cb, enc_add_shift(X_T2, X_T2, X_T1, 0, 0));    /* t2 = base + rel — absolute target (both conventions) */
                e32(&cb, enc_br(X_T2));                             /* jump — never falls through */
                patch_local_cbz(&cb, cbz_pos, X_T1);                /* .oob: */
                op_illegal(&cb);                                    /* UDF #0 — non-negotiable CFI per ISA §16 */
            }
        } else {
            int rc = emit_instr(&cb, w);
            if (rc != TX_AR_OK) return rc;
        }
        if (cb.overflow) return TX_AR_ERR_BUF_FULL;
    }

    /* One trampoline per exported entry, appended after the body. */
    uint32_t found_off = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < hdr.num_entries; i++) {
        uint32_t tramp_off = cb.len;
        emit_trampoline(&cb, entries[i].offset, scratch_ptr, namepool_ptr);
        if (cb.overflow) return TX_AR_ERR_BUF_FULL;
        if (ar_streq(entries[i].name, entry_name)) found_off = tramp_off;
    }
    if (found_off == 0xFFFFFFFFu) return TX_AR_ERR_ENTRY_NOT_FOUND;

    /* Gap Remediation SIMI Phase 14: reserve + backfill the JMPR jump
     * table, placed AFTER the trampolines. M3: table entries are signed
     * 32-bit RELATIVE offsets — (g_instr_off[pc] − jmpr_table_off), the
     * distance from the table base to the target code — and the dispatch
     * reaches the base PC-RELATIVELY (adr), so the same blob executes
     * under a64_exec's guest convention (addresses are byte offsets into
     * out_buf) AND on real A64 (addresses are actual): base + rel lands
     * on the target in both. The pre-M3 entries were bare out_buf
     * offsets that only the guest convention could branch to — a real
     * `br` to 0xNNN faulted (M3's 12 dynamic-JMPR fixtures).
     * g_instr_off[] is complete for every real pc at this point (the
     * main loop above has finished). */
    uint32_t jmpr_table_off = 0;
    if (g_njmpr_li_pos > 0) {
        /* M2.24: 4-byte entries. Every dispatch target is a pc in
         * [0, num_instr) (the bounds check above), the table sits after
         * the trampolines (so offset − table_off is always negative and
         * ldrsw's sign-extension is what the dispatch wants), and the
         * base placeholder shrinks to ONE word (adr) — same 5-word
         * dispatch as the pre-M3 shape, so no fixture's size moves.
         * This HALVES the naive-mode table (M2.23's 8-byte entries
         * mirrored RV64's 64-bit slots). Under g_alloc=1 the table is
         * absent entirely — every emitted JMPR folds, so no dynamic path
         * (and no table) exists (M2.23). */
        jmpr_table_off = cb.len;
        for (uint32_t q = 0; q < hdr.num_instr; q++) e32(&cb, 0);
        if (cb.overflow) return TX_AR_ERR_BUF_FULL;
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            int32_t rel = (int32_t)((int64_t)g_instr_off[pc] - (int64_t)jmpr_table_off);
            for (int b = 0; b < 4; b++)
                out_buf[jmpr_table_off + pc*4 + b] = (uint8_t)(((uint32_t)rel >> (8*b)) & 0xFF);
        }
        for (uint32_t i = 0; i < g_njmpr_li_pos; i++)
            patch_adr(out_buf, g_jmpr_li_pos[i], jmpr_table_off);
    }

    /* Patch pass: branch/call targets (B/BL/CBZ/CBNZ). imm26/imm19 are
     * word offsets; range checks mirror the ISA's encodable spans. */
    for (uint32_t i = 0; i < g_nfixups; i++) {
        uint32_t pos = g_fixups[i].instr_pos;
        uint32_t target_pc = g_fixups[i].target_pc;
        if (target_pc >= hdr.num_instr) return TX_AR_ERR_BAD_OPCODE;
        int64_t off = (int64_t)g_instr_off[target_pc] - (int64_t)pos;
        if (off & 3) return TX_AR_ERR_BAD_OPCODE;   /* every instruction is 4-byte aligned */
        int64_t words = off >> 2;
        uint32_t word;
        switch (g_fixups[i].kind) {
            case FIX_B:
            case FIX_BL:
                if (words < -0x2000000ll || words > 0x1FFFFFFll) return TX_AR_ERR_BRANCH_OUT_OF_RANGE;
                word = (g_fixups[i].kind == FIX_B) ? enc_b((int32_t)words)
                                                   : enc_bl((int32_t)words);
                break;
            case FIX_B_COND:   /* M2.25: the chain's cmp + b.eq pairs */
                if (words < -0x40000ll || words > 0x3FFFFll) return TX_AR_ERR_BRANCH_OUT_OF_RANGE;
                word = enc_b_cond(g_fixups[i].rt, (int32_t)words);
                break;
            default: /* FIX_CBZ / FIX_CBNZ */
                if (words < -0x40000ll || words > 0x3FFFFll) return TX_AR_ERR_BRANCH_OUT_OF_RANGE;
                word = (g_fixups[i].kind == FIX_CBZ) ? enc_cbz(g_fixups[i].rt, (int32_t)words)
                                                     : enc_cbnz(g_fixups[i].rt, (int32_t)words);
                break;
        }
        patch32(out_buf, pos, word);
    }

    *out_len = cb.len;
    *entry_off = found_off;
    return TX_AR_OK;
}

const char* simi_arm_strerror(int code) {
    switch (code) {
        case TX_AR_OK: return "ok";
        case TX_AR_ERR_BAD_HEADER: return "bad or corrupt SIMI header";
        case TX_AR_ERR_TOO_MANY_INSTR: return "instruction count out of range";
        case TX_AR_ERR_REG_OUT_OF_RANGE: return "register index >= TX_AR_MAX_REGS (64)";
        case TX_AR_ERR_BUF_FULL: return "output buffer too small";
        case TX_AR_ERR_BAD_OPCODE: return "unsupported or malformed opcode";
        case TX_AR_ERR_TOO_MANY_FIXUPS: return "too many branch/call fixups";
        case TX_AR_ERR_ENTRY_NOT_FOUND: return "requested entry name not in object";
        case TX_AR_ERR_LITERAL_OUT_OF_RANGE: return "LOADI64 literal pool index out of range";
        case TX_AR_ERR_TOO_MANY_LITERALS: return "too many 64-bit constants";
        case TX_AR_ERR_BRANCH_OUT_OF_RANGE: return "branch/call target exceeds B/BL/CBZ/CBNZ encodable range";
        case TX_AR_ERR_NAME_OUT_OF_RANGE: return "RESOLVE name-pool index out of range";
        case TX_AR_ERR_FLOAT_UNSUPPORTED: return "operand combination has no float meaning (immediate operand, or float MOD)";
        default: return "unknown error";
    }
}
