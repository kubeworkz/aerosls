#include <stdint.h>
#include "plic.h"

/* QEMU virt PLIC, PHYSICAL MMIO. This kernel runs with paging Bare (satp
 * is never written), so every address here is a physical address — the
 * original design draft's PLIC_BASE_VIRT 0xFFFFFFFF40003000 (a "mapped
 * virtual memory window" address that exists in neither QEMU virt's
 * memory map nor this kernel) was wrong and, being never-called code,
 * never caught: nothing invoked these functions until Phase 9i wired
 * them at boot. The real virt PLIC sits at physical 0x0c000000. */
#define PLIC_BASE_VIRT        0x0c000000UL
#define PLIC_PRIORITY_BASE    0x0000
#define PLIC_ENABLE_BASE      0x2000
#define PLIC_THRESHOLD_BASE   0x200000
#define PLIC_CLAIM_BASE       0x200004

/* UART Peripheral IRQ number on the QEMU Virt machine */
#define UART0_IRQ 10

/* The PLIC's source-10 device: the 16550 UART at physical 0x10000000.
 * The UART's own interrupt-enable register (IER) decides whether an
 * incoming byte actually asserts the interrupt line — the PLIC can only
 * deliver a line that exists. Bit 0 (ERBI) enables the receiver-buffer
 * interrupt; transmit stays polled. */
#define VIRT_UART_BASE 0x10000000UL
#define UART_IER_OFFSET 1
#define UART_IER_RX_ENABLE 0x01

static inline uint32_t plic_read(uint32_t offset) {
    return *(volatile uint32_t*)(PLIC_BASE_VIRT + offset);
}

static inline void plic_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(PLIC_BASE_VIRT + offset) = (value);
}

/* The PLIC context whose interrupt line reaches the mode this build runs
 * in: QEMU virt numbers hart N's contexts 2N+0 (M-mode) and 2N+1
 * (S-mode). The variant is baked at compile time like every other mode
 * switch in this tree (sbi.c's console, trap_riscv.c's vector) — the
 * S-mode build (no RISCV_MMODE) routes source 10 to its S-mode context
 * and enables sie.SEIE; the bare-metal M-mode build routes it to the
 * M-mode context and enables mie.MEIE. */
static inline uint32_t plic_context(uint32_t target_hart_id) {
#if defined(RISCV_MMODE)
    return target_hart_id * 2 + 0;
#else
    return target_hart_id * 2 + 1;
#endif
}

void init_riscv_plic(uint32_t target_hart_id) {
    uint32_t ctx = plic_context(target_hart_id);

    // 1. Set the priority of the UART interrupt vector line.
    // Priorities range from 0 (disabled) to 7 (highest). We assign a strong 5.
    plic_write(PLIC_PRIORITY_BASE + (UART0_IRQ * 4), 5);

    // 2. Enable UART IRQ 10 for the target Hart's mode context.
    // The enable register for context C is PLIC_ENABLE_BASE + C * 0x80.
    uint32_t enable_offset = PLIC_ENABLE_BASE + (ctx * 0x80);
    uint32_t current_mask = plic_read(enable_offset);
    current_mask |= (1 << UART0_IRQ); // Set bit 10 to unmask the line
    plic_write(enable_offset, current_mask);

    // 3. Set the mode-context Priority Threshold for this Hart.
    // The PLIC filters out any interrupts with priority <= this threshold;
    // 0 lets every priority > 0 source through.
    uint32_t threshold_offset = PLIC_THRESHOLD_BASE + (ctx * 0x1000);
    plic_write(threshold_offset, 0);

    // 4. Enable the local external-interrupt bit for the running mode:
    // sie.SEIE (bit 9) under S-mode, mie.MEIE (bit 11) under M-mode.
#if defined(RISCV_MMODE)
    uint64_t mie_val;
    __asm__ volatile("csrr %0, mie" : "=r"(mie_val));
    mie_val |= (1ULL << 11); // MEIE
    __asm__ volatile("csrw mie, %0" : : "r"(mie_val));
#else
    uint64_t sie_val;
    __asm__ volatile("csrr %0, sie" : "=r"(sie_val));
    sie_val |= (1ULL << 9); // SEIE
    __asm__ volatile("csrw sie, %0" : : "r"(sie_val));
#endif

    // 5. Make the device actually raise its line: enable the UART's
    // receiver interrupt (IER bit 0). Steps 1-4 route an asserted line to
    // the mode's context; without this step no byte ever asserts it.
    volatile uint8_t* ier = (volatile uint8_t*)(VIRT_UART_BASE + UART_IER_OFFSET);
    *ier = (uint8_t)(*ier | UART_IER_RX_ENABLE);
}

// Polling/Acknowledgment router handler run inside 'handle_riscv_supervisor_interrupt'
uint32_t plic_claim_interrupt(uint32_t target_hart_id) {
    uint32_t ctx = plic_context(target_hart_id);
    uint32_t claim_offset = PLIC_CLAIM_BASE + (ctx * 0x1000);
    return plic_read(claim_offset); // Returns the active IRQ number (e.g., 10)
}

void plic_complete_interrupt(uint32_t target_hart_id, uint32_t irq) {
    uint32_t ctx = plic_context(target_hart_id);
    uint32_t claim_offset = PLIC_CLAIM_BASE + (ctx * 0x1000);
    plic_write(claim_offset, irq); // Signal the PLIC hardware that we processed the IRQ
}