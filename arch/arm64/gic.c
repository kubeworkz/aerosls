/* arch/arm64/gic.c — M5.2: the qemu virt GICv2 + the ARMv8 EL1
 * physical generic timer (plan doc §6 M5.2, §10.194/§10.195).
 *
 * The kernel is TTBR1-pure, so the GIC MMIO is reached at high-VA
 * device pages (0xFFFF000008000000 distributor, 0xFFFF000008010000
 * CPU interface) mapped by boot_arm64.S's kernel table — the same
 * attr-0/PXN device discipline as the PL011 at 0xFFFF000009000000.
 *
 * The spike pinned the two unknowns the scope flagged (§10.194): the
 * machine is GICv2 (not v3) under TCG with <= 8 CPUs, and the NS EL1
 * physical timer is PPI 14 (qemu's own FDT: /timer interrupts
 * (1, 14, 260)). Both recorded in §10.195. */
#include <stdint.h>

#include "arch/arm64/gic.h"

#define GICD_BASE  0xFFFF000008000000ULL
#define GICC_BASE  0xFFFF000008010000ULL

#define GICD_CTLR      (GICD_BASE + 0x0000)
#define GICD_IGROUPR0  (GICD_BASE + 0x0080)
#define GICD_ISENABLER0 (GICD_BASE + 0x0100)
#define GICD_ICPENDR0  (GICD_BASE + 0x0280)
#define GICC_CTLR      (GICC_BASE + 0x0000)
#define GICC_PMR       (GICC_BASE + 0x0004)
#define GICC_IAR       (GICC_BASE + 0x000C)
#define GICC_EOIR      (GICC_BASE + 0x0010)

/* The non-secure EL1 physical timer (empirically pinned, §10.195).
 * qemu's FDT says PPI 14 — that is the PPI NUMBER; a PPI's GIC INTID
 * is ppi + 16 (SGIs are 0-15, PPIs are 16-31), so the timer's INTID
 * is 30. The third M5.2 bring-up bug was enabling bit 14 (SGI 14)
 * instead of bit 30 — the GICD_ISENABLER0 readback of 0xffff was just
 * the always-enabled SGIs, and the timer IRQ never asserted (§10.195). */
#define GIC_NS_EL1_PHYS_TIMER_INTID 30u

static inline void mmio_w32(uint64_t addr, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)addr = v;
}

static inline uint32_t mmio_r32(uint64_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

void gic_init(void)
{
    /* Distributor: EnableGrp1, route the timer PPI to group 1, clear
     * any pending, enable it. (In GICv2 the PPI/SGI enables live in
     * the distributor's GICD_ISENABLER0, unlike v3's redistributor.) */
    mmio_w32(GICD_CTLR, 0x2);
    mmio_w32(GICD_IGROUPR0,
             mmio_r32(GICD_IGROUPR0) | (1u << GIC_NS_EL1_PHYS_TIMER_INTID));
    mmio_w32(GICD_ICPENDR0, (1u << GIC_NS_EL1_PHYS_TIMER_INTID));
    mmio_w32(GICD_ISENABLER0, (1u << GIC_NS_EL1_PHYS_TIMER_INTID));
    /* CPU interface: EnableGrp1 + priority mask 0xFF (allow all).
     * GICC_CTLR bit 0 is EnableGrp0 and bit 1 is EnableGrp1 — the
     * second M5.2 bring-up bug was writing 0x1 (grp0, secure) while
     * the PPI is in group 1, so the interface silently disabled the
     * interrupt (§10.195). */
    mmio_w32(GICC_CTLR, 0x2);
    mmio_w32(GICC_PMR, 0xFF);
    asm volatile("dsb sy" ::: "memory");
}

uint32_t gic_iar(void)
{
    return mmio_r32(GICC_IAR) & 0x3FF;
}

void gic_eoir(uint32_t intid)
{
    mmio_w32(GICC_EOIR, intid);
}

static uint64_t g_cntfrq;

void arm_timer_init(void)
{
    asm volatile("mrs %0, cntfrq_el0" : "=r"(g_cntfrq));
}

void arm_timer_arm(void)
{
    /* 100 ms of virtual time — the M5.2 contention probe's period
     * (§10.196): the EL0 program's window must exceed it comfortably,
     * and the [TICK 2] re-arm proof needs a real period between
     * consecutive ticks. */
    uint64_t period = g_cntfrq / 10;
    asm volatile("msr cntp_tval_el0, %0" ::"r"(period));
    asm volatile("msr cntp_ctl_el0, %0" ::"r"(1u));   /* ENABLE, IMASK=0 */
    asm volatile("isb" ::: "memory");
}
