/* rv_syscall_exit_test.c — host-side tripwire for the RISC-V kernel's
 * RV_SYS_EXIT exit branches (ISA doc §16 Phase 9g).
 *
 * Compiled THREE times from this one source, all for the x86 host with
 * -DSIMI_HOST_TEST (which swaps the RISC-V-only pieces for host
 * behavior, see arch/riscv/trap_riscv.c's top comment):
 *
 *   rv-syscall-exit-test    plain SIMI_HOST_TEST — the S-mode/OpenSBI
 *                           branch: reports the code, attempts the
 *                           firmware reset, and on failure prints the
 *                           "SBI_SRST unsupported or failed -- halting
 *                           hart instead." fallback.
 *   rv-syscall-exit-test-m  SIMI_HOST_TEST + RISCV_MMODE — the
 *                           bare-metal branch: no firmware exists, so
 *                           sbi_system_reset() is never called and the
 *                           "direct M-mode boot: no firmware to power
 *                           off -- halting hart." branch fires.
 *   rv-syscall-exit-test-u  SIMI_HOST_TEST + RV_TEST_A7=<bogus> — the
 *                           unimplemented-syscall branch: a7 is not
 *                           RV_SYS_EXIT, so dispatch reports
 *                           "unimplemented syscall number N -- halting
 *                           hart." without ever touching the reset
 *                           stub.
 *   rv-trap-test           SIMI_HOST_TEST + SIMI_TEST_TRAP — drives
 *                           riscv_trap_dispatch_common() (the routing
 *                           core the CSR-armed wrappers share) with
 *                           INJECTED cause/tval, since x86 has no
 *                           scause/mcause CSRs. Four argv modes:
 *                           "syscall" (scause=3 + a7=RV_SYS_EXIT →
 *                           routes into the S-mode exit path),
 *                           "ecall" (scause=9 + a7=RV_SYS_EXIT → the
 *                           same exit path through the code==9 branch),
 *                           "unhandled" (scause=2 → the [TRAP]
 *                           unhandled-exception branch), "interrupt"
 *                           (bit 63 set → handle_riscv_supervisor_
 *                           interrupt() stub, which RETURNS — the one
 *                           branch that doesn't halt).
 *
 * The a7 register is injected as RV_TEST_A7 (default RV_SYS_EXIT), so
 * the third build reuses the exact same dispatch call with a bogus
 * syscall number instead of special-casing it in C.
 *
 * Under SIMI_HOST_TEST the CSR-armed functions (riscv_trap_init /
 * riscv_trap_dispatch / riscv_trap_dispatch_m) are compiled out — they
 * cannot exist on x86 and the dispatch under test never calls them —
 * and rv_halt() (the wfi-spin terminal halt) becomes exit(0), so
 * "dispatch never returned" is observable as the process terminating
 * from inside the handler.
 *
 * The stubs below close what dispatch touches that would otherwise
 * reach the firmware: sbi_putchar() mirrors the kernel's console to
 * stdout, and sbi_system_reset() is stubbed to FAIL — it returns
 * without powering the machine off, exactly the firmware-lacks-SBI_SRST
 * case — with a stderr marker so the runner can assert whether or not
 * the branch under test attempted the reset. tests/run_riscv_tests.sh
 * asserts the branch-specific messages on stdout, the stub-fired marker
 * presence/absence on stderr, and the ABSENCE of main's post-call
 * "returned" marker.
 */
#include <stdio.h>
#include <string.h>
#include "../../arch/riscv/trap_riscv.h"

/* The syscall number under test. The S-mode and M-mode twins leave it at
 * RV_SYS_EXIT (164); the unimplemented-syscall twin overrides it at
 * compile time with -DRV_TEST_A7=999, exercising the same dispatch call
 * with a number dispatch does not implement. */
#ifndef RV_TEST_A7
#define RV_TEST_A7 RV_SYS_EXIT   /* 164, SYS_SLS_EXIT */
#endif

/* ─── Host stubs for the kernel's SBI layer (arch/riscv/sbi.h) ──────── */

/* Mirrors the kernel console (S-mode path: SBI_DBCN → the serial line).
 * The kernel's own messages pass through this, so the harness can assert
 * them. */
void sbi_putchar(char c) {
    fputc(c, stdout);
}

/* The failing stub: the firmware lacks the SBI_SRST extension, so the
 * call returns without powering anything off. The real kernel's
 * sbi_system_reset() (arch/riscv/sbi.c) only returns in exactly this
 * case (or on a failed reset), which is the branch under test. */
void sbi_system_reset(void) {
    fprintf(stderr, "[HOST] sbi_system_reset() stub: firmware lacks SBI_SRST, returning\n");
}

