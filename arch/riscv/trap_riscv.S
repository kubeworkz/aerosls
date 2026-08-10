# trap_riscv.S — Gap Remediation SIMI Phase 9 (sub-phases 9c/9g): real RISC-V
# trap entry/exit, sscratch-based per-hart-data mechanism.
# See trap_riscv.h for the full design rationale and the RvPerHartData
# layout this file's hardcoded byte offsets must stay in lockstep with
# (trap_riscv.c carries _Static_asserts enforcing that at compile time).
#
# Standard RISC-V supervisor trap idiom (the same shape every real RISC-V
# kernel uses for this, e.g. xv6-riscv's trampoline.S) -- not a literal
# port of anything x86 does, because there is nothing on x86 shaped like
# this to port; see trap_riscv.h's header comment for why.
#
# Two entries, one body (Phase 9g): riscv_trap_entry is armed into stvec
# when the kernel runs in S-mode under OpenSBI (exception PC from sepc,
# return via sret, dispatched by riscv_trap_dispatch), and
# riscv_trap_entry_m is armed into mtvec when the kernel runs as a direct
# M-mode payload (QEMU -bios none: exceptions in M-mode are always taken
# to mtvec). The save/restore body is identical -- the two differ only in
# the exception-PC CSR (sepc vs mepc), the return instruction (sret vs
# mret), and the C dispatcher (which reads the matching cause/tval CSR).
# The sscratch swap works in both modes: sscratch is just a scratch CSR.
.section .text
.align 4

.macro TRAP_ENTRY pc_csr, dispatch_fn, ret_insn
    # sscratch always holds &RvPerHartData outside of trap handling.
    # This single instruction atomically does BOTH: sscratch <- old sp,
    # sp <- &RvPerHartData (the trap_frame's own base address, since
    # trap_frame is RvPerHartData's first member at offset 0).
    csrrw sp, sscratch, sp

    # Save every GPR except x0 (hardwired zero, nothing to save) and sp
    # itself (still sitting in sscratch right now -- saved explicitly
    # below once we're free to clobber a temp register for it).
    sd    ra,   0(sp)
    sd    gp,  16(sp)
    sd    tp,  24(sp)
    sd    t0,  32(sp)
    sd    t1,  40(sp)
    sd    t2,  48(sp)
    sd    s0,  56(sp)
    sd    s1,  64(sp)
    sd    a0,  72(sp)
    sd    a1,  80(sp)
    sd    a2,  88(sp)
    sd    a3,  96(sp)
    sd    a4, 104(sp)
    sd    a5, 112(sp)
    sd    a6, 120(sp)
    sd    a7, 128(sp)
    sd    s2, 136(sp)
    sd    s3, 144(sp)
    sd    s4, 152(sp)
    sd    s5, 160(sp)
    sd    s6, 168(sp)
    sd    s7, 176(sp)
    sd    s8, 184(sp)
    sd    s9, 192(sp)
    sd    s10,200(sp)
    sd    s11,208(sp)
    sd    t3, 216(sp)
    sd    t4, 224(sp)
    sd    t5, 232(sp)
    sd    t6, 240(sp)

    # trap_frame.sp (offset 8) = the real sp we swapped out, currently
    # sitting in sscratch.
    csrr  t0, sscratch
    sd    t0,   8(sp)

    # trap_frame.sepc (offset 248, TF_SEPC*8) = the trapped instruction's
    # own address (\pc_csr: sepc in S-mode, mepc in M-mode) -- needed
    # both to resume correctly for exceptions that DON'T advance past
    # themselves, and so the C dispatcher can advance it by 4 for ecall
    # (else the return instruction would re-execute the same ecall
    # forever).
    csrr  t0, \pc_csr
    sd    t0, 248(sp)

    # s1 already saved (offset 64) -- safe to repurpose as our own
    # "pointer to RvPerHartData, survives the C call" register, since
    # the dispatcher follows the standard calling convention and is free
    # to clobber s1 like any other callee-saved register IT decides to
    # use, but WE restore our own copy from the trap frame on the way out
    # regardless, so whatever the C side does to the live register
    # doesn't matter.
    mv    s1, sp
    # Switch to this hart's real kernel trap stack. kernel_sp moved from
    # offset 256 to 528 when the Design B FP region (TF_F0..TF_F31 +
    # fcsr + sfs, ISA doc §16 Phase 16 audit addendum) grew trap_frame
    # from 32 to 66 slots — the _Static_asserts in trap_riscv.c pin this
    # offset against the C struct (offsetof(kernel_sp) == 528).
    ld    sp, 528(s1)
    mv    a0, s1           # a0 = &RvPerHartData, the dispatcher's argument
    call  \dispatch_fn
    mv    sp, s1           # back to &RvPerHartData (== &trap_frame)

    ld    ra,   0(sp)
    ld    gp,  16(sp)
    ld    tp,  24(sp)
    ld    t0,  32(sp)
    ld    t1,  40(sp)
    ld    t2,  48(sp)
    ld    s0,  56(sp)
    ld    a0,  72(sp)
    ld    a1,  80(sp)
    ld    a2,  88(sp)
    ld    a3,  96(sp)
    ld    a4, 104(sp)
    ld    a5, 112(sp)
    ld    a6, 120(sp)
    ld    a7, 128(sp)
    ld    s2, 136(sp)
    ld    s3, 144(sp)
    ld    s4, 152(sp)
    ld    s5, 160(sp)
    ld    s6, 168(sp)
    ld    s7, 176(sp)
    ld    s8, 184(sp)
    ld    s9, 192(sp)
    ld    s10,200(sp)
    ld    s11,208(sp)
    ld    t3, 216(sp)
    ld    t4, 224(sp)
    ld    t5, 232(sp)
    ld    t6, 240(sp)

    # Restore the exception PC (the dispatcher may have advanced it past
    # ecall/ebreak).
    ld    t0, 248(sp)
    csrw  \pc_csr, t0

    # sscratch must point at &RvPerHartData again for the NEXT trap --
    # sp already does right now, so this is just handing that same value
    # back to the CSR.
    csrw  sscratch, sp

    # s1 was our own scratch above; restore its real saved value now,
    # after every other use of it is done.
    ld    s1,  64(sp)

    # Finally restore the real sp (saved at offset 8) and return.
    ld    t0,   8(sp)
    mv    sp, t0
    \ret_insn
.endm

.global riscv_trap_entry
riscv_trap_entry:
    TRAP_ENTRY sepc, riscv_trap_dispatch, sret

.global riscv_trap_entry_m
riscv_trap_entry_m:
    TRAP_ENTRY mepc, riscv_trap_dispatch_m, mret
