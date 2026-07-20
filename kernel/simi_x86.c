/*
 * simi_x86.c — Phase 3 SIMI-to-x86-64 native translator (kernel copy).
 * Byte-identical to the host toolchain's simi_x86.c, verified there by
 * simi-jit-test: real execution of this exact encoder's output on x86-64
 * hardware, for the full v0.2 core opcode set (arithmetic, DIV/MOD,
 * shifts, CMP relations signed+unsigned, BR/BC/CALL/RET, typed LOAD/STORE,
 * LEA/PTRADD, LOADI/LOADI64) — see AeroSLS-SIMI-ISA-v0.1.md §9. No libc
 * dependency (mirrors kernel/loader.c's own ld_memcpy-style discipline).
 *
 * v0.3 (Phase 6) adds RESOLVE/OBJSIZE/OBJTYPE (real C-ABI calls out to
 * rt_resolve_fn/rt_objsize_fn/rt_objtype_fn, baked in as translate-time
 * constants) — verified on the host side by simi-jit-test executing this
 * exact encoder's output against a mock object catalog; see
 * AeroSLS-SIMI-ISA-v0.1.md §13. This kernel binds those three function
 * pointers to kernel/simi_runtime.c's real object_catalog[] queries
 * instead (see kernel/simi_translate.c).
 *
 * v0.3 (Phase 7) adds the capability-tag region: every symbolic register
 * slot gets a paired tag byte enforcing that T_OBJREF values are tagged
 * (unforgeable) and only RESOLVE/MOV can produce a tagged register —
 * verified on the host side by simi-jit-test actually executing a forged-
 * capability attempt on real hardware and confirming it's rejected. See
 * AeroSLS-SIMI-ISA-v0.1.md §14.
 */
#include "simi_x86.h"

/* ─── Local no-libc helpers ────────────────────────────────────────────── */
static void tx_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static int tx_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Mirrors simi_isa.h's opcode/type/relation numbering exactly (Phase 1). */
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

static int tx_type_signed(int t) { return t==T_I8||t==T_I16||t==T_I32||t==T_I64; }

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
static uint32_t g_instr_off[4096];   /* TX_MAX_INSTR, see simi_x86.h */

/* v0.3 (Phase 6): translate-time context for RESOLVE/OBJSIZE/OBJTYPE
 * codegen, set once at the top of simi_x86_translate() before the main
 * per-instruction loop — same file-scope-global convention as the fixup
 * table above (emit_instr() takes no extra params, so this is how it
 * reaches the runtime function addresses and the name-pool bound). */
static uint64_t g_rt_resolve_fn, g_rt_objsize_fn, g_rt_objtype_fn;
static uint32_t g_num_names;

/* Gap Remediation SIMI Phase 14: JMPR support. out_buf's final runtime
 * address is already known to simi_x86_translate() at call time (the
 * caller mmaps/allocates it BEFORE calling translate(), and nothing
 * relocates it afterward) -- unlike BR/CALL, which use position-
 * independent rel32 fixups purely because that's the natural encoding
 * for direct jumps, JMPR's target is only known at RUNTIME, so its
 * codegen needs an absolute base address it can index into. Baking
 * g_out_buf_addr directly, the same way rt_resolve_fn/scratch_ptr/
 * namepool_ptr already get baked as absolute translate-time constants
 * elsewhere in this file, is simpler than inventing RIP-relative
 * addressing for this one case. g_has_jmpr short-circuits the whole
 * mechanism (table reservation, backfill pass) for the common case of an
 * object with no JMPR at all, avoiding an unconditional per-object
 * num_instr*8-byte cost for every translation. */
static uint64_t g_out_buf_addr;
static int g_has_jmpr;
static uint32_t g_num_instr; /* hdr.num_instr, for JMPR's runtime bounds check --
                               * emit_instr() takes no extra params, same reason
                               * g_rt_resolve_fn etc. above are file-scope globals. */

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

/* ─── Gap Remediation SIMI Phase 11: real register allocation ─────────────
 * Design: docs/AeroSLS-SIMI-ISA-v0.1.md §16 Phase 11. v1 scope: a symbolic
 * register is eligible for allocation into a real physical GP register
 * only if its entire live range (first touch to last touch, in linear PC
 * order) is confined to straight-line code within ONE procedure — no
 * backward branch (any loop) and no call of any kind (CALL to another
 * translated procedure, or a real RESOLVE/OBJSIZE/OBJTYPE runtime-C-
 * function call) may fall within that range. This is a real, honest
 * widening beyond the design doc's own text, which only discusses loops:
 * a CALL's callee is separately-allocated code with no save/restore
 * convention for the pool registers below (its own ENTER prologue is
 * free to clobber any of them for its own symbolic registers), and a
 * RESOLVE/OBJSIZE/OBJTYPE runtime call is a real C function under the
 * SysV ABI, free to clobber every caller-saved register in the pool
 * (rsi/rdi/r8/r9) without notice. Rather than build cross-procedure
 * callee-saved-register preservation (a real, much bigger feature),
 * v1 just forces any register whose live range spans one of these to
 * stay on the stack -- exactly the same "spilled and never-eligible are
 * the same code path" idea decision 1's own text already establishes for
 * loops, generalized to every kind of call.
 *
 * Registers NEVER in the allocation pool: rax/rcx/rdx (the fixed ALU
 * scratch pair this translator's entire codegen shape already assumes
 * every opcode is free to clobber), rbp/rsp (frame/stack pointers), and
 * r10/r11 (emit_runtime_call1()'s own scratch pair for the RSP-realign-
 * then-indirect-call sequence).
 *
 * CORRECTION found by the regression suite, not anticipated in the
 * design doc: rbx/r12/r13/r14/r15 are SysV-ABI *callee-saved* registers
 * -- and this translator's trampoline is a REAL external entry point,
 * called directly from ordinary compiled C (the host JIT test harness
 * today; the kernel's own C caller once wired in). A callee-saved
 * register must be restored to its original value before that call
 * returns, or the C caller's own live values in it are silently
 * corrupted. This file has no push/pop preservation for any register
 * anywhere (emit_prologue/OP_RET/emit_trampoline never save one), so
 * putting rbx/r12-r15 in the pool broke every test whose call chain
 * (trampoline -> ... -> C caller) crossed a symbolic register allocated
 * into one of them -- confirmed the hard way: `make test-native` printed
 * "PASS" (the translated function's own return value was correct) but
 * then exited nonzero, because the *host test harness's own* rbx/r12-r15
 * values -- live across its `fn()` call per its compiler's own codegen
 * -- came back corrupted, and the corruption surfaced somewhere later in
 * main() (print/exit path), not in the comparison that produced "PASS".
 * Building real callee-saved push/pop preservation is a genuine, larger
 * feature (every RET, every procedure, the trampoline) -- explicitly out
 * of v1 scope. Pool is therefore caller-saved-only: rsi, rdi, r8, r9 --
 * registers the SysV ABI already treats as freely clobberable by any
 * call (including the outermost call from real C into the trampoline),
 * meaning zero extra preservation logic is needed for them at all, by
 * construction. Encoding: 0=rax,1=rcx,2=rdx,3=rbx,4=rsp,5=rbp,6=rsi,
 * 7=rdi,8-15=r8-r15. */
