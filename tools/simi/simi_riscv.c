/*
 * simi_riscv.c — Phase 5 SIMI-to-RV64 native translator. See simi_riscv.h
 * for the framing and AeroSLS-SIMI-ISA-v0.1.md §12 for design notes. No
 * libc dependency, mirrors simi_x86.c's discipline so this compiles
 * unmodified under host gcc today and a riscv64 freestanding cross-
 * compiler later (not available in this sandbox — see §12's verification
 * section for how correctness was actually established without one).
 *
 * ─── RV64 design decisions, stated up front ────────────────────────────
 *   - Same naive load-operate-store codegen as x86 (simi_x86.c): every
 *     symbolic register is a stack slot, not a real allocated host
 *     register. Slot i lives at [s0 - 8*(i+1)], s0 (the RV64 ABI frame
 *     pointer) playing the exact role x86's rbp plays.
 *   - t0/t1/t2 (x5-x7, caller-saved temporaries) are the "working
 *     registers" — the direct analog of x86's rax/rcx: every operand is
 *     reloaded from its stack slot at the start of each SIMI instruction's
 *     codegen and the result stored back immediately, no value is ever
 *     assumed to survive across a SIMI instruction boundary.
 *   - CALL/RET use real `jal ra,target` / `jalr x0,0(ra)` so nested and
 *     recursive SIMI calls get a real hardware-adjacent call chain, same
 *     as x86's real `call`/`ret` (§10). Unlike x86's `call`, `jal` does
 *     NOT push a return address onto the stack — it just clobbers `ra` —
 *     so every procedure's prologue/epilogue explicitly saves/restores
 *     `ra` around its own body, and the trampoline (which itself issues a
 *     `jal`) does the same around its one call to the entry procedure.
 *   - RV64 load/store/addi immediates are only 12 bits (±2048), far
 *     short of SIMI's 28-bit LOAD/STORE/LEA/PTRADD displacement field.
 *     Every displacement is therefore materialized into a register first
 *     (emit_li_disp: a plain ADDI if it fits in 12 bits, otherwise the
 *     same auipc+ld literal-pool technique used for 64-bit constants
 *     below) and added to the base pointer, rather than relying on the
 *     load/store instruction's own immediate field.
 *   - RV64 has no single-instruction 64-bit immediate load (no movabs
 *     equivalent). Every 64-bit constant this translator needs — LOADI64
 *     literals and the trampoline's scratch_ptr — is appended to a
 *     literal pool placed after all emitted code, and loaded via the
 *     standard `auipc rd,%pcrel_hi; ld rd,%pcrel_lo(rd)` pattern real
 *     RISC-V toolchains use for `la`/extern-symbol loads. Two-pass, same
 *     spirit as the branch/call fixup pass: emit placeholders + collect
 *     literals during the main pass, then patch both once the literal
 *     pool's final address is known.
 *   - The RV64 M-extension (mul/div/divu/rem/remu) is used directly for
 *     MUL/DIV/MOD — no cqo/xor-edx-style paired-register dance is needed
 *     here the way x86's IDIV/DIV require; RV64's div/rem are independent
 *     single-result R-type instructions. Simpler than x86, not harder.
 *   - CMP's 10 relations are synthesized from slt/sltu/xori (0-2
 *     instructions each) — see emit_cmp() below. RV64 has no flags
 *     register at all (not even the transient one x86's CMP sets), so
 *     there was never a "fuse CMP into the branch" option to weigh the
 *     way there was on x86 (§8 Q2) — register-based comparison is simply
 *     how RV64 works natively, SIMI's design principle #2 (no implicit
 *     machine state) costs nothing extra on this target.
 *
 *   - v0.3 (Phase 7) adds the capability-tag region: a second, TX_RV_MAX_
 *     REGS-byte block sits directly below (more negative than) the
 *     register-slot region, one byte per symbolic register, mirroring
 *     x86's TX_TAG_BASE/tag_disp exactly (same contract: RESOLVE tags the
 *     destination iff the result is nonzero, MOV propagates, every other
 *     register-writing opcode clears the destination's tag, OBJSIZE/
 *     OBJTYPE require the operand's tag before ever calling the runtime
 *     function). RV64's `sltu rd,x0,a0` computes "a0 != 0" in one
 *     instruction — the direct analog of x86's test+setne, no branch
 *     needed for RESOLVE. OBJSIZE/OBJTYPE do need a real branch (skip the
 *     runtime call entirely when untagged) — synthesized with the same
 *     local (intra-instruction), immediately-patched placeholder+patch
 *     technique as the global JAL/BEQ/BNE fixups use, just not registered
 *     in the global fixup table (see emit_beqz_placeholder/emit_jal_
 *     placeholder/patch_local_beqz/patch_local_jal below).
 */
#include "simi_riscv.h"

/* ─── Local no-libc helpers ────────────────────────────────────────────── */
static int rv_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void rv_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}

/* Mirrors simi_isa.h's opcode/type/relation numbering exactly (Phase 1) —
 * same enums as simi_x86.c, kept as an independent copy per this file's
 * zero-shared-header policy (see simi_riscv.h's top comment). */
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

static int rv_type_signed(int t) { return t==T_I8||t==T_I16||t==T_I32||t==T_I64; }

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

/* ─── RV64 registers used by this translator (ABI names / numbers) ──────── */
#define X_ZERO 0
#define X_RA   1
#define X_SP   2
#define X_T0   5
#define X_T1   6
#define X_T2   7
#define X_S0   8
#define X_A0   10   /* v0.3 (Phase 6): real RV64 ABI arg0/return register, used
                      * only around RESOLVE/OBJSIZE/OBJTYPE runtime calls —
                      * every other codegen path here uses t0-t2 exclusively. */

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

/* ─── RV64 instruction word encoders (standard bit layouts) ─────────────── */
static uint32_t enc_r(uint8_t funct7, uint8_t rs2, uint8_t rs1, uint8_t funct3, uint8_t rd, uint8_t opcode) {
    return ((uint32_t)funct7<<25)|((uint32_t)rs2<<20)|((uint32_t)rs1<<15)|((uint32_t)funct3<<12)|((uint32_t)rd<<7)|opcode;
}
static uint32_t enc_i(int32_t imm12, uint8_t rs1, uint8_t funct3, uint8_t rd, uint8_t opcode) {
    return (((uint32_t)imm12 & 0xFFFu)<<20)|((uint32_t)rs1<<15)|((uint32_t)funct3<<12)|((uint32_t)rd<<7)|opcode;
}
static uint32_t enc_s(int32_t imm12, uint8_t rs2, uint8_t rs1, uint8_t funct3, uint8_t opcode) {
    uint32_t u = (uint32_t)imm12;
    uint32_t imm11_5 = (u>>5)&0x7Fu, imm4_0 = u&0x1Fu;
    return (imm11_5<<25)|((uint32_t)rs2<<20)|((uint32_t)rs1<<15)|((uint32_t)funct3<<12)|(imm4_0<<7)|opcode;
}
static uint32_t enc_b(int32_t imm13, uint8_t rs2, uint8_t rs1, uint8_t funct3, uint8_t opcode) {
    uint32_t u = (uint32_t)imm13;
    uint32_t b12=(u>>12)&1u, b10_5=(u>>5)&0x3Fu, b4_1=(u>>1)&0xFu, b11=(u>>11)&1u;
    return (b12<<31)|(b10_5<<25)|((uint32_t)rs2<<20)|((uint32_t)rs1<<15)|((uint32_t)funct3<<12)|(b4_1<<8)|(b11<<7)|opcode;
}
static uint32_t enc_u(int32_t imm20, uint8_t rd, uint8_t opcode) {
    return (((uint32_t)imm20 & 0xFFFFFu)<<12)|((uint32_t)rd<<7)|opcode;
}
static uint32_t enc_j(int32_t imm21, uint8_t rd, uint8_t opcode) {
    uint32_t u = (uint32_t)imm21;
    uint32_t b20=(u>>20)&1u, b10_1=(u>>1)&0x3FFu, b11=(u>>11)&1u, b19_12=(u>>12)&0xFFu;
    return (b20<<31)|(b19_12<<12)|(b11<<20)|(b10_1<<21)|((uint32_t)rd<<7)|opcode;
}

