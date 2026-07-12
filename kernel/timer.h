#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

extern volatile uint64_t kernel_tick_counter;

void timer_irq_handler(void);
void kernel_sleep_ticks(uint32_t ticks);
void init_timer(void);

#endif /* TIMER_H */
