/*
 * storage_quota.c — Storage Isolation Roadmap Phase 1: per-partition
 * on-disk page quota accounting. See storage_quota.h for the full design
 * writeup and scope decisions (combined rowstore+vecstore budget, stream.c
 * deliberately excluded).
 */
#include "storage_quota.h"
#include "kernel_io.h"

static uint64_t partition_page_usage[PARTITION_MAX];
static uint64_t partition_page_quota[PARTITION_MAX];

int storage_page_reserve(uint32_t partition_id)
{
    if (partition_id >= PARTITION_MAX) return 1;   // out of range -> fail closed

    uint64_t quota = partition_page_quota[partition_id];
    if (quota != 0 && partition_page_usage[partition_id] >= quota) {
        // Over quota: fail cleanly before the caller ever advances its
        // bump-allocator cursor or touches NVMe -- same posture as every
        // other quota boundary check in this project.
        return 1;
    }

    partition_page_usage[partition_id]++;
    return 0;
}

int storage_page_release(uint32_t partition_id, uint64_t count)
{
    if (partition_id >= PARTITION_MAX) return 1;
    if (count > partition_page_usage[partition_id]) {
        partition_page_usage[partition_id] = 0;
    } else {
        partition_page_usage[partition_id] -= count;
    }
    return 0;
}

int storage_set_page_quota(uint32_t partition_id, uint64_t page_quota)
{
    if (partition_id >= PARTITION_MAX) return 1;
    partition_page_quota[partition_id] = page_quota;
    kernel_serial_printf("[STORAGE-QUOTA] partition %u: on-disk page quota set to %llu%s\n",
                         (unsigned)partition_id,
                         (unsigned long long)page_quota,
                         page_quota == 0 ? " (unlimited)" : "");
    return 0;
}

uint64_t storage_get_page_usage(uint32_t partition_id)
{
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFFFFFFFFFULL;
    return partition_page_usage[partition_id];
}

uint64_t storage_get_page_quota(uint32_t partition_id)
{
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFFFFFFFFFULL;
    return partition_page_quota[partition_id];
}

uint64_t sys_sls_partition_storage_quota_set(struct SLSPartitionStorageQuotaSetRequest* req)
{
    if (!req) return 1;
    return (uint64_t)storage_set_page_quota(req->partition_id, req->page_quota);
}

void sys_sls_partition_storage_quota_list(void)
{
    kernel_serial_print("\n[STORAGE-QUOTA] Per-partition on-disk page usage/quota (rowstore+vecstore combined):\n");
    int shown = 0;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (partition_page_usage[i] == 0 && partition_page_quota[i] == 0) continue;
        if (partition_page_quota[i] == 0) {
            kernel_serial_printf("  partition %-3u  usage=%-8llu quota=unlimited\n",
                                 (unsigned)i, (unsigned long long)partition_page_usage[i]);
        } else {
            kernel_serial_printf("  partition %-3u  usage=%-8llu quota=%llu\n",
                                 (unsigned)i, (unsigned long long)partition_page_usage[i],
                                 (unsigned long long)partition_page_quota[i]);
        }
        shown++;
    }
    kernel_serial_printf(" %d partition(s) with nonzero usage or a configured quota.\n\n", shown);
}
