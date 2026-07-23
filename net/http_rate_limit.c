#include "http_rate_limit.h"
#include "../kernel/partition.h"
#include "../kernel/timer.h"

// ─── Multitenant Isolation Gap Analysis §5 item 4 / §7 item 1 ─────────────
// See http_rate_limit.h's own header comment for the full design writeup
// (why request-rate not connection-count, why keyed on partition_id not
// uid). This file is deliberately tiny and deliberately independent of
// net/http.c's own heavy dependency graph, so it can be linked into a
// host test for real execution.

static uint32_t partition_rate_count[PARTITION_MAX];
static uint64_t partition_rate_window_start[PARTITION_MAX];

int http_partition_rate_check(uint32_t uid) {
    uint32_t pid = partition_get_for_uid(uid);
    if (pid >= PARTITION_MAX) return 1;   // defensive only -- partition_get_for_uid() can't actually return an out-of-range id via the real assignment table; never let a bad index gate-crash the fairness check itself

    uint64_t now = kernel_tick_counter;
    if (now - partition_rate_window_start[pid] >= HTTP_PARTITION_RATE_WINDOW_TICKS) {
        partition_rate_window_start[pid] = now;
        partition_rate_count[pid] = 0;
    }
    if (partition_rate_count[pid] >= HTTP_PARTITION_RATE_LIMIT) return 0;
    partition_rate_count[pid]++;
    return 1;
}
