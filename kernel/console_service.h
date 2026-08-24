#ifndef CONSOLE_SERVICE_H
#define CONSOLE_SERVICE_H

#include <stdint.h>

/* console_service.h — kernel-side console service for wired sidecars.
 *
 * cap_create_sidecar() wires a manifest CAP_CHAN record whose peer is
 * "kernel.*" (e.g. "kernel.debug.console") so the channel's far end is
 * minted into the kernel context capability table (pid 0). This service
 * drains those endpoints: every message a sidecar sends on its console
 * channel is received here and its payload written to the serial port
 * (kernel_serial_putchar), so sidecar logs land on the same console as
 * kernel logs.
 *
 * The drain is PERIODIC, not event-driven, and deliberately runs from
 * microkernel_service_poll() (the AP core loop / uniprocessor tick) — a
 * non-IRQ kernel context. cap_lock() is a plain spinlock with no
 * interrupt masking, so it must never be acquired from
 * timer_irq_handler(); polling from this loop is the established pattern
 * for lock-taking periodic work (tier_mgr_tick, reconcile_tick). */

/* Drain every kernel-context console channel once. Call from
 * microkernel_service_poll(). */
void console_service_tick(void);

/* Total messages drained so far (diagnostics / host tests). */
uint32_t console_service_drained(void);

#endif /* CONSOLE_SERVICE_H */
