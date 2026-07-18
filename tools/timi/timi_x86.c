/*
 * timi_x86.c — Phase 3 TIMI-to-x86-64 native translator. See timi_x86.h for
 * the design notes and documented v1 scope. No libc dependency (mirrors
 * kernel/loader.c's own ld_memcpy-style discipline) so this file compiles
 * unmodified under both host gcc and a freestanding x86_64 cross-compiler.
 */
#include "timi_x86.h"

/* ─── Local no-libc helpers ────────────────────────────────────────────── */
static void tx_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static int tx_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Mirrors timi_isa.h's opcode/type/relation numbering exactly (Phase 1). */
enum {
    OP_ADD=0, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_SHL, OP_SHR, OP_SAR, OP_NEG, OP_MOV, OP_LOADI, OP_LOADI64, OP_CMP,
    OP_BR, OP_BC, OP_CALL, OP_RET, OP_LOAD, OP_STORE, OP_LEA, OP_PTRADD,
    OP_ENTER, OP_LEAVE,
    OP_RESOLVE, OP_OBJSIZE, OP_OBJTYPE, /* v0.3 (Phase 6) */
    OP_COUNT
};
enum { T_I8=0,T_I16,T_I32,T_I64,T_U8,T_U16,T_U32,T_U64,T_F32,T_F64,T_PTR,T_BOOL,T_OBJREF };
enum { REL_EQ=0,REL_NE,REL_LT,REL_LE,REL_GT,REL_GE,REL_LTU,REL_LEU,REL_GTU,REL_GEU };

#define FLAG_IMM    0x1u
#define FLAG_INVERT 0x1u

static int tx_type_signed(int t) { return t==T_I8||t==T_I16||t==T_I32||t==T_I64; }

/* ─── Instruction word decode (mirrors timi_isa.h bit layout) ───────────── */
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

/* ─── Code buffer + fixups ────────────────────────────────────────────── */
struct CodeBuf { uint8_t* buf; uint32_t cap; uint32_t len; int overflow; };

static void e8(struct CodeBuf* cb, uint8_t b) {
    if (cb->len < cb->cap) cb->buf[cb->len] = b;
    else cb->overflow = 1;
    cb->len++;
}
static void e32(struct CodeBuf* cb, int32_t v) {
    uint32_t u = (uint32_t)v;
    e8(cb, (uint8_t)(u & 0xFF));       e8(cb, (uint8_t)((u>>8)&0xFF));
    e8(cb, (uint8_t)((u>>16)&0xFF));   e8(cb, (uint8_t)((u>>24)&0xFF));
}
static void e64(struct CodeBuf* cb, uint64_t v) {
    for (int i = 0; i < 8; i++) e8(cb, (uint8_t)((v >> (8*i)) & 0xFF));
}

#define TX_MAX_FIXUPS_LOCAL 4096
struct Fixup { uint32_t patch_pos; uint32_t target_pc; };
static struct Fixup g_fixups[TX_MAX_FIXUPS_LOCAL];
static uint32_t g_nfixups;
static uint32_t g_instr_off[4096];   /* TX_MAX_INSTR, see timi_x86.h */

/* v0.3 (Phase 6): translate-time context for RESOLVE/OBJSIZE/OBJTYPE
 * codegen, set once at the top of timi_x86_translate() before the main
 * per-instruction loop — same file-scope-global convention as the fixup
 * table above (emit_instr() takes no extra params, so this is how it
 * reaches the runtime function addresses and the name-pool bound). */
static uint64_t g_rt_resolve_fn, g_rt_objsize_fn, g_rt_objtype_fn;
static uint32_t g_num_names;

static int add_fixup(struct CodeBuf* cb, uint32_t target_pc) {
    if (g_nfixups >= TX_MAX_FIXUPS_LOCAL) return 0;
    /* patch_pos = position of the rel32 field, i.e. current end of buffer
     * once the caller has already emitted the opcode byte(s) before it. */
    g_fixups[g_nfixups].patch_pos = cb->len;
    g_fixups[g_nfixups].target_pc = target_pc;
    g_nfixups++;
    e32(cb, 0); /* placeholder rel32, patched in the fixup pass */
    return 1;
}

/* ─── rbp-relative symbolic register slot helpers ────────────────────────
 * Register i lives at [rbp - 8*(i+1)]. Every procedure reserves a fixed
 * TX_MAX_REGS*8-byte frame regardless of its declared ENTER count — see
 * timi_x86.h's documented v1 scope. */
static int32_t reg_disp(int i) { return -(int32_t)(8 * (i + 1)); }