/* The interrupt stub: riscv_trap_dispatch_common() calls
 * handle_riscv_supervisor_interrupt() (defined in the real kernel) for
 * interrupt causes (scause bit 63 set) and RETURNS -- the one branch
 * that doesn't halt. On the host the stub just marks that it fired, and
 * the trap-routing main asserts the marker plus the return. Not needed
 * by the syscall-dispatch twins (that branch is inside the
 * dispatch_common routing they don't compile in), harmless to define. */
void handle_riscv_supervisor_interrupt(uint64_t scause, uint64_t stval) {
    fprintf(stderr, "[HOST] handle_riscv_supervisor_interrupt stub fired (scause=%llu stval=%llu)\n",
            (unsigned long long)scause, (unsigned long long)stval);
}

/* ─── Syscall-dispatch twins (default builds) ─────────────────────────── */
#if !defined(SIMI_TEST_TRAP)
int main(void) {
    struct RvPerHartData phd;
    memset(&phd, 0, sizeof(phd));
    phd.trap_frame[TF_A7] = RV_TEST_A7;   /* 164, SYS_SLS_EXIT by default */
    phd.trap_frame[TF_A0] = 42;            /* the SIMI boot smoke's result */

    riscv_syscall_dispatch(&phd);

    /* Only reachable if rv_halt() ever returned -- i.e. the fallback
     * path failed to terminate. The harness asserts this line is ABSENT
     * from stderr. */
    fprintf(stderr, "[HOST] FAIL: riscv_syscall_dispatch returned (fallback did not halt)\n");
    return 1;
}
#else /* SIMI_TEST_TRAP: the trap-routing twin (rv-trap-test) */

/* Drives riscv_trap_dispatch_common() with injected cause/tval. Modes:
 *   syscall     scause=3 (ebreak) + a7=RV_SYS_EXIT → routes into the
 *               S-mode syscall exit path (which halts: rc=0 via rv_halt).
 *   ecall       scause=9 (environment call) + a7=RV_SYS_EXIT → the SAME
 *               exit path through the code==9 branch -- pins that the
 *               two triggers route identically (dispatch halts before
 *               the branch's sepc+=4 continuation line, so only the
 *               routing is observable).
 *   unhandled   scause=2 (illegal instruction) → the [TRAP] unhandled
 *               branch (halts).
 *   interrupt   scause bit 63 set → handle_riscv_supervisor_interrupt()
 *               stub, which RETURNS — the one branch that doesn't halt,
 *               so dispatch_common returning here is SUCCESS, not the
 *               FAIL it is for the halting modes.
 * The runner drives each mode as its own process (one scenario per
 * process, like every other twin) via argv[1]. */
int main(int argc, char** argv) {
    int mode = 0;   /* 0=syscall, 1=unhandled, 2=interrupt, 3=ecall */
    if (argc > 1 && strcmp(argv[1], "ecall") == 0) mode = 3;
    else if (argc > 1 && strcmp(argv[1], "unhandled") == 0) mode = 1;
    else if (argc > 1 && strcmp(argv[1], "interrupt") == 0) mode = 2;

    struct RvPerHartData phd;
    memset(&phd, 0, sizeof(phd));
    phd.trap_frame[TF_A7] = RV_SYS_EXIT;              /* 164, SYS_SLS_EXIT */
    phd.trap_frame[TF_A0] = 42;                       /* the SIMI boot smoke's result */
    phd.trap_frame[TF_SEPC] = 0x80201000ULL;          /* a plausible kernel pc */

    if (mode == 0) {
        riscv_trap_dispatch_common(&phd, 3 /* scause: ebreak */, 0);
    } else if (mode == 1) {
        riscv_trap_dispatch_common(&phd, 2 /* scause: illegal instruction */, 0x1234);
    } else if (mode == 3) {
        riscv_trap_dispatch_common(&phd, 9 /* scause: environment call */, 0);
    } else {
        riscv_trap_dispatch_common(&phd, (1ULL << 63) | 5 /* timer interrupt */, 0);
    }

    if (mode == 2) {
        /* The interrupt branch is EXPECTED to return -- the stub fired
         * and dispatch_common handed control back. The harness asserts
         * this marker plus the stub-fired line. */
        fprintf(stderr, "[HOST] interrupt path returned as expected\n");
        return 0;
    }

    /* Only reachable if rv_halt() ever returned -- i.e. the syscall or
     * unhandled branch failed to terminate. The harness asserts this
     * line is ABSENT from stderr. */
    fprintf(stderr, "[HOST] FAIL: riscv_trap_dispatch_common returned (no branch halted)\n");
    return 1;
}
#endif /* SIMI_TEST_TRAP */
