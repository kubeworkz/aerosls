#ifndef NET_EVENT_H
#define NET_EVENT_H

#include <stdint.h>
#include "../arch/x86/lapic.h"

/*
 * Phase E — Event-Driven I/O
 *
 * Replaces the busy-spin e1000 poll loops in tcp.c with cooperative
 * halting (HLT-based yield).  The timer IRQ (IRQ0, ~100 Hz) drives
 * e1000_poll_rx() on every tick, so blocked threads wake within 10 ms
 * of packet arrival instead of burning CPU in a tight spin loop.
 *
 *   Spin-poll model  → hlt-wait model
 *   CPU utilisation:    ~100%          → <1 %  while waiting for network I/O
 *   Connection latency: microseconds   → <10 ms  (one timer period worst-case)
 */

// Navigator-Parity Gap Roadmap Phase 2: cumulative count of
// net_event_hlt_wait() calls -- every time this kernel's BSP core genuinely
// had nothing to do and yielded via HLT until the next interrupt, i.e. the
// same real yield point this file's own header comment above already
// credits with dropping CPU utilization from ~100% (spin-poll) to <1%
// (hlt-wait). Compared against kernel_tick_counter (timer.c, ~100 Hz) over
// a poll window, this gives a real, approximate CPU busy/idle measurement
// for System Health -- approximate because one hlt_wait() call can be woken
// by a non-timer interrupt and so doesn't always correspond to exactly one
// tick, and because this only measures the BSP core's own loop (Core 1's
// microkernel_service_poll() loop in kernel/smp.c never calls this and
// never idles at all -- see that file's own ap_kernel_main()), not a
// whole-system multi-core figure. Same "approximate, not exact -- name it"
// posture as AUTH_TOKEN_TTL_TICKS (kernel/auth.h).
extern volatile uint64_t cpu_idle_wait_count;

// Called from timer_irq_handler() on every IRQ0 tick.
// Drains the e1000 receive ring, dispatching any arrived frames to the
// network stack.  Safe to call from interrupt context.
void net_poll_tick(void);

// Yield the CPU until the next interrupt fires.
//
// On the BSP: STI + HLT must be adjacent — the CPU guarantees one
// instruction of interrupt shadow after STI, so HLT is entered before any
// pending interrupt fires, avoiding a lost-wakeup race. Returns after one
// interrupt (the BSP's armed LAPIC timer, ~10 ms) fires; the caller
// rechecks its condition in a loop.
//
// On an AP: HLT would sleep FOREVER. Only the BSP's LAPIC timer is armed
// (init_timer(), BSP boot); init_local_apic_registers() masks every core's
// LVT timer and nobody arms it on the APs, so an AP has no periodic
// interrupt to wake it, and a device IRQ may never be routed its way —
// caught live: the socket service runs on an AP (ap_kernel_main), and a
// tcp_connect()/tcp_recv() parked inside this wait never returned: no
// SYN-ACK timeout line, no reply to the client, and the AP's console drain
// stalled behind it. A masked LVT timer bit detects that case exactly; the
// AP yields with PAUSE instead, the caller's loop rechecks its condition,
// and the RX that satisfies it is drained by net_poll_tick() from the
// BSP's timer ISR regardless of which core spins here.
static inline void net_event_hlt_wait(void) {
    if (lapic_read(LAPIC_REG_LVT_TMR) & (1u << 16)) {
        /* No local timer (masked): HLT would never wake. */
        __asm__ volatile("pause");
        return;
    }
    cpu_idle_wait_count++;
    // STI + HLT must be adjacent: the CPU guarantees one instruction of
    // interrupt shadow after STI, so HLT is entered before any pending
    // interrupt fires — avoiding a lost-wakeup race.
    __asm__ volatile("sti; hlt");
}

#endif /* NET_EVENT_H */