static void ld_rax(struct CodeBuf* cb, int i) {           /* mov rax,[rbp+disp32] */
    e8(cb,0x48); e8(cb,0x8B); e8(cb,0x85); e32(cb, reg_disp(i));
}
static void ld_rcx(struct CodeBuf* cb, int i) {           /* mov rcx,[rbp+disp32] */
    e8(cb,0x48); e8(cb,0x8B); e8(cb,0x8D); e32(cb, reg_disp(i));
}
static void st_rax(struct CodeBuf* cb, int i) {           /* mov [rbp+disp32],rax */
    e8(cb,0x48); e8(cb,0x89); e8(cb,0x85); e32(cb, reg_disp(i));
}
static void st_rcx(struct CodeBuf* cb, int i) {           /* mov [rbp+disp32],rcx */
    e8(cb,0x48); e8(cb,0x89); e8(cb,0x8D); e32(cb, reg_disp(i));
}
static void ld_imm32_rax(struct CodeBuf* cb, int32_t imm) { /* mov rax,imm32 (sign-ext) */
    e8(cb,0x48); e8(cb,0xC7); e8(cb,0xC0); e32(cb, imm);
}
static void ld_imm64_rax(struct CodeBuf* cb, uint64_t imm) { /* movabs rax,imm64 */
    e8(cb,0x48); e8(cb,0xB8); e64(cb, imm);
}
static void ld_imm64_rcx(struct CodeBuf* cb, uint64_t imm) { /* movabs rcx,imm64 */
    e8(cb,0x48); e8(cb,0xB9); e64(cb, imm);
}

/* ─── v0.3 (Phase 7): capability-tag region ───────────────────────────────
 * A second, TX_MAX_REGS-byte block sits directly below the register slots
 * (i.e. at more-negative displacements) — one byte per symbolic register,
 * 1 = "currently holds an unforged T_OBJREF capability", 0 = plain value.
 * See timi_isa.h / timi_interp.c's Phase 7 design-notes block for the
 * full contract (RESOLVE sets it conditionally, MOV propagates it, every
 * other register-writing opcode clears it, OBJSIZE/OBJTYPE require it).
 * TX_TAG_BASE sits right after (below) reg_disp(TX_MAX_REGS-1), so the
 * frame is one contiguous TX_FRAME_BYTES+TX_MAX_REGS block, zeroed in one
 * pass by emit_prologue(). */
#define TX_TAG_BASE (-(int32_t)(TX_MAX_REGS * 8 + TX_MAX_REGS))
static int32_t tag_disp(int i) { return TX_TAG_BASE + i; }

static void ld_tag_cl(struct CodeBuf* cb, int i) {  /* movzx ecx,byte[rbp+disp32] */
    e8(cb,0x0F); e8(cb,0xB6); e8(cb,0x8D); e32(cb, tag_disp(i));
}
static void st_tag_cl(struct CodeBuf* cb, int i) {  /* mov [rbp+disp32],cl */
    e8(cb,0x88); e8(cb,0x8D); e32(cb, tag_disp(i));
}
static void st_tag_imm(struct CodeBuf* cb, int i, uint8_t val) { /* mov byte[rbp+disp32],imm8 */
    e8(cb,0xC6); e8(cb,0x85); e32(cb, tag_disp(i)); e8(cb, val);
}
static void op_test_rcx(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x85); e8(cb,0xC9); }  /* test rcx,rcx */
static void op_setne_cl(struct CodeBuf* cb) { e8(cb,0x0F); e8(cb,0x95); e8(cb,0xC1); }  /* setne cl */

/* Every opcode that writes a plain (non-capability) value into a register
 * slot goes through these instead of the raw st_rax/st_rcx, so "clear the
 * destination's tag" can never be forgotten at a new call site — the
 * unforgeability property depends on this being true for every opcode
 * except MOV (propagates) and RESOLVE (sets conditionally). */
static void st_rax_untag(struct CodeBuf* cb, int i) { st_rax(cb, i); st_tag_imm(cb, i, 0); }
static void st_rcx_untag(struct CodeBuf* cb, int i) { st_rcx(cb, i); st_tag_imm(cb, i, 0); }

/* ─── Local (intra-instruction) conditional branch helpers ───────────────
 * OBJSIZE/OBJTYPE need a runtime branch on the tag-check result — unlike
 * BR/BC/CALL, the branch target is a few bytes further into the SAME
 * emitted TIMI instruction's code, not another TIMI pc, so this doesn't
 * need the global fixup table (that's keyed by target_pc, resolved in a
 * separate pass once every instruction's offset is known). These patch
 * immediately, in the same emit call, exactly like a miniature one-off
 * fixup. */
static uint32_t emit_jz_placeholder(struct CodeBuf* cb) {
    e8(cb,0x0F); e8(cb,0x84);
    uint32_t pos = cb->len;
    e32(cb, 0);
    return pos;
}
static uint32_t emit_jmp_placeholder(struct CodeBuf* cb) {
    e8(cb,0xE9);
    uint32_t pos = cb->len;
    e32(cb, 0);
    return pos;
}
static void patch_local_rel32(struct CodeBuf* cb, uint32_t patch_pos) {
    if (patch_pos + 4 > cb->cap) return;  /* overflow already flagged elsewhere */
    int32_t rel = (int32_t)(cb->len - (patch_pos + 4));
    cb->buf[patch_pos+0] = (uint8_t)(rel & 0xFF);
    cb->buf[patch_pos+1] = (uint8_t)((rel>>8) & 0xFF);
    cb->buf[patch_pos+2] = (uint8_t)((rel>>16) & 0xFF);
    cb->buf[patch_pos+3] = (uint8_t)((rel>>24) & 0xFF);
}

