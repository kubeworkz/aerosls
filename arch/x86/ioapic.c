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
    uint32_t rte_low  = vector;                    /* fixed, edge, unmasked */
    uint32_t rte_low_masked = vector | (1u << 16); /* mask first, then live */
    uint32_t rte_high = lapic_id << 24;

    /* Deliberately leave the legacy PIC masked (idt.c masked everything
     * at boot): on i440fx the ISA line is level-triggered, so an unmasked
     * PIC would re-deliver the same vector after every EOI while the
     * device line stays asserted (RBR unread), wedging the kernel in an
     * ISR loop and starving user code. The IO-APIC RTE below is
     * edge-triggered: it samples the rising edge once and is immune to
     * the persistent level. Real kernels keep the PIC masked once the
     * IO-APIC is active for exactly this reason. */
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1)), rte_low_masked);
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1) + 1), rte_high);
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1)), rte_low);
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
    uint32_t rte_low  = vector | (masked ? (1u << 16) : 0u);
    uint32_t rte_high = lapic_id << 24;
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1)), rte_low);
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1) + 1), rte_high);
    ioapic_write((uint8_t)(IOAPIC_REDTBL + (pin << 1)), rte_low);
}
