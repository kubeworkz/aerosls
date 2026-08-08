#!/usr/bin/env python3
"""
a64_enc_check.py — M0/M1 transcription-error net.

Independently re-encodes a set of A64 instructions using the ARM bit
layout written from the architecture reference (field positions: Rd@0,
Rn@5, Rm@16, imm12@21:10, shift@23:22, size@31:30, cond@15:12 — the
same layout confirmed independently in the Linux kernel's
arch/arm64/lib/insn.c), then checks the words simi_arm.c actually
emitted (dumped by a64_dump.c).

M1 changed what the check can assert. M0's codegen was deterministic
load-operate-store, so the checker could demand exact word sequences.
M1's x9/x10/x11 register cache makes the BODY allocation-dependent: the
same SIMI program can place a value in x9 or x10 depending on eviction
order, so exact body words would just restate the allocator, not verify
it. The prologue and trampoline stay deterministic (no cache state at
their boundaries) and keep exact-word checks. The body is verified two
allocation-agnostic ways:

  1. Every emitted word must decode — via this independent bit layout —
     to a legal A64 instruction of a class the translator emits. This
     catches the transcription-error class this net exists for: a wrong
     opcode/constant in simi_arm.c shows up as a word that fails to
     decode, even when the C decoder (same author) "agrees" on it.
  2. The movz/movk chains must encode the exact LOADI/literal values the
     program demands, in whatever register the allocator chose — decoded
     and summed here from the raw bits, never taken from simi_arm.c.

What this net CANNOT catch: register selection. A wrong operand register
in an ALU word (add x9,x9,x10 vs add x9,x9,x11) still decodes to a legal
class, so allocation mistakes are invisible here — they are caught by the
four-way execution parity (interp/x86/RV64/ARM run the same .tmo to the
same expected value). This net is a structural complement to that, not a
replacement; a word must fail to decode here OR produce a wrong result
there to be found.

Usage: a64_enc_check.py <dump file> <simi program name>
The dump file is the output of a64_dump.c; the program name selects the
expected value set (straight_line_bench | loop_sum | extra_ops).
"""

import sys
from collections import Counter

