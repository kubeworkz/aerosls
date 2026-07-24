/*
 * tcp_quota.c -- Multitenant Isolation Gap Analysis §5 item 4 / §7 item 1,
 * Network Fairness Phase 2: per-partition concurrent inbound connection
 * quotas. See tcp_quota.h for the full design writeup.
 */
#include "tcp_quota.h"
#include "tcp.h"
#include "../kernel/partition.h"
#include "../kernel/kernel_io.h"

// Indexed by inbound conn_id ([0, TCP_INBOUND_MAX_CONNS) -- see tcp.h).
// Sized to the full TCP_MAX_CONNS range defensively (a conn_id passed in
// out of range is simply bounds-checked and rejected below), but only
// indices below TCP_INBOUND_MAX_CONNS are ever actually written, since
// only inbound connections have a partition to attribute.
static uint32_t tcp_conn_partition[TCP_MAX_CONNS];
static uint16_t partition_conn_count[PARTITION_MAX];
static uint16_t partition_conn_quota[PARTITION_MAX];

void tcp_quota_init(void) {
    // Every slot starts unattributed. See tcp_quota.h's own comment on why
    // this can't just rely on BSS zero-init the way partition_conn_count[]/
    // partition_conn_quota[] safely do.
    for (int i = 0; i < TCP_INBOUND_MAX_CONNS; i++) {
        tcp_conn_partition[i] = TCP_CONN_PARTITION_NONE;
    }
}

int tcp_conn_attribute(int conn_id, uint32_t uid) {
    if (conn_id < 0 || conn_id >= TCP_INBOUND_MAX_CONNS) return 0;

    // Idempotent: once attributed, every subsequent sweep's call for the
    // same still-open connection is a no-op success, exactly like
    // http_partition_rate_check() being re-invoked per request -- callers
    // are expected to call this repeatedly until it returns 1 or the
    // connection is torn down.
    if (tcp_conn_partition[conn_id] != TCP_CONN_PARTITION_NONE) return 1;

    uint32_t pid = partition_get_for_uid(uid);
    if (pid >= PARTITION_MAX) return 0;   // defensive only, mirrors http_rate_limit.c's own guard

    uint16_t quota = partition_conn_quota[pid];
    if (quota != 0 && partition_conn_count[pid] >= quota) {
        // Over quota: deny before attributing -- same "denial happens
        // before any side effect" posture as storage_page_reserve().
        return 0;
    }

    tcp_conn_partition[conn_id] = pid;
    partition_conn_count[pid]++;
    return 1;
}

void tcp_conn_release(int conn_id) {
    if (conn_id < 0 || conn_id >= TCP_INBOUND_MAX_CONNS) return;
    uint32_t pid = tcp_conn_partition[conn_id];
    if (pid == TCP_CONN_PARTITION_NONE) return;   // never attributed -- safe no-op

    if (pid < PARTITION_MAX) {
        if (partition_conn_count[pid] > 0) partition_conn_count[pid]--;
    }
    tcp_conn_partition[conn_id] = TCP_CONN_PARTITION_NONE;
}

int tcp_partition_set_conn_quota(uint32_t partition_id, uint16_t quota) {
    if (partition_id >= PARTITION_MAX) return 1;
    partition_conn_quota[partition_id] = quota;
    kernel_serial_printf("[TCP-QUOTA] partition %u: concurrent inbound connection quota set to %u%s\n",
                         (unsigned)partition_id, (unsigned)quota,
                         quota == 0 ? " (unlimited)" : "");
    return 0;
}

uint16_t tcp_partition_get_conn_usage(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0xFFFFu;
    return partition_conn_count[partition_id];
}

uint16_t tcp_partition_get_conn_quota(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0xFFFFu;
    return partition_conn_quota[partition_id];
}

uint64_t sys_sls_partition_conn_quota_set(struct SLSPartitionConnQuotaSetRequest* req) {
    if (!req) return 1;
    return (uint64_t)tcp_partition_set_conn_quota(req->partition_id, req->quota);
}

void sys_sls_partition_conn_quota_list(void) {
    kernel_serial_print("\n[TCP-QUOTA] Per-partition concurrent inbound connection usage/quota:\n");
    int shown = 0;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (partition_conn_count[i] == 0 && partition_conn_quota[i] == 0) continue;
        if (partition_conn_quota[i] == 0) {
            kernel_serial_printf("  partition %-3u  usage=%-5u quota=unlimited\n",
                                 (unsigned)i, (unsigned)partition_conn_count[i]);
        } else {
            kernel_serial_printf("  partition %-3u  usage=%-5u quota=%u\n",
                                 (unsigned)i, (unsigned)partition_conn_count[i],
                                 (unsigned)partition_conn_quota[i]);
        }
        shown++;
    }
    kernel_serial_printf(" %d partition(s) with nonzero usage or a configured quota.\n\n", shown);
}
