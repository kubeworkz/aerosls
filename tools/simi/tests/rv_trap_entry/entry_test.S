# tools/simi/tests/rv_trap_entry/entry_test.S — Phase 9h (ISA doc §16):
# the trap-entry fixture's test driver. Same source, two builds, exactly
# the kernel's own RISCV_MMODE pattern: the default build is a direct
# M-mode payload under qemu-system-riscv64 -bios none (single hart) —
# arms mtvec + sscratch against riscv_trap_entry_m; the -DFIXTURE_SMODE
# build boots under OpenSBI (default -bios) at 0x80200000 — arms stvec +
# sscratch against riscv_trap_entry. Both use the REAL entry
# (arch/riscv/trap_riscv.S), load distinctive values into every pinable
# GPR, trap via ebreak, and verify the entry's save/restore round trip.
#
# The restore+return half of the entry is the part NO kernel boot
# exercises: every real trap in the kernel either halts (unhandled
# exception, M-mode exit) or powers the machine off (S-mode exit) — so
# the `ld` restore sequence, the exception-PC restore
# (dispatcher-advanced), the sscratch re-arm, the sp restore, and
# sret/mret have never executed anywhere until this fixture's dispatcher
# stub (entry_test.c) RETURNS.
#
# Register pinning: every GPR the entry saves is loaded with a
# distinctive value — TF index i carries 0x1000+i for i in 0..30 (x2/sp
# is excluded: it must stay a valid stack pointer and is verified
# separately against g_pre_trap_sp; index 31/sepc is verified separately
# against g_expected_sepc). The dispatcher stub checks the saved frame
# from inside the trap; this file checks the restored registers after
# mret. t0 (TF_T0) is the one register the entry does NOT round-trip:
# its restore is deliberately overwritten by the sp-restore scratch
# (`ld t0, 8(sp); mv sp, t0`) — caller-saved, so ABI-legal — and this
# file verifies its actual post-trap value (the pre-trap sp) instead.
# t5 is the fixture's own capture pointer (the post-mret dump below) so
# its round-trip is not directly observable — the other 29 pinned
# registers are. gp/tp are deliberately pinned too — the C side is
# compiled with -msmall-data-limit=0 -mno-relax so nothing addresses
# data through gp, and tp is unused (no TLS) — see the Makefile target.
.section .text.init,"ax",@progbits
.global _start
_start:
    la   sp, _stack_top

    # Point sscratch at the fixture's RvPerHartData, arm the mode's
    # trap vector with the real entry (stvec/riscv_trap_entry under
    # OpenSBI, mtvec/riscv_trap_entry_m bare-metal — both emitted by
    # trap_riscv.S), and give the entry a real kernel trap stack.
    la   t0, g_phd
    csrw sscratch, t0
#ifdef FIXTURE_SMODE
    la   t1, riscv_trap_entry
    csrw stvec, t1
#else
    la   t1, riscv_trap_entry_m
    csrw mtvec, t1
#endif
    la   t2, _stack_top
    sd   t2, 256(t0)          # kernel_sp (offset 256) = stack top

    # Record what the trap must observe: the ebreak's address (the
    # exception PC: sepc in S-mode, mepc in M-mode) and the sp live at
    # trap time.
    la   t3, ebreak_site
    la   t4, g_expected_sepc
    sd   t3, 0(t4)
    la   t5, g_pre_trap_sp
    sd   sp, 0(t5)

    # Pin every register the entry saves: TF index i -> 0x1000+i.
    li   ra,   0x1000
    li   gp,   0x1002
    li   tp,   0x1003
    li   t0,   0x1004
    li   t1,   0x1005
    li   t2,   0x1006
    li   s0,   0x1007
    li   s1,   0x1008
    li   a0,   0x1009
    li   a1,   0x100A
    li   a2,   0x100B
    li   a3,   0x100C
    li   a4,   0x100D
    li   a5,   0x100E
    li   a6,   0x100F
    li   a7,   0x1010
    li   s2,   0x1011
    li   s3,   0x1012
    li   s4,   0x1013
    li   s5,   0x1014
    li   s6,   0x1015
    li   s7,   0x1016
    li   s8,   0x1017
    li   s9,   0x1018
    li   s10,  0x1019
    li   s11,  0x101A
    li   t3,   0x101B
    li   t4,   0x101C
    li   t5,   0x101D
    li   t6,   0x101E