#define TX_POOL_SIZE 4
static const int8_t g_pool[TX_POOL_SIZE] = {6,7,8,9};

/* -1 = this symbolic register stays on its rbp-relative stack slot
 * (spilled, or simply never eligible -- same thing, see above); else the
 * physical GP register (x86 encoding 0-15) it's bound to for the
 * duration of the procedure currently being translated. Recomputed by
 * compute_alloc_for_proc() at every OP_ENTER; every ld_rax/ld_rcx/
 * st_rax/st_rcx call below consults it. */
static int8_t g_physreg_of[TX_MAX_REGS];

/* mov dst64, src64 (REX.W + 0x89 /r: MOV r/m64, r64 -- reg=src, rm=dst
 * in register-direct form). dst==src can't actually occur given the pool
 * above never contains 0/1 (rax/rcx, the only dst/src the ld_ and st_
 * helpers below ever pass), but the no-op elision is free and correct to
 * keep regardless. */
static void mov_reg64_reg64(struct CodeBuf* cb, int dst, int src) {
    if (dst == src) return;
    uint8_t rex = (uint8_t)(0x48u | ((src>=8)?0x04u:0u) | ((dst>=8)?0x01u:0u));
    uint8_t modrm = (uint8_t)(0xC0u | ((src & 7) << 3) | (dst & 7));
    e8(cb, rex); e8(cb, 0x89); e8(cb, modrm);
}

/* ─── rbp-relative symbolic register slot helpers ────────────────────────
 * Register i lives at [rbp - 8*(i+1)]. Every procedure reserves a fixed
 * TX_MAX_REGS*8-byte frame regardless of its declared ENTER count — see
 * simi_x86.h's documented v1 scope. Gap Remediation SIMI Phase 11: each
 * of the four primitives below now checks g_physreg_of[i] first — an
 * allocated symbolic register redirects to a register-register mov
 * instead of a real rbp-relative memory access, transparently to every
 * one of this file's ~20 opcode cases, none of which needed to change. */
static int32_t reg_disp(int i) { return -(int32_t)(8 * (i + 1)); }

