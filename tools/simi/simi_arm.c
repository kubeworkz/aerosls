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
/* Materialize a signed displacement into `reg`: one movz when it fits the
 * 16-bit unsigned field (the overwhelmingly common case for the small
 * LOAD/STORE/LEA offsets the test corpus uses), li64 otherwise. A64's
 * add/sub-immediate rn=31 means SP, not XZR, so unlike RV64 there is no
 * `addi reg, x0, disp` shortcut — movz is the correct 1-instruction path. */
static void emit_li_disp(struct CodeBuf* cb, uint8_t reg, int32_t disp) {
    if (disp >= 0 && disp <= 65535) e32(cb, enc_movz(reg, (uint16_t)disp, 0));
    else emit_li64(cb, reg, (uint64_t)(int64_t)disp);
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
 * Operands in t0 (lhs), t1 (rhs); result (0/1) always ends up in t0.
 * `cmp x0, x1` (subs xzr, x0, x1) writes NZCV; `cset x0, cond` reads it.
 * The A64 condition code for each SIMI relation:
 *   EQ=0 NE=1 HS=2 LO=3 HI=8 LS=9 GE=10 LT=11 GT=12 LE=13. */
static int emit_cmp(struct CodeBuf* cb, int rel) {
    e32(cb, enc_subs_shift(31, X_T0, X_T1, 0, 0));   /* cmp x9, x10 */
    switch (rel) {
        case REL_EQ:  e32(cb, enc_cset(X_T0, 0)); break;   /* eq */
        case REL_NE:  e32(cb, enc_cset(X_T0, 1)); break;   /* ne */
        case REL_LT:  e32(cb, enc_cset(X_T0, 11)); break;  /* lt */
        case REL_GT:  e32(cb, enc_cset(X_T0, 12)); break;  /* gt */
        case REL_LE:  e32(cb, enc_cset(X_T0, 13)); break;  /* le */
        case REL_GE:  e32(cb, enc_cset(X_T0, 10)); break;  /* ge */
        case REL_LTU: e32(cb, enc_cset(X_T0, 3)); break;   /* lo (cc) */
        case REL_GTU: e32(cb, enc_cset(X_T0, 8)); break;   /* hi */
        case REL_LEU: e32(cb, enc_cset(X_T0, 9)); break;   /* ls */
        case REL_GEU: e32(cb, enc_cset(X_T0, 2)); break;   /* hs (cs) */
        default: return 0;
    }
    return 1;
}

/* ─── LOAD/STORE width+signedness (mirrors type_width() in simi_x86.c) —
 * A64 has a native load/store opcode for every SIMI width/signedness
 * combination (opc 01 zero-extends, opc 10 sign-extends, 32-bit rt
 * zeroes the upper half), so like RV64 the mapping is 1:1 with no x86
 * movsx/movzx zoo. */
static void load_typed(struct CodeBuf* cb, int type, uint8_t rd, uint8_t rn) {
    switch (type) {
        case T_I8:  e32(cb, enc_ldrsb(rd, rn, 0)); break;
        case T_U8:  case T_BOOL: e32(cb, enc_ldrb(rd, rn, 0)); break;
        case T_I16: e32(cb, enc_ldrsh(rd, rn, 0)); break;
        case T_U16: e32(cb, enc_ldrh(rd, rn, 0)); break;
        case T_I32: case T_F32: e32(cb, enc_ldrsw(rd, rn, 0)); break;
        case T_U32: e32(cb, enc_ldr_w(rd, rn, 0)); break;
        default:    e32(cb, enc_ldr(rd, rn, 0)); break;   /* i64/u64/f64/ptr */
    }
}
static void store_typed(struct CodeBuf* cb, int type, uint8_t rs, uint8_t rn) {
    switch (type) {
        case T_I8: case T_U8: case T_BOOL: e32(cb, enc_strb(rs, rn, 0)); break;
        case T_I16: case T_U16:            e32(cb, enc_strh(rs, rn, 0)); break;
        case T_I32: case T_U32: case T_F32: e32(cb, enc_str_w(rs, rn, 0)); break;
        default: e32(cb, enc_str(rs, rn, 0)); break;
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
    st_slot(cb, X_T0, rd);
    st_tag(cb, X_T1, rd);   /* Gap Remediation SIMI Phase 12: propagate r0's tag */
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
        ld_slot(cb, X_T0, ra);
        if (flags & FLAG_IMM) emit_li64(cb, X_T1, (uint64_t)(int64_t)w_imm28(w));
        else                  ld_slot(cb, X_T1, w_rb_reg(w));
        switch (op) {
            case OP_ADD: e32(cb, enc_add_shift(X_T0, X_T0, X_T1, 0, 0)); break;
            case OP_SUB: e32(cb, enc_sub_shift(X_T0, X_T0, X_T1, 0, 0)); break;
            case OP_AND: e32(cb, enc_and_shift(X_T0, X_T0, X_T1, 0, 0)); break;
            case OP_OR:  e32(cb, enc_orr_shift(X_T0, X_T0, X_T1, 0, 0)); break;
            case OP_XOR: e32(cb, enc_eor_shift(X_T0, X_T0, X_T1, 0, 0)); break;
            case OP_MUL: e32(cb, enc_madd(X_T0, X_T0, X_T1, 31)); break;  /* MUL */
            case OP_SHL: e32(cb, enc_lslv(X_T0, X_T0, X_T1)); break;
            case OP_SHR: e32(cb, enc_lsrv(X_T0, X_T0, X_T1)); break;
            case OP_SAR: e32(cb, enc_asrv(X_T0, X_T0, X_T1)); break;
        }
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_DIV: case OP_MOD: {
        ld_slot(cb, X_T0, ra);
        if (flags & FLAG_IMM) emit_li64(cb, X_T1, (uint64_t)(int64_t)w_imm28(w));
        else                  ld_slot(cb, X_T1, w_rb_reg(w));
        int sgn = ar_type_signed(type);
        /* A64 has no remainder instruction: sdiv/udiv then msub folds the
         * quotient back (t = t - (t/d)*d), yielding the dividend-sign
         * remainder RV64's rem/remu produce. */
        if (op == OP_DIV) {
            e32(cb, sgn ? enc_sdiv(X_T0, X_T0, X_T1) : enc_udiv(X_T0, X_T0, X_T1));
        } else {
            e32(cb, sgn ? enc_sdiv(X_T2, X_T0, X_T1) : enc_udiv(X_T2, X_T0, X_T1));
            e32(cb, enc_msub(X_T0, X_T2, X_T1, X_T0));
        }
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_NOT:
        ld_slot(cb, X_T0, ra);
        e32(cb, enc_orn(X_T0, X_T0));               /* mvn x9, x9 */
        st_slot_untag(cb, X_T0, rd);
        break;
    case OP_NEG:
        ld_slot(cb, X_T0, ra);
        e32(cb, enc_sub_shift(X_T0, 31, X_T0, 0, 0));  /* sub x9, xzr, x9 */
        st_slot_untag(cb, X_T0, rd);
        break;
    case OP_MOV:
        /* v0.3 (Phase 7): the one opcode besides RESOLVE that can produce
         * a tagged register — propagating an existing capability is still
         * a valid capability. */
        ld_slot(cb, X_T0, ra);
        st_slot(cb, X_T0, rd);
        ld_tag(cb, X_T1, ra);
        st_tag(cb, X_T1, rd);
        break;
    case OP_LOADI:
        emit_li64(cb, X_T0, (uint64_t)(int64_t)w_imm28(w));
        st_slot_untag(cb, X_T0, rd);
        break;
    case OP_LOADI64:
        return TX_AR_ERR_BAD_OPCODE; /* unreachable: handled specially in translate(), needs literal pool value */
    case OP_CMP: {
        ld_slot(cb, X_T0, ra);
        ld_slot(cb, X_T1, w_rb_reg(w));
        if (flags >= 10 || !emit_cmp(cb, flags)) return TX_AR_ERR_BAD_OPCODE;
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_BR: return TX_AR_ERR_BAD_OPCODE;  /* handled specially in translate() (needs pc) */
    case OP_LEA: {
        ld_slot(cb, X_T0, ra);
        emit_li_disp(cb, X_T1, w_imm28(w));
        e32(cb, enc_add_shift(X_T0, X_T0, X_T1, 0, 0));
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_PTRADD: {
        /* v0.3 (Phase 7): always untagged — pointer arithmetic must never
         * yield a capability even when rA is currently tagged. */
        ld_slot(cb, X_T0, ra);   /* base */
        int shift = type_shift(type);
        if (flags & FLAG_IMM) {
            int64_t idx = w_imm28(w);
            emit_li64(cb, X_T1, (uint64_t)(idx << shift));
            e32(cb, enc_add_shift(X_T0, X_T0, X_T1, 0, 0));
        } else {
            ld_slot(cb, X_T1, w_rb_reg(w));
            /* add x9, x9, x10, lsl #shift — A64's shifted register operand
             * folds the scale into the add, one instruction where RV64
             * needs a separate slli. */
            e32(cb, enc_add_shift(X_T0, X_T0, X_T1, 0, (uint8_t)shift));
        }
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_LOAD: {
        ld_slot(cb, X_T0, ra);                        /* t0 = base pointer */
        emit_li_disp(cb, X_T1, w_imm28(w));
        e32(cb, enc_add_shift(X_T1, X_T0, X_T1, 0, 0));   /* t1 = effective address */
        load_typed(cb, type, X_T0, X_T1);             /* load into t0 */
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_STORE: {
        ld_slot(cb, X_T0, ra);                        /* t0 = base pointer */
        ld_slot(cb, X_T1, rd);                        /* rd holds the *source* value register, same convention as x86 */
        emit_li_disp(cb, X_T2, w_imm28(w));
        e32(cb, enc_add_shift(X_T2, X_T0, X_T2, 0, 0));   /* t2 = effective address */
        store_typed(cb, type, X_T1, X_T2);
        break;
    }
    case OP_ENTER: emit_prologue(cb); break;
    case OP_LEAVE: /* no-op directive, matches Phase 1 interpreter */ break;
    case OP_RESOLVE: {
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
    case OP_JMPR: {
        /* Gap Remediation SIMI Phase 14: indirect jump through the runtime
         * table of guest-space code offsets (see g_jmpr_li_pos's design
         * note above and the reservation+backfill glue in
         * simi_arm_translate()). Unlike BR/BC/CALL this needs no pc from
         * the caller and no global fixup entry — the target is only known
         * at runtime. The bounds check (t0 < num_instr) is the
         * non-negotiable CFI requirement of ISA §16: `cmp x9, x11; cset
         * x10, lo` then cbz to UDF #0 on the out-of-bounds path. */
        ld_slot(cb, X_T0, ra);                            /* t0 = target abstract pc */
        e32(cb, enc_movz(X_T2, (uint16_t)g_num_instr, 0)); /* num_instr <= 4096 fits one movz */
        e32(cb, enc_subs_shift(31, X_T0, X_T2, 0, 0));    /* cmp x9, x11 */
        e32(cb, enc_cset(X_T1, 3));                       /* cset x10, lo — unsigned less */
        uint32_t cbz_pos = emit_cbz_placeholder(cb); /* cbz t1, .oob */
        /* Table base: 4-word movz+3xmovk placeholder, patched once the
         * table's offset is known (see the final pass in translate()). */
        if (g_njmpr_li_pos >= TX_AR_MAX_FIXUPS) return TX_AR_ERR_TOO_MANY_FIXUPS;
        g_jmpr_li_pos[g_njmpr_li_pos++] = cb->len;
        e32(cb, 0); e32(cb, 0); e32(cb, 0); e32(cb, 0);
        e32(cb, enc_add_shift(X_T2, X_T2, X_T0, 0, 3));   /* t2 = base + (target << 3) */
        e32(cb, enc_ldr(X_T2, X_T2, 0));                  /* t2 = table[target] */
        e32(cb, enc_br(X_T2));                            /* jump — never falls through */
        patch_local_cbz(cb, cbz_pos, X_T1);               /* .oob: */
        op_illegal(cb);                                   /* UDF #0 — real undefined-instruction trap, non-negotiable CFI per ISA §16 */
        break;
    }
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

    for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
        g_instr_off[pc] = cb.len;
        uint64_t w = instrs[pc];
        uint8_t op = w_op(w);

        if (op == OP_LOADI64) {
            uint32_t idx = w_rb_raw(w);
            if (idx >= hdr.num_literals) return TX_AR_ERR_LITERAL_OUT_OF_RANGE;
            uint16_t rd = w_rd(w);
            if (rd >= TX_AR_MAX_REGS) return TX_AR_ERR_REG_OUT_OF_RANGE;
            emit_li64(&cb, X_T0, literals[idx]);
            st_slot_untag(&cb, X_T0, rd);
        } else if (op == OP_BR) {
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            op_b(&cb, target);
        } else if (op == OP_BC) {
            uint16_t ra = w_ra(w);
            if (ra >= TX_AR_MAX_REGS) return TX_AR_ERR_REG_OUT_OF_RANGE;
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            ld_slot(&cb, X_T0, ra);
            if (w_flags(w) & FLAG_INVERT) op_cbz(&cb, target, X_T0);
            else                          op_cbnz(&cb, target, X_T0);
        } else if (op == OP_CALL) {
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            /* CALL has no rD in the ISA — the call site always stores the
             * result into the CALLER's r0, same as x86/RV64. */
            emit_call_site(&cb, target, 0);
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
