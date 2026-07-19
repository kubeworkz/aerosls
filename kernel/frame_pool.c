/*
 * frame_pool.c — physical frame bitmap allocator. Phase 13 (LPAR) adds
 * per-partition usage accounting and quota enforcement on top of the
 * original, unchanged bitmap scan — see frame_pool.h for the API split
 * and design rationale.
 */
#include "frame_pool.h"
#include "kernel_io.h"
#include "../include/sls_mmu.h"
#include <stddef.h>

#define TOTAL_FRAMES 1048576

static uint64_t physical_memory_bitmap[TOTAL_FRAMES / 64];

/* Phase 13: per-partition frame accounting. PARTITION_SYSTEM's counter also
 * absorbs every unaccounted/kernel-infrastructure frame handed out via the
 * plain allocate_physical_ram_frame() path (page-table internals, NVMe
 * queues, SMP stacks, the shared TIMI activation cache, catalog index
 * nodes) — so partition_frame_usage[PARTITION_SYSTEM] is deliberately a
 * "total unaccounted + system tenant" number, not a pure per-tenant one.
 * See the Phase 13 findings addendum for the full call-site audit. Quota
 * defaults to 0 (BSS zero-init) = unlimited, same backward-compatible-by-
 * construction discipline as every prior LPAR phase. */
static uint64_t partition_frame_usage[PARTITION_MAX];
static uint64_t partition_frame_quota[PARTITION_MAX];

static void *alloc_raw_frame(void)
{
    // Start at frame 1 (skip frame 0: address 0x0 == NULL in C)
    for (size_t i = 0; i < (TOTAL_FRAMES / 64); i++)
    {
        if (physical_memory_bitmap[i] != 0xFFFFFFFFFFFFFFFFULL)
        {
            for (int bit = 0; bit < 64; bit++)
            {
                // Skip the very first frame (frame 0 = address 0x0 = NULL)
                if (i == 0 && bit == 0) continue;
                if (!(physical_memory_bitmap[i] & (1ULL << bit)))
                {
                    physical_memory_bitmap[i] |= (1ULL << bit);
                    return (void *)(((i * 64) + bit) * 4096);
                }
            }
        }
    }
    return 0;
}

void *allocate_physical_ram_frame(void)
{
    void *frame = alloc_raw_frame();
    if (frame) partition_frame_usage[PARTITION_SYSTEM]++;
    return frame;
}

void *allocate_physical_ram_frame_for_partition(uint32_t partition_id)
{
    if (partition_id >= PARTITION_MAX) return 0;   // out of range -> fail closed

    uint64_t quota = partition_frame_quota[partition_id];
    if (quota != 0 && partition_frame_usage[partition_id] >= quota) {
        // Over quota: fail cleanly before touching the bitmap at all —
        // no partial allocation, same posture as every other LPAR boundary
        // check in this project (denial happens before any side effect).
        return 0;
    }

    void *frame = alloc_raw_frame();
    if (frame) partition_frame_usage[partition_id]++;
    return frame;
}

// Gap Remediation Phase F: shared validation + bitmap-clear for both free
// entry points below. Returns 1 (failure, bitmap untouched) if addr isn't a
// currently-allocated, in-range, page-aligned, non-zero frame address --
// see frame_pool.h's own comment on free_physical_ram_frame() for the full
// rationale. Returns 0 and clears the bit on success.
static int free_raw_frame(void* addr) {
    uint64_t a = (uint64_t)(uintptr_t)addr;
    if (a == 0 || (a % FRAME_SIZE) != 0) return 1;          // NULL or misaligned
    uint64_t frame_index = a / FRAME_SIZE;
    if (frame_index == 0 || frame_index >= TOTAL_FRAMES) return 1;  // out of range
    size_t word = frame_index / 64;
    int    bit  = (int)(frame_index % 64);
    if (!(physical_memory_bitmap[word] & (1ULL << bit))) return 1;  // not allocated -- double free or bogus
    physical_memory_bitmap[word] &= ~(1ULL << bit);
    return 0;
}

int free_physical_ram_frame(void* frame) {
    if (free_raw_frame(frame)) return 1;
    if (partition_frame_usage[PARTITION_SYSTEM] > 0) partition_frame_usage[PARTITION_SYSTEM]--;
    return 0;
}

int free_physical_ram_frame_for_partition(void* frame, uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 1;   // out of range -> fail closed, bitmap untouched
    if (free_raw_frame(frame)) return 1;
    if (partition_frame_usage[partition_id] > 0) partition_frame_usage[partition_id]--;
    return 0;
}

int partition_set_frame_quota(uint32_t partition_id, uint64_t frame_quota)
{
    if (partition_id >= PARTITION_MAX) return 1;
    partition_frame_quota[partition_id] = frame_quota;
    kernel_serial_printf("[QUOTA] partition %u: frame quota set to %llu%s\n",
                         (unsigned)partition_id,
                         (unsigned long long)frame_quota,
                         frame_quota == 0 ? " (unlimited)" : "");
    return 0;
}

uint64_t partition_get_frame_usage(uint32_t partition_id)
{
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFFFFFFFFFULL;
    return partition_frame_usage[partition_id];
}

uint64_t partition_get_frame_quota(uint32_t partition_id)
{
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFFFFFFFFFULL;
    return partition_frame_quota[partition_id];
}

int partition_reset_frame_usage(uint32_t partition_id)
{
    if (partition_id >= PARTITION_MAX) return 1;
    partition_frame_usage[partition_id] = 0;
    kernel_serial_printf("[QUOTA] partition %u: usage counter reset to 0 "
                         "(accounting only -- see Phase 14 findings for why "
                         "this does not reclaim physical frames).\n",
                         (unsigned)partition_id);
    return 0;
}

uint64_t sys_sls_partition_quota_set(struct SLSPartitionQuotaSetRequest* req)
{
    if (!req) return 1;
    return (uint64_t)partition_set_frame_quota(req->partition_id, req->frame_quota);
}

void sys_sls_partition_quota_list(void)
{
    kernel_serial_print("\n[QUOTA] Per-partition frame usage/quota:\n");
    int shown = 0;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (partition_frame_usage[i] == 0 && partition_frame_quota[i] == 0) continue;
        if (partition_frame_quota[i] == 0) {
            kernel_serial_printf("  partition %-3u  usage=%-8llu quota=unlimited\n",
                                 (unsigned)i, (unsigned long long)partition_frame_usage[i]);
        } else {
            kernel_serial_printf("  partition %-3u  usage=%-8llu quota=%llu\n",
                                 (unsigned)i, (unsigned long long)partition_frame_usage[i],
                                 (unsigned long long)partition_frame_quota[i]);
        }
        shown++;
    }
    kernel_serial_printf(" %d partition(s) with nonzero usage or a configured quota.\n\n", shown);
}