static void ld_rax(struct CodeBuf* cb, int i) {           /* mov rax,[rbp+disp32] (or mov rax,<physreg>) */
    if (i >= 0 && i < TX_MAX_REGS && g_physreg_of[i] >= 0) { mov_reg64_reg64(cb, 0, g_physreg_of[i]); return; }
    e8(cb,0x48); e8(cb,0x8B); e8(cb,0x85); e32(cb, reg_disp(i));
}
static void ld_rcx(struct CodeBuf* cb, int i) {           /* mov rcx,[rbp+disp32] (or mov rcx,<physreg>) */
    if (i >= 0 && i < TX_MAX_REGS && g_physreg_of[i] >= 0) { mov_reg64_reg64(cb, 1, g_physreg_of[i]); return; }
    e8(cb,0x48); e8(cb,0x8B); e8(cb,0x8D); e32(cb, reg_disp(i));
}
static void st_rax(struct CodeBuf* cb, int i) {           /* mov [rbp+disp32],rax (or mov <physreg>,rax) */
    if (i >= 0 && i < TX_MAX_REGS && g_physreg_of[i] >= 0) { mov_reg64_reg64(cb, g_physreg_of[i], 0); return; }
    e8(cb,0x48); e8(cb,0x89); e8(cb,0x85); e32(cb, reg_disp(i));
}
static void st_rcx(struct CodeBuf* cb, int i) {           /* mov [rbp+disp32],rcx (or mov <physreg>,rcx) */
    if (i >= 0 && i < TX_MAX_REGS && g_physreg_of[i] >= 0) { mov_reg64_reg64(cb, g_physreg_of[i], 1); return; }
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
 * See simi_isa.h / simi_interp.c's Phase 7 design-notes block for the
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

/* ─── Gap Remediation SIMI Phase 12: capability tag propagation across
 * CALL/RET ─────────────────────────────────────────────────────────────
 * The r0-r7 argument copy (call site -> callee prologue) and the r0
 * return-value copy (callee RET -> call site) are already plain,
 * uncomputed value copies -- these helpers extend both to also carry the
 * source register's tag, exactly the same rule MOV already applies within
 * one frame (see the Phase 7 design-notes block in simi_interp.c for the
 * full unforgeability argument: a plain copy can never synthesize a
 * capability, so propagating the tag through one is exactly as safe as
 * MOV already is). Design: docs/AeroSLS-SIMI-ISA-v0.1.md §16 Phase 12.
 *
 * Arguments: values are marshaled through a 64-byte (8x8) outgoing-arg
 * scratch area at [rsp+0..63] (call site) / [rbp+16..79] (callee
 * prologue) -- unchanged. A 9th qword slot at [rsp+64]/[rbp+80] now
 * carries an 8-bit mask (bit i = source register i's tag), built by the
 * call site with a plain shift/OR accumulation and consumed by the
 * prologue with a plain shift/AND per argument -- no branches either
 * side, matching the branch-free "compute a boolean into a tag byte"
 * idiom RESOLVE's own codegen already uses (test+setne, above).
 *
 * Return value: only one bit needs to cross, so it rides in `dl` (part of
 * the caller-saved, otherwise-untouched-by-this-path rdx) alongside the
 * value already riding in `rax` -- loaded by RET immediately before the
 * epilogue (mov rsp,rbp; pop rbp; ret), none of which touch rdx, so dl
 * survives intact to the call site's post-`call` code. */
#define TX_ARG_MASK_DISP (16 + 64)   /* [rbp+80]: the 9th outgoing-arg qword */
static void ld_argmask_cl(struct CodeBuf* cb) {   /* movzx ecx, byte[rbp+80] */
    e8(cb,0x0F); e8(cb,0xB6); e8(cb,0x8D); e32(cb, TX_ARG_MASK_DISP);
}
static void st_rsp_al(struct CodeBuf* cb, int8_t disp8) {   /* mov [rsp+disp8],al */
    e8(cb,0x88); e8(cb,0x44); e8(cb,0x24); e8(cb,(uint8_t)disp8);
}
static void op_shl_cl_imm(struct CodeBuf* cb, uint8_t imm) { e8(cb,0xC0); e8(cb,0xE1); e8(cb,imm); }  /* shl cl,imm8 */
static void op_shr_cl_imm(struct CodeBuf* cb, uint8_t imm) { e8(cb,0xC0); e8(cb,0xE9); e8(cb,imm); }  /* shr cl,imm8 */
static void op_and_cl_imm8(struct CodeBuf* cb, uint8_t imm){ e8(cb,0x80); e8(cb,0xE1); e8(cb,imm); }  /* and cl,imm8 */
static void op_or_al_cl(struct CodeBuf* cb) { e8(cb,0x08); e8(cb,0xC8); }   /* or al,cl */
static void ld_tag_dl(struct CodeBuf* cb, int i) {   /* movzx edx, byte[rbp+disp32] */
    e8(cb,0x0F); e8(cb,0xB6); e8(cb,0x95); e32(cb, tag_disp(i));
}
static void st_tag_dl(struct CodeBuf* cb, int i) {   /* mov [rbp+disp32],dl */
    e8(cb,0x88); e8(cb,0x95); e32(cb, tag_disp(i));
}

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
 * emitted SIMI instruction's code, not another SIMI pc, so this doesn't
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
/* Gap Remediation SIMI Phase 14: JAE (jump if CF=0, i.e. unsigned >=) --
 * JMPR's bounds check uses this to reach its trap path. */
static uint32_t emit_jae_placeholder(struct CodeBuf* cb) {
    e8(cb,0x0F); e8(cb,0x83);
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
/* Gap Remediation SIMI Phase 14: JMPR codegen primitives. */
static void op_cmp_rcx_imm32(struct CodeBuf* cb, int32_t imm) {  /* cmp rcx,imm32 (sign-ext) */
    e8(cb,0x48); e8(cb,0x81); e8(cb,0xF9); e32(cb, imm);
}
static void op_lea_rax_rax_rcx8(struct CodeBuf* cb) {  /* lea rax,[rax+rcx*8] */
    e8(cb,0x48); e8(cb,0x8D); e8(cb,0x04); e8(cb,0xC8);
}
static void op_mov_rax_mem_rax(struct CodeBuf* cb) {   /* mov rax,[rax] */
    e8(cb,0x48); e8(cb,0x8B); e8(cb,0x00);
}
static void op_jmp_rax(struct CodeBuf* cb) {           /* jmp rax (indirect) */
    e8(cb,0xFF); e8(cb,0xE0);
}
static void op_ud2(struct CodeBuf* cb) {                /* ud2 -- real #UD trap */
    e8(cb,0x0F); e8(cb,0x0B);
}
/* Writes 8 bytes at an ALREADY-RESERVED absolute offset within cb (not an
 * append -- unlike e64(), this must not advance cb->len, since it's used
 * to backfill the jump-table region reserved before the main translate
 * loop runs, well after cb->len has moved past it). */
static void poke64(struct CodeBuf* cb, uint32_t at, uint64_t v) {
    if ((uint64_t)at + 8 > cb->cap) return; /* overflow already flagged elsewhere */
    for (int i = 0; i < 8; i++) cb->buf[at + i] = (uint8_t)((v >> (8*i)) & 0xFF);
}
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

/* ─── Gap Remediation SIMI Phase 10: float opcode support ─────────────────
 * T_F32/T_F64 reuse the existing ADD/SUB/MUL/DIV/NEG/CMP opcodes (the ISA's
 * type field was designed for exactly this since v0.1) rather than adding
 * dedicated float opcodes. Two constraints shape the codegen below:
 *
 *   1. This file compiles unmodified into the kernel build under
 *      -mno-sse -mno-sse2 -mno-mmx (see simi_x86.h's top comment) — so,
 *      exactly like the rest of this translator, float support here means
 *      hand-emitted SSE2 machine code operating on raw uint64_t bit
 *      patterns, never a real C `double`/`float` anywhere in this file.
 *   2. Every symbolic register is a memory slot (Phase 3's naive
 *      load-operate-store model, unchanged for floats) — so float ADD/
 *      SUB/MUL/DIV bounce operands through XMM0/XMM1 via MOVQ from the
 *      already-loaded RAX/RCX GP registers (reusing ld_rax/ld_rcx exactly
 *      as the integer path does) rather than adding a second, XMM-direct
 *      memory-addressing path — fewer new addressing helpers, same slot
 *      layout, same st_rax_untag() write-back.
 *
 * NEG's float case needs no XMM instruction at all: negating an IEEE
 * float is purely a sign-bit flip, so a plain GP XOR against a sign-mask
 * constant on the raw bits (reusing op_xor's existing rax,rcx form for
 * f64; a new 32-bit eax,ecx form for f32, whose write conveniently
 * zero-extends the upper 32 bits of rax for free — exactly the "zero high
 * 32 bits on f32 write" convention this translator already follows
 * elsewhere) does the whole job.
 *
 * CMP's float case uses UCOMISD/UCOMISS (unordered compare — sets
 * ZF/PF/CF per real IEEE-754 semantics, including NaN) instead of the
 * integer path's CMP+SETcc. GT/GE, and LT/LE with operands swapped,
 * resolve with a single SETcc each: UCOMISD's flag encoding makes the
 * *unordered* case and the *first-operand-is-less* case both set CF=1, so
 * seta/setae (which key off CF=0) are false for NaN automatically — no
 * separate parity check needed. EQ needs an explicit ordered check (sete
 * AND setnp, since unordered also sets ZF=1) because there's no
 * operand-swap trick for equality. NE doesn't need its own comparison:
 * IEEE-754 defines != as the exact logical negation of == in every case,
 * including NaN (unlike the other four relations, which are not simple
 * negations of each other once NaN is in play) — so NE is just EQ's
 * result XORed with 1.
 *
 * FLAG_IMM combined with a float type is rejected outright: a 28-bit
 * immediate can't hold a meaningful IEEE bit pattern, and int-to-float
 * conversion is a different, undesigned semantic — not silently
 * misinterpreted as raw bits.
 */
static void op_movq_xmm0_rax(struct CodeBuf* cb) { e8(cb,0x66); e8(cb,0x48); e8(cb,0x0F); e8(cb,0x6E); e8(cb,0xC0); }
static void op_movq_xmm1_rcx(struct CodeBuf* cb) { e8(cb,0x66); e8(cb,0x48); e8(cb,0x0F); e8(cb,0x6E); e8(cb,0xC9); }
static void op_movq_rax_xmm0(struct CodeBuf* cb) { e8(cb,0x66); e8(cb,0x48); e8(cb,0x0F); e8(cb,0x7E); e8(cb,0xC0); }

static void op_addsd_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0xF2); e8(cb,0x0F); e8(cb,0x58); e8(cb,0xC1); }
static void op_subsd_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0xF2); e8(cb,0x0F); e8(cb,0x5C); e8(cb,0xC1); }
static void op_mulsd_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0xF2); e8(cb,0x0F); e8(cb,0x59); e8(cb,0xC1); }
static void op_divsd_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0xF2); e8(cb,0x0F); e8(cb,0x5E); e8(cb,0xC1); }
static void op_addss_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0xF3); e8(cb,0x0F); e8(cb,0x58); e8(cb,0xC1); }
static void op_subss_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0xF3); e8(cb,0x0F); e8(cb,0x5C); e8(cb,0xC1); }
static void op_mulss_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0xF3); e8(cb,0x0F); e8(cb,0x59); e8(cb,0xC1); }
static void op_divss_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0xF3); e8(cb,0x0F); e8(cb,0x5E); e8(cb,0xC1); }