# ── Independent encoders/decoders, from the ARM bit layout ───────────────
def movz(rd, imm16, hw=0):  return 0xD2800000 | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | rd
def movk(rd, imm16, hw):    return 0xF2800000 | ((hw & 3) << 21) | ((imm16 & 0xFFFF) << 5) | rd
def add_imm(rd, rn, imm12): return 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd
def sub_imm(rd, rn, imm12): return 0xD1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd
def subs_imm(rd, rn, imm12):return 0xF1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd
def add_sh(rd, rn, rm, imm6=0): return 0x8B000000 | (rm << 16) | ((imm6 & 0x3F) << 10) | (rn << 5) | rd
def sub_sh(rd, rn, rm):     return 0xCB000000 | (rm << 16) | (rn << 5) | rd
def subs_sh(rd, rn, rm):    return 0xEB000000 | (rm << 16) | (rn << 5) | rd
def and_sh(rd, rn, rm):     return 0x8A000000 | (rm << 16) | (rn << 5) | rd
def orr_sh(rd, rn, rm, imm6=0): return 0xAA000000 | (rm << 16) | ((imm6 & 0x3F) << 10) | (rn << 5) | rd
def eor_sh(rd, rn, rm):     return 0xCA000000 | (rm << 16) | (rn << 5) | rd
def orn(rd, rm):            return 0xAA200000 | (rm << 16) | (31 << 5) | rd   # ORN Xd, XZR, Xm (MVN)
def lslv(rd, rn, rm):       return 0x9AC02000 | (rm << 16) | (rn << 5) | rd
def lsrv(rd, rn, rm):       return 0x9AC02400 | (rm << 16) | (rn << 5) | rd
def asrv(rd, rn, rm):       return 0x9AC02800 | (rm << 16) | (rn << 5) | rd
def madd(rd, rn, rm, ra):   return 0x9B000000 | (rm << 16) | (ra << 10) | (rn << 5) | rd
def msub(rd, rn, rm, ra):   return 0x9B008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd
def sdiv(rd, rn, rm):       return 0x9AC00C00 | (rm << 16) | (rn << 5) | rd
def udiv(rd, rn, rm):       return 0x9AC00800 | (rm << 16) | (rn << 5) | rd
def cset(rd, cond):         return 0x9A9F07E0 | ((cond ^ 1) << 12) | rd   # CSINC Xd, XZR, XZR, !cond
def ldr(rt, rn, imm12):     return 0xF9400000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rt
def stp_(rt, rn, imm12):    return 0xF9000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rt   # STR X
def ldrb(rt, rn, imm12):    return 0x39400000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rt
def strb(rt, rn, imm12):    return 0x39000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rt
def ldrsw(rt, rn):          return 0xB9800000 | (rn << 5) | rt
def ldr_w(rt, rn):          return 0xB9400000 | (rn << 5) | rt
def str_w(rt, rn):          return 0xB9000000 | (rn << 5) | rt
def ldrsh(rt, rn):          return 0x79800000 | (rn << 5) | rt
def ldrh(rt, rn):           return 0x79400000 | (rn << 5) | rt
def strh(rt, rn):           return 0x79000000 | (rn << 5) | rt
def ldrsb(rt, rn):          return 0x39800000 | (rn << 5) | rt
# M2.10: unscaled load/store register (ldur/stur) — size:2 111 0 00
# opc 0 imm9:9 00 Rn Rt. imm9 is a signed UNSCALED 9-bit displacement
# (-256..255) at bits 20:12, bits 11:10 = 00 select unscaled (01 =
# post-indexed, 11 = pre-indexed — the M2.9 pre-indexed form was
# superseded by this one in M2.10), bit 21 = 0. stur x29, [sp, #-16]
# == 0xF81F03FD (base 0xF8000000 | (0x1F0 << 12) | (31 << 5) | 29).
def ldur(rt, rn, imm9):  return 0xF8400000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def stur(rt, rn, imm9):  return 0xF8000000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def ldurb(rt, rn, imm9): return 0x38400000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def sturb(rt, rn, imm9): return 0x38000000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def ldurh(rt, rn, imm9): return 0x78400000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def sturh(rt, rn, imm9): return 0x78000000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def ldur_w(rt, rn, imm9):return 0xB8400000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def stur_w(rt, rn, imm9):return 0xB8000000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def ldursb(rt, rn, imm9):return 0x38800000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def ldursh(rt, rn, imm9):return 0x78800000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
def ldursw(rt, rn, imm9):return 0xB8800000 | ((imm9 & 0x1FF) << 12) | (rn << 5) | rt
# M2.11: load/store register, REGISTER offset — size:2 111 0 00 opc 1
# Rm opt:3 s:1 10 Rn Rt (bit 21 = 1, bits 11:10 = 10). Two options, as
# QEMU's a64.decode @ldst specifies: opt 011 (LSL #0) for the 64-bit
# forms (Xm used in full), opt 110 (SXTW) for the 8/16/32-bit forms
# (low 32 bits sign-extended — how a negative two's-complement
# displacement stays negative; LSL #0 on a Wm would zero-extend it).
# ldr x0, [x1, x2] == 0xF8626820 (base 0xF8606800 | (2 << 16) | (1 << 5)).
def ldr_reg(rt, rn, rm):  return 0xF8606800 | (rm << 16) | (rn << 5) | rt
def str_reg(rt, rn, rm):  return 0xF8206800 | (rm << 16) | (rn << 5) | rt
def ldrb_reg(rt, rn, rm): return 0x3860C800 | (rm << 16) | (rn << 5) | rt
def strb_reg(rt, rn, rm): return 0x3820C800 | (rm << 16) | (rn << 5) | rt
def ldrh_reg(rt, rn, rm): return 0x7860C800 | (rm << 16) | (rn << 5) | rt
def strh_reg(rt, rn, rm): return 0x7820C800 | (rm << 16) | (rn << 5) | rt
def ldr_w_reg(rt, rn, rm):return 0xB860C800 | (rm << 16) | (rn << 5) | rt
def str_w_reg(rt, rn, rm):return 0xB820C800 | (rm << 16) | (rn << 5) | rt
def ldrsb_reg(rt, rn, rm):return 0x38A0C800 | (rm << 16) | (rn << 5) | rt
def ldrsh_reg(rt, rn, rm):return 0x78A0C800 | (rm << 16) | (rn << 5) | rt
def ldrsw_reg(rt, rn, rm):return 0xB8A0C800 | (rm << 16) | (rn << 5) | rt
def br(rn):                 return 0xD61F0000 | (rn << 5)
def blr(rn):                return 0xD63F0000 | (rn << 5)
def bcond(cond, imm19):     return 0x54000000 | (cond << 12) | ((imm19 & 0x7FFFF) << 5)

