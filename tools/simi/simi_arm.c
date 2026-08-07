/*
 * simi_arm.c — M0: SIMI-to-AArch64 (A64) native translator, the third
 * real target after x86-64 (Phase 3) and RV64 (Phase 5). See simi_arm.h
 * for the framing and docs/AeroSLS-SIMI-ARM-Backend-Plan-v0.1.md for the
 * phase plan. No libc dependency, mirrors simi_riscv.c's discipline so
 * this compiles unmodified under host gcc today and an aarch64
 * freestanding cross-compiler later (neither available in this sandbox —
 * correctness is established the Phase 5 way, via a64_exec.c actually
 * executing the emitted bytes; see the plan doc §2).
 *
 * ─── A64 design decisions, stated up front ─────────────────────────────
 *   - Same naive load-operate-store codegen as x86/RV64: every symbolic
 *     register is a stack slot, not a real allocated host register, and
 *     no value is ever assumed to survive across a SIMI instruction
 *     boundary. x9/x10/x11 are the "working registers" (t0/t1/t2), the
 *     direct analog of RV64's x5/x6/x7.
 *   - FRAME: SP-relative addressing, one structural deviation from the
 *     plan doc's §4.2 "xBASE base-register" sketch, adopted at build time
 *     for a concrete reason. The prologue is byte-for-byte the RV64 shape
 *     (sub sp,sp,#16; stp-style pair save; sub sp,sp,#576), so during a
 *     procedure body sp == entry_sp - 592. A64's unsigned scaled load/
 *     store immediate is non-negative, but every slot/tag offset relative
 *     to sp is POSITIVE: slot i at sp + (568 - 8i) (imm12 71-i, i=0..63),
 *     tag i at sp + i (imm12 i). The plan's xBASE version (x28 = x29-592,
 *     slots/tags off x28) has the same positive-offset property but needs
 *     a dedicated register that every callee's prologue necessarily
 *     re-bases and no epilogue restores — SP-relative addressing removes
 *     the register, the recompute-after-every-call, and the audit burden
 *     in one move. The physical frame is identical to RV64's (entry-24-8i
 *     for slot i, entry-592+i for tag i); only the anchor register
 *     differs. All offsets fit the 12-bit scaled immediate; 592 is a
 *     valid sub-immediate; the total sp delta (16 + 576 = 592 = 37x16)
 *     keeps SP 16-byte aligned at every point.
 *   - CONSTANTS: movz/movk materialize ANY 64-bit constant in at most
 *     four fixed-width instructions. This deletes the RV64 literal pool
 *     wholesale — no g_literals, no pool emission, no second patch pass
 *     for auipc+ld pairs. The one value not known at emit time (JMPR's
 *     table base offset) gets a fixed 4-word movz+3xmovk placeholder
 *     patched in a tiny final pass.
 *   - BRANCHES: B/BL (imm26) for BR/CALL, CBZ/CBNZ (imm19) for BC —
 *     A64's branch-only conditionality is a perfect fit for SIMI's
 *     test-a-register-and-jump BC; the plan's §4 note that M0 ships
 *     flag-free with NZCV entering at M1's CMP is amended at build time:
 *     branch_cmp.simi and loop_sum.simi both need CMP, so M0 models the
 *     full NZCV flags (subs + cset) — see emit_cmp() below.
 *   - CMP's 10 relations: A64 has no slt analog; the synthesis is
 *     `cmp xN, xM` (subs xzr, xN, xM) followed by `cset xN, <cond>`,
 *     one NZCV-read per relation, exactly the two-instruction idiom real
 *     compilers emit. NZCV is the ONE piece of implicit machine state
 *     this subset carries, and it is always write-consumed adjacently
 *     (emit_cmp, RESOLVE's tag test, JMPR's bounds test, the prologue's
 *     argument-tag extraction) — no flag value ever crosses a SIMI
 *     instruction boundary, preserving SIMI design principle #2.
 *   - DIV/MOD: A64 has div but no remainder instruction; MOD is the
 *     compiler idiom sdiv/udiv + msub (t = t - (t/d)*d), which yields
 *     the dividend-sign remainder RV64's rem/remu produce.
 *   - HOST CALLS (RESOLVE/OBJSIZE/OBJTYPE): real `blr xN` to a baked-in
 *     address, x0 as arg/result — A64's analog of x86's movabs+call reg
 *     and RV64's auipc+ld+jalr. The verifier's a64_exec.c redirects
 *     sentinel addresses (AR_EXEC_HOSTFN_BASE) to host C functions.
 *   - ILLEGAL WORD: 0x00000000 is A64's permanently-reserved UDF #0
 *     (unconditional undefined instruction) — the direct analog of
 *     RV64's reserved all-zeros word and x86's ud2; it is what JMPR's
 *     out-of-bounds path lands on, per the ISA §16 CFI requirement.
 *   - v0.3 (Phase 7): capability-tag region, same contract as the other
 *     targets — one byte per symbolic register at sp + i, RESOLVE tags
 *     iff nonzero (cmp+cset ne), MOV propagates, every other
 *     register-writing opcode clears, OBJSIZE/OBJTYPE require the tag
 *     before calling the runtime. The argument-tag mask rides in the
 *     outgoing-arg area at entry_sp + 64 exactly like RV64's.
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
static void e64(struct CodeBuf* cb, uint64_t v) {
    for (int i = 0; i < 8; i++) e8(cb, (uint8_t)((v >> (8*i)) & 0xFF));
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
/* Add/subtract immediate, sh=0: 1 00/10/11 100010 0 imm12 Rn Rd. */
static uint32_t enc_add_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
    return 0x91000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | rd;
}
static uint32_t enc_sub_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
    return 0xD1000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | rd;
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
/* Load/store register, unsigned scaled immediate: size:2 111 0 01 opc imm12. */
static uint32_t enc_ldr (uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xF9400000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_str (uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xF9000000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrb(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x39400000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_strb(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x39000000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrh(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x79400000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_strh(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x79000000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldr_w(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xB9400000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_str_w(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xB9000000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsb(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x39800000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsh(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0x79800000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
static uint32_t enc_ldrsw(uint8_t rt, uint8_t rn, uint16_t imm12) { return 0xB9800000u | ((uint32_t)(imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | rt; }
/* Branches. imm26/imm19 are in units of 4 bytes (word offsets). */
static uint32_t enc_b   (int32_t imm26) { return 0x14000000u | ((uint32_t)imm26 & 0x03FFFFFFu); }
static uint32_t enc_bl  (int32_t imm26) { return 0x94000000u | ((uint32_t)imm26 & 0x03FFFFFFu); }
static uint32_t enc_cbz (uint8_t rt, int32_t imm19) { return 0xB4000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | rt; }
static uint32_t enc_cbnz(uint8_t rt, int32_t imm19) { return 0xB5000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | rt; }
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
 *      occupant first, and the round-robin victim of cache_reserve is
 *      never spilled by the in-flight instruction's own operand fetches
 *      (g_resv_slot). A64 reads all operands before writing the result,
 *      so the result register may alias an operand register.
 * The naive (g_alloc=0) path degrades these helpers to exactly the M0
 * sequences, so the JMPR fallback is exercised by jmpr_basic/jmpr_oob.
 */
#define AR_CACHE_N 3
static int g_alloc;                    /* 0 when any JMPR cannot be folded to a direct branch */
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
static int      g_jmpr_fold[4096];      /* per-JMPR-pc folded target, or -1 */
static int      g_jmpr_fold_prev[4096]; /* fixpoint convergence snapshot (file-scope like the other arrays) */

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
 * directory pointed at a fresh one. Only when rd is not resident does the
 * round-robin victim get evicted (spilled) and claimed. */
static int cache_reserve(struct CodeBuf* cb, int rd) {
    if (!g_alloc) { g_resv_slot = -1; g_resv_reuse = 0; return -1; }
    int v = cache_find(rd);
    if (v >= 0) {
        g_resv_reuse = 1;
    } else {
        v = g_cache_round;
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
/* Materialize `imm` into a scratch register that is neither X_T0 (first
 * operand) nor the reserved result register; returns that register. */
static uint8_t materialize_imm(struct CodeBuf* cb, uint64_t imm) {
    if (g_resv_slot == 1) { clobber_scratch(cb, X_T2); emit_li64(cb, X_T2, imm); return X_T2; }
    clobber_scratch(cb, X_T1); emit_li64(cb, X_T1, imm); return X_T1;
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
enum { FIX_B, FIX_BL, FIX_CBZ, FIX_CBNZ };
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
 * Each JMPR site emits a fixed 4-word movz+3xmovk placeholder for the
 * table base offset (the only constant not known at emit time); the
 * position of each placeholder is recorded here and rewritten in the
 * final pass once the table's offset is known. */
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

/* ─── CMP: synthesize all 10 relations from cmp+cset (§4) ────────────────
 * Operands in t0 (lhs), t1 (rhs); result (0/1) ends up in `rd` (M1: the
 * reserved result register, which may alias an operand register — A64
 * reads all operands before writing). `cmp x0, x1` (subs xzr, x0, x1)
 * writes NZCV; `cset xd, cond` reads it. The A64 condition code for each
 * SIMI relation: EQ=0 NE=1 HS=2 LO=3 HI=8 LS=9 GE=10 LT=11 GT=12 LE=13. */
static int emit_cmp(struct CodeBuf* cb, int rel, uint8_t rd) {
    e32(cb, enc_subs_shift(31, X_T0, X_T1, 0, 0));   /* cmp x9, x10 */
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
        case T_I32: case T_F32: e32(cb, enc_ldrsw(rd, rn, imm12)); break;
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
static int type_shift(int t) {   /* log2(byte width) — used for PTRADD scaling */
    switch (t) {
        case T_I8: case T_U8: case T_BOOL: return 0;
        case T_I16: case T_U16: return 1;
        case T_I32: case T_U32: case T_F32: return 2;
        default: return 3;
    }
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
        int v = cache_reserve(cb, rd);
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
         op==OP_PTRADD) && !(flags & FLAG_IMM) && w_rb_reg(w) >= TX_AR_MAX_REGS)
        return TX_AR_ERR_REG_OUT_OF_RANGE;
    /* Gap Remediation SIMI Phase 10: this translator has no float codegen
     * at all (scoped out of M0 — see TX_AR_ERR_FLOAT_UNSUPPORTED's comment
     * in simi_arm.h). Every opcode that would have float meaning on x86
     * must reject T_F32/T_F64 explicitly here, up front, rather than
     * falling into codegen that reads the slot as a plain 64-bit integer
     * and silently produces a wrong answer on real IEEE bit patterns. */
    if ((type==T_F64 || type==T_F32) &&
        (op==OP_ADD||op==OP_SUB||op==OP_MUL||op==OP_DIV||op==OP_MOD||
         op==OP_NEG||op==OP_CMP))
        return TX_AR_ERR_FLOAT_UNSUPPORTED;

    switch (op) {
    case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR:
    case OP_MUL: case OP_SHL: case OP_SHR: case OP_SAR: {
        int v = cache_reserve(cb, rd);
        uint8_t rh = result_host(v);
        int rd_live = (rd == ra) || (!(flags & FLAG_IMM) && rd == w_rb_reg(w));
        get_operand(cb, ra, X_T0, rd_live);
        uint8_t rhs = X_T1;
        if (flags & FLAG_IMM) rhs = materialize_imm(cb, (uint64_t)(int64_t)w_imm28(w));
        else                 get_operand(cb, w_rb_reg(w), X_T1, rd_live);
        switch (op) {
            case OP_ADD: e32(cb, enc_add_shift(rh, X_T0, rhs, 0, 0)); break;
            case OP_SUB: e32(cb, enc_sub_shift(rh, X_T0, rhs, 0, 0)); break;
            case OP_AND: e32(cb, enc_and_shift(rh, X_T0, rhs, 0, 0)); break;
            case OP_OR:  e32(cb, enc_orr_shift(rh, X_T0, rhs, 0, 0)); break;
            case OP_XOR: e32(cb, enc_eor_shift(rh, X_T0, rhs, 0, 0)); break;
            case OP_MUL: e32(cb, enc_madd(rh, X_T0, rhs, 31)); break;  /* MUL */
            case OP_SHL: e32(cb, enc_lslv(rh, X_T0, rhs)); break;
            case OP_SHR: e32(cb, enc_lsrv(rh, X_T0, rhs)); break;
            case OP_SAR: e32(cb, enc_asrv(rh, X_T0, rhs)); break;
        }
        store_result(cb, rd);
        break;
    }
    case OP_DIV: case OP_MOD: {
        int v = cache_reserve(cb, rd);
        uint8_t rh = result_host(v);
        int rd_live = (rd == ra) || (!(flags & FLAG_IMM) && rd == w_rb_reg(w));
        get_operand(cb, ra, X_T0, rd_live);
        uint8_t rhs = X_T1;
        if (flags & FLAG_IMM) rhs = materialize_imm(cb, (uint64_t)(int64_t)w_imm28(w));
        else                 get_operand(cb, w_rb_reg(w), X_T1, rd_live);
        int sgn = ar_type_signed(type);
        /* A64 has no remainder instruction: sdiv/udiv then msub folds the
         * quotient back (t = t - (t/d)*d), yielding the dividend-sign
         * remainder RV64's rem/remu produce. M1 keeps the dividend and
         * divisor live in x9/x10, so the quotient uses x0 (dead between
         * hostfn calls — every such call flushes first) instead of x11,
         * which may be the reserved result register. */
        if (op == OP_DIV) {
            e32(cb, sgn ? enc_sdiv(rh, X_T0, rhs) : enc_udiv(rh, X_T0, rhs));
        } else {
            e32(cb, sgn ? enc_sdiv(X_ARG, X_T0, rhs) : enc_udiv(X_ARG, X_T0, rhs));
            e32(cb, enc_msub(rh, X_ARG, rhs, X_T0));
        }
        store_result(cb, rd);
        break;
    }
    case OP_NOT:
    case OP_NEG: {
        int v = cache_reserve(cb, rd);
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
            int v = cache_reserve(cb, rd);
            uint8_t rh = result_host(v);
            /* Subtle: when ra is resident at the reserve victim slot,
             * i0 == v and the copy below is skipped — correct because
             * reserve's spill STORES the old value but never clears the
             * register, so it still physically holds ra. Fragile-looking
             * but sound: a store cannot modify a register. */
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
        int v = cache_reserve(cb, rd);
        emit_li64(cb, result_host(v), (uint64_t)(int64_t)w_imm28(w));
        store_result(cb, rd);
        break;
    }
    case OP_LOADI64:
        return TX_AR_ERR_BAD_OPCODE; /* unreachable: handled specially in translate(), needs literal pool value */
    case OP_CMP: {
        int v = cache_reserve(cb, rd);
        uint8_t rh = result_host(v);
        int rd_live = (rd == ra) || (rd == w_rb_reg(w));
        get_operand(cb, ra, X_T0, rd_live);
        get_operand(cb, w_rb_reg(w), X_T1, rd_live);
        if (flags >= 10 || !emit_cmp(cb, flags, rh)) return TX_AR_ERR_BAD_OPCODE;
        store_result(cb, rd);
        break;
    }
    case OP_BR: return TX_AR_ERR_BAD_OPCODE;  /* handled specially in translate() (needs pc) */
    case OP_LEA: {
        int v = cache_reserve(cb, rd);
        uint8_t rh = result_host(v);
        get_operand(cb, ra, X_T0, rd == ra);
        int32_t disp = w_imm28(w);
        if (disp >= 0 && disp <= 4095) {
            e32(cb, enc_add_imm(rh, X_T0, (uint32_t)disp));
        } else {
            uint8_t sc = materialize_imm(cb, (uint64_t)(int64_t)disp);
            e32(cb, enc_add_shift(rh, X_T0, sc, 0, 0));
        }
        store_result(cb, rd);
        break;
    }
    case OP_PTRADD: {
        /* v0.3 (Phase 7): always untagged — pointer arithmetic must never
         * yield a capability even when rA is currently tagged. */
        int v = cache_reserve(cb, rd);
        uint8_t rh = result_host(v);
        int rd_live = (rd == ra) || (!(flags & FLAG_IMM) && rd == w_rb_reg(w));
        get_operand(cb, ra, X_T0, rd_live);   /* base */
        int shift = type_shift(type);
        if (flags & FLAG_IMM) {
            int64_t scaled = (int64_t)w_imm28(w) << shift;
            if (scaled >= 0 && scaled <= 4095) {
                e32(cb, enc_add_imm(rh, X_T0, (uint32_t)scaled));
            } else {
                uint8_t sc = materialize_imm(cb, (uint64_t)scaled);
                e32(cb, enc_add_shift(rh, X_T0, sc, 0, 0));
            }
        } else {
            get_operand(cb, w_rb_reg(w), X_T1, rd_live);
            /* add xd, x9, x10, lsl #shift — A64's shifted register operand
             * folds the scale into the add, one instruction where RV64
             * needs a separate slli. */
            e32(cb, enc_add_shift(rh, X_T0, X_T1, 0, (uint8_t)shift));
        }
        store_result(cb, rd);
        break;
    }
    case OP_LOAD: {
        int v = cache_reserve(cb, rd);
        uint8_t rh = result_host(v);
        get_operand(cb, ra, X_T0, rd == ra);   /* base */
        int32_t disp = w_imm28(w);
        int sh = type_shift(type);
        if (disp >= 0 && (disp & ((1 << sh) - 1)) == 0 && (disp >> sh) <= 0xFFF) {
            /* the offset folds into the scaled immediate — no address math */
            load_typed(cb, type, rh, X_T0, (uint16_t)(disp >> sh));
        } else {
            clobber_scratch(cb, X_T0);
            uint8_t sc = materialize_imm(cb, (uint64_t)(int64_t)disp);
            e32(cb, enc_add_shift(X_T0, X_T0, sc, 0, 0));
            load_typed(cb, type, rh, X_T0, 0);
        }
        store_result(cb, rd);
        break;
    }
    case OP_STORE: {
        /* No reservation in STORE (g_resv_slot == -1), so the hint is
         * inert — passed as 0 for the signature. */
        get_operand(cb, ra, X_T0, 0);   /* base */
        get_operand(cb, rd, X_T1, 0);   /* rd holds the *source* value register, same convention as x86 */
        int32_t disp = w_imm28(w);
        int sh = type_shift(type);
        if (disp >= 0 && (disp & ((1 << sh) - 1)) == 0 && (disp >> sh) <= 0xFFF) {
            store_typed(cb, type, X_T1, X_T0, (uint16_t)(disp >> sh));
        } else {
            clobber_scratch(cb, X_T0);
            clobber_scratch(cb, X_T2);
            emit_li64(cb, X_T2, (uint64_t)(int64_t)disp);
            e32(cb, enc_add_shift(X_T0, X_T0, X_T2, 0, 0));
            store_typed(cb, type, X_T1, X_T0, 0);
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
    default: return TX_AR_ERR_BAD_OPCODE;
    }
    return TX_AR_OK;
}

/* Rewrite a 4-word movz+3xmovk placeholder (JMPR table base) in place. */
static void patch_li64(uint8_t* out_buf, uint32_t pos, uint64_t imm) {
    patch32(out_buf, pos,      enc_movz(X_T2, (uint16_t)(imm & 0xFFFF), 0));
    patch32(out_buf, pos + 4,  enc_movk(X_T2, (uint16_t)((imm >> 16) & 0xFFFF), 1));
    patch32(out_buf, pos + 8,  enc_movk(X_T2, (uint16_t)((imm >> 32) & 0xFFFF), 2));
    patch32(out_buf, pos + 12, enc_movk(X_T2, (uint16_t)((imm >> 48) & 0xFFFF), 3));
}

/* ─── Top-level translate: two passes (emit + patch fixups) ─────────────
 * No literal pool pass — movz/movk made it unnecessary; the only post-emit
 * rewrite besides branch/call fixups is the JMPR table-base li64 (see
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
    for (;;) {
        for (int i = 0; i < 4096; i++) g_jmpr_fold_prev[i] = g_jmpr_fold[i];
        for (int i = 0; i < TX_AR_MAX_REGS; i++) g_const_known[i] = 0;
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            if (g_pc_target[pc]) {   /* block head: constants do not survive a join */
                for (int i = 0; i < TX_AR_MAX_REGS; i++) g_const_known[i] = 0;
            }
            uint64_t w = instrs[pc];
            uint8_t op = w_op(w);
            uint16_t rd = w_rd(w), ra = w_ra(w);
            if (op == OP_LOADI) {
                if (rd < TX_AR_MAX_REGS) { g_const_known[rd] = 1; g_const_val[rd] = (uint64_t)(int64_t)w_imm28(w); }
            } else if (op == OP_LOADI64) {
                uint32_t idx = w_rb_raw(w);
                if (idx < hdr.num_literals && rd < TX_AR_MAX_REGS) {
                    g_const_known[rd] = 1; g_const_val[rd] = literals[idx];
                }
            } else if (op == OP_MOV) {
                if (rd < TX_AR_MAX_REGS) {
                    g_const_known[rd] = (ra < TX_AR_MAX_REGS) ? g_const_known[ra] : 0;
                    g_const_val[rd] = (ra < TX_AR_MAX_REGS) ? g_const_val[ra] : 0;
                }
            } else if (op == OP_ADD || op == OP_SUB) {
                /* M2.1: fold arithmetic constants. The emitted ALU is a
                 * plain 64-bit add/sub for every SIMI type (no width or
                 * signedness rounding — the translator never truncates),
                 * and the imm28 is sign-extended exactly as
                 * materialize_imm does, so the 64-bit wrap of a+b / a-b
                 * here is bit-identical to runtime. Deliberately limited
                 * to ADD/SUB: DIV/MOD would change behavior on a
                 * translate-time division by zero; MUL would fold just as
                 * safely (plain 64-bit multiply, never faults, type-
                 * agnostic like ADD/SUB) but is left with the rest of the
                 * ALU family for the same later pass — a conservative
                 * omission, not a soundness one. */
                if (rd < TX_AR_MAX_REGS) {
                    int k = (ra < TX_AR_MAX_REGS) ? g_const_known[ra] : 0;
                    uint64_t a = (ra < TX_AR_MAX_REGS) ? g_const_val[ra] : 0;
                    int fold = 0;
                    uint64_t b = 0;
                    if (w_flags(w) & FLAG_IMM) {
                        b = (uint64_t)(int64_t)w_imm28(w);
                        fold = k;
                    } else {
                        uint16_t rb = w_rb_reg(w);
                        fold = k && rb < TX_AR_MAX_REGS && g_const_known[rb];
                        if (fold) b = g_const_val[rb];
                    }
                    if (fold) { g_const_known[rd] = 1; g_const_val[rd] = (op == OP_ADD) ? a + b : a - b; }
                    else      { g_const_known[rd] = 0; }
                }
            } else if (op == OP_ENTER || op == OP_RET) {
                for (int i = 0; i < TX_AR_MAX_REGS; i++) g_const_known[i] = 0;  /* prologue/terminal: no constant survives */
            } else if (op == OP_JMPR) {
                g_jmpr_fold[pc] = (ra < TX_AR_MAX_REGS && g_const_known[ra] &&
                                   g_const_val[ra] < hdr.num_instr) ? (int)g_const_val[ra] : -1;
            } else if (rd < TX_AR_MAX_REGS) {
                g_const_known[rd] = 0;   /* every other register-writing opcode breaks the constant */
            }
        }
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++)   /* fold targets are block heads (rule 1) */
            if (g_jmpr_fold[pc] >= 0) g_pc_target[(uint32_t)g_jmpr_fold[pc]] = 1;
        int same = 1;
        for (int i = 0; i < 4096; i++)
            if (g_jmpr_fold[i] != g_jmpr_fold_prev[i]) { same = 0; break; }
        if (same) break;
        /* The retroactive-split case — a fold target landing inside
         * another JMPR's chain, before that JMPR's constant source — is
         * covered by this re-scan (the added reset un-folds that JMPR)
         * but has no dedicated test: constructing it requires two JMPRs
         * tangled with branches in a way the corpus does not contain.
         * The reset argument holds regardless; see plan doc §10. */
    }
    /* g_alloc = cache enabled iff every JMPR folds. One non-foldable JMPR
     * can reach any pc, which makes every pc a block head — no cache.
     * Folded JMPRs are direct branches either way (they need no cache). */
    g_alloc = 1;
    for (uint32_t pc = 0; pc < hdr.num_instr; pc++)
        if (w_op(instrs[pc]) == OP_JMPR && g_jmpr_fold[pc] < 0) { g_alloc = 0; break; }
    for (int i = 0; i < AR_CACHE_N; i++) g_cache_guest[i] = -1;
    g_cache_round = 0;
    g_resv_slot = -1;

    for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
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
        uint64_t w = instrs[pc];
        uint8_t op = w_op(w);

        if (op == OP_LOADI64) {
            uint32_t idx = w_rb_raw(w);
            if (idx >= hdr.num_literals) return TX_AR_ERR_LITERAL_OUT_OF_RANGE;
            uint16_t rd = w_rd(w);
            if (rd >= TX_AR_MAX_REGS) return TX_AR_ERR_REG_OUT_OF_RANGE;
            int v = cache_reserve(&cb, rd);
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
                cache_flush(&cb);
                op_b(&cb, (uint32_t)g_jmpr_fold[pc]);
            } else {
                cache_flush(&cb);
                ld_slot(&cb, X_T0, ra);                             /* t0 = target abstract pc */
                e32(&cb, enc_movz(X_T2, (uint16_t)g_num_instr, 0)); /* num_instr <= 4096 fits one movz */
                e32(&cb, enc_subs_shift(31, X_T0, X_T2, 0, 0));     /* cmp x9, x11 */
                e32(&cb, enc_cset(X_T1, 3));                        /* cset x10, lo — unsigned less */
                uint32_t cbz_pos = emit_cbz_placeholder(&cb);       /* cbz t1, .oob */
                if (g_njmpr_li_pos >= TX_AR_MAX_FIXUPS) return TX_AR_ERR_TOO_MANY_FIXUPS;
                g_jmpr_li_pos[g_njmpr_li_pos++] = cb.len;           /* 4-word table-base placeholder */
                e32(&cb, 0); e32(&cb, 0); e32(&cb, 0); e32(&cb, 0);
                e32(&cb, enc_add_shift(X_T2, X_T2, X_T0, 0, 3));    /* t2 = base + (target << 3) */
                e32(&cb, enc_ldr(X_T2, X_T2, 0));                   /* t2 = table[target] */
                e32(&cb, enc_br(X_T2));                             /* jump — never falls through */
                patch_local_cbz(&cb, cbz_pos, X_T1);                /* .oob: */
                op_illegal(&cb);                                    /* UDF #0 — non-negotiable CFI per ISA §16 */
            }
        } else {
            int rc = emit_instr(&cb, w);
            if (rc != TX_AR_OK) return rc;
        }
        /* rule 1 (fall-through half): the next pc is a block head — the
         * fall-through must arrive with current memory and no cache. */
        if (pc + 1 < hdr.num_instr && g_pc_target[pc + 1]) cache_flush(&cb);
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
     * table, placed AFTER the trampolines. Table entries are plain byte
     * offsets into out_buf (g_instr_off[pc]), NOT host addresses — see
     * g_jmpr_li_pos's design note above. g_instr_off[] is complete for
     * every real pc at this point (the main loop above has finished). */
    uint32_t jmpr_table_off = 0;
    if (g_njmpr_li_pos > 0) {
        jmpr_table_off = cb.len;
        for (uint32_t q = 0; q < hdr.num_instr; q++) e64(&cb, 0);
        if (cb.overflow) return TX_AR_ERR_BUF_FULL;
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            uint64_t off = (uint64_t)g_instr_off[pc];
            for (int b = 0; b < 8; b++)
                out_buf[jmpr_table_off + pc*8 + b] = (uint8_t)((off >> (8*b)) & 0xFF);
        }
        for (uint32_t i = 0; i < g_njmpr_li_pos; i++)
            patch_li64(out_buf, g_jmpr_li_pos[i], (uint64_t)jmpr_table_off);
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
        case TX_AR_ERR_FLOAT_UNSUPPORTED: return "float (T_F32/T_F64) not supported by the A64 translator (scoped out of M0)";
        default: return "unknown error";
    }
}