/* Opcodes */
#define OPC_LOAD    0x03u
#define OPC_OPIMM   0x13u
#define OPC_STORE   0x23u
#define OPC_OP      0x33u
#define OPC_BRANCH  0x63u
#define OPC_JALR    0x67u
#define OPC_JAL     0x6Fu
#define OPC_AUIPC   0x17u

static void i_addi (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm, rs1, 0x0, rd, OPC_OPIMM)); }
static void i_xori  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm, rs1, 0x4, rd, OPC_OPIMM)); }
static void i_sltiu (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm, rs1, 0x3, rd, OPC_OPIMM)); }
static void i_andi  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm, rs1, 0x7, rd, OPC_OPIMM)); }
/* SLLI/SRLI: imm here is a 0-7 shift amount (this file only ever shifts by
 * an argument index < 8), well within the 6-bit shamt field with funct7's
 * high bits implicitly zero -- no separate shamt encoder needed. Gap
 * Remediation SIMI Phase 12 (capability-tag argument mask, see
 * emit_call_site/emit_prologue below). */
static void i_slli  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t shamt) { e32(cb, enc_i(shamt, rs1, 0x1, rd, OPC_OPIMM)); }
static void i_srli  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t shamt) { e32(cb, enc_i(shamt, rs1, 0x5, rd, OPC_OPIMM)); }
static void r_add  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x00,rs2,rs1,0x0,rd,OPC_OP)); }
static void r_sub  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x20,rs2,rs1,0x0,rd,OPC_OP)); }
static void r_and  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x00,rs2,rs1,0x7,rd,OPC_OP)); }
static void r_or   (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x00,rs2,rs1,0x6,rd,OPC_OP)); }
static void r_xor  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x00,rs2,rs1,0x4,rd,OPC_OP)); }
static void r_sll  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x00,rs2,rs1,0x1,rd,OPC_OP)); }
static void r_srl  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x00,rs2,rs1,0x5,rd,OPC_OP)); }
static void r_sra  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x20,rs2,rs1,0x5,rd,OPC_OP)); }
static void r_slt  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x00,rs2,rs1,0x2,rd,OPC_OP)); }
static void r_sltu (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x00,rs2,rs1,0x3,rd,OPC_OP)); }
static void r_mul  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x01,rs2,rs1,0x0,rd,OPC_OP)); }
static void r_div  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x01,rs2,rs1,0x4,rd,OPC_OP)); }
static void r_divu (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x01,rs2,rs1,0x5,rd,OPC_OP)); }
static void r_rem  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x01,rs2,rs1,0x6,rd,OPC_OP)); }
static void r_remu (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, uint8_t rs2) { e32(cb, enc_r(0x01,rs2,rs1,0x7,rd,OPC_OP)); }

static void i_ld  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm,rs1,0x3,rd,OPC_LOAD)); }  /* 64-bit */
static void i_lw  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm,rs1,0x2,rd,OPC_LOAD)); }  /* i32 sign */
static void i_lwu (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm,rs1,0x6,rd,OPC_LOAD)); }  /* u32 zero */
static void i_lh  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm,rs1,0x1,rd,OPC_LOAD)); }  /* i16 sign */
static void i_lhu (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm,rs1,0x5,rd,OPC_LOAD)); }  /* u16 zero */
static void i_lb  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm,rs1,0x0,rd,OPC_LOAD)); }  /* i8 sign */
static void i_lbu (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm,rs1,0x4,rd,OPC_LOAD)); }  /* u8 zero */
static void s_sd  (struct CodeBuf* cb, uint8_t rs2, uint8_t rs1, int32_t imm) { e32(cb, enc_s(imm,rs2,rs1,0x3,OPC_STORE)); }
static void s_sw  (struct CodeBuf* cb, uint8_t rs2, uint8_t rs1, int32_t imm) { e32(cb, enc_s(imm,rs2,rs1,0x2,OPC_STORE)); }
static void s_sh  (struct CodeBuf* cb, uint8_t rs2, uint8_t rs1, int32_t imm) { e32(cb, enc_s(imm,rs2,rs1,0x1,OPC_STORE)); }
static void s_sb  (struct CodeBuf* cb, uint8_t rs2, uint8_t rs1, int32_t imm) { e32(cb, enc_s(imm,rs2,rs1,0x0,OPC_STORE)); }

static void u_auipc (struct CodeBuf* cb, uint8_t rd, int32_t imm20) { e32(cb, enc_u(imm20,rd,OPC_AUIPC)); }
static void i_jalr  (struct CodeBuf* cb, uint8_t rd, uint8_t rs1, int32_t imm) { e32(cb, enc_i(imm,rs1,0x0,rd,OPC_JALR)); }

/* ─── Symbolic register slot helpers: reg i lives at [s0 - 8*(i+1) - 16] ──
 * Direct RV64 analog of simi_x86.c's rbp-relative reg_disp/ld_rax/st_rax,
 * with one RV64-specific wrinkle: s0 == SP_entry (the sp value at
 * procedure entry, before this procedure's own prologue runs), and the
 * prologue also saves the caller's ra/s0 at [s0-8] / [s0-16] (see
 * emit_prologue) — exactly where slots 0 and 1 would land without the
 * extra -16 offset here. Register slots therefore start at s0-24, below
 * the saved ra/s0 pair, not at s0-8. (An earlier version of this file
 * omitted the -16 and slot 0 silently aliased the saved return address —
 * caught by rv64_exec.c actually executing the output and returning to
 * garbage, not by review; see AeroSLS-SIMI-ISA-v0.1.md §12.)
 * TX_RV_MAX_REGS*8+16 = 528 still keeps every offset within I-type's
 * 12-bit signed range (max magnitude 528), so no materialization is ever
 * needed for these loads/stores, unlike the general LOAD/STORE opcodes
 * below. */
static int32_t reg_disp(int i) { return -(int32_t)(8 * (i + 1) + 16); }
static void ld_slot(struct CodeBuf* cb, uint8_t rd, int i) { i_ld(cb, rd, X_S0, reg_disp(i)); }
static void st_slot(struct CodeBuf* cb, uint8_t rs, int i) { s_sd(cb, rs, X_S0, reg_disp(i)); }

/* ─── v0.3 (Phase 7): capability-tag region ───────────────────────────────
 * TX_RV_MAX_REGS bytes, one per symbolic register, placed directly below
 * (more negative than) the register-slot region: reg_disp(TX_RV_MAX_REGS-1)
 * == -528 is the lowest register byte, so the tag region occupies
 * [-592, -529], contiguous with it. 1 = register currently holds an
 * unforged T_OBJREF capability, 0 = plain value. See simi_x86.c's mirror
 * of this same region (TX_TAG_BASE/tag_disp) for the full design writeup —
 * this is a function-for-function port of that mechanism onto RV64's
 * load/store-byte instructions. */
#define TX_RV_TAG_BASE (-(int32_t)(TX_RV_MAX_REGS * 8 + 16 + TX_RV_MAX_REGS))
static int32_t tag_disp(int i) { return TX_RV_TAG_BASE + i; }
static void ld_tag(struct CodeBuf* cb, uint8_t rd, int i) { i_lbu(cb, rd, X_S0, tag_disp(i)); }
static void st_tag(struct CodeBuf* cb, uint8_t rs, int i) { s_sb(cb, rs, X_S0, tag_disp(i)); }
/* clear the tag byte directly — x0 always holds 0, no scratch register or
 * extra instruction needed the way an arbitrary immediate would require. */
