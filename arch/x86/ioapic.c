/* arch/x86/ioapic.c — Driver SDK ABI v0.1 s4.3: IO-APIC side of
 * cap_irq_unmask. k_irq_bind registers a vector and then calls this hook;
 * for legacy ISA lines (vectors 0x20..0x2F, the remap idt.c performs) the
 * pin is vector - 0x20 and the RTE is programmed with fixed edge delivery
 * to the BSP LAPIC, unmasked — so a real device edge (e.g. the 16550's
 * IRQ4 in loopback) reaches cap_irq_notify through the stub table.
 * The 0..4 GiB identity map (boot.asm) covers 0xFEC00000. */
#include "../../kernel/cap.h"
#include "lapic.h"
#include "../../kernel/kernel_io.h"
#include <stdint.h>

#define IOAPIC_BASE         0xFEC00000ULL
#define IOAPIC_REG_INDEX    0x00u        /* IOREGSEL */
#define IOAPIC_REG_DATA     0x10u        /* IOWIN   */
#define IOAPIC_VER          0x01u
#define IOAPIC_REDTBL       0x10u        /* first redirection entry */

static uint8_t g_ioapic_pins = 0;   /* 0 = unprobed / absent */

/* ─── IO-APIC access lock (irqsave) ─────────────────────────────────────
 * The IO-APIC register file is addressed by an INDEX register followed by
 * a DATA write — a two-store pair with no atomicity. RTE reprogramming
 * happens from three contexts here: task context (k_irq_mask, IF=1), the
 * serial device ISR (self-mask on delivery), and the AP core's timer
 * service loop. Without a lock, a timer/serial interrupt in the middle
 * of a task-context RTE write re-enters ioapic_write for a DIFFERENT
 * register; the interrupted half of the pair resumes against the wrong
 * IOREGSEL, and the write is silently lost. Lost self-mask writes are
 * not cosmetic: the emulated IO-APIC drops edge requests while a pin is
 * masked, so a driver that observes a delivery inside its masked window
 * (the irqtest stuck-driver probe's ch=leak FAIL) is direct evidence the
 * RTE never actually got masked. The lock is irqsave — cli on entry, IF
 * restored on release — so an ISR caller (already cli'd by the gate)
 * simply saves/restores a 0 and the lock can never self-deadlock. The
 * same-DSL inter-core hazard (BSP task write vs AP service write) is
 * covered by the spin; the local-IF part is what makes the ISR case
 * safe. x86_64-only TU: this file is not in the riscv/arm64 builds. */
static volatile unsigned int g_ioapic_lock = 0;

static inline unsigned long ioapic_lock_irqsave(void) {
    unsigned long flags;
    __asm__ volatile(
        "pushfq\n\tpopq %0\n\tcli"
        : "=r"(flags)
        :
        : "memory");
    while (__atomic_exchange_n(&g_ioapic_lock, 1u, __ATOMIC_ACQUIRE)) { }
    return flags;
}

static inline void ioapic_unlock_irqrestore(unsigned long flags) {
    __atomic_store_n(&g_ioapic_lock, 0u, __ATOMIC_RELEASE);
    if (flags & 0x200UL)
        __asm__ volatile("sti" ::: "memory");
}

static uint32_t ioapic_read(uint8_t reg) {
    volatile uint32_t* base = (volatile uint32_t*)(uintptr_t)IOAPIC_BASE;
    base[IOAPIC_REG_INDEX / 4] = reg;
    return base[IOAPIC_REG_DATA / 4];
}

static void ioapic_write(uint8_t reg, uint32_t val) {
    volatile uint32_t* base = (volatile uint32_t*)(uintptr_t)IOAPIC_BASE;
    base[IOAPIC_REG_INDEX / 4] = reg;
    base[IOAPIC_REG_DATA / 4] = val;
}

/* Locked RTE update: the ONLY sanctioned way to touch a redirection
 * entry. Every ioapic_write(IOAPIC_REDTBL...) caller in this file goes
 * through here; bare ioapic_write remains for the INDEX/DATA probe in
 * ioapic_init (reg selection there is never interleaved by an RTE
 * writer because those paths take this same lock for their RTE work). */
static void ioapic_rte_write(uint8_t pin, uint32_t low, uint32_t high) {
    unsigned long flags = ioapic_lock_irqsave();
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1)), low);
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1) + 1), high);
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1)), low);
    ioapic_unlock_irqrestore(flags);
}

