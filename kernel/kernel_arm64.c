/* kernel/kernel_arm64.c — M4a: the minimal arm64 kernel main.
 *
 * Gap Remediation SIMI Phase 10 / M4 (docs/AeroSLS-SIMI-ARM-Backend-Plan-
 * v0.1.md §6 M4, §10.187): the first bootable AArch64 kernel, mirroring
 * the RISC-V kernel's Phase 9 shape — a self-contained qemu -M virt
 * build whose boot prints a banner and ends in a clean power-off. The
 * arm64 analog of SBI_SRST is PSCI: qemu -M virt emulates PSCI 0.2
 * through its EL3 monitor, so `smc #0` with the SYSTEM_OFF function id
 * powers the machine off and QEMU exits rc=0.
 *
 * M4a deliberately has NO SIMI yet (that is M4b: link kernel/simi_arm.c,
 * provide the {memcpy, memset} pair, translate an embedded .tmo and call
 * it). It also has no MMU (VMSAv8-64, deferred to M4c) and no interrupts
 * (polled console only) — the honest scope cuts §10.187 names.
 *
 * No libc, no FP/SIMD: compiled -mgeneral-regs-only, mirroring the x86
 * kernel's -mno-sse discipline. */
#include <stdint.h>

#include "arch/arm64/uart_pl011.h"

/* PSCI 0.2 function ids (smc #0 to the virt EL3 monitor). */
#define PSCI_FN_SYSTEM_OFF 0x84000008UL

static void psci_system_off(void)
{
    register uint64_t x0 asm("x0") = PSCI_FN_SYSTEM_OFF;
    asm volatile("smc #0" : : "r"(x0) : "memory");
    /* Not reached when the monitor honors the call. */
    for (;;)
        ;
}

static void print_el(void)
{
    uint64_t el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    uart_puts("[M4] exception level: EL");
    uart_putc('0' + (char)((el >> 2) & 3));
    uart_puts("\r\n");
}

void kernel_arm64_main(void)
{
    uart_init();
    uart_puts("AeroSLS ARM64 Node Kernel Online!\r\n");
    uart_puts("[M4] minimal arm64 kernel (M4a) booted under qemu -M virt\r\n");
    print_el();
    uart_puts("[M4] issuing PSCI SYSTEM_OFF\r\n");
    psci_system_off();
    /* PSCI should have powered us off; a return here is a bug. */
    for (;;)
        ;
}
