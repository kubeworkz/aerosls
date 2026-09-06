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
 * for lock-taking periodic work (tier_mgr_tick, reconcile_tick).
 *
 * The service is CLOSE-AWARE: a child's explicit k_chan_close or its
 * death (cap_table_teardown marks close_evt on the kernel end) stops the
 * drain — the child's last messages are printed, then the kernel end is
 * revoked (cap_revoke on the slot), freeing the kernel's CHAN_R/CHAN_W
 * and destroying the channel. A dead child's console channel never
 * lingers in the kernel context table. */

/* Drain every kernel-context console channel once. Call from
 * microkernel_service_poll().
 *
 * IRQ DEFERRAL: console_service_tick takes cap spinlocks, and the timer ISR
 * historically called it directly as the uniprocessor fallback — a plain
 * spinlock can deadlock when the timer fires on CPU0 while a process-context
 * cap op holds the same lock (the ISR spins on its own interrupted holder,
 * IF=0, forever; observed live as the parked machine of the e1000
 * first-nettest freeze, RIP on cap_recv_msg's xchg retry, RFLAGS=0x2).
 * timer_irq_handler therefore only LATCHES a pending flag here, and the
 * actual drain runs in process context from smp_uniprocessor_tick (the same
 * context the SMP path already uses). */
void console_service_irq_defer(void);
void console_service_tick(void);
void console_service_deferred_tick(void);

/* Total messages drained so far (diagnostics / host tests). */
uint32_t console_service_drained(void);

#endif /* CONSOLE_SERVICE_H */
