#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdint.h>

struct SLSTelemetry {
    uint64_t total_page_faults;
    uint64_t total_cache_hits;
    uint64_t total_evictions;
    uint64_t average_fault_latency_cycles;
    uint64_t dynamic_pending_ios;
};

static struct SLSTelemetry global_telemetry = {0};

/* A monotonically increasing hardware counter, per architecture.
 *
 * The #else used to `return 0` on ARM64, which was not a live bug -- no file in
 * AR_C_SRC includes this header -- but was a trap: the first ARM64 source to
 * include it would have received a plausible, silent, always-zero timing
 * measurement with nothing to indicate the value was fabricated.
 *
 * That matters beyond ordinary timing. The TLS work needs jitter entropy on
 * ARM64, because no ARM core in scope has an architectural RNG instruction --
 * RNDR is ARMv8.5 (FEAT_RNG) and Cortex-A53, Cortex-A72 (Pi 4) and Neoverse N1
 * (Oracle Ampere) are all older. Jitter entropy measured with a counter that
 * always reads zero yields a perfectly uniform stream of nothing, and would
 * look exactly like a working entropy source from every angle except the one
 * that matters. See docs/AeroSLS-TLS-Design-v0.1.md §2.3.
 *
 * CNTVCT_EL0 is the architectural virtual counter, mandatory on every ARMv8
 * core, readable from EL0/EL1 without configuration. The isb() is required:
 * without it the read may be speculated and reordered against surrounding
 * work, which is fatal to a jitter measurement whose entire signal is when the
 * read happened relative to that work.
 *
 * The #else is now an #error. An architecture with no counter must fail to
 * build rather than return a number that is indistinguishable from a real one. */
static inline uint64_t read_tsc(void) {
#if defined(__x86_64__)
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
#elif defined(__aarch64__)
    uint64_t cnt;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(cnt) :: "memory");
    return cnt;
#elif defined(__riscv)
    uint64_t cycles;
    __asm__ volatile("csrr %0, cycle" : "=r"(cycles));
    return cycles;
#else
#  error "read_tsc(): no cycle counter for this architecture. Add one rather than returning 0 -- a fabricated timestamp is indistinguishable from a real one at every call site."
#endif
}

void dashboard_log_fault_start(uint16_t token);
void dashboard_log_fault_end(uint16_t token);
void stream_realtime_dashboard(void);

#endif