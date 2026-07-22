/*
 * frame_pool.h — Phase 13 (LPAR) physical memory quotas. See
 * AeroSLS-LPAR-Roadmap-v0.1.md §7 for the design writeup.
 *
 * Before this phase, frame_pool.c had no header at all — every caller of
 * allocate_physical_ram_frame() carried its own ad hoc `extern` declaration.
 * This file is both that missing header and the new per-partition
 * accounting API.
 *
 * Two allocation entry points, deliberately kept separate rather than
 * folding partition_id into the existing signature as a parameter every
 * caller must pass:
 *
 *   - allocate_physical_ram_frame() — unchanged signature, unchanged
 *     behavior. Every pre-Phase-13 caller (arch/{x86,riscv} page-table
 *     walkers, drivers/nvme*.c's controller queues, kernel/smp.c's AP
 *     bring-up stacks, kernel/lockfree_map.c's catalog index nodes,
 *     kernel/pte_migrate.c, kernel/simi_translate.c's shared activation
 *     cache) keeps compiling and behaving exactly as before, attributed to
 *     PARTITION_SYSTEM's usage counter for visibility but never
 *     quota-checked — the same "always-reachable regardless of partition"
 *     treatment Phase 11 gave the kernel-service IPC ports. This is a
 *     deliberate scope boundary, not an oversight: see the Phase 13
 *     findings addendum for which call sites were and weren't threaded
 *     through to the accounted path, and why.
 *
 *   - allocate_physical_ram_frame_for_partition(partition_id) — the new,
 *     quota-checked entry point. Used at the two call sites that are
 *     directly, unboundedly attributable to one tenant's own workload
 *     growth: process.c's per-process stack/code pages, and loader.c's
 *     per-binary segment frames. Fails closed (returns NULL, touches
 *     nothing) if the partition is already at/over its configured quota —
 *     same fail-cleanly-only posture the roadmap scoped for this phase's
 *     first cut; eviction is explicitly out of scope.
 *
 * Backward compatible by construction, same discipline as every prior LPAR
 * phase: every partition's quota defaults to 0 (BSS zero-init), and 0 means
 * "unlimited" — so nothing enforces a limit anywhere until
 * partition_set_frame_quota() is called explicitly.
 */
#ifndef FRAME_POOL_H
#define FRAME_POOL_H

#include <stdint.h>
#include "partition.h"

#define FRAME_SIZE 4096

/* Unaccounted allocation path — see the header comment above for exactly
 * which subsystems stay on this path and why. Never quota-checked; can
 * still return 0 on genuine physical OOM. */
void *allocate_physical_ram_frame(void);

/* Quota-checked allocation path. Returns 0 (without touching the bitmap at
 * all) if partition_id is out of range, or has a nonzero quota already met
 * or exceeded. Otherwise behaves exactly like allocate_physical_ram_frame()
 * and increments partition_id's usage counter on success. */
void *allocate_physical_ram_frame_for_partition(uint32_t partition_id);

/* Sets partition_id's frame quota. 0 = unlimited (the default for every
 * partition until this is called). Returns 0 on success, 1 if partition_id
 * is out of range ([0, PARTITION_MAX)). Does not validate that
 * partition_id was actually partition_create()'d — mirrors
 * partition_get_for_uid()'s own "never fails, every id maps to something"
 * posture rather than partition_assign_uid()'s stricter validation, since
 * a quota can usefully be pre-configured before a partition is created. */
int partition_set_frame_quota(uint32_t partition_id, uint64_t frame_quota);

/* Introspection. Both return 0xFFFFFFFFFFFFFFFFULL for an out-of-range
 * partition_id (mirrors partition_create()'s 0xFFFFFFFF sentinel style,
 * widened to 64 bits since these are frame counts, not ids). */
uint64_t partition_get_frame_usage(uint32_t partition_id);
uint64_t partition_get_frame_quota(uint32_t partition_id);

/* Gap Remediation Phase F: the free-side counterpart to
 * allocate_physical_ram_frame()/allocate_physical_ram_frame_for_partition().
 * Before this phase, frame_pool.c had NO free primitive at all for any
 * subsystem -- every frame ever handed out stayed marked allocated in
 * physical_memory_bitmap forever, kernel-wide, not just for partitions.
 *
 * Clears frame's bit in the bitmap and decrements the relevant partition's
 * usage counter (floored at 0, defensive against an already-inconsistent
 * counter). Validates before touching anything: frame must be page-aligned,
 * in range, non-NULL/non-frame-0 (frame 0 is never handed out, see
 * alloc_raw_frame()'s own skip), and its bit must currently be SET --
 * calling this on an address that was never allocated, or double-freeing
 * one already freed, is rejected as a no-op failure (returns 1) rather than
 * corrupting the bitmap or a partition counter. Same fail-cleanly-before-
 * any-side-effect posture as every other allocator boundary check in this
 * project.
 *
 * free_physical_ram_frame() decrements PARTITION_SYSTEM's counter, mirroring
 * how allocate_physical_ram_frame() always increments it.
 * free_physical_ram_frame_for_partition() decrements the given partition_id
 * instead, mirroring the accounted alloc path -- fails (returns 1, bitmap
 * untouched) if partition_id is out of range.
 *
 * Multi-Node Partition Scaling Roadmap Phase 3: NEITHER of these two
 * single-frame primitives is called from partition_destroy() -- it uses
 * the bulk partition_reclaim_all_frames() below instead, which is built on
 * top of the same free_raw_frame()-equivalent bitmap-clearing logic these
 * two use, applied to every frame this partition holds rather than one
 * address at a time. See partition_reclaim_all_frames()'s own comment for
 * why a real per-frame owner map makes that safe without needing the
 * per-process page-table walker described below. */
