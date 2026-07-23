/*
 * storage_quota.h — Storage Isolation Roadmap Phase 1: per-partition
 * on-disk page quota accounting. See docs/AeroSLS-Storage-Isolation-
 * Roadmap-v0.1.md §4 Phase 1 for the design writeup this implements.
 *
 * Mirrors kernel/frame_pool.c's LPAR Phase 13 frame-quota shape exactly,
 * just for on-disk 4 KiB pages instead of physical RAM frames: a flat
 * PARTITION_MAX-sized usage/quota array pair, a setter, two getters, 0 =
 * unlimited (BSS zero-init, no special-case code needed), and denial
 * happening cleanly before any allocation side effect.
 *
 * Scope decision (Storage Isolation Roadmap §5's own open questions,
 * resolved here rather than left open):
 *
 *   - rowstore.c and vecstore.c share ONE combined per-partition page
 *     budget, not two separate quota pools. Both subsystems already use an
 *     identical 4 KiB page size (ROWSTORE_PAGE_SIZE == VECSTORE_PAGE_SIZE
 *     == 4096, confirmed by direct read of both headers), so a single
 *     "disk pages consumed" counter per partition is simpler than two
 *     parallel tables and mirrors how frame_pool.c already tracks one flat
 *     "frames consumed" number regardless of whether they went to process
 *     code, stack, or ELF segments — the resource being protected is "disk
 *     space," not "rowstore space" and "vecstore space" as two unrelated
 *     things a customer would think about separately.
 *   - stream.c is deliberately NOT included. STREAM_MAX (8) fixed 64 MiB
 *     on-disk slots is already a hard, small, system-wide ceiling on its
 *     own — a per-partition quota on top of an 8-slot total would be
 *     redundant bookkeeping for no real additional protection, and stream
 *     RAM frame allocation is already quota-checked separately (Multitenant
 *     Isolation Gap Analysis §10). Revisit only if STREAM_MAX is ever
 *     raised enough that "one tenant claims every slot" becomes a real risk
 *     again.
 *
 * This is Phase 1 only: accounting, not physical isolation. A partition's
 * pages are still scattered arbitrarily within the shared rowstore/vecstore
 * LBA pools, never contiguous or reserved — same "accounting layer bolted
 * onto an unchanged shared pool" posture the frame quota already proved
 * out. Real per-tenant LBA sub-ranges are Storage Isolation Roadmap Phase 3,
 * deliberately deferred until a dedicated-storage tier is an actual product
 * decision.
 */
#ifndef STORAGE_QUOTA_H
#define STORAGE_QUOTA_H

#include <stdint.h>
#include "partition.h"

/* Quota-checked page reservation. Returns 0 (and increments partition_id's
 * usage counter) if the reservation is admitted, or 1 (touching nothing) if
 * partition_id is out of range, or has a nonzero quota already met or
 * exceeded. Callers (rowstore_alloc_page()/vecstore_alloc_page()) must call
 * this BEFORE advancing their own bump-allocator cursor or touching NVMe —
 * same "denial happens before any side effect" posture as
 * allocate_physical_ram_frame_for_partition(). This function does not
 * itself claim a page_id or write to disk; it only answers "is partition_id
 * allowed one more page," matching frame_pool.c's own separation between
 * quota admission and the underlying bitmap/cursor allocator. */
int storage_page_reserve(uint32_t partition_id);

/* Accounting-only release: decrements partition_id's page-usage counter by
 * count (floored at 0, defensive against an already-inconsistent counter —
 * same posture as free_physical_ram_frame()'s usage decrement). Returns 0
 * on success, 1 if partition_id is out of range. Not wired into any real
 * reclaim path in Phase 1 — rowstore.c/vecstore.c have no page-free
 * primitive at all yet ("No reclaim in this first cut" per both files' own
 * rowstore_alloc_page()/vecstore_alloc_page() comments), so there is
 * nothing for this to be called from today. It exists now so a future
 * reclaim path (mirroring frame_pool.c's two-tier partition_reset_frame_
 * usage() vs. partition_reclaim_all_frames() split, per this roadmap's own
 * §2 caveat) has a real accounting primitive to build on rather than
 * inventing one under pressure later. */
int storage_page_release(uint32_t partition_id, uint64_t count);

/* Sets partition_id's on-disk page quota. 0 = unlimited (the default for
 * every partition until this is called). Returns 0 on success, 1 if
 * partition_id is out of range ([0, PARTITION_MAX)). Does not validate that
 * partition_id was actually partition_create()'d, mirroring partition_set_
 * frame_quota()'s own "pre-configurable before creation" posture. */
int storage_set_page_quota(uint32_t partition_id, uint64_t page_quota);

/* Introspection. Both return 0xFFFFFFFFFFFFFFFFULL for an out-of-range
 * partition_id, mirroring partition_get_frame_usage()/_quota()'s sentinel
 * convention exactly. */
uint64_t storage_get_page_usage(uint32_t partition_id);
uint64_t storage_get_page_quota(uint32_t partition_id);

/* Debug/introspection listing, mirrors sys_sls_partition_quota_list()'s
 * style. */
void sys_sls_partition_storage_quota_list(void);

// ─── Syscalls ─────────────────────────────────────────────────────────────
// Confirmed as the next free syscall numbers via a fresh grep of every
// kernel/*.h and net/net.h SYS_SLS_* define before picking them (highest
// existing value found: 274, SYS_SLS_PARTITION_CPU_WEIGHT_LIST), per this
// codebase's own established convention.
#define SYS_SLS_PARTITION_STORAGE_QUOTA_SET  275
#define SYS_SLS_PARTITION_STORAGE_QUOTA_LIST 276

struct SLSPartitionStorageQuotaSetRequest {
    uint32_t partition_id;
    uint64_t page_quota;   // 0 = unlimited
};

/* Thin syscall wrapper, same shape as sys_sls_partition_quota_set(). */
uint64_t sys_sls_partition_storage_quota_set(struct SLSPartitionStorageQuotaSetRequest* req);

#endif /* STORAGE_QUOTA_H */
