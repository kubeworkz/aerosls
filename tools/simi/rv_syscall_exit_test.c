/* rv_syscall_exit_test.c — host-side tripwire for the RISC-V kernel's
 * RV_SYS_EXIT SBI_SRST failure fallback (ISA doc §16 Phase 9g).
 *
 * Compiles the REAL arch/riscv/trap_riscv.c for the x86 host with
 * -DSIMI_HOST_TEST, which swaps the RISC-V-only pieces for host behavior
 * (see that file's top comment): the CSR-armed functions
 * (riscv_trap_init / riscv_trap_dispatch / riscv_trap_dispatch_m) are
 * compiled out — they cannot exist on x86 and the dispatch under test
 * never calls them — and rv_halt() (the wfi-spin terminal halt) becomes
 * exit(0), so "dispatch never returned" is observable as the process
 * terminating from inside the handler.
 *
 * The stubs below close the two things dispatch touches that would
 * otherwise reach the firmware: sbi_putchar() mirrors the kernel's
 * console to stdout, and sbi_system_reset() is stubbed to FAIL — it
 * returns without powering the machine off, exactly the
 * firmware-lacks-SBI_SRST case. tests/run_riscv_tests.sh asserts the
 * fallback messages on stdout, the stub-fired marker on stderr, and the
 * ABSENCE of main's post-call "returned" marker.
 */
#include <stdio.h>
#include <string.h>
#include "../../arch/riscv/trap_riscv.h"

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
    phd.trap_frame[TF_A7] = RV_SYS_EXIT;   /* 164, SYS_SLS_EXIT */
    phd.trap_frame[TF_A0] = 42;            /* the SIMI boot smoke's result */

    riscv_syscall_dispatch(&phd);

    /* Only reachable if rv_halt() ever returned -- i.e. the fallback
     * path failed to terminate. The harness asserts this line is ABSENT
     * from stderr. */
    fprintf(stderr, "[HOST] FAIL: riscv_syscall_dispatch returned (fallback did not halt)\n");
    return 1;
}
