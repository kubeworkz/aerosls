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
 * to PARTITION_SYSTEM."
 *
 * CORRECTION. This comment used to claim that ambiguity was "safe by
 * construction" because partition_reclaim_all_frames() also checks
 * physical_memory_bitmap's real allocated bit before acting, so a
 * never-allocated frame's default-0 tag could not be mistaken for a live
 * PARTITION_SYSTEM allocation. That is true of a never-allocated frame and
 * false of a RESERVED one: fp_mark_used() sets the bitmap bit, so the boot
 * reservation covering the kernel image passes the check with its owner tag
 * still at the default 0. The bitmap bit distinguishes "allocated" from
 * "free"; it says nothing about "owned by a tenant" versus "owned by the
 * machine", which is the distinction that actually mattered here.
 *
 * The real guard is the reserved_below/reserved_above watermark pair below,
 * checked by frame_pool_frame_is_machine_owned(). See its comment. */
static uint8_t frame_owner[TOTAL_FRAMES];

/* ─── Boot-time reservation ───────────────────────────────────────────
 * See frame_pool.h for the full account of what went wrong without this. */

/* Provided by arch/x86/linker.ld. Its ADDRESS is the end of the loaded
 * image; the object itself is never read, which is why it is declared as
 * an array (taking &x of a zero-sized extern is the portable idiom). */
extern char _kernel_image_end[];

static uint64_t frames_reserved = 0;

/* ─── Boot reservations are not allocations ───────────────────────────────
 * frame_owner[] is a uint8_t and PARTITION_MAX is 256, so every value it can
 * hold is a valid partition id and there is no spare sentinel meaning "this
 * frame belongs to the machine, not to a tenant." The boot reservations below
 * therefore leave frame_owner[] at its BSS-zero default, which reads as
 * PARTITION_SYSTEM -- and a reserved kernel frame becomes indistinguishable
 * from a frame genuinely allocated to the system partition.
 *
 * frame_owner[]'s own comment notices this ambiguity and concludes it is safe
 * because every reader also checks the bitmap bit before acting. That
 * reasoning does not hold: fp_mark_used() SETS the bitmap bit, so a reserved
 * frame passes the check. partition_reclaim_all_frames(PARTITION_SYSTEM)
 * would walk frames 1..N, match every one of them on owner, confirm the bit,
 * and free the kernel's own .text, .data, .bss, page tables and 64 KiB
 * bootstrap stack into the allocator.
 *
 * Today both call sites -- partition_destroy() and partition_migrate() --
 * refuse PARTITION_SYSTEM before they get here, so this is a landmine rather
 * than a live bug. It is guarded here anyway, at the function that would do
 * the damage, because "unreachable" is a property of two call sites that
 * could gain a third, and the failure mode is the kernel handing out its own
 * running stack as scratch memory.
 *
 * These two watermarks bound the reserved regions: everything below
 * `reserved_below` (the kernel image) and everything at or above
 * `reserved_above` (memory that does not physically exist) is machine state
 * that no partition owns and no reclaim may touch. */
static uint64_t reserved_below = 0;              /* frames [0, reserved_below) */
static uint64_t reserved_above = TOTAL_FRAMES;   /* frames [reserved_above, TOTAL_FRAMES) */

int frame_pool_frame_is_machine_owned(uint64_t frame_index) {
    return frame_index < reserved_below || frame_index >= reserved_above;
}

static void fp_mark_used(uint64_t frame_index) {
    if (frame_index >= TOTAL_FRAMES) return;
    uint64_t word = frame_index / 64, bit = frame_index % 64;
    if (physical_memory_bitmap[word] & (1ULL << bit)) return;   /* already */
    physical_memory_bitmap[word] |= (1ULL << bit);
    frames_reserved++;
}