/* UCOMISD/UCOMISS in both operand orders — the swapped form is what lets
 * LT/LE reuse the seta/setae condition codes (see design note above). */
static void op_ucomisd_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0x66); e8(cb,0x0F); e8(cb,0x2E); e8(cb,0xC1); }
static void op_ucomisd_xmm1_xmm0(struct CodeBuf* cb) { e8(cb,0x66); e8(cb,0x0F); e8(cb,0x2E); e8(cb,0xC8); }
static void op_ucomiss_xmm0_xmm1(struct CodeBuf* cb) { e8(cb,0x0F); e8(cb,0x2E); e8(cb,0xC1); }
static void op_ucomiss_xmm1_xmm0(struct CodeBuf* cb) { e8(cb,0x0F); e8(cb,0x2E); e8(cb,0xC8); }

static void op_seta_al(struct CodeBuf* cb)   { e8(cb,0x0F); e8(cb,0x97); e8(cb,0xC0); }
static void op_setae_al(struct CodeBuf* cb)  { e8(cb,0x0F); e8(cb,0x93); e8(cb,0xC0); }
static void op_sete_al(struct CodeBuf* cb)   { e8(cb,0x0F); e8(cb,0x94); e8(cb,0xC0); }
static void op_setnp_cl(struct CodeBuf* cb)  { e8(cb,0x0F); e8(cb,0x9B); e8(cb,0xC1); }
static void op_and_al_cl(struct CodeBuf* cb) { e8(cb,0x20); e8(cb,0xC8); }              /* and al,cl */
static void op_xor_al_imm8(struct CodeBuf* cb, uint8_t imm) { e8(cb,0x34); e8(cb,imm); } /* xor al,imm8 */

static void ld_imm32_rcx(struct CodeBuf* cb, int32_t imm) { /* mov rcx,imm32 (sign-ext) */
    e8(cb,0x48); e8(cb,0xC7); e8(cb,0xC1); e32(cb, imm);
}
static void op_xor32_eax_ecx(struct CodeBuf* cb) { e8(cb,0x31); e8(cb,0xC8); } /* xor eax,ecx (32-bit form; zero-extends rax) */

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
 * simi_x86.h). These bytes are the standard "align rsp to 16, call, put it
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
    /* copy incoming args into r0..r7 */
    int nargs = TX_MAX_REGS < 8 ? TX_MAX_REGS : 8;
    for (int i = 0; i < nargs; i++) {
        ld_rax_disp(cb, 16 + 8*i);
        st_rax(cb, i);
    }
    /* Gap Remediation SIMI Phase 12: apply the caller's argument-tag mask
     * (see the design-notes block above ld_argmask_cl()) -- a plain,
     * branch-free per-argument bit test, one shift+and+store per arg. */
    for (int i = 0; i < nargs; i++) {
        ld_argmask_cl(cb);
        if (i > 0) op_shr_cl_imm(cb, (uint8_t)i);
        op_and_cl_imm8(cb, 1);
        st_tag_cl(cb, i);
    }
}

/* ─── Call-site codegen: marshal r0..r7, call, store result into rD ──────
 * Gap Remediation SIMI Phase 12: the outgoing-arg scratch area grew from
 * 64 to 72 bytes (the 9th qword carries the argument-tag mask, see the
 * design-notes block above ld_argmask_cl()), and the result is stored via
 * a real tag propagation (st_tag_dl, fed by the callee's RET codegen)
 * instead of the old unconditional st_rax_untag. */
static void emit_call_site(struct CodeBuf* cb, uint32_t target_pc, int rd) {
    op_sub_rsp_imm8(cb, 72);
    int nargs = TX_MAX_REGS < 8 ? TX_MAX_REGS : 8;
    for (int i = 0; i < nargs; i++) {
        ld_rax(cb, i);
        st_rsp_rax(cb, (int8_t)(8*i));
    }
    for (int i = nargs; i < 8; i++) { /* TX_MAX_REGS < 8 is not expected, but stay safe */
        op_xor_eax(cb);
        st_rsp_rax(cb, (int8_t)(8*i));
    }
    /* Build the outgoing argument-tag mask AFTER all values are already
     * stored to their slots above -- clobbering eax/ecx here is then
     * safe. bit i = register i's current tag. */
    op_xor_eax(cb);
    for (int i = 0; i < nargs; i++) {
        ld_tag_cl(cb, i);
        if (i > 0) op_shl_cl_imm(cb, (uint8_t)i);
        op_or_al_cl(cb);
    }
    st_rsp_al(cb, 64);
    op_call(cb, target_pc);
    op_add_rsp_imm8(cb, 72);
    st_rax(cb, rd);
    st_tag_dl(cb, rd);   /* Gap Remediation SIMI Phase 12: propagate r0's tag */
}

/* ─── Trampoline: zero r0..r5, r6=namepool_ptr, r7=scratch_ptr, call entry,
 * ret ─────────────────────────────────────────────────────────────────
 * Always ends in a plain `ret` — this makes the translated entry point a
 * normal callable function under the SysV ABI (int64_t fn(void) returning
 * the SIMI program's r0 in RAX), which is what lets the host JIT test
 * harness call it directly via a function pointer. The kernel wraps this
 * trampoline in its own tiny stub that turns that final return into a
 * SYS_SLS_EXIT syscall instead — see kernel/simi_translate.c.
 *
 * v0.3 (Phase 6): r6 joins r7 as a second trampoline-provided constant —
 * see simi_x86.h's top comment for why RESOLVE needs a raw pointer into
 * the object's own name pool rather than a copied buffer.
 *
 * Gap Remediation SIMI Phase 12: the outgoing-arg area grew to 72 bytes
 * (see emit_call_site()) — the trampoline is itself a call site (it calls
 * "main"), so it must write a 9th mask byte too, unconditionally zero:
 * nothing before "main" starts can hold a real capability (r0-r5 are
 * zeroed, r6/r7 are raw pointers, not RESOLVE results), and main's
 * prologue now unconditionally reads that byte -- leaving it
 * uninitialized would apply garbage stack bytes as bogus tags. */
