/*
 * smp_uniprocessor_host_test.c — running on one CPU.
 *
 * ─── What broke, and what this pins ───────────────────────────────────────
 * boot_application_processors() waited for the AP with an unbounded
 * `while (ap_bootstrap_lock == 0) pause;`. On `-smp 1` no APIC id 1 exists
 * to answer the SIPI, so the kernel hung there forever, silently, before
 * any subsystem had started. Scenario 1 is that: the wait must END.
 *
 * The rest is the consequence. With no AP, nobody runs flush_daemon_tick()
 * or microkernel_service_poll() -- which is where reconcile_tick() lives --
 * so the BSP has to. The two properties that matter:
 *
 *   - it runs when there is NO AP        (scenario 2)
 *   - it does NOT run when there IS one  (scenario 3)
 *
 * The second is the one worth having a test for. A fallback that keeps
 * firing after the real driver appears means two cores calling
 * reconcile_tick() concurrently, which is the exact race the AP/BSP split
 * and the SPSC intent ring were built to avoid.
 *
 * ─── What is real here, and what is not ───────────────────────────────────
 * smp_ap_online() and smp_uniprocessor_tick() are the REAL ones, linked
 * from kernel/smp.c -- they are where every decision above lives, so a copy
 * in this file that drifted out of step would be worse than no test.
 *
 * boot_application_processors() is NOT called. It writes the AP trampoline
 * to physical 0x08000, which a host process cannot map (below
 * mmap_min_addr), reads CR3, and pokes the LAPIC ICR. Scenario 1 therefore
 * reproduces its wait loop against the same constant and the same clock
 * rather than executing it. Named rather than glossed: this proves the
 * deadline arithmetic terminates, not that the kernel boots on one CPU.
 * Only a real single-core boot shows that.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -o /tmp/smp_uniprocessor_host_test \
 *       tests/smp_uniprocessor_host_test.c kernel/smp.c
 *   /tmp/smp_uniprocessor_host_test
 */
#include "kernel/smp.h"
#include <stdio.h>
#include <stdint.h>

/* smp.c's AP tick calls into the QEMU-SLS PGO scanner. Stubbed rather than
 * linked: kernel/qemu_sls_pgo.c pulls in kernel/qemu_sls_tcache.c and onward,
 * and this file is about what the tick loop does with ONE cpu, not about
 * profile-guided optimisation. The stub counts calls so "the AP tick ran" stays
 * observable -- a stub that silently did nothing would let a tick loop that
 * never fires look identical to one that does. */
static int g_pgo_scan_ticks = 0;
void qemu_sls_pgo_scan_tick(void) { g_pgo_scan_ticks++; }

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── what kernel/smp.c needs in order to link ─────────────────────────── */
volatile uint64_t kernel_tick_counter = 0;
extern volatile uint32_t ap_bootstrap_lock;   /* defined in smp.c */

/* The two pieces of work the fallback drives. Counting stubs -- the whole
 * question this file asks is "who calls these, and how often". */
static int flush_calls = 0;
static int poll_calls  = 0;
void flush_daemon_tick(void)        { flush_calls++; }
void microkernel_service_poll(void) { poll_calls++;  }

/* Referenced by boot_application_processors()/ap_kernel_main(), neither of
 * which this test calls -- but the symbols must resolve. Real storage for
 * the trampoline bounds so the pointer arithmetic in smp.c stays defined. */
uint8_t  trampoline_start[16];
uint8_t  trampoline_end[1];
uint64_t gdt_ptr = 0;
void* allocate_physical_ram_frame(void) { return NULL; }
void  init_local_apic_registers(void) { }
void  lapic_write(uint32_t reg, uint32_t value) { (void)reg; (void)value; }
void  kernel_sleep_ticks(uint32_t ticks) { kernel_tick_counter += ticks; }
void  kernel_serial_print(const char* s) { (void)s; }
void  kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* The wait loop from boot_application_processors(), minus the hardware --
 * see the header note on why this one is reproduced rather than linked. */
static int ap_wait_terminates(uint32_t max_spins, uint32_t* spins_used) {
    uint64_t deadline = kernel_tick_counter + AP_BOOT_TIMEOUT_TICKS;
    uint32_t n = 0;
    while (ap_bootstrap_lock == 0) {
        if (kernel_tick_counter >= deadline) { *spins_used = n; return 0; }
        kernel_tick_counter++;            /* the timer IRQ still fires */
        if (++n > max_spins) { *spins_used = n; return -1; }   /* hung */
    }
    *spins_used = n;
    return 1;
}