/* ─── ALU rax,rcx (rax = rax OP rcx) ─────────────────────────────────────*/
static void op_add(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x01); e8(cb,0xC8); }
static void op_sub(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x29); e8(cb,0xC8); }
static void op_and(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x21); e8(cb,0xC8); }
static void op_or (struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x09); e8(cb,0xC8); }
static void op_xor(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x31); e8(cb,0xC8); }
static void op_imul(struct CodeBuf* cb){ e8(cb,0x48); e8(cb,0x0F); e8(cb,0xAF); e8(cb,0xC1); }
static void op_cmp(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x39); e8(cb,0xC8); }
static void op_test_rax(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x85); e8(cb,0xC0); }
static void op_not_rax(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0xF7); e8(cb,0xD0); }
static void op_neg_rax(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0xF7); e8(cb,0xD8); }
static void op_cqo(struct CodeBuf* cb)     { e8(cb,0x48); e8(cb,0x99); }
static void op_xor_edx(struct CodeBuf* cb) { e8(cb,0x31); e8(cb,0xD2); }
static void op_idiv_rcx(struct CodeBuf* cb){ e8(cb,0x48); e8(cb,0xF7); e8(cb,0xF9); }
static void op_div_rcx(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0xF7); e8(cb,0xF1); }
static void op_shl_cl(struct CodeBuf* cb)  { e8(cb,0x48); e8(cb,0xD3); e8(cb,0xE0); }
static void op_shr_cl(struct CodeBuf* cb)  { e8(cb,0x48); e8(cb,0xD3); e8(cb,0xE8); }
static void op_sar_cl(struct CodeBuf* cb)  { e8(cb,0x48); e8(cb,0xD3); e8(cb,0xF8); }
static void op_movzx_rax_al(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x0F); e8(cb,0xB6); e8(cb,0xC0); }

static void op_setcc(struct CodeBuf* cb, int rel) {
    static const uint8_t opc[10] = {0x94,0x95,0x9C,0x9E,0x9F,0x9D,0x92,0x96,0x97,0x93};
    e8(cb,0x0F); e8(cb, opc[rel]); e8(cb,0xC0);
}

/* ─── Control flow ────────────────────────────────────────────────────── */
static void op_jmp(struct CodeBuf* cb, uint32_t target_pc) { e8(cb,0xE9); add_fixup(cb, target_pc); }
static void op_jz (struct CodeBuf* cb, uint32_t target_pc) { e8(cb,0x0F); e8(cb,0x84); add_fixup(cb, target_pc); }
static void op_jnz(struct CodeBuf* cb, uint32_t target_pc) { e8(cb,0x0F); e8(cb,0x85); add_fixup(cb, target_pc); }
static void op_call(struct CodeBuf* cb, uint32_t target_pc){ e8(cb,0xE8); add_fixup(cb, target_pc); }

/* ─── Procedure prologue/epilogue ────────────────────────────────────────*/
static void op_push_rbp(struct CodeBuf* cb) { e8(cb,0x55); }
static void op_mov_rbp_rsp(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x89); e8(cb,0xE5); }
static void op_sub_rsp_imm32(struct CodeBuf* cb, int32_t v) { e8(cb,0x48); e8(cb,0x81); e8(cb,0xEC); e32(cb,v); }
static void op_sub_rsp_imm8(struct CodeBuf* cb, uint8_t v)  { e8(cb,0x48); e8(cb,0x83); e8(cb,0xEC); e8(cb,v); }
static void op_add_rsp_imm8(struct CodeBuf* cb, uint8_t v)  { e8(cb,0x48); e8(cb,0x83); e8(cb,0xC4); e8(cb,v); }
static void op_mov_rsp_rbp(struct CodeBuf* cb) { e8(cb,0x48); e8(cb,0x89); e8(cb,0xEC); }
static void op_pop_rbp(struct CodeBuf* cb) { e8(cb,0x5D); }
static void op_ret(struct CodeBuf* cb) { e8(cb,0xC3); }

/* lea rdi,[rbp+disp32] */
static void op_lea_rdi_rbp(struct CodeBuf* cb, int32_t disp) {
    e8(cb,0x48); e8(cb,0x8D); e8(cb,0xBD); e32(cb,disp);
}
static void op_xor_eax(struct CodeBuf* cb) { e8(cb,0x31); e8(cb,0xC0); }
static void op_mov_ecx_imm32(struct CodeBuf* cb, int32_t v) { e8(cb,0xB9); e32(cb,v); }
static void op_rep_stosq(struct CodeBuf* cb) { e8(cb,0xF3); e8(cb,0x48); e8(cb,0xAB); }

/* mov rax,[rbp+disp32] with an arbitrary (possibly positive) disp — used for
 * reading incoming call arguments off the stack in the procedure prologue. */
static void ld_rax_disp(struct CodeBuf* cb, int32_t disp) {
    e8(cb,0x48); e8(cb,0x8B); e8(cb,0x85); e32(cb,disp);
}
/* mov [rsp+disp8],rax — used to marshal outgoing call arguments. disp8 must
 * be in -128..127 (true for all 8 argument slots, max offset 56). */
static void st_rsp_rax(struct CodeBuf* cb, int8_t disp8) {
    e8(cb,0x48); e8(cb,0x89); e8(cb,0x44); e8(cb,0x24); e8(cb,(uint8_t)disp8);
}

