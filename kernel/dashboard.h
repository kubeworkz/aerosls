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

static inline uint64_t read_tsc(void) {
#if defined(__x86_64__)
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
#elif defined(__riscv)
    uint64_t cycles;
    __asm__ volatile("csrr %0, cycle" : "=r"(cycles));
    return cycles;
#else
    return 0;
#endif
}

void dashboard_log_fault_start(uint16_t token);
void dashboard_log_fault_end(uint16_t token);
void stream_realtime_dashboard(void);

#endif