#!/usr/bin/env python3
"""
a64_enc_check.py — M0 transcription-error net.

Independently re-encodes a set of A64 instructions using the ARM bit
layout written from the architecture reference (field positions: Rd@0,
Rn@5, Rm@16, imm12@21:10, shift@23:22, size@31:30, cond@15:12 — the
same layout confirmed independently in the Linux kernel's
arch/arm64/lib/insn.c), then checks that the words simi_arm.c actually
emitted (dumped by a64_dump.c) contain exactly those encodings.

This is deliberately NOT the C decoder (which shares simi_arm.c's
author); it is a from-scratch Python encoder catching transcription
errors in the C encoder's constants. The C decoder executing the output
is the stronger end-to-end proof; this catches the class of error where
encoder AND decoder agree on a wrong constant.

Usage: a64_enc_check.py <dump file> <simi program name>
The dump file is the output of a64_dump.c; the program name selects the
expected word set (straight_line_bench | loop_sum | extra_ops).
"""

import sys
from collections import Counter

# ── Independent encoders, from the ARM bit layout ────────────────────────
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
def br(rn):                 return 0xD61F0000 | (rn << 5)
def blr(rn):                return 0xD63F0000 | (rn << 5)

def li64(rd, imm):
    words = [movz(rd, imm & 0xFFFF)]
    for hw in (1, 2, 3):
        half = (imm >> (16 * hw)) & 0xFFFF
        if half:
            words.append(movk(rd, half, hw))
    return words

# Register numbers used by simi_arm.c
T0, T1, T2, SP, FP, LR, XZR = 9, 10, 11, 31, 29, 30, 31
X0 = 0

# ── Expected word sets per program (all static encodings) ────────────────
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

def loadi(rd, imm):
    return li64(T0, imm & 0xFFFFFFFFFFFFFFFF) + [stp_(T0, SP, 71 - rd), strb(XZR, SP, rd)]

def alu2(opword, rd, ra, rb):
    return [ldr(T0, SP, 71 - ra), ldr(T1, SP, 71 - rb), opword, stp_(T0, SP, 71 - rd), strb(XZR, SP, rd)]

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
        exp = []
        for rd, v in [(0, 1), (1, 2), (2, 3), (3, 4), (4, 5), (5, 6)]:
            exp += loadi(rd, v)
        exp += alu2(add_sh(T0, T0, T1), 6, 0, 1)
        exp += alu2(add_sh(T0, T0, T1), 7, 2, 3)
        exp += alu2(add_sh(T0, T0, T1), 8, 4, 5)
        exp += alu2(add_sh(T0, T0, T1), 9, 6, 7)
        exp += alu2(add_sh(T0, T0, T1), 10, 9, 8)
        # MOV r0, r10
        exp += [ldr(T0, SP, 71 - 10), stp_(T0, SP, 71 - 0), ldrb(T1, SP, 10), strb(T1, SP, 0)]
        ok &= check(dump, exp, "body")
    elif prog == "loop_sum":
        exp = loadi(0, 0) + loadi(1, 1) + loadi(2, 10) + loadi(4, 1)
        # CMP r3, r1, r2, GT
        exp += [ldr(T0, SP, 71 - 1), ldr(T1, SP, 71 - 2), subs_sh(XZR, T0, T1),
                cset(T0, 12), stp_(T0, SP, 71 - 3), strb(XZR, SP, 3)]
        # ADD r0, r0, r1; ADD r1, r1, r4
        exp += alu2(add_sh(T0, T0, T1), 0, 0, 1)
        exp += alu2(add_sh(T0, T0, T1), 1, 1, 4)
        # RET: ldr t0,slot0; ldrb t1,tag0; add sp,sp,#576; ldr x30,[sp,#8];
        #      ldr x29,[sp,#0]; add sp,sp,#16; br x30
        exp += [ldr(T0, SP, 71), ldrb(T1, SP, 0), add_imm(SP, SP, 576),
                0xF94007FE, 0xF94003FD, 0x910043FF, br(LR)]
        ok &= check(dump, exp, "body")
    elif prog == "extra_ops":
        exp = loadi(0, 17) + loadi(1, 5)
        # DIV r2, r0, r1 (signed): sdiv t0,t0,t1
        exp += [ldr(T0, SP, 71 - 0), ldr(T1, SP, 71 - 1), sdiv(T0, T0, T1),
                stp_(T0, SP, 71 - 2), strb(XZR, SP, 2)]
        # MOD r3, r0, r1: sdiv t2,t0,t1; msub t0,t2,t1,t0
        exp += [ldr(T0, SP, 71 - 0), ldr(T1, SP, 71 - 1), sdiv(T2, T0, T1),
                msub(T0, T2, T1, T0), stp_(T0, SP, 71 - 3), strb(XZR, SP, 3)]
        # ADD r4, r2, r3
        exp += alu2(add_sh(T0, T0, T1), 4, 2, 3)
        # SHL r5, r4, #2 (imm path): li64 t1, 2; lslv
        exp += [ldr(T0, SP, 71 - 4)] + li64(T1, 2) + [lslv(T0, T0, T1),
                stp_(T0, SP, 71 - 5), strb(XZR, SP, 5)]
        # SHR r6, r5, #1: lsrv
        exp += [ldr(T0, SP, 71 - 5)] + li64(T1, 1) + [lsrv(T0, T0, T1),
                stp_(T0, SP, 71 - 6), strb(XZR, SP, 6)]
        # LOADI r8, #-8 (sign-extended 64-bit)
        exp += li64(T0, 0xFFFFFFFFFFFFFFF8) + [stp_(T0, SP, 71 - 8), strb(XZR, SP, 8)]
        # SAR r9, r8, #1: asrv
        exp += [ldr(T0, SP, 71 - 8)] + li64(T1, 1) + [asrv(T0, T0, T1),
                stp_(T0, SP, 71 - 9), strb(XZR, SP, 9)]
        # NEG r10, r9: sub t0, xzr, t0
        exp += [ldr(T0, SP, 71 - 9), sub_sh(T0, XZR, T0),
                stp_(T0, SP, 71 - 10), strb(XZR, SP, 10)]
        # ADD r11, r6, r10
        exp += alu2(add_sh(T0, T0, T1), 11, 6, 10)
        # NOT r0, r11: mvn (orn t0, xzr, t0)
        exp += [ldr(T0, SP, 71 - 11), orn(T0, T0),
                stp_(T0, SP, 71 - 0), strb(XZR, SP, 0)]
        ok &= check(dump, exp, "body")
    else:
        print("unknown program %s" % prog)
        return 2
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