static void emit_trampoline(struct CodeBuf* cb, uint32_t target_pc,
                             uint64_t scratch_ptr, uint64_t namepool_ptr) {
    op_sub_rsp_imm8(cb, 72);
    op_xor_eax(cb);
    for (int i = 0; i < 6; i++) st_rsp_rax(cb, (int8_t)(8*i));
    ld_imm64_rax(cb, namepool_ptr);
    st_rsp_rax(cb, 48);
    ld_imm64_rax(cb, scratch_ptr);
    st_rsp_rax(cb, 56);
    op_xor_eax(cb);       /* re-zero al -- clobbered by scratch_ptr above */
    st_rsp_al(cb, 64);
    op_call(cb, target_pc);
    op_add_rsp_imm8(cb, 72);
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
    case OP_ADD: case OP_SUB: case OP_MUL: {
        /* Gap Remediation SIMI Phase 10: float dispatch. See the design-
         * notes block above op_movq_xmm0_rax() for the XMM0/XMM1-bounce
         * rationale. */
        if (type == T_F64 || type == T_F32) {
            if (flags & FLAG_IMM) return TX_ERR_FLOAT_UNSUPPORTED;
            ld_rax(cb, ra);
            ld_rcx(cb, w_rb_reg(w));
            op_movq_xmm0_rax(cb);
            op_movq_xmm1_rcx(cb);
            if (type == T_F64) {
                switch (op) {
                    case OP_ADD: op_addsd_xmm0_xmm1(cb); break;
                    case OP_SUB: op_subsd_xmm0_xmm1(cb); break;
                    case OP_MUL: op_mulsd_xmm0_xmm1(cb); break;
                }
            } else {
                switch (op) {
                    case OP_ADD: op_addss_xmm0_xmm1(cb); break;
                    case OP_SUB: op_subss_xmm0_xmm1(cb); break;
                    case OP_MUL: op_mulss_xmm0_xmm1(cb); break;
                }
            }
            op_movq_rax_xmm0(cb);
            st_rax_untag(cb, rd);
            break;
        }
        ld_rax(cb, ra);
        if (flags & FLAG_IMM) ld_imm64_rcx(cb, (uint64_t)(int64_t)w_imm28(w));
        else                  ld_rcx(cb, w_rb_reg(w));
        switch (op) {
            case OP_ADD: op_add(cb); break;
            case OP_SUB: op_sub(cb); break;
            case OP_MUL: op_imul(cb); break;
        }
        st_rax_untag(cb, rd);
        break;
    }
    case OP_AND: case OP_OR: case OP_XOR:
    case OP_SHL: case OP_SHR: case OP_SAR: {
        /* No float meaning for any of these — always the integer path,
         * regardless of the instruction's declared type. */
        ld_rax(cb, ra);
        if (flags & FLAG_IMM) ld_imm64_rcx(cb, (uint64_t)(int64_t)w_imm28(w));
        else                  ld_rcx(cb, w_rb_reg(w));
        switch (op) {
            case OP_AND: op_and(cb); break;
            case OP_OR:  op_or(cb);  break;
            case OP_XOR: op_xor(cb); break;
            case OP_SHL: op_shl_cl(cb); break;
            case OP_SHR: op_shr_cl(cb); break;
            case OP_SAR: op_sar_cl(cb); break;
        }
        st_rax_untag(cb, rd);
        break;
    }
    case OP_DIV: {
        /* Gap Remediation SIMI Phase 10: float DIV is real (IEEE div-by-
         * zero is a defined result, not a hazard) — unlike MOD, which has
         * no hardware instruction for float on any target and stays
         * rejected below. */
        if (type == T_F64 || type == T_F32) {
            if (flags & FLAG_IMM) return TX_ERR_FLOAT_UNSUPPORTED;
            ld_rax(cb, ra);
            ld_rcx(cb, w_rb_reg(w));
            op_movq_xmm0_rax(cb);
            op_movq_xmm1_rcx(cb);
            if (type == T_F64) op_divsd_xmm0_xmm1(cb); else op_divss_xmm0_xmm1(cb);
            op_movq_rax_xmm0(cb);
            st_rax_untag(cb, rd);
            break;
        }
        ld_rax(cb, ra);
        if (flags & FLAG_IMM) ld_imm64_rcx(cb, (uint64_t)(int64_t)w_imm28(w));
        else                  ld_rcx(cb, w_rb_reg(w));
        if (tx_type_signed(type)) { op_cqo(cb); op_idiv_rcx(cb); }
        else                      { op_xor_edx(cb); op_div_rcx(cb); }
        st_rax_untag(cb, rd);
        break;
    }
    case OP_MOD: {
        /* Gap Remediation SIMI Phase 10: permanently rejected for float —
         * no hardware instruction on any target; compose it from
         * DIV+MUL+SUB instead (see simi_interp.c's matching die() for the
         * same permanent-not-temporary framing). */
        if (type == T_F64 || type == T_F32) return TX_ERR_FLOAT_UNSUPPORTED;
        ld_rax(cb, ra);
        if (flags & FLAG_IMM) ld_imm64_rcx(cb, (uint64_t)(int64_t)w_imm28(w));
        else                  ld_rcx(cb, w_rb_reg(w));
        if (tx_type_signed(type)) { op_cqo(cb); op_idiv_rcx(cb); }
        else                      { op_xor_edx(cb); op_div_rcx(cb); }
        /* remainder is in rdx; move to rax's slot via a dedicated
         * mov rax,rdx first (st_rax's pattern always sources rax). */
        e8(cb,0x48); e8(cb,0x89); e8(cb,0xD0); /* mov rax,rdx */
        st_rax_untag(cb, rd);
        break;
    }
    case OP_NOT: ld_rax(cb, ra); op_not_rax(cb); st_rax_untag(cb, rd); break;
    case OP_NEG: {
        /* Gap Remediation SIMI Phase 10: float NEG is a pure sign-bit
         * flip — plain GP-register XOR against a sign-mask constant, no
         * XMM instruction needed at all (see design note above). */
        if (type == T_F64) {
            ld_rax(cb, ra);
            ld_imm64_rcx(cb, 0x8000000000000000ULL);
            op_xor(cb);                 /* rax ^= rcx */
            st_rax_untag(cb, rd);
        } else if (type == T_F32) {
            ld_rax(cb, ra);
            ld_imm32_rcx(cb, (int32_t)0x80000000);
            op_xor32_eax_ecx(cb);       /* eax ^= ecx; zero-extends rax's high 32 bits for free */
            st_rax_untag(cb, rd);
        } else {
            ld_rax(cb, ra); op_neg_rax(cb); st_rax_untag(cb, rd);
        }
        break;
    }
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
        /* Gap Remediation SIMI Phase 10: float CMP via UCOMISD/UCOMISS —
         * see the design-notes block above op_movq_xmm0_rax() for the
         * seta/setae-via-operand-swap and NE=!EQ derivations. Unsigned
         * relations (LTU/LEU/GTU/GEU) have no float meaning, same as
         * every other relation set here has no meaning for the wrong
         * category — rejected rather than silently reinterpreted. */
        if (type == T_F64 || type == T_F32) {
            if (flags >= REL_LTU) return TX_ERR_BAD_OPCODE;
            ld_rax(cb, ra);
            ld_rcx(cb, w_rb_reg(w));
            op_movq_xmm0_rax(cb);
            op_movq_xmm1_rcx(cb);
            switch (flags) {
                case REL_EQ:
                    if (type==T_F64) op_ucomisd_xmm0_xmm1(cb); else op_ucomiss_xmm0_xmm1(cb);
                    op_sete_al(cb); op_setnp_cl(cb); op_and_al_cl(cb);
                    break;
                case REL_NE:
                    if (type==T_F64) op_ucomisd_xmm0_xmm1(cb); else op_ucomiss_xmm0_xmm1(cb);
                    op_sete_al(cb); op_setnp_cl(cb); op_and_al_cl(cb);
                    op_xor_al_imm8(cb, 1);   /* NE = !EQ under IEEE-754, always (see design note) */
                    break;
                case REL_LT:
                    if (type==T_F64) op_ucomisd_xmm1_xmm0(cb); else op_ucomiss_xmm1_xmm0(cb);
                    op_seta_al(cb);
                    break;
                case REL_LE:
                    if (type==T_F64) op_ucomisd_xmm1_xmm0(cb); else op_ucomiss_xmm1_xmm0(cb);
                    op_setae_al(cb);
                    break;
                case REL_GT:
                    if (type==T_F64) op_ucomisd_xmm0_xmm1(cb); else op_ucomiss_xmm0_xmm1(cb);
                    op_seta_al(cb);
                    break;
                case REL_GE:
                    if (type==T_F64) op_ucomisd_xmm0_xmm1(cb); else op_ucomiss_xmm0_xmm1(cb);
                    op_setae_al(cb);
                    break;
                default: return TX_ERR_BAD_OPCODE;
            }
            op_movzx_rax_al(cb);
            st_rax_untag(cb, rd);
            break;
        }
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
        /* rb_raw holds the name-pool index (see FMT_RESOLVE in simi_isa.h).
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
        /* Gap Remediation SIMI Phase 12: r0's tag rides in dl -- none of
         * mov rsp,rbp / pop rbp / ret touch rdx, so it survives intact to
         * the call site's post-`call` code (see emit_call_site()). */
        ld_tag_dl(cb, 0);
        op_mov_rsp_rbp(cb);
        op_pop_rbp(cb);
        op_ret(cb);
        break;
    case OP_CALL:
        return TX_ERR_BAD_OPCODE; /* handled specially in translate() (needs pc) */
    case OP_JMPR: {
        /* Gap Remediation SIMI Phase 14: indirect jump through the runtime
         * table reserved at out_buf offset 0 (see g_has_jmpr / the
         * reservation + backfill passes in simi_x86_translate()). Unlike
         * BR/BC/CALL this needs no fixup and no pc from the caller -- the
         * target is only known at runtime, and the table's base address
         * (g_out_buf_addr) is already a translate-time constant. */
        ld_rcx(cb, ra);                                    /* rcx = target abstract pc */
        op_cmp_rcx_imm32(cb, (int32_t)g_num_instr);         /* cmp rcx, num_instr (unsigned compare) */
        uint32_t jae_pos = emit_jae_placeholder(cb);        /* jae .oob (rcx >= num_instr) */
        ld_imm64_rax(cb, g_out_buf_addr);                   /* rax = &table[0] */
        op_lea_rax_rax_rcx8(cb);                            /* rax = &table[rcx] */
        op_mov_rax_mem_rax(cb);                             /* rax = table[rcx] (real machine addr) */
        op_jmp_rax(cb);                                     /* jmp rax -- never falls through */
        patch_local_rel32(cb, jae_pos);                     /* .oob: */
        op_ud2(cb);                                         /* real #UD trap -- non-negotiable CFI per ISA §16 */
        break;
    }
    default:
        return TX_ERR_BAD_OPCODE;
    }
    return TX_OK;
}

