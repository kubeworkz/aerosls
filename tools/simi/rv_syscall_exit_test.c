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
