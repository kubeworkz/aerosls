#include "timer.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/lapic.h"
#include "net_event.h"
#include "cap.h"   /* Phase 5: cap_park_deadline_tick (weak default / process.c override) */

volatile uint64_t kernel_tick_counter = 0;

void timer_irq_handler(void) {
    kernel_tick_counter++;
    net_poll_tick();
    /* Phase 5 channel transport: wake any process parked in k_chan_wait /
     * k_chan_send whose deadline has passed (cap_park_deadline_tick, the
     * strong override in process.c; weak no-op elsewhere). Runs here — in
     * the ISR, BEFORE isr32_stub's schedule_ring3 call — so a woken
     * process is schedulable this very tick (~10 ms granularity, matching
     * KERNEL_TICK_NS). This is a pure proc_table state flip (BLOCKED →
     * SUSPENDED + resume_kernel), takes NO cap locks, and runs on the BSP
     * only (init_timer is BSP-only; the AP cores busy-wait on
     * kernel_sleep_ticks) — the same CPU that schedule_ring3 scans, so
     * there is no cross-CPU race (unlike the AP core's console-service
     * wake, which is pre-existing and separate). */
    cap_park_deadline_tick();
    lapic_write(LAPIC_REG_EOI, 0);
}

void kernel_sleep_ticks(uint32_t ticks) {
    uint64_t target = kernel_tick_counter + ticks;
    while (kernel_tick_counter < target)
        __asm__ volatile("pause");
}

#include "../arch/x86/isr_stubs.h"

void init_timer(void) {
    // Register ISR32 in the IDT
    set_idt_gate(32, (uint64_t)isr32_stub, 0x8E);

    // Configure the Local APIC timer for periodic interrupts at ~100 Hz.
    // Divide-by-16, initial count 1,000,000 (fires roughly every 10 ms at
    // a ~1.6 GHz effective LAPIC clock; exact rate is calibration-dependent).
    lapic_write(LAPIC_REG_TDCR, 0x03);          // divide by 16
    lapic_write(LAPIC_REG_TICR, 1000000);        // initial count
    lapic_write(LAPIC_REG_LVT_TMR,               // periodic, vector 32
                0x20 | (1U << 17));
}