void frame_pool_reserve_below(uint64_t end_addr) {
    /* Round UP: a partially-occupied final frame is still occupied. */
    uint64_t last = (end_addr + FRAME_SIZE - 1) / FRAME_SIZE;
    if (last > TOTAL_FRAMES) last = TOTAL_FRAMES;
    for (uint64_t f = 0; f < last; f++) fp_mark_used(f);
    if (last > reserved_below) reserved_below = last;
}

void frame_pool_reserve_range(uint64_t lo_addr, uint64_t hi_addr) {
    /* Not redundant with the loop bound below. For an inverted range spanning
     * frames, first > last leaves the loop empty anyway -- but an inverted
     * range WITHIN one frame rounds to first == N, last == N + 1 and would
     * reserve that frame for a range that describes no memory. */
    if (hi_addr <= lo_addr) return;
    uint64_t first = lo_addr / FRAME_SIZE;                       /* round DOWN */
    uint64_t last  = (hi_addr + FRAME_SIZE - 1) / FRAME_SIZE;    /* round UP   */
    if (last > TOTAL_FRAMES) last = TOTAL_FRAMES;
    for (uint64_t f = first; f < last; f++) fp_mark_used(f);
    if (last > reserved_below && first == 0) reserved_below = last;
}

/* The bootstrap stack, from arch/x86/boot.asm. Their ADDRESSES are the bounds. */
extern char stack_bottom[], stack_top[];

void frame_pool_init(void) {
    uint64_t end = (uint64_t)(uintptr_t)_kernel_image_end;
    frame_pool_reserve_below(end);

    /* ─── Verify, do not assume, that the stack is inside that ────────────
     * The linker script puts .bootstrap_stack last inside .bss and then
     * defines _kernel_image_end above it, so reserving below the image end
     * covers the stack. That is a property of a linker script that nothing
     * checked, across a section name that appears in exactly one place, and
     * the cost of it being wrong is the allocator handing out the memory the
     * kernel is standing on -- which corrupts the return address of whatever
     * runs next and surfaces as a #GP on a poisoned rip, several layers away
     * from the write that caused it.
     *
     * So the stack is reserved AGAIN by its own exported bounds. If that adds
     * frames, the image-end reservation did not cover it and we say so
     * loudly, because at that point every other assumption resting on
     * _kernel_image_end is suspect too. */
    uint64_t sb = (uint64_t)(uintptr_t)stack_bottom;
    uint64_t stp = (uint64_t)(uintptr_t)stack_top;
    uint64_t before_stack = frames_reserved;
    frame_pool_reserve_range(sb, stp);
    if (frames_reserved != before_stack) {
        kernel_serial_printf(
            "[FRAME] *** ERROR: the bootstrap stack [0x%llx,0x%llx) was NOT covered by the "
            "kernel image end 0x%llx -- %llu frame(s) of live kernel stack were allocatable. "
            "Reserved now, but the linker script and _kernel_image_end disagree and every "
            "other bound derived from it should be re-checked. ***\n",
            (unsigned long long)sb, (unsigned long long)stp, (unsigned long long)end,
            (unsigned long long)(frames_reserved - before_stack));
    }
    kernel_serial_printf(
        "[FRAME] reserved %llu frames (%llu MiB) below the kernel image end "
        "0x%llx -- allocator now starts above the kernel.\n",
        (unsigned long long)frames_reserved,
        (unsigned long long)((frames_reserved * FRAME_SIZE) >> 20),
        (unsigned long long)end);
}

void frame_pool_limit_ram(uint64_t top_addr) {
    if (top_addr == 0) {
        kernel_serial_print("[FRAME] no usable-RAM top reported -- pool left at its "
                            "compile-time 4 GiB span (see frame_pool.h).\n");
        return;
    }
    uint64_t first_absent = top_addr / FRAME_SIZE;   /* round DOWN: a partial frame at the top is not usable */
    if (first_absent >= TOTAL_FRAMES) return;        /* machine has at least as much RAM as we track */

    uint64_t before = frames_reserved;
    for (uint64_t f = first_absent; f < TOTAL_FRAMES; f++) fp_mark_used(f);
    if (first_absent < reserved_above) reserved_above = first_absent;
    kernel_serial_printf(
        "[FRAME] reserved %llu frames above 0x%llx -- that memory does not exist.\n",
        (unsigned long long)(frames_reserved - before),
        (unsigned long long)top_addr);
}