static void st_untag(struct CodeBuf* cb, int i) { s_sb(cb, X_ZERO, X_S0, tag_disp(i)); }
/* Every opcode that writes a plain (non-capability) value goes through
 * this instead of the raw st_slot, so "clear the destination's tag" can
 * never be forgotten at a new call site — mirrors x86's st_rax_untag/
 * st_rcx_untag. */
static void st_slot_untag(struct CodeBuf* cb, uint8_t rs, int i) { st_slot(cb, rs, i); st_untag(cb, i); }

/* ─── Gap Remediation SIMI Phase 12: capability tag propagation across
 * CALL/RET -- direct RV64 port of simi_x86.c's identical mechanism (see
 * its Phase 12 design-notes block for the full unforgeability argument).
 * Arguments: the outgoing-arg area grows by one byte, an 8-bit mask (bit
 * i = source register i's tag) at [sp+64] (call site) / [s0+64] (callee
 * prologue) -- TX_RV_ARG_MASK_DISP needs no +16 the way x86's
 * TX_ARG_MASK_DISP does, because RV64's incoming args already land at
 * [s0+8*i] with no return-address/saved-rbp offset baked in (see
 * reg_disp()'s comment above for why RV64's frame layout differs from
 * x86's here). Return value: the tag rides in t1 (X_T1) alongside the
 * value already riding in t0 -- neither is touched by the RET epilogue
 * (addi sp; ld ra; ld s0; addi sp; jalr), so both survive intact to the
 * call site's post-jal code, mirroring x86's rax/dl pairing. */
#define TX_RV_ARG_MASK_DISP 64
static void ld_argmask(struct CodeBuf* cb, uint8_t rd) { i_lbu(cb, rd, X_S0, TX_RV_ARG_MASK_DISP); }

/* ─── Local (intra-instruction) conditional branch helpers ───────────────
 * OBJSIZE/OBJTYPE need a runtime branch on the tag-check result — same
 * rationale as simi_x86.c's emit_jz_placeholder/patch_local_rel32: the
 * branch target is a few instructions further into this same emitted
 * SIMI instruction's code, not another SIMI pc, so it doesn't go through
 * the global g_fixups table (that's keyed by target_pc, resolved in a
 * separate pass once every instruction's offset is known) — it's patched
 * immediately, in the same emit call. */
static uint32_t emit_beqz_placeholder(struct CodeBuf* cb) {
    uint32_t pos = cb->len;
    e32(cb, 0);   /* placeholder, patched below once the branch target is known */
    return pos;
}
static void patch_local_beqz(struct CodeBuf* cb, uint32_t pos, uint8_t rs1) {
    int32_t off = (int32_t)(cb->len - pos);
    patch32(cb->buf, pos, enc_b(off, X_ZERO, rs1, 0x0, OPC_BRANCH));
}
static uint32_t emit_jal_placeholder(struct CodeBuf* cb) {
    uint32_t pos = cb->len;
    e32(cb, 0);
    return pos;
}
static void patch_local_jal(struct CodeBuf* cb, uint32_t pos) {
    int32_t off = (int32_t)(cb->len - pos);
    patch32(cb->buf, pos, enc_j(off, X_ZERO, OPC_JAL));
}
/* Gap Remediation SIMI Phase 14: the all-zero-bits 32-bit word is
 * permanently reserved by the base RV64 ISA as an illegal instruction
 * (RISC-V spec: an all-zeros word is guaranteed never to be a valid
 * encoding, precisely so software can use it to force a trap) -- the
 * direct RV64 analog of x86's `ud2` used for the same JMPR OOB hazard. */
static void op_illegal(struct CodeBuf* cb) { e32(cb, 0); }

/* ─── Literal pool + li64 fixups (auipc+ld standing in for x86 movabs) ───
 * emit_li64() emits a placeholder auipc+ld pair against the target
 * register and records (auipc_pos, literal index, register) so the final
 * patch pass — once the literal pool's address is known — can compute the
 * %pcrel_hi/%pcrel_lo split and rewrite both words in place. */
#define TX_RV_MAX_LITERALS 4160
static uint64_t g_literals[TX_RV_MAX_LITERALS];
static uint32_t g_nliterals;

struct LiFixup { uint32_t auipc_pos; uint32_t literal_idx; uint8_t reg; };
static struct LiFixup g_li_fixups[TX_RV_MAX_LITERALS];
static uint32_t g_nli_fixups;

static int add_literal(uint64_t val) {
    if (g_nliterals >= TX_RV_MAX_LITERALS) return -1;
    g_literals[g_nliterals] = val;
    return (int)(g_nliterals++);
}
static int emit_li64(struct CodeBuf* cb, uint8_t reg, uint64_t imm) {
    int idx = add_literal(imm);
    if (idx < 0) return 0;
    if (g_nli_fixups >= TX_RV_MAX_LITERALS) return 0;
    g_li_fixups[g_nli_fixups].auipc_pos = cb->len;
    g_li_fixups[g_nli_fixups].literal_idx = (uint32_t)idx;
    g_li_fixups[g_nli_fixups].reg = reg;
    g_nli_fixups++;
    u_auipc(cb, reg, 0);      /* placeholder, patched later */
    i_ld(cb, reg, reg, 0);    /* placeholder, patched later */
    return 1;
}
/* Gap Remediation SIMI Phase 14: same auipc+ld emission as emit_li64(),
 * but against an ALREADY-reserved literal-pool slot rather than adding a
 * new one -- every JMPR in an object shares the single table-base literal
 * reserved by the first one (see g_jmpr_lit_idx above), so this is called
 * once per JMPR site while add_literal() for that value is called at most
 * once per object. */
static int emit_li64_from_idx(struct CodeBuf* cb, uint8_t reg, uint32_t idx) {
    if (g_nli_fixups >= TX_RV_MAX_LITERALS) return 0;
    g_li_fixups[g_nli_fixups].auipc_pos = cb->len;
    g_li_fixups[g_nli_fixups].literal_idx = idx;
    g_li_fixups[g_nli_fixups].reg = reg;
    g_nli_fixups++;
    u_auipc(cb, reg, 0);
    i_ld(cb, reg, reg, 0);
    return 1;
}
/* Materialize a signed displacement into `reg`: a plain ADDI when it fits
 * in 12 bits (the common case for small LOAD/STORE/LEA offsets), the
 * literal-pool path otherwise — see the "RV64 design decisions" note
 * above on why the load/store immediate field itself can't be used
 * directly for SIMI's full 28-bit displacement range. */
static int emit_li_disp(struct CodeBuf* cb, uint8_t reg, int32_t disp) {
    if (disp >= -2048 && disp <= 2047) { i_addi(cb, reg, X_ZERO, disp); return 1; }
    return emit_li64(cb, reg, (uint64_t)(int64_t)disp);
}

/* ─── Branch/call fixups (JAL / BEQ / BNE targets) ───────────────────────
 * Same two-pass shape as simi_x86.c's g_fixups, adapted for RV64's
 * bit-scrambled J-type/B-type encodings — the whole 4-byte instruction
 * word is recomputed and overwritten in the patch pass rather than
 * patching a sub-field, since there's no contiguous rel-offset field to
 * patch in place the way x86's rel32 has. */
enum { FIX_JAL, FIX_BEQ, FIX_BNE };
#define TX_RV_MAX_FIXUPS 4096
struct Fixup { uint32_t instr_pos; uint32_t target_pc; uint8_t kind; uint8_t rd_or_rs1; };
static struct Fixup g_fixups[TX_RV_MAX_FIXUPS];
static uint32_t g_nfixups;
static uint32_t g_instr_off[4096];   /* TX_MAX_INSTR, mirrors simi_x86.c */

