/*
 * trap_riscv.c — see trap_riscv.h for the full design rationale. Gap
 * Remediation SIMI Phase 9 (sub-phases 9c/9d/9f/9g).
 *
 * SIMI_HOST_TEST (host-side test build only): this file is also compiled
 * for the x86 host by tools/simi/rv_syscall_exit_test.c, which stubs
 * sbi_putchar/sbi_system_reset and drives riscv_syscall_dispatch()'s
 * SBI_SRST failure fallback. Under that macro the RISC-V-only pieces are
 * swapped for host behavior: the CSR-armed functions are compiled out
 * entirely (the dispatch under test never calls them) and rv_halt()
 * exits the process instead of wfi-spinning (see rv_halt below).
 */
#include "trap_riscv.h"
#include "sbi.h"
#include <stddef.h>
#if defined(SIMI_HOST_TEST)
#include <stdlib.h>   /* exit() -- host-side test build only, see rv_halt() */
#endif

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

/* Deliberate terminal halt for the syscall/unhandled paths. Real kernel:
 * wfi-spin forever, never returning. Host-side test build
 * (SIMI_HOST_TEST): exit the process cleanly instead, so the harness
 * observes "dispatch never returned" as the process terminating from
 * inside the handler rather than falling through to main. */
static void rv_halt(void) {
#if defined(SIMI_HOST_TEST)
    exit(0);
#else
    while (1) { __asm__ volatile("wfi"); }
#endif
}

#if !defined(SIMI_HOST_TEST)
void riscv_trap_init(struct RvPerHartData* phd, uint64_t kernel_stack_top) {
    phd->kernel_sp = kernel_stack_top;
    for (int i = 0; i < TF_COUNT; i++) phd->trap_frame[i] = 0;

    /* sscratch <- &RvPerHartData. Must happen before the trap vector is
     * armed -- the very first trap after the csrw below will immediately
     * swap sp with whatever sscratch currently holds, so sscratch has to
     * already be correct by then, not "correct eventually." */
    __asm__ volatile("csrw sscratch, %0" : : "r"(phd) : "memory");

    /* Phase 9g: arm the vector for the build's mode -- the variant is
     * baked in at compile time (-DRISCV_MMODE), not detected at runtime:
     * reading mstatus (the only CSR that would reveal the privilege
     * mode) traps as an illegal instruction from S-mode (see §16 Phase
     * 9g). Under OpenSBI (S-mode) exceptions go to stvec; as a direct
     * M-mode payload they always go to mtvec, and arming only stvec
     * would leave every trap jumping to address 0. The M-mode entry
     * reads mepc/mcause/mtval and returns via mret (trap_riscv.S). */
#if defined(RISCV_MMODE)
    extern void riscv_trap_entry_m(void);
    __asm__ volatile("csrw mtvec, %0" : : "r"(&riscv_trap_entry_m) : "memory");
    rv_print_str("[TRAP] mtvec + sscratch armed for this hart (direct M-mode boot).\n");
#else
    extern void riscv_trap_entry(void);
    __asm__ volatile("csrw stvec, %0" : : "r"(&riscv_trap_entry) : "memory");
    rv_print_str("[TRAP] stvec + sscratch armed for this hart.\n");
#endif
}
#endif /* !SIMI_HOST_TEST: CSR-armed riscv_trap_init only -- riscv_syscall_dispatch below is ALWAYS compiled, because the host test drives it */

/* Gap Remediation SIMI Phase 9 (sub-phases 9d/9f): the minimal,
 * headless-only syscall surface. Only SYS_SLS_EXIT is wired -- there is
 * no process table, no scheduler, and no user/kernel privilege
 * separation on RISC-V yet (paging is still Bare everywhere, see
 * user_paging_riscv.h), so kernel/process.c's real process_exit() (which
 * assumes a live ProcessDescriptor entered via kernel_enter_ring3(), an
 * x86-only mechanism this phase does not port -- see §16 Phase 9's
 * findings) has nothing valid to act on here. Since 9f the exit is a
 * REAL machine shutdown through OpenSBI's SBI_SRST extension (an ecall
 * to M-mode OpenSBI powers the machine off -- on QEMU the process exits
 * rc=0), which is the honest, correct behavior for "a bare translated
 * SIMI program finished running with nothing else going on," not a
 * stand-in for real process teardown. Reached either from an ecall
 * (exception 9, only once/if OpenSBI ever delegates it) or from the
 * ebreak trigger (exception 3, delegated today -- see
 * riscv_trap_dispatch()'s comment and trap_riscv.h). */