static void ioapic_init(void) {
    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint8_t major = (uint8_t)(ver & 0xFF);
    uint8_t max_entry = (uint8_t)((ver >> 16) & 0xFF);
    if (major == 0 && max_entry == 0) {
        kernel_serial_print("[IRQ] ioapic probe: ABSENT\n");
        return;   /* no IO-APIC behind the fixed base (host tooling) */
    }
    g_ioapic_pins = (uint8_t)(max_entry + 1);
    kernel_serial_printf("[IRQ] ioapic probe: ver=0x%x pins=%u\n",
                         (unsigned)ver, (unsigned)g_ioapic_pins);
}

/* Strong override of cap.c's weak hook — only the kernel links this file. */
void cap_irq_unmask(uint32_t vector) {
    if (vector < 0x20u || vector > 0x2Fu) return;   /* legacy ISA lines only */
    if (g_ioapic_pins == 0) ioapic_init();
    if (g_ioapic_pins == 0) return;
    uint8_t pin = (uint8_t)(vector - 0x20u);
    if (pin >= g_ioapic_pins) return;

    uint32_t lapic_id = (lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu;
    /* LEVEL-triggered, active-high. The ISA device lines (the 16550's IRQ
     * is level: asserted until the FIFO is drained) paired with an
     * edge-triggered RTE lose interrupts whenever the line is already
     * asserted at unmask time — no rising edge exists to sample (caught
     * live: the phase-4 storm kills the driver mid-storm with unread RX
     * bytes; the respawned driver's rebind unmasks into the held-high
     * line and NO edge ever fires again — the boot wedges silently).
     * With the ISR's self-mask-on-delivery discipline the level RTE is
     * bounded (the mask blocks the post-EOI re-fire while the line stays
     * asserted) and every re-arm/rebind recovers an asserted line. */
    uint32_t rte_low  = vector | (1u << 15);       /* fixed, LEVEL, unmasked */
    uint32_t rte_low_masked = rte_low | (1u << 16); /* mask first, then live */
    uint32_t rte_high = lapic_id << 24;

    /* Deliberately leave the legacy PIC masked (idt.c masked everything
     * at boot): on i440fx the ISA line is level-triggered, so an unmasked
     * PIC would re-deliver the same vector after every EOI while the
     * device line stays asserted (RBR unread), wedging the kernel in an
     * ISR loop and starving user code. The IO-APIC RTE below is
     * edge-triggered: it samples the rising edge once and is immune to
     * the persistent level. Real kernels keep the PIC masked once the
     * IO-APIC is active for exactly this reason. */
    ioapic_rte_write(pin, rte_low_masked, rte_high);
    ioapic_rte_write(pin, rte_low, rte_high);
    kernel_serial_printf("[IRQ] unmask vector %u: pin %u rte 0x%08x/%08x\n",
                         (unsigned)vector, (unsigned)pin,
                         (unsigned)rte_low, (unsigned)rte_high);

}

/* Strong override of cap.c's weak hook (Driver SDK ABI v0.1 s4.5): flip
 * the RTE's masked bit. The kernel self-masks on every delivery (an edge
 * pin whose device line stays asserted otherwise re-fires after each EOI
 * in the emulated IO-APIC); the driver re-arms via SYS_IRQ_MASK once it
 * has drained the device and dropped the line. */
void cap_irq_set_mask(uint32_t vector, int masked) {
    if (vector < 0x20u || vector > 0x2Fu) return;   /* legacy ISA lines only */
    if (g_ioapic_pins == 0) ioapic_init();
    if (g_ioapic_pins == 0) return;
    uint8_t pin = (uint8_t)(vector - 0x20u);
    if (pin >= g_ioapic_pins) return;

    uint32_t lapic_id = (lapic_read(LAPIC_REG_ID) >> 24) & 0xFFu;
    /* LEVEL (see cap_irq_unmask): an unmask with the device line still
     * asserted re-delivers immediately instead of waiting for a rising
     * edge that may never come. The caller's mask bit still bounds it. */
    uint32_t rte_low  = vector | (1u << 15) | (masked ? (1u << 16) : 0u);
    uint32_t rte_high = lapic_id << 24;
    ioapic_rte_write(pin, rte_low, rte_high);
}