def li64(rd, imm):
    words = [movz(rd, imm & 0xFFFF)]
    for hw in (1, 2, 3):
        half = (imm >> (16 * hw)) & 0xFFFF
        if half:
            words.append(movk(rd, half, hw))
    return words

# ── Decode a single emitted word back to a class + payload, INDEPENDENTLY ─
# Every mask/constant below is re-derived from the bit layout above (which
# itself came from the ARM reference), never read from simi_arm.c. If
# simi_arm.c and this file disagree on a constant, the word fails to
# decode or decodes to the wrong class — exactly the net's purpose.
def decode(w):
    def rd(w):  return w & 0x1F
    def rn(w):  return (w >> 5) & 0x1F
    def rm(w):  return (w >> 16) & 0x1F
    def imm12(w): return (w >> 10) & 0xFFF
    def hw(w):  return (w >> 21) & 3
    def imm16(w): return (w >> 5) & 0xFFFF
    def is_masked(w, mask, k):  return (w & mask) == (k & mask)
    top = w & 0xFF000000
    if top in (0xD2000000, 0xF2000000):                      # move wide
        # 64-bit: MOVZ = 110100101 (0x1A5), MOVK = 111100101 (0x1E5) in
        # bits 31:23 — they differ at bit 28.
        return ("movz" if (w >> 23) == 0x1A5 else "movk", hw(w), imm16(w), rd(w))
    # ADD/SUB immediate: sh (bit 22, imm12 << 12) is masked away by
    # 0xFF000000, so a sh=1 word (M2.7's shifted-immediate fold) still
    # classifies as add_imm/sub_imm; imm12() extracts the raw field. The
    # body check is class-based only, so the unshifted payload is fine
    # (a64_exec.c carries the same note).
    if is_masked(w, 0xFF000000, 0x91000000): return ("add_imm", imm12(w), rn(w), rd(w))
    if is_masked(w, 0xFF000000, 0xD1000000): return ("sub_imm", imm12(w), rn(w), rd(w))
    if is_masked(w, 0xFF000000, 0xF1000000): return ("subs_imm", imm12(w), rn(w), rd(w))
    if is_masked(w, 0xFFE00000, 0x8B000000): return ("add_shift", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE00000, 0xCB000000): return ("sub_shift", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE00000, 0xEB000000): return ("subs_shift", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE00000, 0x8A000000): return ("and_shift", rm(w), rn(w), rd(w))
    # Logical shifted: sf 001010 0 N Rm imm6 Rn Rd — N@21 distinguishes
    # ORR (0xAA000000) from ORN (0xAA200000); MVN = ORN Xd, XZR, Xm.
    if is_masked(w, 0xFFE00000, 0xAA200000): return ("orn", rn(w), rd(w))
    if is_masked(w, 0xFFE00000, 0xAA000000): return ("orr_shift", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE00000, 0xCA000000): return ("eor_shift", rm(w), rn(w), rd(w))
    # Variable shifts: free Rm@20:16, Rn@9:5, Rd@4:0.
    if is_masked(w, 0xFFE0FC00, 0x9AC02000): return ("lslv", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE0FC00, 0x9AC02400): return ("lsrv", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE0FC00, 0x9AC02800): return ("asrv", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE0FC00, 0x9AC00C00): return ("sdiv", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE0FC00, 0x9AC00800): return ("udiv", rm(w), rn(w), rd(w))
    # MADD/MSUB: free Rm@20:16, Ra@14:10, Rn@9:5, Rd@4:0; bit 15 is the
    # multiply-add/subtract selector.
    if is_masked(w, 0xFFE08000, 0x9B000000): return ("madd", rm(w), rn(w), rd(w))
    if is_masked(w, 0xFFE08000, 0x9B008000): return ("msub", rm(w), rn(w), rd(w))
    # CSINC Xd, XZR, XZR, !cond: bits 31:16 = 0x9A9F, bits 11:5 =
    # 0x7E0 (op@11:10=01, Rn@9:5=31) — cond@15:12 and Rd@4:0 are free.
    if is_masked(w, 0xFFFF07E0, 0x9A9F07E0): return ("cset", (w >> 12) & 0xF, rd(w))
    if (w & 0xFFC00000) == 0xF9400000: return ("ldr", imm12(w), rn(w), rd(w))
    if (w & 0xFFC00000) == 0xF9000000: return ("str", imm12(w), rn(w), rd(w))
    if (w & 0xFFC00000) == 0x39400000: return ("ldrb", imm12(w), rn(w), rd(w))
    if (w & 0xFFC00000) == 0x39000000: return ("strb", imm12(w), rn(w), rd(w))
    if (w & 0xFFC00000) == 0xB9800000: return ("ldrsw", rn(w), rd(w))
    if (w & 0xFFC00000) == 0xB9400000: return ("ldr_w", rn(w), rd(w))
    if (w & 0xFFC00000) == 0xB9000000: return ("str_w", rn(w), rd(w))
    if (w & 0xFFC00000) == 0x79800000: return ("ldrsh", rn(w), rd(w))
    if (w & 0xFFC00000) == 0x79400000: return ("ldrh", rn(w), rd(w))
    if (w & 0xFFC00000) == 0x79000000: return ("strh", rn(w), rd(w))
    if (w & 0xFFC00000) == 0x39800000: return ("ldrsb", rn(w), rd(w))
    # Unscaled ldur/stur: the 0xFFC00C00 top mask with bits 11:10 = 00
    # (both mask and target) pin the family exactly — 01 = post-indexed
    # and 11 = pre-indexed are never emitted and decode as None; imm9
    # at 20:12 is signed.
    if (w & 0xFFC00C00) == 0xF8400000: return ("ldur", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0xF8000000: return ("stur", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0x38400000: return ("ldurb", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0x38000000: return ("sturb", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0x78400000: return ("ldurh", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0x78000000: return ("sturh", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0xB8400000: return ("ldur_w", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0xB8000000: return ("stur_w", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0x38800000: return ("ldursb", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0x78800000: return ("ldursh", (w >> 12) & 0x1FF, rn(w), rd(w))
    if (w & 0xFFC00C00) == 0xB8800000: return ("ldursw", (w >> 12) & 0x1FF, rn(w), rd(w))
    # Register offset: the 0x3F20FC00 mask pins bits 29:24, bit 21 = 1 and
    # bits 15:8 (opt, s, and the 10 marker); the two emitted options are
    # opt 011 (target 0x68) and opt 110 (target 0xC8) — any other option
    # decodes as None. sz@31:30 and opc@23:22 map to the width classes;
    # rm@20:16, rn@9:5, rt@4:0 are free.
    reg_base = w & 0x3F20FC00
    if reg_base in (0x38206800, 0x3820C800):
        sz = (w >> 30) & 3
        opc = (w >> 22) & 3
        name = {0: {0: "strb_reg", 1: "ldrb_reg", 2: "ldrsb_reg"},
                1: {0: "strh_reg", 1: "ldrh_reg", 2: "ldrsh_reg"},
                2: {0: "str_w_reg", 1: "ldr_w_reg", 2: "ldrsw_reg"},
                3: {0: "str_reg", 1: "ldr_reg"}}.get(sz, {}).get(opc)
        if name: return (name, rm(w), rn(w), rd(w))
    if (w & 0xFFFFFC1F) == 0xD61F0000: return ("br", rn(w))
    if (w & 0xFFFFFC1F) == 0xD63F0000: return ("blr", rn(w))
    if (w & 0xFC000000) == 0x14000000: return ("b", (w & 0x3FFFFFF) | (-(1 << 26) if w & 0x2000000 else 0))
    if (w & 0xFC000000) == 0x94000000: return ("bl", (w & 0x3FFFFFF) | (-(1 << 26) if w & 0x2000000 else 0))
    # CBZ/CBNZ (64-bit): 0xB4/0xB5 in bits 31:24 — bit 24 selects
    # zero/nonzero, imm19@23:5 and Rt@4:0 are free.
    if (w & 0xFE000000) == 0xB4000000: return ("cbz", (w >> 5) & 0x7FFFF, rd(w))
    if (w & 0xFE000000) == 0xB5000000: return ("cbnz", (w >> 5) & 0x7FFFF, rd(w))
    # B.cond: 0101 0100 0 imm19 0 cond 00000 — bits 31:24 and bit 4 are
    # fixed; cond@15:12 and imm19@23:5 are free. M2.25's inline JMPR
    # chain emits cmp (subs_imm XZR) + b.eq pairs.
    if (w & 0xFF000010) == 0x54000000: return ("bcond", (w >> 12) & 0xF, (w >> 5) & 0x7FFFF)
    return None

# Classes simi_arm.c may legally emit (M0/M1). Anything else in the dump
# is a wrong constant.
ALLOWED = {"movz", "movk", "add_imm", "sub_imm", "subs_imm", "add_shift",
           "sub_shift", "subs_shift", "and_shift", "orr_shift", "eor_shift",
           "orn", "lslv", "lsrv", "asrv", "sdiv", "udiv", "madd", "msub",
           "cset", "ldr", "str", "ldrb", "strb", "ldrsw", "ldr_w", "str_w",
           "ldrsh", "ldrh", "strh", "ldrsb", "ldur", "stur",
           "ldurb", "sturb", "ldurh", "sturh", "ldur_w", "stur_w",
           "ldursb", "ldursh", "ldursw",
           "ldr_reg", "str_reg", "ldrb_reg", "strb_reg", "ldrh_reg",
           "strh_reg", "ldr_w_reg", "str_w_reg", "ldrsb_reg",
           "ldrsh_reg", "ldrsw_reg",
           "br", "blr", "b", "bl", "cbz", "cbnz", "bcond"}

# Register numbers used by simi_arm.c
T0, T1, T2, SP, FP, LR, XZR = 9, 10, 11, 31, 29, 30, 31

# ── Deterministic prologue/trampoline checks (exact words) ───────────────
def check_trampoline(dump, label):
    """The trampoline is emitted AFTER the procedure body (like RV64), and
    its only non-static words are the two li64 constants (namepool_ptr and
    scratch_ptr — host addresses baked in by translate(), not predictable
    here). So: locate the trampoline by its distinctive mask-zeroing word
    (strb wzr,[sp,#64] — rt==31 never appears anywhere else), then walk
    backward through the fixed skeleton, skipping movz/movk-x9 words."""
    def is_li9(w):
        # movz/movk x9: fixed prefix is bits 31:23 (0xD2/0xF2) + rd 4:0;
        # imm16 (20:5) and hw (22:21) vary, so mask to 0xFF80001F.
        return (w & 0xFF80001F) == 0xD2800009 or (w & 0xFF80001F) == 0xF2800009
    # last occurrence of strb wzr, [sp, #64]
    i = len(dump) - 1 - dump[::-1].index(strb(XZR, SP, 64)) if strb(XZR, SP, 64) in dump else -1
    if i < 0:
        print("MISMATCH %-22s: no strb wzr,[sp,#64] (trampoline mask zeroing)" % label)
        return False
    # walk backward: str x9,[sp,#56]; li9*; str x9,[sp,#48]; li9*;
    # 6 zero-stores (imm12 5..0); sub sp,sp,#80; add x29,sp,#16;
    # str x29,[sp,#0]; str x30,[sp,#8]; sub sp,sp,#16
    def back_expect(want):
        nonlocal i
        i -= 1
        if i < 0 or dump[i] != want:
            print("MISMATCH %-22s: walking back from mask zeroing, expected %08x at pos %d, found %s"
                  % (label, want, i, "(start of dump)" if i < 0 else "%08x" % dump[i]))
            return False
        return True
    def back_skip_li9():
        nonlocal i
        while i > 0 and is_li9(dump[i - 1]):
            i -= 1
        return True
    if not back_expect(0xF9001FE9): return False      # str x9,[sp,#56] (scratch)
    back_skip_li9()
    if not back_expect(0xF9001BE9): return False      # str x9,[sp,#48] (namepool)
    back_skip_li9()
    for imm in (5, 4, 3, 2, 1, 0):                    # 6 zero arg stores, reversed
        if not back_expect(stp_(XZR, SP, imm)): return False
    if not back_expect(sub_imm(SP, SP, 80)): return False
    if not back_expect(0x910043FD): return False      # add x29, sp, #16
    if not back_expect(0xF90003FD): return False      # str x29, [sp, #0]
    if not back_expect(0xF90007FE): return False      # str x30, [sp, #8]
    if not back_expect(sub_imm(SP, SP, 16)): return False
    print("OK       %-22s: fixed skeleton + 2 li64(x9) sequences present" % label)
    return True

def prologue():
    # sub sp,sp,#16; str x30,[sp,#8]; str x29,[sp,#0]; add x29,sp,#16; sub sp,sp,#576
    w = [sub_imm(SP, SP, 16), 0xF90007FE, 0xF90003FD, 0x910043FD, sub_imm(SP, SP, 576)]
    # zero 64 slots: str xzr,[sp,#(71-i)]; zero 64 tags: strb wzr,[sp,#i]
    w += [stp_(XZR, SP, 71 - i) for i in range(64)]
    w += [strb(XZR, SP, i) for i in range(64)]
    # copy 8 args from [sp + 592 + 8i] (imm12 74+i) into slots
    for i in range(8):
        w += [ldr(T0, SP, 74 + i), stp_(T0, SP, 71 - i)]
    # argmask loop: ldrb t1,[sp,#656]; movz t2,#(1<<i); and t2,t1,t2;
    #               cmp t2,#0; cset t2,ne; strb w2,[sp,#i]
    for i in range(8):
        w += [ldrb(T1, SP, 656), movz(T2, 1 << i), and_sh(T2, T1, T2),
              subs_imm(XZR, T2, 0), cset(T2, 1), strb(T2, SP, i)]
    return w

# ── M1 body checks: every word decodes to an allowed class; every li64 ───
# chain (per destination register, split at any non-movz/movk word)
# encodes a value from the program's expected set, INDEPENDENTLY.
def li64_chains(dump):
    """Yield (value, nwords) for every maximal run of movz/movk words that
    starts with movz hw=0 and stays on one destination register. Runs that
    don't start with hw=0 movz are skipped (the trampoline's 2 li64s are
    in the dump too, but their values are host addresses — they are
    excluded by the register filter below only in the caller)."""
    i = 0
    n = len(dump)
    while i < n:
        dec = decode(dump[i])
        # Only the move-wide family returns 4-tuples; everything else is
        # not a constant chain and is skipped.
        if dec is None or len(dec) != 4:
            i += 1
            continue
        cls, hw, imm16, rd_ = dec
        if cls == "movz" and hw == 0:
            val = imm16
            j = i + 1
            while j < n:
                dec2 = decode(dump[j])
                if dec2 is None or len(dec2) != 4:
                    break
                c2, h2, i2, r2 = dec2
                if c2 == "movk" and r2 == rd_ and h2 == (j - i):
                    val |= i2 << (16 * h2)
                    j += 1
                else:
                    break
            yield (rd_, val, j - i)
            i = j
        else:
            i += 1

def body_checks(dump, expected_values, label):
    ok = True
    bad = []
    for w in dump:
        d = decode(w)
        if d is None or d[0] not in ALLOWED:
            bad.append((w, d))
    if bad:
        print("MISMATCH %-22s: %d word(s) fail independent decode / not an emitted class"
              % (label, len(bad)))
        for w, d in bad[:8]:
            print("    %08x -> %s" % (w, d))
        ok = False
    else:
        print("OK       %-22s: all %d words decode to legal A64 classes"
              % (label, len(dump)))
    # li64 value check: every LOADI constant must appear as a chain. The
    # trampoline's two li64s target x9 and sit after the body; the body's
    # own li64s also use x9/x10/x11. We only require the expected values
    # to be PRESENT (the translator also emits store/load/tag words that
    # may reuse the same register between chains, so chains are split at
    # any non-movz/movk word — exactly what li64_chains does).
    present = Counter(v for (_, v, _) in li64_chains(dump))
    missing = []
    for v in expected_values:
        if present.get(v, 0) > 0:
            present[v] -= 1
        else:
            missing.append(v)
    if missing:
        print("MISMATCH %-22s: %d expected li64 constant(s) not found"
              % (label, len(missing)))
        for v in missing[:8]:
            print("    missing 0x%x" % v)
        ok = False
    else:
        print("OK       %-22s: all %d li64 constants encoded correctly"
              % (label, len(expected_values)))
    return ok

def check(dump_words, expected, label):
    cnt = Counter(dump_words)
    missing = []
    for w in expected:
        if cnt.get(w, 0) > 0:
            cnt[w] -= 1
        else:
            missing.append(w)
    if missing:
        print("MISMATCH %-22s: %d expected word(s) not found in dump" % (label, len(missing)))
        for w in missing[:8]:
            print("    missing %08x" % w)
        return False
    print("OK       %-22s: all %d expected encodings present" % (label, len(expected)))
    return True

def main():
    if len(sys.argv) != 3:
        print("usage: a64_enc_check.py dump.txt program")
        return 2
    with open(sys.argv[1]) as f:
        lines = [l.strip() for l in f if l.strip()]
    if lines[0].startswith("rc="):
        lines = lines[1:]
    dump = [int(l, 16) for l in lines]
    prog = sys.argv[2]
    ok = True

    ok &= check_trampoline(dump, "trampoline")
    ok &= check(dump, prologue(), "prologue")

    if prog == "straight_line_bench":
        # LOADIs r0..r5 = 1..6. The body ADDs/MOV are checked structurally
        # (legal classes, above); the constants are the allocation-free
        # ground truth.
        expected_values = [1, 2, 3, 4, 5, 6]
        ok &= body_checks(dump, expected_values, "body")
    elif prog == "loop_sum":
        # LOADIs r0=0, r1=1, r2=10, r4=1 (loop), plus CMP/ADD/cset/cbz.
        expected_values = [0, 1, 10, 1]
        ok &= body_checks(dump, expected_values, "body")
    elif prog == "extra_ops":
        # LOADIs r0=17, r1=5, r8=-8; plus immediates 2 and 1 materialized
        # for SHL/SHR/SAR register shifts.
        expected_values = [17, 5, 0xFFFFFFFFFFFFFFF8, 2, 1]
        ok &= body_checks(dump, expected_values, "body")
    else:
        print("unknown program %s" % prog)
        return 2
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