/* v0.3 (Phase 6): translate-time context for RESOLVE/OBJSIZE/OBJTYPE
 * codegen — same file-scope-global convention as simi_x86.c's g_rt_*. */
static uint64_t g_rt_resolve_fn, g_rt_objsize_fn, g_rt_objtype_fn;
static uint32_t g_num_names;

/* Gap Remediation SIMI Phase 14: JMPR support.
 *
 * CORRECTION vs. the first pass at this (caught by rv64_exec.c actually
 * running the output, not by review): this file's naive port of
 * simi_x86.c's design baked g_out_buf_addr — (uintptr_t)out_buf, a real
 * HOST pointer — into the jump table, exactly like x86 does. That's wrong
 * here. rv64_exec.c's guest CPU (see its own top comment) doesn't execute
 * this file's output on real hardware at its true host address; it
 * fetches/executes against a small `cpu->mem` buffer indexed from 0, and
 * every control-transfer instruction this file already emits — JAL, BEQ,
 * BNE, even AUIPC — computes its target as an offset relative to the
 * guest's own pc, never a real host address. The ONLY place a genuine
 * host pointer legitimately appears in emitted code is a host-function
 * call (RESOLVE/OBJSIZE/OBJTYPE's rt_*_fn literals), and even those don't
 * reach rv64_exec.c as real addresses — they're recognized by a sentinel
 * bit pattern (RV_EXEC_HOSTFN_MASK/BASE, see rv64_exec.c's JALR case) and
 * redirected to a host callback table instead of ever being fetched as
 * guest code. JMPR is real guest-to-guest control flow (indistinguishable
 * from what JAL already does), so its table must hold plain byte offsets
 * into out_buf — exactly what g_instr_off[] already stores — not
 * out_buf's own host address plus that offset. Removed g_out_buf_addr
 * entirely rather than leave an unused-but-misleading field.
 *
 * g_jmpr_lit_idx is lazily set to the literal-pool index reserved for the
 * table's base OFFSET on the FIRST JMPR instruction seen (-1 means "no
 * JMPR in this object yet" -- an object with none pays zero extra cost).
 * g_num_instr mirrors simi_x86.c's global of the same name -- emit_instr()
 * has no other way to reach it. */
static uint32_t g_num_instr;
static int32_t g_jmpr_lit_idx;

static int add_fixup(struct CodeBuf* cb, uint32_t target_pc, uint8_t kind, uint8_t rd_or_rs1) {
    if (g_nfixups >= TX_RV_MAX_FIXUPS) return 0;
    g_fixups[g_nfixups].instr_pos = cb->len;
    g_fixups[g_nfixups].target_pc = target_pc;
    g_fixups[g_nfixups].kind = kind;
    g_fixups[g_nfixups].rd_or_rs1 = rd_or_rs1;
    g_nfixups++;
    e32(cb, 0);   /* placeholder word, patched later */
    return 1;
}
static void op_jal(struct CodeBuf* cb, uint32_t target_pc, uint8_t rd) { add_fixup(cb, target_pc, FIX_JAL, rd); }
/* BEQ/BNE always compare the condition register (rs1) against x0 here —
 * SIMI's BC tests a single register for zero/nonzero, see emit BC below. */
static void op_beq_z(struct CodeBuf* cb, uint32_t target_pc, uint8_t rs1) { add_fixup(cb, target_pc, FIX_BEQ, rs1); }
static void op_bne_z(struct CodeBuf* cb, uint32_t target_pc, uint8_t rs1) { add_fixup(cb, target_pc, FIX_BNE, rs1); }

/* ─── CMP: synthesize all 10 relations from slt/sltu/xori/sub (§12) ──────
 * Operands in t0 (lhs), t1 (rhs); result (0/1) always ends up in t0. */
static int emit_cmp(struct CodeBuf* cb, int rel) {
    switch (rel) {
        case REL_EQ:  r_sub(cb, X_T0, X_T0, X_T1);  i_sltiu(cb, X_T0, X_T0, 1); break;
        case REL_NE:  r_sub(cb, X_T0, X_T0, X_T1);  r_sltu(cb, X_T0, X_ZERO, X_T0); break;
        case REL_LT:  r_slt(cb, X_T0, X_T0, X_T1); break;
        case REL_GT:  r_slt(cb, X_T0, X_T1, X_T0); break;
        case REL_LE:  r_slt(cb, X_T2, X_T1, X_T0);  i_xori(cb, X_T0, X_T2, 1); break;
        case REL_GE:  r_slt(cb, X_T2, X_T0, X_T1);  i_xori(cb, X_T0, X_T2, 1); break;
        case REL_LTU: r_sltu(cb, X_T0, X_T0, X_T1); break;
        case REL_GTU: r_sltu(cb, X_T0, X_T1, X_T0); break;
        case REL_LEU: r_sltu(cb, X_T2, X_T1, X_T0); i_xori(cb, X_T0, X_T2, 1); break;
        case REL_GEU: r_sltu(cb, X_T2, X_T0, X_T1); i_xori(cb, X_T0, X_T2, 1); break;
        default: return 0;
    }
    return 1;
}

/* ─── LOAD/STORE width+signedness (mirrors type_width() in simi_x86.c and
 * the Phase 1 reference interpreter) — RV64 has a native load/store
 * opcode for every SIMI width/signedness combination, so unlike x86 there
 * is no movsxd/movsx/movzx zoo to hand-pick; the mapping below is 1:1. */
static void load_typed(struct CodeBuf* cb, int type, uint8_t rd, uint8_t rs1) {
    switch (type) {
        case T_I8:  i_lb (cb, rd, rs1, 0); break;
        case T_U8:  case T_BOOL: i_lbu(cb, rd, rs1, 0); break;
        case T_I16: i_lh (cb, rd, rs1, 0); break;
        case T_U16: i_lhu(cb, rd, rs1, 0); break;
        case T_I32: case T_F32: i_lw (cb, rd, rs1, 0); break;
        case T_U32: i_lwu(cb, rd, rs1, 0); break;
        default:    i_ld (cb, rd, rs1, 0); break;   /* i64/u64/f64/ptr */
    }
}
static void store_typed(struct CodeBuf* cb, int type, uint8_t rs2, uint8_t rs1) {
    switch (type) {
        case T_I8: case T_U8: case T_BOOL: s_sb(cb, rs2, rs1, 0); break;
        case T_I16: case T_U16:            s_sh(cb, rs2, rs1, 0); break;
        case T_I32: case T_U32: case T_F32: s_sw(cb, rs2, rs1, 0); break;
        default: s_sd(cb, rs2, rs1, 0); break;
    }
}
static int type_shift(int t) {   /* log2(byte width) — used for PTRADD scaling via SLLI */
    switch (t) {
        case T_I8: case T_U8: case T_BOOL: return 0;
        case T_I16: case T_U16: return 1;
        case T_I32: case T_U32: case T_F32: return 2;
        default: return 3;
    }
}

/* ─── Procedure prologue/epilogue ─────────────────────────────────────────
 * addi sp,sp,-16; sd ra,8(sp); sd s0,0(sp); addi s0,sp,16 — standard RV64
 * non-leaf prologue opening, saving ra explicitly since (unlike x86's
 * `call`) `jal` never pushes it automatically. s0 now equals the sp value
 * at entry, exactly the role x86's rbp plays after push rbp/mov rbp,rsp.
 * Then addi sp,sp,-FRAME reserves the register-slot frame, and every slot
 * is zeroed (byte-at-a-time sd loop — no rep-stosq equivalent on RV64,
 * and TX_RV_MAX_REGS=64 slots is cheap enough to unroll... actually
 * looped, not unrolled, to keep code size bounded regardless of
 * TX_RV_MAX_REGS). Incoming r0..r7 args are then copied in from
 * [s0+8*i] — see emit_call_site for why that's where the caller left
 * them. */
