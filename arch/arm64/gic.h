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

/* Program the GICv2 distributor + CPU interface: keep both timer PPIs
 * (physical 14, virtual 11 — M5.3's nested source) in group 0, the
 * SECURE group — this kernel runs in the Secure world, and a secure
 * access acknowledges group-0 interrupts directly (group-1 reads
 * return 1022 without AckCtl, the §10.197 spike finding). Clear
 * pending, enable both, set their priorities (virtual 0x00 higher
 * than physical 0x80 so the nested fire preempts the physical
 * handler), enable the CPU interface's group 0, and set the priority
 * mask to allow all. Call once at boot, after the MMU is on (the GIC
 * lives at high-VA device pages mapped by boot_arm64.S). */
void gic_init(void);

/* Acknowledge the pending interrupt: returns the INTID (1023 =
 * spurious, nothing to EOIR). Call before re-arming the timer. */
uint32_t gic_iar(void);

/* End the interrupt (write the INTID back). */
void gic_eoir(uint32_t intid);

/* Generic timers. init reads CNTFRQ_EL0. arm_timer_arm reloads
 * CNTP_TVAL_EL0 with a 100 ms period and enables the physical timer
 * (the handler re-arms BEFORE the EOIR — the RISC-V discipline:
 * minimize the window where a tick could be missed). arm_vtimer_arm is
 * M5.3's one-shot 10 ms virtual-timer arm (the nested source);
 * arm_vtimer_disarm disables it so its level deasserts before the
 * nested handler's EOIR. */
void arm_timer_init(void);
uint64_t arm_timer_cntfrq(void);
void arm_timer_arm(void);
void arm_vtimer_arm(void);
void arm_vtimer_disarm(void);

/* M5.3: the virtual timer's GIC INTID (PPI 11 + 16), for the tick
 * handler's source dispatch. */
#define GIC_VIRT_TIMER_INTID 27u

#endif /* ARCH_ARM64_GIC_H */
