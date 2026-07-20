/*
 * trap_riscv.c — see trap_riscv.h for the full design rationale. Gap
 * Remediation SIMI Phase 9 (sub-phases 9c/9d).
 */
#include "trap_riscv.h"
#include "sbi.h"
#include <stddef.h>

extern void handle_riscv_supervisor_interrupt(uint64_t scause, uint64_t stval);
/* Deliberately NOT calling kernel/process.c's process_exit() here -- it
 * assumes a live ProcessDescriptor entered via the x86-only
 * kernel_enter_ring3() mechanism (swapgs, per-CPU GS-relative state),
 * none of which exists on this build. See riscv_syscall_dispatch()'s own
 * comment below. */

/* ─── Layout guards ─────────────────────────────────────────────────────
 * trap_riscv.S hardcodes every one of these as a literal byte offset
 * (asm can't #include this struct). These _Static_asserts are the only
 * thing standing between "the struct changed" and "the assembly silently
 * saves/restores the wrong register at the wrong offset" -- exactly the
 * class of bug this project has caught before by actually running code
 * (see the Phase 11 callee-saved-register corruption bug, or the RV64
 * JMPR host-address bug from Phase 14 Part E) rather than by review
 * alone. There is no way to run this file's own assembly in this sandbox
 * (see trap_riscv.h's header comment and AeroSLS-SIMI-ISA-v0.1.md §16
 * Phase 9 for why), so these compile-time checks are the strongest
 * verification available for this particular file today -- real, but
 * weaker than the execution-based verification every other file in this
 * project gets, and that gap is stated here plainly rather than left
 * implicit. */
_Static_assert(offsetof(struct RvPerHartData, trap_frame) == 0,
                "trap_riscv.S assumes trap_frame is RvPerHartData's first member");
_Static_assert(offsetof(struct RvPerHartData, kernel_sp) == 256,
                "trap_riscv.S hardcodes offset 256 for kernel_sp");
_Static_assert(TF_SP == 1, "trap_riscv.S hardcodes offset 8 for trap_frame.sp");
_Static_assert(TF_SEPC == 31, "trap_riscv.S hardcodes offset 248 for trap_frame.sepc");
_Static_assert(TF_A7 == 16, "riscv_syscall_dispatch reads a7 at trap_frame[16] (offset 128)");
_Static_assert(TF_A0 == 9, "riscv_syscall_dispatch reads/writes a0 at trap_frame[9] (offset 72)");
_Static_assert(sizeof(struct RvPerHartData) == 264,
                "264 = 32*8 (trap_frame) + 8 (kernel_sp) -- if this changes, "
                "trap_riscv.S's hardcoded offsets need updating too");

/* ─── Local no-libc helper (mirrors kernel/simi_x86.c's own convention) ── */
static void rv_print_str(const char* s) {
    while (*s) sbi_putchar(*s++);
}
static void rv_print_udec(uint64_t v) {
    char buf[20];
    int i = 0;
    if (v == 0) { sbi_putchar('0'); return; }
    while (v > 0 && i < 20) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) sbi_putchar(buf[--i]);
}

void riscv_trap_init(struct RvPerHartData* phd, uint64_t kernel_stack_top) {
    phd->kernel_sp = kernel_stack_top;
    for (int i = 0; i < TF_COUNT; i++) phd->trap_frame[i] = 0;

    /* sscratch <- &RvPerHartData. Must happen before stvec is armed --
     * the very first trap after the csrw stvec below will immediately
     * swap sp with whatever sscratch currently holds, so sscratch has to
     * already be correct by then, not "correct eventually." */
    __asm__ volatile("csrw sscratch, %0" : : "r"(phd) : "memory");

    extern void riscv_trap_entry(void);
    __asm__ volatile("csrw stvec, %0" : : "r"(&riscv_trap_entry) : "memory");

    rv_print_str("[TRAP] stvec + sscratch armed for this hart.\n");
}

/* Gap Remediation SIMI Phase 9 (sub-phase 9d): the minimal, headless-only
 * syscall surface. Only SYS_SLS_EXIT is wired -- there is no process
 * table, no scheduler, and no user/kernel privilege separation on RISC-V
 * yet (paging is still Bare everywhere, see user_paging_riscv.h), so
 * kernel/process.c's real process_exit() (which assumes a live
 * ProcessDescriptor entered via kernel_enter_ring3(), an x86-only
 * mechanism this phase does not port -- see §16 Phase 9's findings) has
 * nothing valid to act on here. This just reports the exit code and
 * halts the hart, which is the honest, correct behavior for "a bare
 * translated SIMI program finished running with nothing else going on,"
 * not a stand-in for real process teardown. */
void riscv_syscall_dispatch(struct RvPerHartData* phd) {
    uint64_t num = phd->trap_frame[TF_A7];
    uint64_t arg0 = phd->trap_frame[TF_A0];

    if (num == RV_SYS_EXIT) {
        rv_print_str("[SYSCALL] SYS_SLS_EXIT, code=");
        rv_print_udec(arg0);
        rv_print_str(" -- halting hart (no process table on RISC-V yet, see doc section 16 Phase 9).\n");
        while (1) { __asm__ volatile("wfi"); }
    }

    rv_print_str("[SYSCALL] unimplemented syscall number ");
    rv_print_udec(num);
    rv_print_str(" -- halting hart.\n");
    while (1) { __asm__ volatile("wfi"); }
}

void riscv_trap_dispatch(struct RvPerHartData* phd) {
    uint64_t scause, stval;
    __asm__ volatile("csrr %0, scause" : "=r"(scause));
    __asm__ volatile("csrr %0, stval"  : "=r"(stval));

    int is_interrupt = (int)((scause >> 63) & 1);
    uint64_t code = scause & 0x7FFFFFFFFFFFFFFFULL;

    if (is_interrupt) {
        /* Confirmed-dangling-symbol fix: this is the first real call site
         * for handle_riscv_supervisor_interrupt() anywhere in this
         * project -- see this file's header comment / trap_riscv.h. */
        handle_riscv_supervisor_interrupt(scause, stval);
        return;
    }

    if (code == 9) {   /* Environment call from S-mode */
        riscv_syscall_dispatch(phd);
        /* riscv_syscall_dispatch() never returns today (every path halts
         * the hart) -- but advance sepc past the ecall regardless, so a
         * future syscall that DOES return doesn't re-trap on the same
         * ecall instruction forever. ecall is always a 4-byte instruction
         * (RISC-V has no compressed-C encoding for it), so +4 is exact,
         * not a heuristic. */
        phd->trap_frame[TF_SEPC] += 4;
        return;
    }

    /* Unhandled exception (illegal instruction, page fault, misaligned
     * access, ecall from an unexpected mode, ...). No crash-safe recovery
     * exists for these yet -- print diagnostics and halt rather than
     * `sret` back into the same faulting instruction forever. */
    rv_print_str("[TRAP] unhandled exception, scause=");
    rv_print_udec(scause);
    rv_print_str(", stval=");
    rv_print_udec(stval);
    rv_print_str(", sepc=");
    rv_print_udec(phd->trap_frame[TF_SEPC]);
    rv_print_str(" -- halting hart.\n");
    while (1) { __asm__ volatile("wfi"); }
}
