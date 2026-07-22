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
 * queues, SMP stacks, the shared SIMI activation cache, catalog index
 * nodes) — so partition_frame_usage[PARTITION_SYSTEM] is deliberately a
 * "total unaccounted + system tenant" number, not a pure per-tenant one.
 * See the Phase 13 findings addendum for the full call-site audit. Quota
 * defaults to 0 (BSS zero-init) = unlimited, same backward-compatible-by-
 * construction discipline as every prior LPAR phase. */
static uint64_t partition_frame_usage[PARTITION_MAX];
static uint64_t partition_frame_quota[PARTITION_MAX];

/* Multi-Node Partition Scaling Roadmap Phase 3: real per-frame ownership,
 * one byte per physical frame, alongside the aggregate-only counter above.
 * BSS zero-init means every entry starts as PARTITION_SYSTEM (0) by
 * default -- indistinguishable, by value alone, from "genuinely allocated
 * to PARTITION_SYSTEM." That ambiguity is safe by construction: every
 * reader of this array (partition_reclaim_all_frames() below) also checks
 * physical_memory_bitmap's real allocated bit before ever acting on a
 * frame_owner[] value, so a never-allocated frame's default-0 owner tag is
 * never mistaken for a real, live PARTITION_SYSTEM allocation -- the same
 * "0 is honest, verify before trusting" discipline partition_owner_table[]
 * uses one layer up in kernel/partition.c. See frame_pool.h's own comment
 * on partition_reclaim_all_frames() for the full design writeup. */
static uint8_t frame_owner[TOTAL_FRAMES];

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
    if (frame) {
        partition_frame_usage[PARTITION_SYSTEM]++;
        // Multi-Node Partition Scaling Roadmap Phase 3: tag the owner the
        // same way the usage counter already attributes this path -- see
        // frame_owner[]'s own comment above.
        frame_owner[(uint64_t)(uintptr_t)frame / FRAME_SIZE] = (uint8_t)PARTITION_SYSTEM;
    }
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
    if (frame) {
        partition_frame_usage[partition_id]++;
        // Multi-Node Partition Scaling Roadmap Phase 3: real per-frame
        // ownership tag -- this is what makes partition_reclaim_all_
        // frames() below possible for frames allocated through this path.
        frame_owner[(uint64_t)(uintptr_t)frame / FRAME_SIZE] = (uint8_t)partition_id;
    }
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
    // Multi-Node Partition Scaling Roadmap Phase 3: reset the owner tag
    // back to the default (0/PARTITION_SYSTEM, indistinguishable from
    // "never allocated" by value alone -- see frame_owner[]'s own comment
    // on why that's safe) so a freed frame never carries a stale owner
    // into whatever the next allocate_*() call reuses it for.
    frame_owner[frame_index] = (uint8_t)PARTITION_SYSTEM;
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

uint32_t partition_reclaim_all_frames(uint32_t partition_id)
{
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFu;

    uint32_t freed = 0;
    // Start at frame 1, same skip-frame-0 discipline as alloc_raw_frame()/
    // free_raw_frame() (frame 0 = address 0x0 = NULL, never handed out).
    for (uint64_t frame_index = 1; frame_index < TOTAL_FRAMES; frame_index++) {
        if (frame_owner[frame_index] != (uint8_t)partition_id) continue;
        size_t word = frame_index / 64;
        int    bit  = (int)(frame_index % 64);
        // Defensive, matches this project's discipline everywhere else in
        // this file: an owner tag without the bitmap bit actually set
        // shouldn't happen (free_raw_frame() always clears both together),
        // but this never trusts frame_owner[] alone as proof of a live
        // allocation -- see frame_owner[]'s own comment on why the BSS-
        // zero default value is otherwise ambiguous with PARTITION_SYSTEM.
        if (!(physical_memory_bitmap[word] & (1ULL << bit))) continue;
        physical_memory_bitmap[word] &= ~(1ULL << bit);
        frame_owner[frame_index] = (uint8_t)PARTITION_SYSTEM;
        freed++;
    }
    // Real reclamation happened above -- this reset is now truthful (every
    // frame that made up the old count has actually been freed), not just
    // an accounting fiction the way the old partition_destroy() call site
    // used to leave it.
    partition_frame_usage[partition_id] = 0;

    kernel_serial_printf(
        "[QUOTA] partition %u: %u physical frame(s) actually reclaimed "
        "(bitmap cleared, not just the usage counter -- Multi-Node "
        "Partition Scaling Roadmap Phase 3).\n",
        (unsigned)partition_id, (unsigned)freed);
    return freed;
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

// ─── Navigator-Parity Gap Roadmap Phase 2: system-wide RAM introspection ──────
// Distinct from the per-partition accounting above (partition_get_frame_usage()
// etc.), which only ever tracks each tenant's own usage -- neither the
// bitmap's real total capacity nor a live system-wide allocated count was
// exposed anywhere before this. Portable bit-count (no __builtin_popcount*):
// this kernel builds freestanding with no libgcc linked, and depending on
// optimization level/target flags that builtin can lower to a libgcc call
// (__popcountdi2) instead of inline instructions -- the same class of ABI
// pitfall already named and worked around elsewhere in this codebase (see
// the float-return-ABI x86 cross-build fix). A plain Kernighan loop is
// exactly as portable as the bitmap it's counting.
static uint64_t popcount64(uint64_t v) {
    uint64_t count = 0;
    while (v) { v &= (v - 1); count++; }
    return count;
}

uint64_t frame_pool_total_frames(void) {
    return TOTAL_FRAMES;
}

uint64_t frame_pool_allocated_count(void) {
    uint64_t count = 0;
    for (size_t i = 0; i < (TOTAL_FRAMES / 64); i++) {
        count += popcount64(physical_memory_bitmap[i]);
    }
    return count;
}
