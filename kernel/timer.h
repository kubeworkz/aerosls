#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

extern volatile uint64_t kernel_tick_counter;

/* The length of one tick, in ONE place.
 *
 * KERNEL_TICK_NS used to be a literal `10000000ULL` carrying the comment
 * "~10 ms per tick (documented ~100 Hz)" -- a claim about the hardware that
 * nothing measured. kernel/chan.c converts nanosecond deadlines with it,
 * kernel/auth.h measures token TTL against it, and every consensus/failover
 * constant in the tree is written against it. It is now derived from the
 * rate, so the two cannot drift apart in a future edit.
 *
 * init_timer() programs the LAPIC for exactly this period by MEASURING the
 * timer's real decrement rate against the PIT (kernel/timer.c); the value
 * below is what the machine is brought into agreement with, not a guess at
 * what it happens to be. Measured before that change: 62.5 Hz (TICR=1000000,
 * divide-by-16, QEMU's 1 GHz APIC bus), further reduced to 53 ticks/s on
 * node 1 and 31 on node 3 by interrupts coalescing while the guest was busy.
 */
#define KERNEL_TICK_HZ   100u
#define KERNEL_TICK_NS   (1000000000ULL / KERNEL_TICK_HZ)   /* 10 ms per tick */

/* The reload value this file programmed before it calibrated anything, kept
 * as the honest fallback: if the LAPIC's rate cannot be measured, the timer is
 * configured exactly as it was when it was a guess, and the tick counter falls
 * back to counting interrupts. A labelled fallback that reproduces known
 * behaviour beats a fabricated rate that nobody can tell from a real one. */
#define TIMER_LEGACY_TICR 1000000u

void timer_irq_handler(void);
void kernel_sleep_ticks(uint32_t ticks);
void init_timer(void);

/* Calibration results, for boot logs and guards that want the number rather
 * than an inference drawn from behaviour:
 *   kernel_timer_tsc_hz()     measured TSC rate in Hz; 0 = not measured
 *   kernel_timer_ticr()       the LAPIC reload value actually programmed
 *   kernel_timer_calibrated() non-zero only when BOTH halves measured, i.e.
 *                             the tick counter is tracking elapsed time
 *                             rather than counting serviced interrupts.
 *                             Zero is a real, reportable state, not an error
 *                             to be papered over -- see kernel/timer.c. */
uint64_t kernel_timer_tsc_hz(void);
uint32_t kernel_timer_ticr(void);
int      kernel_timer_calibrated(void);

#endif /* TIMER_H */
