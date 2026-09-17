#include "timer.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/lapic.h"
#include "net_event.h"
#include "cap.h"   /* Phase 5: cap_park_deadline_tick (weak default / process.c override) */
#include "process.h"  /* E1: proc_control_plane_tick (unified-boot yield budget) */
#include "console_service.h"  /* BSP console drain when AP is offline */

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
    /* POSIX-Environments E1: the same kind of wake for the Ring-0 control
     * plane. A unified boot's foreground loop yields the CPU to Ring-3 work
     * (kernel_yield_to_ring3) with a tick budget; this flips the parked
     * pseudo-process back to runnable once that budget expires, so the
     * schedule_ring3() call below resumes it. Pure proc_table state flip —
     * no locks, BSP only, exactly like the park-deadline wake above. On
     * every boot that is not unified there is no control plane and this is
     * one NULL-check. */
    proc_control_plane_tick();
    /* BSP-side console drain: when the AP core never comes online
     * (uniprocessor QEMU), microkernel_service_poll() never runs,
     * so the console sidecar channels must still be drained here.
     * console_service_tick takes cap spinlocks — running it directly in
     * this IRQ deadlocks when the timer fires while a process-context cap
     * op holds the same lock (the ISR spins on its own interrupted
     * holder). Latch a pending flag instead; smp_uniprocessor_tick
     * consumes it in process context. */
    console_service_irq_defer();
    /* Driver SDK: the LAPIC timer is a real device edge. When a sidecar
     * has bound vector 32 (k_irq_bind), deliver the tick as a channel
     * notification; unbound, cap_irq_notify is a silent no-op. Woken
     * drivers are picked up by isr32_stub's schedule_ring3 this tick. */
    cap_irq_notify(32);
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