int main(void) {
    printf("=== Uniprocessor operation (timeout %u ticks, cadence %u) ===\n\n",
           (unsigned)AP_BOOT_TIMEOUT_TICKS, (unsigned)SMP_UNI_TICK_INTERVAL);

    /* ═══ 1: the wait must end ════════════════════════════════════════════ */
    printf("-- 1: no AP answers --\n");
    {
        ap_bootstrap_lock = 0;
        kernel_tick_counter = 0;
        uint32_t spins = 0;
        int r = ap_wait_terminates(1000000, &spins);
        CHECK(r != -1, "*** the wait TERMINATES -- it used to spin forever ***");
        CHECK(r == 0, "...reporting that no AP came up, rather than claiming one did");
        CHECK(spins <= AP_BOOT_TIMEOUT_TICKS + 1,
              "...within the documented deadline, not eventually");
    }

    /* ═══ 2: an AP that does answer is still waited for ═══════════════════ */
    printf("\n-- 2: an AP answers in time --\n");
    {
        ap_bootstrap_lock = 1;            /* already up when we look */
        kernel_tick_counter = 0;
        uint32_t spins = 0;
        CHECK(ap_wait_terminates(1000, &spins) == 1,
              "*** an AP that is up is reported as up ***");
        CHECK(spins == 0, "...and is not waited on needlessly");
    }

    /* ═══ 3: the fallback runs only without an AP ═════════════════════════ */
    printf("\n-- 3: who drives the service loop --\n");
    {
        ap_bootstrap_lock = 0;
        kernel_tick_counter = 10000;
        flush_calls = poll_calls = 0;

        smp_uniprocessor_tick();
        CHECK(flush_calls == 1 && poll_calls == 1,
              "*** with no AP, the BSP runs the flush daemon and service poll ***");

        /* Rate limiting: calling it every sweep of a 100 Hz loop must not
         * run the work every sweep. */
        for (int i = 0; i < 50; i++) smp_uniprocessor_tick();
        CHECK(poll_calls == 1,
              "*** 50 more calls in the same tick do NOT re-run it ***");

        kernel_tick_counter += SMP_UNI_TICK_INTERVAL - 1;
        smp_uniprocessor_tick();
        CHECK(poll_calls == 1, "one tick short of the interval, still not due");

        kernel_tick_counter += 1;
        smp_uniprocessor_tick();
        CHECK(poll_calls == 2, "*** at the interval it runs again ***");
    }

    /* ═══ 4: an AP taking over stops the fallback ═════════════════════════ */
    printf("\n-- 4: a late AP takes the work back --\n");
    {
        ap_bootstrap_lock = 0;
        kernel_tick_counter += 1000;
        flush_calls = poll_calls = 0;
        smp_uniprocessor_tick();
        CHECK(poll_calls == 1, "the BSP is driving");

        /* An AP that answered after the timeout starts its own loop. If the
         * BSP kept going too, both cores would call reconcile_tick() -- the
         * precise race the AP/BSP split exists to prevent. */
        ap_bootstrap_lock = 1;
        kernel_tick_counter += 10000;
        for (int i = 0; i < 100; i++) smp_uniprocessor_tick();
        CHECK(poll_calls == 1,
              "*** once an AP is online the BSP STOPS -- never two drivers ***");
        CHECK(flush_calls == 1, "...for the flush daemon too");

        /* And it is read live, so it can hand back again. */
        ap_bootstrap_lock = 0;
        kernel_tick_counter += 10000;
        smp_uniprocessor_tick();
        CHECK(poll_calls == 2,
              "*** ...and resumes if the AP goes away -- checked live, not latched ***");
    }

    /* ═══ 5: the constants are sane relative to each other ════════════════ */
    printf("\n-- 5: constants --\n");
    {
        CHECK(AP_BOOT_TIMEOUT_TICKS > SMP_UNI_TICK_INTERVAL,
              "the boot deadline is longer than one service interval");
        CHECK(AP_BOOT_TIMEOUT_TICKS >= 100,
              "*** >= ~1 s at 100 Hz -- INIT/SIPI answers in microseconds, so a "
              "shorter deadline would risk calling a live AP dead ***");
        CHECK(SMP_UNI_TICK_INTERVAL == 10,
              "the fallback cadence matches the AP's own kernel_sleep_ticks(10)");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