/* ─── v0.3 (Phase 6): runtime-call helpers — RESOLVE/OBJSIZE/OBJTYPE make
 * real C-ABI calls out to functions the caller supplies as addresses (see
 * timi_x86.h). These bytes are the standard "align rsp to 16, call, put it
 * back" idiom, done explicitly with r10 as a scratch save slot (r10/r11
 * are both caller-saved and otherwise unused by this translator's own
 * codegen, so clobbering them here is always safe). ───────────────────── */
static void op_mov_rdi_rax(struct CodeBuf* cb) {           /* mov rdi,rax */
    e8(cb,0x48); e8(cb,0x89); e8(cb,0xC7);
}
static void op_mov_r10_rsp(struct CodeBuf* cb) {           /* mov r10,rsp */
    e8(cb,0x49); e8(cb,0x89); e8(cb,0xE2);
}
static void op_mov_rsp_r10(struct CodeBuf* cb) {           /* mov rsp,r10 */
    e8(cb,0x4C); e8(cb,0x89); e8(cb,0xD4);
}
static void op_and_rsp_neg16(struct CodeBuf* cb) {         /* and rsp,-16 */
    e8(cb,0x48); e8(cb,0x83); e8(cb,0xE4); e8(cb,0xF0);
}
static void op_movabs_r11(struct CodeBuf* cb, uint64_t imm) { /* movabs r11,imm64 */
    e8(cb,0x49); e8(cb,0xBB); e64(cb, imm);
}
static void op_call_r11(struct CodeBuf* cb) {               /* call r11 */
    e8(cb,0x41); e8(cb,0xFF); e8(cb,0xD3);
}

/* Full runtime-call sequence: rdi must already hold the single argument
 * (caller's job — arg is either a namepool string pointer for RESOLVE or
 * a T_OBJREF value for OBJSIZE/OBJTYPE). Leaves the callee's return value
 * in rax, which the caller then stores into rD via st_rax(). */
static void emit_runtime_call1(struct CodeBuf* cb, uint64_t fn_addr) {
    op_mov_r10_rsp(cb);
    op_and_rsp_neg16(cb);
    op_movabs_r11(cb, fn_addr);
    op_call_r11(cb);
    op_mov_rsp_r10(cb);
}

/* ─── LOAD/STORE with per-type width+signedness (mirrors width_of() in the
 * Phase 1 reference interpreter) — operates on [rax+disp32]. ─────────────*/
static int type_width(int t) {
    switch (t) {
        case T_I8: case T_U8: case T_BOOL: return 1;
        case T_I16: case T_U16: return 2;
        case T_I32: case T_U32: case T_F32: return 4;
        default: return 8;
    }
}
/* NOTE on the ModRM byte used throughout this pair of functions: dest/src
 * register is RCX/ECX/CX/CL (reg field = 001), base register is RAX (rm
 * field = 000), addressing mode is [reg+disp32] (mod = 10). ModRM =
 * (mod<<6)|(reg<<3)|rm = (0b10<<6)|(0b001<<3)|0b000 = 0x88. */
static void load_mem_to_rcx(struct CodeBuf* cb, int type, int32_t disp) {
    int w = type_width(type);
    int is_signed = (type==T_I8||type==T_I16||type==T_I32);
    if (w == 8) { e8(cb,0x48); e8(cb,0x8B); e8(cb,0x88); e32(cb,disp); return; }               /* mov rcx,[rax+disp] */
    if (w == 4) {
        if (is_signed) { e8(cb,0x48); e8(cb,0x63); e8(cb,0x88); e32(cb,disp); }                /* movsxd rcx,[rax+disp] */
        else            { e8(cb,0x8B); e8(cb,0x88); e32(cb,disp); }                            /* mov ecx,[rax+disp] (zero-extends) */
        return;
    }
    if (w == 2) {
        if (is_signed) { e8(cb,0x48); e8(cb,0x0F); e8(cb,0xBF); e8(cb,0x88); e32(cb,disp); }   /* movsx rcx,word[rax+disp] */
        else            { e8(cb,0x48); e8(cb,0x0F); e8(cb,0xB7); e8(cb,0x88); e32(cb,disp); }  /* movzx rcx,word[rax+disp] */
        return;
    }
    /* w == 1 */
    if (is_signed) { e8(cb,0x48); e8(cb,0x0F); e8(cb,0xBE); e8(cb,0x88); e32(cb,disp); }        /* movsx rcx,byte[rax+disp] */
    else            { e8(cb,0x48); e8(cb,0x0F); e8(cb,0xB6); e8(cb,0x88); e32(cb,disp); }       /* movzx rcx,byte[rax+disp] */
}
static void store_rcx_to_mem(struct CodeBuf* cb, int type, int32_t disp) {
    int w = type_width(type);
    if (w == 8) { e8(cb,0x48); e8(cb,0x89); e8(cb,0x88); e32(cb,disp); return; }  /* mov [rax+disp],rcx */
    if (w == 4) { e8(cb,0x89);              e8(cb,0x88); e32(cb,disp); return; } /* mov [rax+disp],ecx */
    if (w == 2) { e8(cb,0x66); e8(cb,0x89); e8(cb,0x88); e32(cb,disp); return; } /* mov [rax+disp],cx  */
    /* w == 1 */ e8(cb,0x88);              e8(cb,0x88); e32(cb,disp);           /* mov [rax+disp],cl  */
}
/* lea rax,[rax+disp32] */
static void op_lea_rax_rax(struct CodeBuf* cb, int32_t disp) {
    e8(cb,0x48); e8(cb,0x8D); e8(cb,0x80); e32(cb,disp);
}
/* imul rcx,rcx,imm32 */
static void op_imul_rcx_imm(struct CodeBuf* cb, int32_t imm) {
    e8(cb,0x48); e8(cb,0x69); e8(cb,0xC9); e32(cb,imm);
}