void riscv_syscall_dispatch(struct RvPerHartData* phd) {
    uint64_t num = phd->trap_frame[TF_A7];
    uint64_t arg0 = phd->trap_frame[TF_A0];

    if (num == RV_SYS_EXIT) {
        rv_print_str("[SYSCALL] SYS_SLS_EXIT, code=");
        rv_print_udec(arg0);
#if defined(RISCV_MMODE)
        /* Direct M-mode boot: there is no firmware, so no SBI_SRST
         * exists to power the machine off -- the honest terminal state
         * is a reported, deliberate halt (QEMU is killed by the CI
         * timeout, rc=124). */
        rv_print_str(" -- direct M-mode boot: no firmware to power off -- halting hart.\n");
#else
        rv_print_str(" -- powering off via OpenSBI SBI_SRST (SHUTDOWN)...\n");
        sbi_system_reset();
        /* Only reached if the firmware lacks the SRST extension (or the
         * reset failed) -- sbi_system_reset() documents that callers
         * must not assume the machine is gone just because it returned. */
        rv_print_str("[SYSCALL] SBI_SRST unsupported or failed -- halting hart instead.\n");
#endif
        rv_halt();
    }

    rv_print_str("[SYSCALL] unimplemented syscall number ");
    rv_print_udec(num);
    rv_print_str(" -- halting hart.\n");
    rv_halt();
}

#if !defined(SIMI_HOST_TEST)
/* Shared routing for both privilege modes; the caller reads the
 * mode-appropriate cause/tval CSRs (scause/stval in S-mode,
 * mcause/mtval in M-mode) and hands them in. The unhandled-exception
 * message below calls the value "scause" for brevity; in M-mode it is
 * the mcause value (same encoding). */
static void riscv_trap_dispatch_common(struct RvPerHartData* phd,
                                       uint64_t scause, uint64_t stval) {
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
         * the hart or powers the machine off) -- but advance sepc past
         * the ecall regardless, so a future syscall that DOES return
         * doesn't re-trap on the same ecall instruction forever. ecall is
         * always a 4-byte instruction (RISC-V has no compressed-C
         * encoding for it), so +4 is exact, not a heuristic. */
        phd->trap_frame[TF_SEPC] += 4;
        return;
    }

    if (code == 3 && phd->trap_frame[TF_A7] == RV_SYS_EXIT) {
        /* S-mode syscall trigger (Phase 9f): OpenSBI's default MEDELEG
         * does not delegate exception 9 (ecall from S-mode), so an ecall
         * carrying the syscall ABI never reaches our stvec handler -- it
         * bounces as an SBI call. ebreak (exception 3) IS delegated, so
         * the kernel's syscall convention is carried through ebreak
         * instead: a7 = syscall number, a0 = argument -- the same ABI
         * the ecall path above uses (see trap_riscv.h). The SIMI boot
         * smoke issues exactly this (kernel/kernel_riscv.c), and
         * riscv_syscall_dispatch() powers the machine off via SBI_SRST,
         * so this is the smoke's terminal event. If dispatch ever
         * returns, advance past the ebreak -- the assembler emits the
         * 4-byte non-compressed form for the `ebreak` mnemonic -- and
         * continue. */
        riscv_syscall_dispatch(phd);
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
    rv_halt();
}

void riscv_trap_dispatch(struct RvPerHartData* phd) {
    uint64_t scause, stval;
    __asm__ volatile("csrr %0, scause" : "=r"(scause));
    __asm__ volatile("csrr %0, stval"  : "=r"(stval));
    riscv_trap_dispatch_common(phd, scause, stval);
}

/* Phase 9g: M-mode twin -- exceptions taken in M-mode populate mcause/
 * mtval, not scause/stval. Called by riscv_trap_entry_m (trap_riscv.S). */
void riscv_trap_dispatch_m(struct RvPerHartData* phd) {
    uint64_t mcause, mtval;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    __asm__ volatile("csrr %0, mtval"  : "=r"(mtval));
    riscv_trap_dispatch_common(phd, mcause, mtval);
}
#endif /* !SIMI_HOST_TEST: CSR-armed trap functions above */
