#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

extern volatile uint64_t kernel_tick_counter;

/* Documented tick rate of the LAPIC periodic timer (init_timer's own
 * comment: "fires roughly every 10 ms at a ~1.6 GHz effective LAPIC
 * clock; exact rate is calibration-dependent"). The Phase 5 channel
 * transport converts k_chan_wait/k_chan_send deadline nanoseconds to
 * ticks with this constant (rounded UP), so a deadline is met within one
 * tick period of its target — the honest granularity of a tick-driven
 * kernel (see the chan.c header's honest limits). */
#define KERNEL_TICK_NS 10000000ULL   /* ~10 ms per tick (documented ~100 Hz) */

void timer_irq_handler(void);
void kernel_sleep_ticks(uint32_t ticks);
void init_timer(void);

#endif /* TIMER_H */