uint64_t frame_pool_reserved_count(void) { return frames_reserved; }

int frame_pool_is_reserved(uint64_t frame_index) {
    if (frame_index >= TOTAL_FRAMES) return 1;   /* outside the pool: never allocatable */
    return (physical_memory_bitmap[frame_index / 64] >> (frame_index % 64)) & 1ULL;
}

void frame_pool_reset(void) {
    for (size_t i = 0; i < (TOTAL_FRAMES / 64); i++) physical_memory_bitmap[i] = 0;
    for (size_t i = 0; i < TOTAL_FRAMES; i++) frame_owner[i] = 0;
    frames_reserved = 0;
    reserved_below  = 0;
    reserved_above  = TOTAL_FRAMES;
}


/* The caller's own stack pointer. The allocator must never return the frame
 * this is sitting in: doing so lets the next write of tenant data land on the
 * kernel's live stack, and the failure surfaces as a corrupted return address
 * in unrelated code long after the allocation. One compare per allocation to
 * make that specific catastrophe impossible rather than merely unlikely. */
/* Test seam. Zero in production (BSS), and nothing in the kernel ever writes
 * it. It exists because the withhold path below is otherwise unreachable from
 * a host test: the fake allocator hands out addresses derived from the frame
 * index (0x1000, 0x2000, ...) while the host's real stack pointer lives far
 * above them, so "no frame contained my stack" is true no matter what the
 * guard does. A mutation deleting the guard outright passed a 45-check suite.
 *
 * A test-only global in production code is a smell. An untested guard against
 * the kernel handing out its own live stack is worse, so this is the trade
 * being made deliberately and in writing. */
uint64_t frame_pool_test_sp_override = 0;

static inline uint64_t fp_current_sp(void) {
    if (frame_pool_test_sp_override) return frame_pool_test_sp_override;
#if defined(__x86_64__)
    uint64_t sp; __asm__ volatile("mov %%rsp, %0" : "=r"(sp)); return sp;
#else
    return 0;
#endif
}

int fp_frame_contains(uint64_t frame_base, uint64_t addr) {
    return addr != 0 && addr >= frame_base && addr < frame_base + FRAME_SIZE;
}

uint64_t frame_pool_live_stack_withheld = 0;

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
                    uint64_t base = (uint64_t)(((i * 64) + bit) * 4096);
                    /* Last line of defence. If the reservations above were
                     * right this never fires; if it does, the reservation was
                     * wrong and this is the only thing between a tenant's
                     * upload and the kernel's own return addresses. */
                    if (fp_frame_contains(base, fp_current_sp())) {
                        physical_memory_bitmap[i] |= (1ULL << bit);  /* withhold, do not reuse */
                        frames_reserved++;
                        frame_pool_live_stack_withheld++;
                        kernel_serial_printf(
                            "[FRAME] *** WITHHELD frame 0x%llx: it contains the LIVE KERNEL "
                            "STACK. Handing it out would have let the next write land on a "
                            "return address. The boot reservation missed it -- see "
                            "frame_pool_init(). ***\n", (unsigned long long)base);
                        continue;
                    }
                    physical_memory_bitmap[i] |= (1ULL << bit);
                    return (void *)(uintptr_t)base;
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
        /* Boot reservations are machine state, not anybody's allocation.
         * Without this, reclaiming PARTITION_SYSTEM frees the kernel image --
         * see the reserved_below/reserved_above comment above. */
        if (frame_pool_frame_is_machine_owned(frame_index)) continue;
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
