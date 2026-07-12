#ifndef NET_EVENT_H
#define NET_EVENT_H

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

// Called from timer_irq_handler() on every IRQ0 tick.
// Drains the e1000 receive ring, dispatching any arrived frames to the
// network stack.  Safe to call from interrupt context.
void net_poll_tick(void);

// Yield the CPU until the next interrupt fires.
// Enables interrupts (STI) immediately before HLT so the CPU can wake
// on the timer tick that drives net_poll_tick().  Returns after one
// interrupt fires; the caller rechecks its condition in a loop.
static inline void net_event_hlt_wait(void) {
    // STI + HLT must be adjacent: the CPU guarantees one instruction of
    // interrupt shadow after STI, so HLT is entered before any pending
    // interrupt fires — avoiding a lost-wakeup race.
    __asm__ volatile("sti; hlt");
}

#endif /* NET_EVENT_H */