/* ─── One procedure's prologue (emitted at OP_ENTER) ─────────────────────
 * push rbp; mov rbp,rsp; sub rsp,FRAME; zero the frame; copy incoming
 * r0..r7 args from the caller's outgoing-argument stack slots at
 * [rbp+16 .. rbp+16+56] (pushed there by op_call_site below).
 *
 * v0.3 (Phase 7): TX_FRAME_BYTES now covers the register slots AND the
 * capability-tag region below them (TX_TOTAL_FRAME_BYTES) as one
 * contiguous block, zeroed in a single rep-stosq pass — every register
 * starts untagged, matching "tags don't cross CALL" (incoming args are
 * copied in via plain st_rax below, onto already-zeroed tag bytes). */
#define TX_FRAME_BYTES       (TX_MAX_REGS * 8)
#define TX_TOTAL_FRAME_BYTES (TX_FRAME_BYTES + TX_MAX_REGS)

static void emit_prologue(struct CodeBuf* cb) {
    op_push_rbp(cb);
    op_mov_rbp_rsp(cb);
    op_sub_rsp_imm32(cb, TX_TOTAL_FRAME_BYTES);
    /* zero the frame + tag region in one pass: rdi = the lowest address in
     * either (the bottom of the tag region, TX_TAG_BASE == -TX_TOTAL_FRAME_BYTES). */
    op_xor_eax(cb);
    op_lea_rdi_rbp(cb, -(int32_t)TX_TOTAL_FRAME_BYTES);
    op_mov_ecx_imm32(cb, TX_TOTAL_FRAME_BYTES / 8);
    op_rep_stosq(cb);
    /* copy incoming args into r0..r7 (tag bytes already zero from above —
     * capabilities never cross a CALL boundary in this v1, see the Phase 7
     * design notes at ld_tag_cl()/st_tag_cl() above) */
    int nargs = TX_MAX_REGS < 8 ? TX_MAX_REGS : 8;
    for (int i = 0; i < nargs; i++) {
        ld_rax_disp(cb, 16 + 8*i);
        st_rax(cb, i);
    }
}

/* ─── Call-site codegen: marshal r0..r7, call, store result into rD ──────
 * v0.3 (Phase 7): result stored via st_rax_untag — capabilities don't
 * cross a CALL boundary in this v1 (a "capability" returned via r0
 * arrives untagged in the caller, same as an argument arrives untagged
 * in the callee — see emit_prologue()'s comment). */
static void emit_call_site(struct CodeBuf* cb, uint32_t target_pc, int rd) {
    op_sub_rsp_imm8(cb, 64);
    int nargs = TX_MAX_REGS < 8 ? TX_MAX_REGS : 8;
    for (int i = 0; i < nargs; i++) {
        ld_rax(cb, i);
        st_rsp_rax(cb, (int8_t)(8*i));
    }
    for (int i = nargs; i < 8; i++) { /* TX_MAX_REGS < 8 is not expected, but stay safe */
        op_xor_eax(cb);
        st_rsp_rax(cb, (int8_t)(8*i));
    }
    op_call(cb, target_pc);
    op_add_rsp_imm8(cb, 64);
    st_rax_untag(cb, rd);
}

/* ─── Trampoline: zero r0..r5, r6=namepool_ptr, r7=scratch_ptr, call entry,
 * ret ─────────────────────────────────────────────────────────────────
 * Always ends in a plain `ret` — this makes the translated entry point a
 * normal callable function under the SysV ABI (int64_t fn(void) returning
 * the TIMI program's r0 in RAX), which is what lets the host JIT test
 * harness call it directly via a function pointer. The kernel wraps this
 * trampoline in its own tiny stub that turns that final return into a
 * SYS_SLS_EXIT syscall instead — see kernel/timi_translate.c.
 *
 * v0.3 (Phase 6): r6 joins r7 as a second trampoline-provided constant —
 * see timi_x86.h's top comment for why RESOLVE needs a raw pointer into
 * the object's own name pool rather than a copied buffer. */
static void emit_trampoline(struct CodeBuf* cb, uint32_t target_pc,
                             uint64_t scratch_ptr, uint64_t namepool_ptr) {
    op_sub_rsp_imm8(cb, 64);
    op_xor_eax(cb);
    for (int i = 0; i < 6; i++) st_rsp_rax(cb, (int8_t)(8*i));
    ld_imm64_rax(cb, namepool_ptr);
    st_rsp_rax(cb, 48);
    ld_imm64_rax(cb, scratch_ptr);
    st_rsp_rax(cb, 56);
    op_call(cb, target_pc);
    op_add_rsp_imm8(cb, 64);
    op_ret(cb);
}

