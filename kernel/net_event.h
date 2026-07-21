#ifndef NET_EVENT_H
#define NET_EVENT_H

#include <stdint.h>

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
// Enables interrupts (STI) immediately before HLT so the CPU can wake
// on the timer tick that drives net_poll_tick().  Returns after one
// interrupt fires; the caller rechecks its condition in a loop.
static inline void net_event_hlt_wait(void) {
    cpu_idle_wait_count++;
    // STI + HLT must be adjacent: the CPU guarantees one instruction of
    // interrupt shadow after STI, so HLT is entered before any pending
    // interrupt fires — avoiding a lost-wakeup race.
    __asm__ volatile("sti; hlt");
}

#endif /* NET_EVENT_H */
