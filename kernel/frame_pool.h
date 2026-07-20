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
 * NEITHER is currently called from partition_destroy(). See that function's
 * own comment (kernel/partition.c) and the Phase F findings addendum for
 * why: frame_pool.c only ever tracked an aggregate per-partition frame
 * COUNT, never which individual physical frames belong to which partition
 * or which process, so partition_destroy() has no list of addresses to
 * pass here. Building that list would mean walking each of the partition's
 * processes' own page tables at teardown -- and this investigation found
 * that doing so naively would be actively dangerous, not just unbuilt:
 * user_clone_page_table() (arch/x86/user_paging.c) currently clones a new
 * process's PML4 by copying all 512 raw entries from the running kernel's
 * own PML4, meaning a freshly-cloned process's page table can point at
 * intermediate PDPT/PD/PT frames it doesn't itself own and never allocated
 * -- a walker that frees everything reachable from a process's own PML4
 * without first distinguishing "this process's own mappings" from
 * "inherited-by-copy-at-clone-time" entries would free live kernel/shared
 * page-table frames out from under the rest of the system. That's a real,
 * separate, larger project (a correct per-process mapping walker, doubled
 * for both the x86 and RISC-V arch targets this kernel supports) -- named
 * here rather than attempted unsafely in this pass. This phase closes the
 * narrower, concretely-scoped gap the roadmap named: frame_pool.c now HAS a
 * free primitive, for the first time, available to any future caller that
 * does have a specific frame address in hand. */
int free_physical_ram_frame(void* frame);
int free_physical_ram_frame_for_partition(void* frame, uint32_t partition_id);

/* Phase 14 (LPAR): resets partition_id's frame-usage counter to 0.
 * ACCOUNTING-LEVEL RECLAMATION ONLY — this does NOT clear any bits in the
 * underlying physical bitmap. frame_pool.c only ever tracked an aggregate
 * per-partition frame COUNT (Phase 13's scope), never which individual
 * physical frames belong to which partition, so there is no way to know
 * which bitmap bits to clear. The frames a destroyed partition once held
 * remain permanently marked allocated in physical_memory_bitmap; this call
 * only lets a future allocate_physical_ram_frame_for_partition() against
 * this (possibly reused) partition_id start counting from zero again. See
 * the Phase 14 findings addendum for the full implication. Returns 0 on
 * success, 1 if partition_id is out of range. Called from
 * partition_destroy(). */
int partition_reset_frame_usage(uint32_t partition_id);

/* Debug/introspection listing, mirrors sys_sls_partition_list()'s style. */
void sys_sls_partition_quota_list(void);

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