#define TX_RV_FRAME_BYTES (TX_RV_MAX_REGS * 8)
/* v0.3 (Phase 7): total stack reserved for the register slots AND the
 * capability-tag region below them, as one contiguous block — mirrors
 * x86's TX_TOTAL_FRAME_BYTES. */
#define TX_RV_TOTAL_FRAME_BYTES (TX_RV_FRAME_BYTES + TX_RV_MAX_REGS)

static void emit_prologue(struct CodeBuf* cb) {
    i_addi(cb, X_SP, X_SP, -16);
    s_sd(cb, X_RA, X_SP, 8);
    s_sd(cb, X_S0, X_SP, 0);
    i_addi(cb, X_S0, X_SP, 16);
    i_addi(cb, X_SP, X_SP, -(int32_t)TX_RV_TOTAL_FRAME_BYTES);
    for (int i = 0; i < TX_RV_MAX_REGS; i++) st_slot(cb, X_ZERO, i);
    /* v0.3 (Phase 7): zero the tag region too — every register starts
     * untagged. Gap Remediation SIMI Phase 12: registers 8+ (never
     * touched by the argument-tag mask loop below, which only covers
     * 0..nargs-1) correctly stay untagged from this zeroing alone; r0-r7
     * get their real tag applied right after, by the mask loop below. */
    for (int i = 0; i < TX_RV_MAX_REGS; i++) st_untag(cb, i);
    int nargs = TX_RV_MAX_REGS < 8 ? TX_RV_MAX_REGS : 8;
    for (int i = 0; i < nargs; i++) {
        i_ld(cb, X_T0, X_S0, 8*i);
        st_slot(cb, X_T0, i);
    }
    /* Gap Remediation SIMI Phase 12: apply the caller's argument-tag mask
     * (see the design-notes block above ld_argmask()) -- a plain,
     * branch-free per-argument bit test, one reload+shift+and+store per
     * arg (reloaded fresh each iteration rather than shifted in place, so
     * each iteration starts from the same unmodified mask byte). */
    for (int i = 0; i < nargs; i++) {
        ld_argmask(cb, X_T1);
        if (i > 0) i_srli(cb, X_T1, X_T1, i);
        i_andi(cb, X_T1, X_T1, 1);
        st_tag(cb, X_T1, i);
    }
}

/* ─── Call-site codegen: marshal r0..r7 into the outgoing-argument area
 * ([sp+8*i], sp at the point of the jal becoming the callee's s0), call,
 * then read the result back from t0 (see RET below for why it's still
 * live there) into rd's slot. Mirrors simi_x86.c's emit_call_site.
 *
 * Gap Remediation SIMI Phase 12: the outgoing-arg area grew from 64 to 72
 * bytes (the 9th byte at [sp+64] carries the argument-tag mask, see the
 * design-notes block above ld_argmask()), and the result now propagates
 * a real tag (read from t1, set by the callee's RET) instead of the old
 * unconditional st_slot_untag. ─────────────────────────────────────────*/
static void emit_call_site(struct CodeBuf* cb, uint32_t target_pc, int rd) {
    i_addi(cb, X_SP, X_SP, -72);
    int nargs = TX_RV_MAX_REGS < 8 ? TX_RV_MAX_REGS : 8;
    for (int i = 0; i < nargs; i++) {
        ld_slot(cb, X_T0, i);
        s_sd(cb, X_T0, X_SP, 8*i);
    }
    for (int i = nargs; i < 8; i++) s_sd(cb, X_ZERO, X_SP, 8*i);
    /* Build the outgoing argument-tag mask AFTER all values are already
     * stored to their slots above -- clobbering t0/t1/t2 here is then
     * safe. bit i = register i's current tag. */
    i_addi(cb, X_T1, X_ZERO, 0);   /* t1 = 0, mask accumulator */
    for (int i = 0; i < nargs; i++) {
        ld_tag(cb, X_T2, i);
        if (i > 0) i_slli(cb, X_T2, X_T2, i);
        r_or(cb, X_T1, X_T1, X_T2);
    }
    s_sb(cb, X_T1, X_SP, 64);
    op_jal(cb, target_pc, X_RA);
    i_addi(cb, X_SP, X_SP, 72);
    st_slot(cb, X_T0, rd);
    st_tag(cb, X_T1, rd);   /* Gap Remediation SIMI Phase 12: propagate r0's tag */
}

/* ─── Trampoline: zero r0..r6, r7=scratch_ptr, call entry, return ────────
 * Unlike x86's trampoline (which needs no return-address bookkeeping
 * because `call` pushes one automatically), this one must explicitly
 * save its own incoming `ra` before its `jal` clobbers it, and restore it
 * before its own final `jalr` — see the design-notes block above. Ends by
 * returning through `ra` rather than a bare `ret` opcode (RV64 doesn't
 * have one; `jalr x0,0(ra)` is the real instruction the `ret`
 * pseudo-op expands to) so this trampoline is a normal callable
 * subroutine, mirroring x86's "always ends in a plain ret" property. */
static int emit_trampoline(struct CodeBuf* cb, uint32_t target_pc, uint64_t scratch_ptr,
                            uint64_t namepool_ptr) {
    i_addi(cb, X_SP, X_SP, -16);
    s_sd(cb, X_RA, X_SP, 8);
    i_addi(cb, X_SP, X_SP, -72);
    for (int i = 0; i < 6; i++) s_sd(cb, X_ZERO, X_SP, 8*i);
    if (!emit_li64(cb, X_T0, namepool_ptr)) return 0;
    s_sd(cb, X_T0, X_SP, 48);
    if (!emit_li64(cb, X_T0, scratch_ptr)) return 0;
    s_sd(cb, X_T0, X_SP, 56);
    /* Gap Remediation SIMI Phase 12: zero the argument-tag mask too --
     * nothing before "main" starts can hold a real capability (r0-r6 are
     * zeroed, r7 is a raw pointer, not a RESOLVE result), and main's
     * prologue now unconditionally reads this byte. */
    s_sb(cb, X_ZERO, X_SP, 64);
    op_jal(cb, target_pc, X_RA);
    i_addi(cb, X_SP, X_SP, 72);
    i_ld(cb, X_RA, X_SP, 8);
    i_addi(cb, X_SP, X_SP, 16);
    i_jalr(cb, X_ZERO, X_RA, 0);
    return 1;
}

