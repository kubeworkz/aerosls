/*
 * usage_metering.c -- Multitenant Isolation Gap Analysis §5 item 6 / §7
 * item 6 implementation. See usage_metering.h for the full design writeup.
 */
#include "usage_metering.h"
#include "partition.h"
#include "frame_pool.h"
#include "timer.h"       // kernel_tick_counter
#include "kernel_io.h"

struct SLSUsageEntry usage_table[PARTITION_MAX];

// ─── usage_metering_record_request ──────────────────────────────────────
void usage_metering_record_request(uint32_t uid) {
    uint32_t partition_id = partition_get_for_uid(uid);
    if (partition_id >= PARTITION_MAX) return;   // defensive; partition_get_for_uid() never actually returns out-of-range
    usage_table[partition_id].http_requests_total++;
}

// ─── usage_metering_tick ─────────────────────────────────────────────────
static uint32_t usage_tick_counter = 0;

void usage_metering_tick(void) {
    usage_tick_counter++;
    if (usage_tick_counter % USAGE_METER_TICK_GATE != 0) return;

    uint64_t now = kernel_tick_counter;

    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (!partition_table[i].active) continue;
        struct SLSUsageEntry* u = &usage_table[i];

        if (!u->initialized) {
            // First observation for this partition -- seed the clock, don't
            // charge a bogus jump for time before this partition was ever
            // sampled (see header comment).
            u->last_sample_tick = now;
            u->initialized = 1;
            continue;
        }

        uint64_t elapsed = now - u->last_sample_tick;
        uint64_t frame_usage = partition_get_frame_usage(partition_table[i].partition_id);
        u->frame_ticks_total += frame_usage * elapsed;
        u->last_sample_tick = now;
    }
}

// ─── Read accessors ──────────────────────────────────────────────────────
uint64_t usage_metering_get_requests(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0;
    return usage_table[partition_id].http_requests_total;
}

uint64_t usage_metering_get_frame_ticks(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0;
    return usage_table[partition_id].frame_ticks_total;
}

// ─── usage_metering_list ─────────────────────────────────────────────────
void usage_metering_list(void) {
    kernel_serial_printf(
        "\n[USAGE] Per-Partition Usage Report (cumulative since boot)\n"
        " %-6s %-16s %14s %16s %14s\n",
        "PART", "NAME", "HTTP_REQUESTS", "FRAME_TICKS", "FRAMES_NOW");
    int shown = 0;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (!partition_table[i].active) continue;
        struct SLSUsageEntry* u = &usage_table[i];
        uint64_t frames_now = partition_get_frame_usage(partition_table[i].partition_id);
        kernel_serial_printf(" %-6u %-16s %14llu %16llu %14llu\n",
                             partition_table[i].partition_id,
                             partition_table[i].name,
                             (unsigned long long)u->http_requests_total,
                             (unsigned long long)u->frame_ticks_total,
                             (unsigned long long)frames_now);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no active partitions)\n");
    kernel_serial_printf(" %d partition(s).\n\n", shown);
}

// ─── Syscall wrapper ─────────────────────────────────────────────────────
void sys_sls_usage_report(void) {
    usage_metering_list();
}