/* ─── Gap Remediation SIMI Phase 11: per-procedure live-range allocator ───
 * Runs once per procedure (every OP_ENTER), over that procedure's own
 * [start_pc, end_pc) instruction range only — register slot numbering and
 * physical binding are both per-procedure, exactly like the rbp-relative
 * frame itself already is. See the design-notes block above g_pool[] for
 * the eligibility rule (straight-line, no loop, no call of any kind). */
struct TxLive { int32_t first_pc, last_pc; uint8_t state; }; /* state: 0=untouched,1=eligible,2=excluded */
static struct TxLive g_live[TX_MAX_REGS];

struct TxSpan { int32_t lo, hi; };
#define TX_MAX_BOUNDARY_LOCAL 4096   /* matches TX_MAX_INSTR_LOCAL-scale caps elsewhere in this file */
static struct TxSpan g_boundary[TX_MAX_BOUNDARY_LOCAL];
static uint32_t g_nboundary;

static void tx_touch(int idx, uint32_t pc) {
    if (idx < 0 || idx >= TX_MAX_REGS) return;
    if (g_live[idx].state == 0) { g_live[idx].first_pc = (int32_t)pc; g_live[idx].state = 1; }
    g_live[idx].last_pc = (int32_t)pc;
}
static void tx_boundary(int32_t lo, int32_t hi) {
    if (g_nboundary < TX_MAX_BOUNDARY_LOCAL) { g_boundary[g_nboundary].lo = lo; g_boundary[g_nboundary].hi = hi; g_nboundary++; }
    /* else: pool of boundary slots exhausted -- degrade safely by simply
     * not recording it, which can only make allocation MORE permissive,
     * never wrong in the sense of "silently corrupting a value"... except
     * that's not actually safe. TX_MAX_BOUNDARY_LOCAL (4096) already
     * matches this translator's own TX_MAX_INSTR ceiling (simi_x86.h),
     * i.e. one boundary slot per possible instruction, so this can never
     * actually trigger -- kept as a defensive bound, not a real limit. */
}

