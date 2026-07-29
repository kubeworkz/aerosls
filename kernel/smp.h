/*
 * smp.h — application-processor bring-up, and running without one.
 *
 * ─── Why uniprocessor support exists ──────────────────────────────────────
 * Two reasons, one correctness and one capacity.
 *
 * Correctness: boot_application_processors() used to wait for the AP with
 *
 *     while (ap_bootstrap_lock == 0) { __asm__ volatile("pause"); }
 *
 * an unbounded spin. Booted with `-smp 1` there is no APIC id 1 to answer
 * the SIPI, so the flag never set and the kernel hung at boot with no
 * message. Not "degraded on one core" -- dead.
 *
 * Capacity: the AP's loop never idles. It runs
 * flush_daemon_tick()/microkernel_service_poll() and then spins in
 * kernel_sleep_ticks(), which is a `pause` loop and not a `hlt` -- so every
 * node consumed a full host core whether or not it had anything to do,
 * while the BSP's HTTP loop was already yielding properly through
 * net_event_hlt_wait(). On a virtualised host that made node density
 * CPU-bound rather than memory-bound: roughly (cores - 1) nodes, no matter
 * how much RAM was free. Running single-core removes the spinning half
 * entirely, and an idle node then costs almost nothing.
 * See docs/AeroSLS-N-Node-Launcher-Plan-v0.1.md §2.
 *
 * ─── How the work still gets done ─────────────────────────────────────────
 * The AP was not idle-spinning for fun; it drove the only loop that ticked
 * reliably. With no AP the BSP has to drive that work itself, from its own
 * idle points -- the HTTP server's sweep, and the shell's input poll.
 * smp_uniprocessor_tick() is that, and it is a no-op the moment an AP is
 * online, so this is a fallback and never a second driver.
 */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>

/* How long to wait for an AP to answer the SIPI before concluding there
 * isn't one. ~1 s at the ~100 Hz LAPIC tick -- INIT/SIPI answers in
 * microseconds on real hardware and under QEMU, so this is generous by
 * three orders of magnitude and still bounded. */
#define AP_BOOT_TIMEOUT_TICKS 100u

/* Cadence for smp_uniprocessor_tick()'s work, matching the AP's own
 * kernel_sleep_ticks(10) so single-core behaviour is not a different
 * schedule, just a different core. */
#define SMP_UNI_TICK_INTERVAL 10u

/*
 * Start the AP at `target_apic_id`.
 *
 * Returns 1 if it came up, 0 if it did not answer within
 * AP_BOOT_TIMEOUT_TICKS. Returning rather than hanging is the point: a
 * single-CPU machine is a supported configuration, not a fault.
 */
int boot_application_processors(uint8_t target_apic_id);

/* Whether an AP is currently running its service loop. Read live rather
 * than latched at boot, so a genuinely slow AP that answers after the
 * timeout still takes the work back and the BSP stops duplicating it. */
int smp_ap_online(void);

/*
 * Run the AP's periodic work from the BSP, when and only when there is no
 * AP. Call from the BSP's idle points; it rate-limits itself to
 * SMP_UNI_TICK_INTERVAL and returns immediately if an AP is online.
 *
 * Safe on the BSP -- safer, in fact. reconcile_tick() is documented as
 * queueing persist work for the BSP rather than calling persist_*()
 * directly, because it normally runs on the AP where that would be unsafe
 * (kernel/workload.h). Running it here means producer and consumer are the
 * same core: the SPSC ring degenerates to a plain queue and the race it
 * exists to avoid cannot occur.
 */
void smp_uniprocessor_tick(void);

#endif /* SMP_H */