/* ─── Main per-instruction codegen ────────────────────────────────────── */
static int emit_instr(struct CodeBuf* cb, uint64_t w) {
    uint8_t op = w_op(w), type = w_type(w), flags = w_flags(w);
    uint16_t rd = w_rd(w), ra = w_ra(w);

    if (rd >= TX_RV_MAX_REGS || ra >= TX_RV_MAX_REGS) return TX_RV_ERR_REG_OUT_OF_RANGE;
    if ((op==OP_ADD||op==OP_SUB||op==OP_MUL||op==OP_DIV||op==OP_MOD||op==OP_AND||
         op==OP_OR||op==OP_XOR||op==OP_SHL||op==OP_SHR||op==OP_SAR||op==OP_CMP||
         op==OP_PTRADD) && !(flags & FLAG_IMM) && w_rb_reg(w) >= TX_RV_MAX_REGS)
        return TX_RV_ERR_REG_OUT_OF_RANGE;
    /* Gap Remediation SIMI Phase 10: this translator has no float codegen
     * at all (scoped out of v1 -- see TX_RV_ERR_FLOAT_UNSUPPORTED's
     * comment in simi_riscv.h). Every opcode below that would have float
     * meaning on x86 (ADD/SUB/MUL/DIV/MOD/NEG/CMP) must reject T_F32/
     * T_F64 explicitly here, up front, rather than falling into codegen
     * that reads the slot as a plain 64-bit integer and silently produces
     * a wrong answer on real IEEE bit patterns. */
    if ((type==T_F64 || type==T_F32) &&
        (op==OP_ADD||op==OP_SUB||op==OP_MUL||op==OP_DIV||op==OP_MOD||
         op==OP_NEG||op==OP_CMP))
        return TX_RV_ERR_FLOAT_UNSUPPORTED;

    switch (op) {
    case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR:
    case OP_MUL: case OP_SHL: case OP_SHR: case OP_SAR: {
        ld_slot(cb, X_T0, ra);
        if (flags & FLAG_IMM) { if (!emit_li64(cb, X_T1, (uint64_t)(int64_t)w_imm28(w))) return TX_RV_ERR_TOO_MANY_LITERALS; }
        else                  ld_slot(cb, X_T1, w_rb_reg(w));
        switch (op) {
            case OP_ADD: r_add(cb, X_T0, X_T0, X_T1); break;
            case OP_SUB: r_sub(cb, X_T0, X_T0, X_T1); break;
            case OP_AND: r_and(cb, X_T0, X_T0, X_T1); break;
            case OP_OR:  r_or (cb, X_T0, X_T0, X_T1); break;
            case OP_XOR: r_xor(cb, X_T0, X_T0, X_T1); break;
            case OP_MUL: r_mul(cb, X_T0, X_T0, X_T1); break;
            case OP_SHL: r_sll(cb, X_T0, X_T0, X_T1); break;
            case OP_SHR: r_srl(cb, X_T0, X_T0, X_T1); break;
            case OP_SAR: r_sra(cb, X_T0, X_T0, X_T1); break;
        }
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_DIV: case OP_MOD: {
        ld_slot(cb, X_T0, ra);
        if (flags & FLAG_IMM) { if (!emit_li64(cb, X_T1, (uint64_t)(int64_t)w_imm28(w))) return TX_RV_ERR_TOO_MANY_LITERALS; }
        else                  ld_slot(cb, X_T1, w_rb_reg(w));
        int sgn = rv_type_signed(type);
        if (op == OP_DIV) { if (sgn) r_div(cb,X_T0,X_T0,X_T1); else r_divu(cb,X_T0,X_T0,X_T1); }
        else               { if (sgn) r_rem(cb,X_T0,X_T0,X_T1); else r_remu(cb,X_T0,X_T0,X_T1); }
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_NOT: ld_slot(cb,X_T0,ra); i_xori(cb,X_T0,X_T0,-1); st_slot_untag(cb,X_T0,rd); break;
    case OP_NEG: ld_slot(cb,X_T0,ra); r_sub(cb,X_T0,X_ZERO,X_T0); st_slot_untag(cb,X_T0,rd); break;
    case OP_MOV:
        /* v0.3 (Phase 7): the one opcode besides RESOLVE that can produce
         * a tagged register — propagating an existing capability is still
         * a valid capability, that's how it gets passed around. */
        ld_slot(cb,X_T0,ra); st_slot(cb,X_T0,rd);
        ld_tag(cb,X_T1,ra); st_tag(cb,X_T1,rd);
        break;
    case OP_LOADI:
        if (!emit_li64(cb, X_T0, (uint64_t)(int64_t)w_imm28(w))) return TX_RV_ERR_TOO_MANY_LITERALS;
        st_slot_untag(cb, X_T0, rd);
        break;
    case OP_LOADI64:
        return TX_RV_ERR_BAD_OPCODE; /* unreachable: handled specially in translate(), needs literal pool value */
    case OP_CMP: {
        ld_slot(cb, X_T0, ra);
        ld_slot(cb, X_T1, w_rb_reg(w));
        if (flags >= 10 || !emit_cmp(cb, flags)) return TX_RV_ERR_BAD_OPCODE;
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_BR: return TX_RV_ERR_BAD_OPCODE;  /* handled specially in translate() (needs pc) */
    case OP_LEA: {
        ld_slot(cb, X_T0, ra);
        if (!emit_li_disp(cb, X_T1, w_imm28(w))) return TX_RV_ERR_TOO_MANY_LITERALS;
        r_add(cb, X_T0, X_T0, X_T1);
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_PTRADD: {
        /* v0.3 (Phase 7): always untagged — pointer arithmetic must never
         * yield a capability even when rA is currently tagged, the core
         * unforgeability property (see simi_x86.c's identical note). */
        ld_slot(cb, X_T0, ra);   /* base */
        int shift = type_shift(type);
        if (flags & FLAG_IMM) {
            int64_t idx = w_imm28(w);
            if (!emit_li64(cb, X_T1, (uint64_t)(idx << shift))) return TX_RV_ERR_TOO_MANY_LITERALS;
        } else {
            ld_slot(cb, X_T1, w_rb_reg(w));
            if (shift) e32(cb, enc_i(shift, X_T1, 0x1, X_T1, OPC_OPIMM)); /* slli t1,t1,shift */
        }
        r_add(cb, X_T0, X_T0, X_T1);
        st_slot_untag(cb, X_T0, rd);
        break;
    }
    case OP_LOAD: {
        ld_slot(cb, X_T0, ra);                        /* t0 = base pointer */
        if (!emit_li_disp(cb, X_T1, w_imm28(w))) return TX_RV_ERR_TOO_MANY_LITERALS;
        r_add(cb, X_T1, X_T0, X_T1);                  /* t1 = effective address */
        load_typed(cb, type, X_T1, X_T1);
        st_slot_untag(cb, X_T1, rd);
        break;
    }
    case OP_STORE: {
        ld_slot(cb, X_T0, ra);                        /* t0 = base pointer */
        ld_slot(cb, X_T1, rd);                        /* rd holds the *source* value register, same convention as x86 */
        if (!emit_li_disp(cb, X_T2, w_imm28(w))) return TX_RV_ERR_TOO_MANY_LITERALS;
        r_add(cb, X_T2, X_T0, X_T2);                  /* t2 = effective address */
        store_typed(cb, type, X_T1, X_T2);
        break;
    }
    case OP_ENTER: emit_prologue(cb); break;
    case OP_LEAVE: /* no-op directive, matches Phase 1 interpreter */ break;
    case OP_RESOLVE: {
        /* rb_raw holds the name-pool index (FMT_RESOLVE, simi_isa.h). a0 =
         * namepool_ptr(r6) + idx*TX_RV_NAME_SIZE, a raw pointer straight
         * into the object's own name-pool bytes — no copy, mirrors x86.
         *
         * v0.3 (Phase 7): tag the destination iff the resolve succeeded
         * (result != 0). `sltu t1,x0,a0` computes "a0 != 0" (1 or 0) in a
         * single instruction — RV64's direct analog of x86's test+setne,
         * no branch needed. Doesn't touch a0, so ordering vs. the result
         * store doesn't matter. */
        uint32_t idx = w_rb_raw(w);
        if (idx >= g_num_names) return TX_RV_ERR_NAME_OUT_OF_RANGE;
        ld_slot(cb, X_A0, TX_RV_NAMEPOOL_ARG_IDX);
        if (!emit_li_disp(cb, X_T1, (int32_t)(idx * TX_RV_NAME_SIZE))) return TX_RV_ERR_TOO_MANY_LITERALS;
        r_add(cb, X_A0, X_A0, X_T1);
        if (!emit_li64(cb, X_T2, g_rt_resolve_fn)) return TX_RV_ERR_TOO_MANY_LITERALS;
        i_jalr(cb, X_RA, X_T2, 0);
        r_sltu(cb, X_T1, X_ZERO, X_A0);   /* t1 = (a0 != 0) */
        st_slot(cb, X_A0, rd);
        st_tag(cb, X_T1, rd);
        break;
    }
    case OP_OBJSIZE: case OP_OBJTYPE: {
        /* v0.3 (Phase 7): require rA to currently carry a valid capability
         * tag before ever consulting the runtime catalog — an untagged
         * operand is rejected with the same sentinel used for "no such
         * object," and the runtime function is never called at all. This
         * is what closes the forgery hole (see simi_x86.c's identical
         * note and tests/cap_forge.simi). */
        uint64_t fn = (op == OP_OBJSIZE) ? g_rt_objsize_fn : g_rt_objtype_fn;
        uint64_t invalid_sentinel = (op == OP_OBJSIZE) ? 0ull : 0xFFFFFFFFull;
        ld_tag(cb, X_T1, ra);
        uint32_t beqz_pos = emit_beqz_placeholder(cb);     /* beqz t1, .invalid */
        ld_slot(cb, X_A0, ra);                             /* T_OBJREF value */
        if (!emit_li64(cb, X_T2, fn)) return TX_RV_ERR_TOO_MANY_LITERALS;
        i_jalr(cb, X_RA, X_T2, 0);
        uint32_t jal_pos = emit_jal_placeholder(cb);       /* jal .done */
        patch_local_beqz(cb, beqz_pos, X_T1);              /* .invalid: */
        if (!emit_li64(cb, X_A0, invalid_sentinel)) return TX_RV_ERR_TOO_MANY_LITERALS;
        patch_local_jal(cb, jal_pos);                      /* .done: */
        st_slot_untag(cb, X_A0, rd);
        break;
    }
    case OP_RET:
        ld_slot(cb, X_T0, 0);      /* r0 is the return-value register, §4.8; t0 survives the epilogue below untouched */
        /* Gap Remediation SIMI Phase 12: r0's tag rides in t1, alongside
         * t0 -- the epilogue below (addi sp; ld ra; ld s0; addi sp; jalr)
         * touches neither, so both survive intact to the call site's
         * post-jal code (see emit_call_site()). */
        ld_tag(cb, X_T1, 0);
        i_addi(cb, X_SP, X_SP, (int32_t)TX_RV_TOTAL_FRAME_BYTES);
        i_ld(cb, X_RA, X_SP, 8);
        i_ld(cb, X_S0, X_SP, 0);
        i_addi(cb, X_SP, X_SP, 16);
        i_jalr(cb, X_ZERO, X_RA, 0);
        break;
    case OP_CALL: return TX_RV_ERR_BAD_OPCODE; /* handled specially in translate() (needs pc) */
    case OP_JMPR: {
        /* Gap Remediation SIMI Phase 14: indirect jump through the runtime
         * table of guest-space code offsets (see g_jmpr_lit_idx's
         * design-notes block above and the reservation+backfill glue in
         * simi_riscv_translate()). Unlike BR/BC/CALL this needs no pc from
         * the caller and no global fixup entry -- the target is only
         * known at runtime, and the table's own base offset reaches this
         * code the same way every 64-bit constant in this file does
         * (literal pool + auipc/ld), it just happens to hold an offset
         * rather than a genuine external host pointer this time. */
        if (g_jmpr_lit_idx < 0) {
            g_jmpr_lit_idx = add_literal(0);
            if (g_jmpr_lit_idx < 0) return TX_RV_ERR_TOO_MANY_LITERALS;
        }
        ld_slot(cb, X_T0, ra);                              /* t0 = target abstract pc */
        /* emit_li_disp materializes ANY signed 32-bit constant (ADDI if it
         * fits in 12 bits, else the literal pool) -- reused here for
         * g_num_instr, not an actual memory displacement. num_instr is at
         * most 4096 (TX_RV_MAX_INSTR), so this may fall to the literal-pool
         * path but never fails outright. */
        if (!emit_li_disp(cb, X_T2, (int32_t)g_num_instr)) return TX_RV_ERR_TOO_MANY_LITERALS;
        r_sltu(cb, X_T1, X_T0, X_T2);                       /* t1 = (target < num_instr) */
        uint32_t beqz_pos = emit_beqz_placeholder(cb);       /* beqz t1, .oob */
        if (!emit_li64_from_idx(cb, X_T2, (uint32_t)g_jmpr_lit_idx)) return TX_RV_ERR_TOO_MANY_LITERALS;
        i_slli(cb, X_T1, X_T0, 3);                           /* t1 = target*8 */
        r_add(cb, X_T2, X_T2, X_T1);                         /* t2 = &table[target] */
        i_ld(cb, X_T2, X_T2, 0);                             /* t2 = table[target] (real machine addr) */
        i_jalr(cb, X_ZERO, X_T2, 0);                         /* jump -- never falls through */
        patch_local_beqz(cb, beqz_pos, X_T1);                 /* .oob: */
        op_illegal(cb);                                       /* real illegal-instruction trap -- non-negotiable CFI per ISA §16 */
        break;
    }
    default: return TX_RV_ERR_BAD_OPCODE;
    }
    return TX_RV_OK;
}

/* ─── Top-level translate: two passes (emit + patch fixups+literals) ────*/
int simi_riscv_translate(const uint8_t* obj_data, uint32_t obj_size,
                          uint8_t* out_buf, uint32_t out_cap,
                          const char* entry_name, uint64_t scratch_ptr,
                          uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                          uint64_t rt_objtype_fn,
                          uint32_t* out_len, uint32_t* entry_off) {
    if (!obj_data || obj_size < sizeof(struct SimiObjHdrRV)) return TX_RV_ERR_BAD_HEADER;
    struct SimiObjHdrRV hdr;
    rv_memcpy(&hdr, obj_data, sizeof(hdr));
    if (hdr.magic != SIMI_RV_MAGIC) return TX_RV_ERR_BAD_HEADER;

    uint64_t expect = sizeof(struct SimiObjHdrRV);
    expect += (uint64_t)hdr.num_instr * 8;
    expect += (uint64_t)hdr.num_literals * 8;
    expect += (uint64_t)hdr.num_entries * sizeof(struct TxEntryRecRV);
    expect += (uint64_t)hdr.num_names * sizeof(struct TxNameRecRV);
    if (expect != (uint64_t)obj_size) return TX_RV_ERR_BAD_HEADER;
    if (hdr.num_instr == 0 || hdr.num_instr > 4096) return TX_RV_ERR_TOO_MANY_INSTR;

    const uint64_t* instrs = (const uint64_t*)(obj_data + sizeof(struct SimiObjHdrRV));
    const uint64_t* literals = (const uint64_t*)((const uint8_t*)instrs + hdr.num_instr * 8);
    const struct TxEntryRecRV* entries = (const struct TxEntryRecRV*)
        ((const uint8_t*)literals + hdr.num_literals * 8);
    const struct TxNameRecRV* names = (const struct TxNameRecRV*)
        ((const uint8_t*)entries + hdr.num_entries * sizeof(struct TxEntryRecRV));
    /* v0.3: namepool_ptr is baked as a raw pointer straight into obj_data's
     * own name-pool bytes — mirrors simi_x86.c exactly. */
    uint64_t namepool_ptr = (uint64_t)(uintptr_t)names;

    g_rt_resolve_fn = rt_resolve_fn;
    g_rt_objsize_fn = rt_objsize_fn;
    g_rt_objtype_fn = rt_objtype_fn;
    g_num_names = hdr.num_names;
    /* Gap Remediation SIMI Phase 14: see g_jmpr_lit_idx's design-notes
     * block above -- reset per translate() call, same as every other
     * file-scope global here. */
    g_num_instr = hdr.num_instr;
    g_jmpr_lit_idx = -1;

    struct CodeBuf cb; cb.buf = out_buf; cb.cap = out_cap; cb.len = 0; cb.overflow = 0;
    g_nfixups = 0; g_nliterals = 0; g_nli_fixups = 0;

    for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
        g_instr_off[pc] = cb.len;
        uint64_t w = instrs[pc];
        uint8_t op = w_op(w);

        if (op == OP_LOADI64) {
            uint32_t idx = w_rb_raw(w);
            if (idx >= hdr.num_literals) return TX_RV_ERR_LITERAL_OUT_OF_RANGE;
            uint16_t rd = w_rd(w);
            if (rd >= TX_RV_MAX_REGS) return TX_RV_ERR_REG_OUT_OF_RANGE;
            if (!emit_li64(&cb, X_T0, literals[idx])) return TX_RV_ERR_TOO_MANY_LITERALS;
            st_slot_untag(&cb, X_T0, rd);
        } else if (op == OP_BR) {
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            op_jal(&cb, target, X_ZERO);
        } else if (op == OP_BC) {
            uint16_t ra = w_ra(w);
            if (ra >= TX_RV_MAX_REGS) return TX_RV_ERR_REG_OUT_OF_RANGE;
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            ld_slot(&cb, X_T0, ra);
            if (w_flags(w) & FLAG_INVERT) op_beq_z(&cb, target, X_T0);
            else                          op_bne_z(&cb, target, X_T0);
        } else if (op == OP_CALL) {
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            /* CALL has no rD in the ISA — the call site always stores the
             * result into the CALLER's r0, same as x86. */
            emit_call_site(&cb, target, 0);
        } else {
            int rc = emit_instr(&cb, w);
            if (rc != TX_RV_OK) return rc;
        }
        if (cb.overflow) return TX_RV_ERR_BUF_FULL;
    }

    /* One trampoline per exported entry, appended after the body. */
    uint32_t found_off = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < hdr.num_entries; i++) {
        uint32_t tramp_off = cb.len;
        if (!emit_trampoline(&cb, entries[i].offset, scratch_ptr, namepool_ptr)) return TX_RV_ERR_TOO_MANY_LITERALS;
        if (cb.overflow) return TX_RV_ERR_BUF_FULL;
        if (rv_streq(entries[i].name, entry_name)) found_off = tramp_off;
    }
    if (found_off == 0xFFFFFFFFu) return TX_RV_ERR_ENTRY_NOT_FOUND;

    /* Gap Remediation SIMI Phase 14: reserve + backfill the JMPR jump
     * table, placed AFTER the trampolines but BEFORE the literal pool so
     * its final guest-space offset is known in time to backfill
     * g_literals[g_jmpr_lit_idx] before the pool-emission loop just below
     * writes that slot's bytes. g_instr_off[] is already complete for
     * every real pc at this point (the main per-instruction loop above
     * has finished), so the table contents themselves can be written now
     * too, directly into out_buf.
     *
     * Table entries are plain byte offsets into out_buf (g_instr_off[pc]),
     * NOT host addresses -- see g_jmpr_lit_idx's design-notes block above
     * for why: rv64_exec.c's guest CPU addresses its own code purely by
     * offset into a bounded guest memory buffer, exactly like every JAL/
     * BEQ/BNE target this file already computes. */
    uint32_t jmpr_table_off = 0;
    if (g_jmpr_lit_idx >= 0) {
        jmpr_table_off = cb.len;
        for (uint32_t q = 0; q < hdr.num_instr; q++) e64(&cb, 0);
        if (cb.overflow) return TX_RV_ERR_BUF_FULL;
        g_literals[g_jmpr_lit_idx] = jmpr_table_off;
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            uint64_t off = (uint64_t)g_instr_off[pc];
            for (int b = 0; b < 8; b++)
                out_buf[jmpr_table_off + pc*8 + b] = (uint8_t)((off >> (8*b)) & 0xFF);
        }
    }

    /* Literal pool, placed right after all code (and the JMPR table, if
     * this object has one). */
    uint32_t pool_base = cb.len;
    for (uint32_t i = 0; i < g_nliterals; i++) e64(&cb, g_literals[i]);
    if (cb.overflow) return TX_RV_ERR_BUF_FULL;

    /* Patch pass 1: branch/call targets (JAL/BEQ/BNE). */
    for (uint32_t i = 0; i < g_nfixups; i++) {
        uint32_t pos = g_fixups[i].instr_pos;
        uint32_t target_pc = g_fixups[i].target_pc;
        if (target_pc >= hdr.num_instr) return TX_RV_ERR_BAD_OPCODE;
        int64_t off = (int64_t)g_instr_off[target_pc] - (int64_t)pos;
        uint32_t word;
        switch (g_fixups[i].kind) {
            case FIX_JAL:
                if (off < -1048576 || off > 1048574) return TX_RV_ERR_BRANCH_OUT_OF_RANGE;
                word = enc_j((int32_t)off, g_fixups[i].rd_or_rs1, OPC_JAL);
                break;
            case FIX_BEQ:
                if (off < -4096 || off > 4094) return TX_RV_ERR_BRANCH_OUT_OF_RANGE;
                word = enc_b((int32_t)off, X_ZERO, g_fixups[i].rd_or_rs1, 0x0, OPC_BRANCH);
                break;
            default: /* FIX_BNE */
                if (off < -4096 || off > 4094) return TX_RV_ERR_BRANCH_OUT_OF_RANGE;
                word = enc_b((int32_t)off, X_ZERO, g_fixups[i].rd_or_rs1, 0x1, OPC_BRANCH);
                break;
        }
        patch32(out_buf, pos, word);
    }

    /* Patch pass 2: literal-pool auipc+ld pairs. */
    for (uint32_t i = 0; i < g_nli_fixups; i++) {
        uint32_t auipc_pos = g_li_fixups[i].auipc_pos;
        uint64_t literal_addr = (uint64_t)pool_base + 8ull * g_li_fixups[i].literal_idx;
        int64_t off = (int64_t)literal_addr - (int64_t)auipc_pos;
        int32_t hi = (int32_t)((off + 0x800) >> 12);
        int32_t lo = (int32_t)(off - ((int64_t)hi << 12));
        uint8_t reg = g_li_fixups[i].reg;
        patch32(out_buf, auipc_pos,   enc_u(hi, reg, OPC_AUIPC));
        patch32(out_buf, auipc_pos+4, enc_i(lo, reg, 0x3, reg, OPC_LOAD));
    }

    *out_len = cb.len;
    *entry_off = found_off;
    return TX_RV_OK;
}

const char* simi_riscv_strerror(int code) {
    switch (code) {
        case TX_RV_OK: return "ok";
        case TX_RV_ERR_BAD_HEADER: return "bad or corrupt SIMI header";
        case TX_RV_ERR_TOO_MANY_INSTR: return "instruction count out of range";
        case TX_RV_ERR_REG_OUT_OF_RANGE: return "register index >= TX_RV_MAX_REGS (64)";
        case TX_RV_ERR_BUF_FULL: return "output buffer too small";
        case TX_RV_ERR_BAD_OPCODE: return "unsupported or malformed opcode";
        case TX_RV_ERR_TOO_MANY_FIXUPS: return "too many branch/call fixups";
        case TX_RV_ERR_ENTRY_NOT_FOUND: return "requested entry name not in object";
        case TX_RV_ERR_LITERAL_OUT_OF_RANGE: return "LOADI64 literal pool index out of range";
        case TX_RV_ERR_TOO_MANY_LITERALS: return "too many 64-bit constants for the literal pool";
        case TX_RV_ERR_BRANCH_OUT_OF_RANGE: return "branch/call target exceeds JAL/BEQ/BNE encodable range";
        case TX_RV_ERR_NAME_OUT_OF_RANGE: return "RESOLVE name-pool index out of range";
        case TX_RV_ERR_FLOAT_UNSUPPORTED: return "float (T_F32/T_F64) not supported by the RV64 translator (scoped out of Phase 10 v1)";
        default: return "unknown error";
    }
}