static void compute_alloc_for_proc(const uint64_t* instrs, uint32_t start_pc, uint32_t end_pc) {
    int nargs = TX_MAX_REGS < 8 ? TX_MAX_REGS : 8;
    for (int i = 0; i < TX_MAX_REGS; i++) { g_live[i].state = 0; g_physreg_of[i] = -1; }
    g_nboundary = 0;

    /* Pass 1: record every symbolic register's [first_pc,last_pc] touch
     * range, and every "boundary" pc a register's range must not span:
     * a backward branch's [target_pc, source_pc] loop body, or a single-
     * point boundary at any CALL/RESOLVE/OBJSIZE/OBJTYPE. Register
     * direction (def vs. use) doesn't matter here -- only the pc range
     * during which a register's value must stay valid in whatever
     * location it's bound to, which [min touch, max touch] already gives
     * exactly, given every symbolic register is written before it's ever
     * read (true for any well-formed SIMI program; r0-r7 are the one
     * exception, seeded by ENTER's implicit touch below). */
    for (uint32_t pc = start_pc; pc < end_pc; pc++) {
        uint64_t w = instrs[pc];
        uint8_t op = w_op(w), flags = w_flags(w);
        uint16_t rd = w_rd(w), ra = w_ra(w);
        switch (op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
        case OP_SHL: case OP_SHR: case OP_SAR: case OP_DIV: case OP_MOD: case OP_PTRADD:
            tx_touch(rd, pc); tx_touch(ra, pc);
            if (!(flags & FLAG_IMM)) tx_touch(w_rb_reg(w), pc);
            break;
        case OP_NOT: case OP_NEG: case OP_LEA: case OP_LOAD: case OP_MOV:
            tx_touch(rd, pc); tx_touch(ra, pc);
            break;
        case OP_OBJSIZE: case OP_OBJTYPE:
            /* Real C runtime call (emit_runtime_call1 -> simi_rt_objsize/
             * simi_rt_objtype) -- same external-call hazard as RESOLVE
             * below: an arbitrary compiled C function is free to clobber
             * every caller-saved register (the entire pool) internally,
             * not just the ones this file's own pre-call codegen happens
             * to touch explicitly. Missing this boundary was a real bug
             * caught by the regression suite, not anticipated up front. */
            tx_touch(rd, pc); tx_touch(ra, pc);
            tx_boundary((int32_t)pc, (int32_t)pc);
            break;
        case OP_LOADI: case OP_LOADI64: case OP_RESOLVE:
            tx_touch(rd, pc);
            if (op == OP_RESOLVE) tx_boundary((int32_t)pc, (int32_t)pc);
            break;
        case OP_CMP:
            tx_touch(rd, pc); tx_touch(ra, pc); tx_touch(w_rb_reg(w), pc);
            break;
        case OP_STORE:
            /* rd holds the *source* value register for STORE, not a
             * destination -- see emit_instr's own comment at its case.
             * Direction doesn't matter for touch-tracking either way. */
            tx_touch(ra, pc); tx_touch(rd, pc);
            break;
        case OP_BC: {
            tx_touch(ra, pc);
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            if (target <= pc) tx_boundary((int32_t)target, (int32_t)pc);
            break;
        }
        case OP_BR: {
            uint32_t target = (uint32_t)((int64_t)pc + 1 + w_imm28(w));
            if (target <= pc) tx_boundary((int32_t)target, (int32_t)pc);
            break;
        }
        case OP_CALL:
            for (int i = 0; i < nargs; i++) tx_touch(i, pc);
            tx_boundary((int32_t)pc, (int32_t)pc);
            break;
        case OP_RET:
            tx_touch(0, pc);
            break;
        case OP_ENTER:
            /* incoming r0-r7 argument copy in emit_prologue() -- this IS
             * the start_pc instruction itself for this procedure. */
            for (int i = 0; i < nargs; i++) tx_touch(i, pc);
            break;
        case OP_JMPR:
            /* Gap Remediation SIMI Phase 14: rA feeds an indirect jump
             * through the runtime table (see emit_instr's OP_JMPR case).
             * A single-point boundary here is conservative but correct --
             * JMPR's own codegen is straight-line (no real branch INTO
             * the allocator's own view of the world), but treating it
             * like CALL/RESOLVE keeps this switch's safety story uniform
             * and costs nothing (JMPR is rare and terminal in a block). */
            tx_touch(ra, pc);
            tx_boundary((int32_t)pc, (int32_t)pc);
            break;
        case OP_LEAVE: default:
            break;
        }
    }

    /* Pass 2: exclude any register whose range overlaps any boundary. */
    for (int i = 0; i < TX_MAX_REGS; i++) {
        if (g_live[i].state != 1) continue;
        for (uint32_t b = 0; b < g_nboundary; b++) {
            if (g_live[i].first_pc <= g_boundary[b].hi && g_live[i].last_pc >= g_boundary[b].lo) {
                g_live[i].state = 2;
                break;
            }
        }
    }

    /* Pass 3: linear-scan assignment over the remaining eligible
     * registers, sorted by first_pc (insertion sort -- TX_MAX_REGS is 64,
     * this is never worth anything fancier). Pool exhaustion at a given
     * point just leaves that register unallocated (spilled to its
     * existing stack slot) -- no eviction/reassignment logic, matching
     * the design's own "spilled and never-eligible are the same code
     * path" framing. */
    int order[TX_MAX_REGS], norder = 0;
    for (int i = 0; i < TX_MAX_REGS; i++) if (g_live[i].state == 1) order[norder++] = i;
    for (int a = 1; a < norder; a++) {
        int key = order[a]; int32_t kfp = g_live[key].first_pc; int b = a - 1;
        while (b >= 0 && g_live[order[b]].first_pc > kfp) { order[b+1] = order[b]; b--; }
        order[b+1] = key;
    }
    int32_t pool_holds_until[TX_POOL_SIZE];
    for (int p = 0; p < TX_POOL_SIZE; p++) pool_holds_until[p] = -1;
    for (int oi = 0; oi < norder; oi++) {
        int idx = order[oi];
        int32_t fp = g_live[idx].first_pc, lp = g_live[idx].last_pc;
        for (int p = 0; p < TX_POOL_SIZE; p++) if (pool_holds_until[p] >= 0 && pool_holds_until[p] < fp) pool_holds_until[p] = -1;
        for (int p = 0; p < TX_POOL_SIZE; p++) {
            if (pool_holds_until[p] < 0) { pool_holds_until[p] = lp; g_physreg_of[idx] = g_pool[p]; break; }
        }
    }
}