int free_physical_ram_frame(void* frame);
int free_physical_ram_frame_for_partition(void* frame, uint32_t partition_id);

/* Phase 14 (LPAR): resets partition_id's frame-usage counter to 0.
 * ACCOUNTING-LEVEL RESET ONLY — this does NOT clear any bits in the
 * underlying physical bitmap. As of Multi-Node Partition Scaling Roadmap
 * Phase 3, partition_destroy() no longer calls this function directly --
 * it calls partition_reclaim_all_frames() instead, which does real bitmap
 * reclamation AND zeroes this same counter as part of that (truthfully,
 * since by then the frames are actually gone, not just uncounted). This
 * function is kept as a still-valid, lower-level primitive for any future
 * caller that genuinely wants an accounting-only reset without touching
 * the bitmap (e.g. correcting a counter known to have drifted for reasons
 * unrelated to the frames themselves), and remains exactly what it always
 * was -- see the Phase 14 findings addendum for the original rationale.
 * Returns 0 on success, 1 if partition_id is out of range. */
int partition_reset_frame_usage(uint32_t partition_id);

/* Multi-Node Partition Scaling Roadmap Phase 3: real per-partition physical
 * frame reclamation, closing the gap LPAR Phase 14's own findings named
 * ("arguably its own phase") and this roadmap's §2 design principle 2
 * treated as a prerequisite gate before migration (Phase 6) makes the same
 * leak run on a per-migration clock instead of a per-partition-lifetime one.
 *
 * frame_pool.c now keeps a real per-frame owner map (frame_owner[], one
 * byte per physical frame) alongside the existing bitmap and the Phase 13
 * aggregate usage COUNTER -- populated at both accounted allocation call
 * sites (allocate_physical_ram_frame() attributes to PARTITION_SYSTEM,
 * allocate_physical_ram_frame_for_partition() attributes to the real
 * caller-supplied partition_id) and cleared back out whenever a frame is
 * freed through any of the free_* entry points above. This is deliberately
 * scoped to the frames LPAR Phase 13 already made partition-aware at
 * allocation time -- process.c's three call sites and loader.c's two -- not
 * a re-audit of every allocate_physical_ram_frame() call site in the tree;
 * frames handed out through the plain, unaccounted path (page-table
 * internals, NVMe queues, SMP stacks, the shared SIMI activation cache,
 * catalog index nodes) are attributed to PARTITION_SYSTEM the same as
 * before and are never reclaimed per-partition, since PARTITION_SYSTEM can
 * never itself be destroyed (partition_destroy() rejects it outright).
 *
 * partition_reclaim_all_frames(partition_id) walks the owner map, clears
 * the bitmap bit and owner tag for every frame this specific partition_id
 * owns, and zeroes the usage counter once real reclamation is done. This is
 * safe WITHOUT needing the per-process page-table walker the Phase F
 * comment above (and the LPAR Phase 14 findings addendum) named as its own,
 * separate, larger, and currently-unsafe project: this function never
 * touches any process's page table at all, it only consults frame_pool.c's
 * own tracking structures for frames that were specifically tagged with a
 * real partition_id at allocation time. It does not, and cannot, reclaim a
 * frame a process's page table merely *points at* without frame_pool.c
 * itself having attributed that frame to the partition being destroyed --
 * that broader page-table-walker gap (inherited/copied PML4 entries from
 * user_clone_page_table()'s current design) remains exactly as unresolved
 * and exactly as named as it was before this phase.
 *
 * Returns the number of frames actually reclaimed (0 or more -- 0 is a
 * legitimate answer for a partition that never allocated through the
 * accounted path), or 0xFFFFFFFFu if partition_id is out of range, mirroring
 * partition_create()'s own out-of-range sentinel convention. */
uint32_t partition_reclaim_all_frames(uint32_t partition_id);

/* Debug/introspection listing, mirrors sys_sls_partition_list()'s style. */
void sys_sls_partition_quota_list(void);

/* Navigator-Parity Gap Roadmap Phase 2: system-wide RAM introspection --
 * distinct from partition_get_frame_usage()/_quota() above, which only ever
 * report one partition's own accounting. frame_pool_total_frames() is the
 * bitmap's fixed capacity (TOTAL_FRAMES); frame_pool_allocated_count() is a
 * live popcount of physical_memory_bitmap, i.e. every frame currently
 * allocated to anyone, system-wide, regardless of partition attribution. */
uint64_t frame_pool_total_frames(void);
uint64_t frame_pool_allocated_count(void);

// ─── Syscalls ─────────────────────────────────────────────────────────────
#define SYS_SLS_PARTITION_QUOTA_SET 213

struct SLSPartitionQuotaSetRequest {
    uint32_t partition_id;
    uint64_t frame_quota;
};

/* Thin syscall wrapper, same shape as sys_sls_partition_create()/
 * sys_sls_partition_assign() — returns a 64-bit value regardless of the
 * underlying function's natural width, matching do_syscall()'s ABI. */
uint64_t sys_sls_partition_quota_set(struct SLSPartitionQuotaSetRequest* req);

/* Gap Remediation Phase F: sys_sls_partition_quota_list() (above) was fully
 * implemented since Phase 13 but had no syscall number and no dispatcher
 * case -- syscall-unreachable, not just shell/HTTP-unreachable, unlike
 * every other gap this roadmap has closed so far. 217 is free (210-212,
 * 213, 214-216 already taken; 220+ belongs to the SQL/Vector Store
 * syscalls). No request struct needed -- takes no arguments at all, same
 * shape as sys_sls_partition_list()'s own dispatch case. */
#define SYS_SLS_PARTITION_QUOTA_LIST 217

#endif /* FRAME_POOL_H */
