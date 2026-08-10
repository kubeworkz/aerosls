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
#define GICD_IPRIORITYR (GICD_BASE + 0x0400)   /* byte-per-INTID */
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

/* M5.3: the virtual timer (CNTV) is the NESTED interrupt source — the
 * ARM generic timer's second timer, PPI 11 -> INTID 27 (the same
 * /timer FDT node lists it; §10.197). It preempts the physical timer's
 * handler, so its GIC priority (0x00) must be HIGHER (numerically
 * lower) than the physical's (0x80). */
#define GIC_VIRT_TIMER_INTID 27u
#define GIC_PHYS_TIMER_PRIO  0x80u
#define GIC_VIRT_TIMER_PRIO  0x00u

static inline void mmio_w32(uint64_t addr, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)addr = v;
}

static inline uint32_t mmio_r32(uint64_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void gic_set_priority(uint32_t intid, uint8_t prio)
{
    /* GICD_IPRIORITYR is byte-per-INTID; do a read-modify-write on the
     * covering 32-bit register. */
    uint32_t word = mmio_r32(GICD_IPRIORITYR + (intid & ~3u));
    uint32_t shift = 8u * (intid & 3u);
    word = (word & ~(0xFFu << shift)) | ((uint32_t)prio << shift);
    mmio_w32(GICD_IPRIORITYR + (intid & ~3u), word);
}

void gic_init(void)
{
    /* Distributor: EnableGrp0, keep BOTH timer PPIs in group 0 (the
     * secure group — the default), clear any pending, enable them.
     * (In GICv2 the PPI/SGI enables live in the distributor's
     * GICD_ISENABLER0, unlike v3's redistributor.)
     *
     * Group 0 is the honest choice — the M5.3 spike proved it the hard
     * way (§10.197): this kernel boots at EL1 in the SECURE world
     * (qemu's -kernel path leaves SCR_EL3.NS=0), so all GIC MMIO
     * accesses are secure. qemu's GICv2 then HIDES group-1 interrupts
     * from the IAR read — it returns 1022 unless GICC_CTLR.AckCtl is
     * set. M5.2's "group 1" configuration never actually delivered an
     * interrupt: every IAR read returned 1022, and the gate passed
     * only because the handler re-armed the timer and the level
     * deasserted, so the cadence looked right. Group 0 needs no
     * AckCtl and matches real-hardware semantics for a secure kernel:
     * a secure access acknowledges group-0 interrupts directly. */
    mmio_w32(GICD_CTLR, 0x1);
    mmio_w32(GICD_ICPENDR0, (1u << GIC_NS_EL1_PHYS_TIMER_INTID)
                          | (1u << GIC_VIRT_TIMER_INTID));
    mmio_w32(GICD_ISENABLER0, (1u << GIC_NS_EL1_PHYS_TIMER_INTID)
                            | (1u << GIC_VIRT_TIMER_INTID));
    /* M5.3 nesting priorities: the virtual timer must preempt the
     * physical handler, so it gets the higher priority (0x00) and the
     * physical gets 0x80. GICv2 preemption: a pending interrupt with
     * priority better than the ACTIVE one's preempts it (§10.197). */
    gic_set_priority(GIC_VIRT_TIMER_INTID, GIC_VIRT_TIMER_PRIO);
    gic_set_priority(GIC_NS_EL1_PHYS_TIMER_INTID, GIC_PHYS_TIMER_PRIO);
    /* CPU interface: EnableGrp0 + priority mask 0xFF (allow all). */
    mmio_w32(GICC_CTLR, 0x1);
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

uint64_t arm_timer_cntfrq(void)
{
    return g_cntfrq;
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

void arm_vtimer_arm(void)
{
    /* M5.3 (§10.197): one-shot — a 10 ms countdown. The nested handler
     * disarms it (arm_vtimer_disarm) so it never fires twice. */
    asm volatile("msr cntv_tval_el0, %0" ::"r"(g_cntfrq / 100));
    asm volatile("msr cntv_ctl_el0, %0" ::"r"(1u));   /* ENABLE, IMASK=0 */
    asm volatile("isb" ::: "memory");
}

void arm_vtimer_disarm(void)
{
    /* M5.3: disable the virtual timer so its level deasserts BEFORE the
     * nested handler's EOIR — otherwise the line stays asserted and the
     * interrupt re-pends immediately (a storm). */
    asm volatile("msr cntv_ctl_el0, %0" ::"r"(0u));
    asm volatile("isb" ::: "memory");
}