ebreak_site:
    # ebreak, FORCED to the 4-byte form. The dispatcher advances the
    # exception PC by 4 (the kernel's documented contract, trap_riscv.c);
    # with the C extension enabled the plain `ebreak` mnemonic assembles
    # to the 2-byte compressed 0x9002, and PC+4 would land MID-
    # INSTRUCTION (a real latent bug the fixture exposed in the kernel's
    # own trigger too -- kernel/kernel_riscv.c). `.option norvc` pins
    # the 32-bit 0x00100073 so the +4 resume is exact.
    .option norvc
    ebreak
    .option rvc

    # ===== post-trap: the entry restored every register; verify. =====
    # FIRST capture the post-mret register state to g_reg_dump, BEFORE
    # any check clobbers a register (t5 is the pointer, so its own slot
    # holds the pointer value and is noted as such in entry_test.c; the
    # other 29 slots are exact). The fail path then prints this captured
    # truth instead of re-reading registers the checks have clobbered.
    la   t5, g_reg_dump
    sd   ra,   0(t5)
    sd   gp,   8(t5)
    sd   tp,  16(t5)
    sd   t0,  24(t5)
    sd   t1,  32(t5)
    sd   t2,  40(t5)
    sd   s0,  48(t5)
    sd   s1,  56(t5)
    sd   a0,  64(t5)
    sd   a1,  72(t5)
    sd   a2,  80(t5)
    sd   a3,  88(t5)
    sd   a4,  96(t5)
    sd   a5, 104(t5)
    sd   a6, 112(t5)
    sd   a7, 120(t5)
    sd   s2, 128(t5)
    sd   s3, 136(t5)
    sd   s4, 144(t5)
    sd   s5, 152(t5)
    sd   s6, 160(t5)
    sd   s7, 168(t5)
    sd   s8, 176(t5)
    sd   s9, 184(t5)
    sd   s10,192(t5)
    sd   s11,200(t5)
    sd   t3, 208(t5)
    sd   t4, 216(t5)
    sd   t5, 224(t5)
    sd   t6, 232(t5)

    # Now the checks; registers may be clobbered freely.
    # If the dispatcher did NOT advance the exception PC past the
    # ebreak, we never get here (sret/mret re-traps on the same ebreak
    # forever -> the runner sees a hang with no PASS, i.e. a failure).
    #
    # t0 is the ONE register the entry does not round-trip, by design:
    # after its restore it is reused as the sp-restore scratch
    # (trap_riscv.S: `ld t0, 8(sp); mv sp, t0`) -- caller-saved, so the
    # ABI permits the trap to clobber it. Its post-trap value is the
    # pre-trap sp, which we verify here.
    #
    # Scratch discipline: t5 is the universal scratch. The capture above
    # already clobbered t5 to the dump pointer, so its round-trip is
    # unobservable either way -- using it as scratch loses nothing, and
    # NO pinned register (t0, t4, t6) is destroyed before its own check.
    # t4/t6 are checked against their frame slots: the dispatcher
    # verified every slot against the same constants, so
    # restored-reg != frame[i] catches a restore-offset bug.
    la   t5, g_pre_trap_sp
    ld   t5, 0(t5)
    bne  t0, t5, fail              # t0 vs the pre-trap sp
    la   t5, g_phd
    ld   t5, 224(t5)               # frame[28] (the saved t4)
    bne  t4, t5, fail
    la   t5, g_phd
    ld   t5, 240(t5)               # frame[30] (the saved t6)
    bne  t6, t5, fail

    li   t6, 0x1000; bne ra,  t6, fail
    li   t6, 0x1002; bne gp,  t6, fail
    li   t6, 0x1003; bne tp,  t6, fail
    li   t6, 0x1005; bne t1,  t6, fail
    li   t6, 0x1006; bne t2,  t6, fail
    li   t6, 0x1007; bne s0,  t6, fail
    li   t6, 0x1008; bne s1,  t6, fail
    li   t6, 0x1009; bne a0,  t6, fail
    li   t6, 0x100A; bne a1,  t6, fail
    li   t6, 0x100B; bne a2,  t6, fail
    li   t6, 0x100C; bne a3,  t6, fail
    li   t6, 0x100D; bne a4,  t6, fail
    li   t6, 0x100E; bne a5,  t6, fail
    li   t6, 0x100F; bne a6,  t6, fail
    li   t6, 0x1010; bne a7,  t6, fail
    li   t6, 0x1011; bne s2,  t6, fail
    li   t6, 0x1012; bne s3,  t6, fail
    li   t6, 0x1013; bne s4,  t6, fail
    li   t6, 0x1014; bne s5,  t6, fail
    li   t6, 0x1015; bne s6,  t6, fail
    li   t6, 0x1016; bne s7,  t6, fail
    li   t6, 0x1017; bne s8,  t6, fail
    li   t6, 0x1018; bne s9,  t6, fail
    li   t6, 0x1019; bne s10, t6, fail
    li   t6, 0x101A; bne s11, t6, fail
    li   t6, 0x101B; bne t3,  t6, fail

    # The entry must have re-armed sscratch to &g_phd for the next trap.
    csrr t5, sscratch
    la   t6, g_phd
    bne  t5, t6, fail

    la   a0, msg_pass
    call uart_print
halt:
    wfi
    j    halt

fail:
    # Print the CAPTURED post-mret state (g_reg_dump, taken before the
    # checks ran) -- re-reading the live registers here would show the
    # checks' own clobbers, not the round-trip truth.
    la   a0, msg_fail
    call uart_print
    call dump_and_halt
    j    halt

.section .rodata
#ifdef FIXTURE_SMODE
msg_pass: .asciz "[FIXTURE] PASS: S-mode entry save/restore round-trip verified\n"
#else
msg_pass: .asciz "[FIXTURE] PASS: entry save/restore round-trip verified\n"
#endif
msg_fail: .asciz "[FIXTURE] FAIL: register round-trip mismatch\n"
