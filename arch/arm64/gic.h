/* arch/arm64/gic.h — M5.2: the qemu virt GIC + the ARMv8 generic timer
 * (plan doc §6 M5.2, §10.194/§10.195).
 *
 * Empirically pinned by the M5.2 spike (the scope's own discipline):
 * the virt machine defaults to GICv2 under TCG with <= 8 CPUs
 * (finalize_gic_version: NOSEL -> v2 when supported and max_cpus <=
 * GIC_NCPU), so the CPU interface is MMIO at 0x08010000, not the
 * GICv3 ICC_* sysregs; and the non-secure EL1 physical timer (CNTP) is
 * PPI 14 — read from qemu's own FDT (/timer interrupts: (1, 14, 260)).
 */
#ifndef ARCH_ARM64_GIC_H
#define ARCH_ARM64_GIC_H

#include <stdint.h>

/* Program the GICv2 distributor + CPU interface: route the EL1
 * physical timer PPI (14) to group 1, clear pending, enable it, enable
 * the CPU interface's group 1, and set the priority mask to allow all.
 * Call once at boot, after the MMU is on (the GIC lives at high-VA
 * device pages mapped by boot_arm64.S). */
void gic_init(void);

/* Acknowledge the pending interrupt: returns the INTID (1023 =
 * spurious, nothing to EOIR). Call before re-arming the timer. */
uint32_t gic_iar(void);

/* End the interrupt (write the INTID back). */
void gic_eoir(uint32_t intid);

/* Generic timer (CNTP, the EL1 physical timer). init reads CNTFRQ_EL0;
 * arm reloads CNTP_TVAL_EL0 with a 100 ms period and enables the timer.
 * The handler re-arms BEFORE the EOIR (the RISC-V discipline: minimize
 * the window where a tick could be missed). */
void arm_timer_init(void);
void arm_timer_arm(void);

#endif /* ARCH_ARM64_GIC_H */