/* ─── Main per-instruction codegen ────────────────────────────────────── */
static int emit_instr(struct CodeBuf* cb, uint64_t w) {
    uint8_t op = w_op(w), type = w_type(w), flags = w_flags(w);
    uint16_t rd = w_rd(w), ra = w_ra(w);

    if (rd >= TX_MAX_REGS || ra >= TX_MAX_REGS) return TX_ERR_REG_OUT_OF_RANGE;
    if ((op==OP_ADD||op==OP_SUB||op==OP_MUL||op==OP_DIV||op==OP_MOD||op==OP_AND||
         op==OP_OR||op==OP_XOR||op==OP_SHL||op==OP_SHR||op==OP_SAR||op==OP_CMP||
         op==OP_PTRADD) && !(flags & FLAG_IMM) && w_rb_reg(w) >= TX_MAX_REGS)
        return TX_ERR_REG_OUT_OF_RANGE;

    switch (op) {
    case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR:
    case OP_MUL: case OP_SHL: case OP_SHR: case OP_SAR: {
        ld_rax(cb, ra);
        if (flags & FLAG_IMM) ld_imm64_rcx(cb, (uint64_t)(int64_t)w_imm28(w));
        else                  ld_rcx(cb, w_rb_reg(w));
        switch (op) {
            case OP_ADD: op_add(cb); break;
            case OP_SUB: op_sub(cb); break;
            case OP_AND: op_and(cb); break;
            case OP_OR:  op_or(cb);  break;
            case OP_XOR: op_xor(cb); break;
            case OP_MUL: op_imul(cb); break;
            case OP_SHL: op_shl_cl(cb); break;
            case OP_SHR: op_shr_cl(cb); break;
            case OP_SAR: op_sar_cl(cb); break;
        }
        st_rax_untag(cb, rd);
        break;
    }
    case OP_DIV: case OP_MOD: {
        ld_rax(cb, ra);
        if (flags & FLAG_IMM) ld_imm64_rcx(cb, (uint64_t)(int64_t)w_imm28(w));
        else                  ld_rcx(cb, w_rb_reg(w));
        if (tx_type_signed(type)) { op_cqo(cb); op_idiv_rcx(cb); }
        else                      { op_xor_edx(cb); op_div_rcx(cb); }
        if (op == OP_DIV) st_rax_untag(cb, rd);
        else { /* remainder is in rdx; move to rax's slot via rax<-rdx is not
                 * directly encoded above, so store rdx by reusing st_rax's
                 * pattern is wrong — emit a dedicated mov rax,rdx first. */
            e8(cb,0x48); e8(cb,0x89); e8(cb,0xD0); /* mov rax,rdx */
            st_rax_untag(cb, rd);
        }
        break;
    }
    case OP_NOT: ld_rax(cb, ra); op_not_rax(cb); st_rax_untag(cb, rd); break;
    case OP_NEG: ld_rax(cb, ra); op_neg_rax(cb); st_rax_untag(cb, rd); break;
    case OP_MOV:
        /* v0.3 (Phase 7): the one opcode besides RESOLVE that can produce
         * a tagged register — a plain copy of an existing capability is
         * still a valid capability, that's how it gets passed around. */
        ld_rax(cb, ra); st_rax(cb, rd);
        ld_tag_cl(cb, ra); st_tag_cl(cb, rd);
        break;
    case OP_LOADI: ld_imm32_rax(cb, w_imm28(w)); st_rax_untag(cb, rd); break;
    case OP_LOADI64: {
        /* rb_raw holds the literal pool index; resolved to a value by the
         * caller before calling emit_instr via a pre-pass substitution —
         * see translate() below, which rewrites this case inline. */
        return TX_ERR_BAD_OPCODE; /* unreachable: handled specially in translate() */
    }
    case OP_CMP: {
        ld_rax(cb, ra);
        ld_rcx(cb, w_rb_reg(w));
        op_cmp(cb);
        if (flags >= 10) return TX_ERR_BAD_OPCODE;
        op_setcc(cb, flags);
        op_movzx_rax_al(cb);
        st_rax_untag(cb, rd);
        break;
    }
    case OP_BR: {
        int32_t disp = w_imm28(w);
        /* target_pc computed by caller (translate loop knows current pc) */
        (void)disp;
        return TX_ERR_BAD_OPCODE; /* handled specially in translate() (needs pc) */
    }
    case OP_LEA: {
        ld_rax(cb, ra);
        op_lea_rax_rax(cb, w_imm28(w));
        st_rax_untag(cb, rd);
        break;
    }
    case OP_PTRADD: {
        /* v0.3 (Phase 7): always untagged — pointer arithmetic must never
         * yield a capability, even when rA itself is currently tagged.
         * This is the core unforgeability property: if arithmetic could
         * preserve or produce a tag, an attacker could derive a "valid"
         * capability for an arbitrary neighboring address from a real one. */
        ld_rax(cb, ra);
        int scale = type_width(type);
        if (flags & FLAG_IMM) {
            int64_t idx = w_imm28(w);
            ld_imm64_rcx(cb, (uint64_t)(idx * scale));
        } else {
            ld_rcx(cb, w_rb_reg(w));
            op_imul_rcx_imm(cb, scale);
        }
        op_add(cb);
        st_rax_untag(cb, rd);
        break;
    }
    case OP_LOAD: {
        ld_rax(cb, ra);                       /* rax = base pointer */
        load_mem_to_rcx(cb, type, w_imm28(w));
        st_rcx_untag(cb, rd);
        break;
    }
    case OP_STORE: {
        ld_rax(cb, ra);                       /* rax = base pointer */
        ld_rcx(cb, rd);                       /* rd holds the *source* value register for STORE */
        store_rcx_to_mem(cb, type, w_imm28(w));
        break;
    }
    case OP_ENTER: emit_prologue(cb); break;
    case OP_LEAVE: /* no-op directive, matches Phase 1 interpreter */ break;
    case OP_RESOLVE: {
        /* rb_raw holds the name-pool index (see FMT_RESOLVE in timi_isa.h).
         * rdi = namepool_ptr(r6) + idx*TX_NAME_SIZE, a raw pointer straight
         * into the object's own name-pool bytes — no copy.
         *
         * v0.3 (Phase 7): tag the destination iff the resolve succeeded
         * (result != 0) — computed via test+setne BEFORE storing the
         * result (TEST/SETcc don't touch rax, so this ordering is safe),
         * then both the value and its tag are written. */
        uint32_t idx = w_rb_raw(w);
        if (idx >= g_num_names) return TX_ERR_NAME_OUT_OF_RANGE;
        ld_rax(cb, TX_NAMEPOOL_ARG_IDX);
        op_lea_rax_rax(cb, (int32_t)(idx * TX_NAME_SIZE));
        op_mov_rdi_rax(cb);
        emit_runtime_call1(cb, g_rt_resolve_fn);
        op_test_rax(cb);
        op_setne_cl(cb);
        st_rax(cb, rd);
        st_tag_cl(cb, rd);
        break;
    }
    case OP_OBJSIZE: case OP_OBJTYPE: {
        /* v0.3 (Phase 7): require rA to currently carry a valid capability
         * tag before ever consulting the runtime catalog. An untagged
         * operand — no matter what bit pattern it holds, even one that
         * happens to equal a real object's base_vaddr — is rejected with
         * the same sentinel used for "no such object," and the runtime
         * function is never called at all: this is what closes the
         * forgery hole (`LOADI r0,#0x2000,objref` followed by OBJSIZE
         * must not be able to read out a real object's size). */
        uint64_t fn = (op == OP_OBJSIZE) ? g_rt_objsize_fn : g_rt_objtype_fn;
        uint64_t invalid_sentinel = (op == OP_OBJSIZE) ? 0ull : 0xFFFFFFFFull;
        ld_tag_cl(cb, ra);
        op_test_rcx(cb);
        uint32_t jz_pos = emit_jz_placeholder(cb);       /* jz .invalid */
        ld_rax(cb, ra);                                  /* T_OBJREF value */
        op_mov_rdi_rax(cb);
        emit_runtime_call1(cb, fn);
        uint32_t jmp_pos = emit_jmp_placeholder(cb);      /* jmp .done */
        patch_local_rel32(cb, jz_pos);                    /* .invalid: */
        ld_imm64_rax(cb, invalid_sentinel);
        patch_local_rel32(cb, jmp_pos);                   /* .done: */
        st_rax_untag(cb, rd);
        break;
    }
    case OP_RET:
        ld_rax(cb, 0);       /* r0 is the return-value register, §4.8 */
        op_mov_rsp_rbp(cb);
        op_pop_rbp(cb);
        op_ret(cb);
        break;
    case OP_CALL:
        return TX_ERR_BAD_OPCODE; /* handled specially in translate() (needs pc) */
    default:
        return TX_ERR_BAD_OPCODE;
    }
    return TX_OK;
}