/* ─── Top-level translate: two passes (emit + patch fixups) ─────────────*/
int simi_x86_translate(const uint8_t* obj_data, uint32_t obj_size,
                        uint8_t* out_buf, uint32_t out_cap,
                        const char* entry_name, uint64_t scratch_ptr,
                        uint64_t rt_resolve_fn, uint64_t rt_objsize_fn,
                        uint64_t rt_objtype_fn,
                        uint32_t* out_len, uint32_t* entry_off) {
    if (!obj_data || obj_size < sizeof(struct SimiObjHdr)) return TX_ERR_BAD_HEADER;
    struct SimiObjHdr hdr;
    tx_memcpy(&hdr, obj_data, sizeof(hdr));
    if (hdr.magic != SIMI_X_MAGIC) return TX_ERR_BAD_HEADER;

    uint64_t expect = sizeof(struct SimiObjHdr);
    expect += (uint64_t)hdr.num_instr * 8;
    expect += (uint64_t)hdr.num_literals * 8;
    expect += (uint64_t)hdr.num_entries * sizeof(struct TxEntryRec);
    expect += (uint64_t)hdr.num_names * sizeof(struct TxNameRec);
    if (expect != (uint64_t)obj_size) return TX_ERR_BAD_HEADER;
    if (hdr.num_instr == 0 || hdr.num_instr > 4096) return TX_ERR_TOO_MANY_INSTR;

    const uint64_t* instrs = (const uint64_t*)(obj_data + sizeof(struct SimiObjHdr));
    const uint64_t* literals = (const uint64_t*)((const uint8_t*)instrs + hdr.num_instr * 8);
    const struct TxEntryRec* entries = (const struct TxEntryRec*)
        ((const uint8_t*)literals + hdr.num_literals * 8);
    const struct TxNameRec* names = (const struct TxNameRec*)
        ((const uint8_t*)entries + hdr.num_entries * sizeof(struct TxEntryRec));
    /* v0.3: namepool_ptr is baked as a raw pointer straight into obj_data's
     * own name-pool bytes — see simi_x86.h's top comment. */
    uint64_t namepool_ptr = (uint64_t)(uintptr_t)names;

    g_rt_resolve_fn = rt_resolve_fn;
    g_rt_objsize_fn = rt_objsize_fn;
    g_rt_objtype_fn = rt_objtype_fn;
    g_num_names = hdr.num_names;

    /* Gap Remediation SIMI Phase 14: cheap pre-scan so an object with no
     * JMPR at all pays zero extra cost (no table, no reservation, no
     * backfill pass). g_out_buf_addr/g_num_instr are set unconditionally
     * since they're cheap and OP_JMPR's own codegen reads them regardless
     * of this scan (a well-formed object either has JMPR or doesn't; if
     * it does, g_has_jmpr must already be true by the time emit_instr()
     * reaches it, which this upfront scan guarantees). */
    g_out_buf_addr = (uint64_t)(uintptr_t)out_buf;
    g_num_instr = hdr.num_instr;
    g_has_jmpr = 0;
    for (uint32_t q = 0; q < hdr.num_instr; q++) {
        if (w_op(instrs[q]) == OP_JMPR) { g_has_jmpr = 1; break; }
    }

    struct CodeBuf cb; cb.buf = out_buf; cb.cap = out_cap; cb.len = 0; cb.overflow = 0;
    g_nfixups = 0;

    /* Reserve the jump table as raw data at offset 0 -- num_instr*8 bytes,
     * one 8-byte slot per abstract pc, backfilled with real absolute
     * machine addresses once g_instr_off[] is complete (see the backfill
     * pass after the trampoline loop below). Zeroed here so an untouched
     * slot (a pc no JMPR ever targets) is at least deterministic, though
     * OP_JMPR's own runtime bounds check against g_num_instr means a
     * *valid* target pc is always backfilled before anything can read it. */
    if (g_has_jmpr) {
        for (uint32_t q = 0; q < hdr.num_instr; q++) e64(&cb, 0);
        if (cb.overflow) return TX_ERR_BUF_FULL;
    }
    /* Gap Remediation SIMI Phase 11: safe default for any code preceding
     * the first OP_ENTER (not expected in a well-formed object, but -1
     * means "always use the stack slot," the same behavior this file had
     * before this phase existed, so there's no eligible-but-wrong state
     * to worry about here). */
    for (int i = 0; i < TX_MAX_REGS; i++) g_physreg_of[i] = -1;

    for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
        g_instr_off[pc] = cb.len;
        uint64_t w = instrs[pc];
        uint8_t op = w_op(w);

        if (op == OP_ENTER) {
            /* Gap Remediation SIMI Phase 11: recompute allocation fresh
             * for this procedure's own [pc, end_pc) range -- register
             * numbering and physical binding are both per-procedure,
             * exactly like the rbp-relative frame itself already is. */
            uint32_t end_pc = hdr.num_instr;
            for (uint32_t q = pc + 1; q < hdr.num_instr; q++) {
                if (w_op(instrs[q]) == OP_ENTER) { end_pc = q; break; }
            }
            compute_alloc_for_proc(instrs, pc, end_pc);
        }

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

    /* Gap Remediation SIMI Phase 14: backfill the jump table now that
     * g_instr_off[] is complete for every real pc (main loop finished,
     * trampolines appended, fixups patched -- table slots need only the
     * former, but doing this last keeps every out_buf-mutating pass in
     * one fixed order for the whole function). */
    if (g_has_jmpr) {
        for (uint32_t pc = 0; pc < hdr.num_instr; pc++) {
            poke64(&cb, pc * 8, g_out_buf_addr + g_instr_off[pc]);
        }
    }

    *out_len = cb.len;
    *entry_off = found_off;
    return TX_OK;
}

const char* simi_x86_strerror(int code) {
    switch (code) {
        case TX_OK: return "ok";
        case TX_ERR_BAD_HEADER: return "bad or corrupt SIMI header";
        case TX_ERR_TOO_MANY_INSTR: return "instruction count out of range";
        case TX_ERR_REG_OUT_OF_RANGE: return "register index >= TX_MAX_REGS (64)";
        case TX_ERR_BUF_FULL: return "output buffer too small";
        case TX_ERR_BAD_OPCODE: return "unsupported or malformed opcode";
        case TX_ERR_TOO_MANY_FIXUPS: return "too many branch/call fixups";
        case TX_ERR_ENTRY_NOT_FOUND: return "requested entry name not in object";
        case TX_ERR_LITERAL_OUT_OF_RANGE: return "LOADI64 literal pool index out of range";
        case TX_ERR_NAME_OUT_OF_RANGE: return "RESOLVE name-pool index out of range";
        case TX_ERR_FLOAT_UNSUPPORTED: return "operand combination has no float meaning (immediate operand, or MOD)";
        default: return "unknown error";
    }
}