/* ─── Top-level translate: two passes (emit + patch fixups) ─────────────*/
int timi_x86_translate(const uint8_t* obj_data, uint32_t obj_size,
                        uint8_t* out_buf, uint32_t out_cap,
                        const char* entry_name, uint64_t scratch_ptr,
                        uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                        uint64_t rt_objtype_fn,
                        uint32_t* out_len, uint32_t* entry_off) {
    if (!obj_data || obj_size < sizeof(struct TimiObjHdr)) return TX_ERR_BAD_HEADER;
    struct TimiObjHdr hdr;
    tx_memcpy(&hdr, obj_data, sizeof(hdr));
    if (hdr.magic != TIMI_X_MAGIC) return TX_ERR_BAD_HEADER;

    uint64_t expect = sizeof(struct TimiObjHdr);
    expect += (uint64_t)hdr.num_instr * 8;
    expect += (uint64_t)hdr.num_literals * 8;
    expect += (uint64_t)hdr.num_entries * sizeof(struct TxEntryRec);
    expect += (uint64_t)hdr.num_names * sizeof(struct TxNameRec);
    if (expect != (uint64_t)obj_size) return TX_ERR_BAD_HEADER;
    if (hdr.num_instr == 0 || hdr.num_instr > 4096) return TX_ERR_TOO_MANY_INSTR;

    const uint64_t* instrs = (const uint64_t*)(obj_data + sizeof(struct TimiObjHdr));
    const uint64_t* literals = (const uint64_t*)((const uint8_t*)instrs + hdr.num_instr * 8);
    const struct TxEntryRec* entries = (const struct TxEntryRec*)
        ((const uint8_t*)literals + hdr.num_literals * 8);
    const struct TxNameRec* names = (const struct TxNameRec*)
        ((const uint8_t*)entries + hdr.num_entries * sizeof(struct TxEntryRec));
    /* v0.3: namepool_ptr is baked as a raw pointer straight into obj_data's
     * own name-pool bytes — see timi_x86.h's top comment. */
    uint64_t namepool_ptr = (uint64_t)(uintptr_t)names;

    g_rt_resolve_fn = rt_resolve_fn;
    g_rt_objsize_fn = rt_objsize_fn;
    g_rt_objtype_fn = rt_objtype_fn;
    g_num_names = hdr.num_names;

    struct CodeBuf cb; cb.buf = out_buf; cb.cap = out_cap; cb.len = 0; cb.overflow = 0;
    g_nfixups = 0;

    for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
        g_instr_off[pc] = cb.len;
        uint64_t w = instrs[pc];
        uint8_t op = w_op(w);

        if (op == OP_LOADI64) {
            uint32_t idx = w_rb_raw(w);
            if (idx >= hdr.num_literals) return TX_ERR_LITERAL_OUT_OF_RANGE;
            uint16_t rd = w_rd(w);
            if (rd >= TX_MAX_REGS) return TX_ERR_REG_OUT_OF_RANGE;
            ld_imm64_rax(&cb, literals[idx]);
            st_rax_untag(&cb, rd);
        } else if (op == OP_BR) {
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            op_jmp(&cb, target);
        } else if (op == OP_BC) {
            uint16_t ra = w_ra(w);
            if (ra >= TX_MAX_REGS) return TX_ERR_REG_OUT_OF_RANGE;
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            ld_rax(&cb, ra);
            op_test_rax(&cb);
            if (w_flags(w) & FLAG_INVERT) op_jz(&cb, target);
            else                          op_jnz(&cb, target);
        } else if (op == OP_CALL) {
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            /* CALL has no rD in the ISA (it's a pure control-transfer —
             * the callee's RET convention always writes r0), so the call
             * site always stores the result into the CALLER's r0. */
            emit_call_site(&cb, target, 0);
        } else {
            int rc = emit_instr(&cb, w);
            if (rc != TX_OK) return rc;
        }
        if (cb.overflow) return TX_ERR_BUF_FULL;
    }

    /* One trampoline per exported entry, appended after the body. */
    uint32_t found_off = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < hdr.num_entries; i++) {
        uint32_t tramp_off = cb.len;
        emit_trampoline(&cb, entries[i].offset, scratch_ptr, namepool_ptr);
        if (cb.overflow) return TX_ERR_BUF_FULL;
        if (tx_streq(entries[i].name, entry_name)) found_off = tramp_off;
    }
    if (found_off == 0xFFFFFFFFu) return TX_ERR_ENTRY_NOT_FOUND;

    /* Patch pass: every fixup's 4-byte field currently holds 0; rel32 is
     * relative to the byte immediately following the field itself. */
    for (uint32_t i = 0; i < g_nfixups; i++) {
        uint32_t pos = g_fixups[i].patch_pos;
        uint32_t target_pc = g_fixups[i].target_pc;
        if (target_pc >= hdr.num_instr) return TX_ERR_BAD_OPCODE;
        int32_t rel = (int32_t)(g_instr_off[target_pc] - (pos + 4));
        out_buf[pos+0] = (uint8_t)(rel & 0xFF);
        out_buf[pos+1] = (uint8_t)((rel>>8) & 0xFF);
        out_buf[pos+2] = (uint8_t)((rel>>16) & 0xFF);
        out_buf[pos+3] = (uint8_t)((rel>>24) & 0xFF);
    }

    *out_len = cb.len;
    *entry_off = found_off;
    return TX_OK;
}

const char* timi_x86_strerror(int code) {
    switch (code) {
        case TX_OK: return "ok";
        case TX_ERR_BAD_HEADER: return "bad or corrupt TIMI header";
        case TX_ERR_TOO_MANY_INSTR: return "instruction count out of range";
        case TX_ERR_REG_OUT_OF_RANGE: return "register index >= TX_MAX_REGS (64)";
        case TX_ERR_BUF_FULL: return "output buffer too small";
        case TX_ERR_BAD_OPCODE: return "unsupported or malformed opcode";
        case TX_ERR_TOO_MANY_FIXUPS: return "too many branch/call fixups";
        case TX_ERR_ENTRY_NOT_FOUND: return "requested entry name not in object";
        case TX_ERR_LITERAL_OUT_OF_RANGE: return "LOADI64 literal pool index out of range";
        case TX_ERR_NAME_OUT_OF_RANGE: return "RESOLVE name-pool index out of range";
        default: return "unknown error";
    }
}
