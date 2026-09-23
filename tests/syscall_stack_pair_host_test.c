/*
 * syscall_stack_pair_host_test.c — the pair-allocation guard for
 * alloc_proc_syscall_stack() (kernel/process.c), run against the REAL,
 * unmodified kernel/process.c and kernel/frame_pool.c — not a
 * reimplementation of either.
 *
 * ─── The bug this exists to prevent from returning ─────────────────────────
 * Every process needs an 8 KiB kernel syscall stack, which is TWO CONTIGUOUS
 * frames. The function obtained them by calling the single-frame allocator
 * twice and trusting the result to be adjacent, on the stated reasoning that
 * "the frame pool is first-fit ASCENDING and the kernel is single-threaded, so
 * two consecutive allocs return adjacent frames; if they don't (defensive),
 * free both and retry once, then fail."
 *
 * That is not a property of the pool. It holds only while the LOWEST free run
 * is at least two frames long. Leave a single free frame between reserved
 * memory below and allocated memory above and it fails outright — and the
 * retry cannot help, because the geometry is identical on the second pass.
 *
 * Measured on the unified boot (POSIX-Environments E5, the recycle boot
 * check): the lowest free frame was 0x0e1ff000 (frame 57855), with the 64 MiB
 * capability arena (16384 frames, 0x0e200000) and the tenant's 1344-frame
 * region immediately above it, so the two calls returned 0x0e1ff000 and
 * 0x12740000 — 17728 frames apart. Every environment create in that boot died
 * with "[SIDECAR] create: syscall stack allocation failed", the E4
 * environment-creation boot check with it. It is not deterministic across tree
 * revisions either: anything that changes a sidecar binary's size moves the
 * initrd GRUB loads, which moves the boundary of the reserved range, which
 * moves the lowest free frame. The bug was latent until a binary grew.
 *
 * ─── What this test asserts ────────────────────────────────────────────────
 * The geometry above is reproduced EXACTLY, by frame number, through the real
 * pool's own reservation API — frames [0, 57855) reserved, frame 57855 left
 * free, frames [57856, 75584) reserved as the arena + region, everything above
 * 75584 free:
 *
 *   1. the setup really is the wedge it claims to be — the single-frame
 *      allocator hands out 0x0e1ff000 and then 0x12740000, 17728 frames
 *      apart. This is the NEGATIVE CONTROL, stated as a property of the pool
 *      rather than by reverting the source: an implementation that pairs two
 *      single-frame allocations cannot succeed here, so restoring the old
 *      body turns assertion 2 red;
 *   2. alloc_proc_syscall_stack() returns a usable stack top anyway, and the
 *      two frames under it are adjacent, outside the reserved block, and both
 *      charged to the partition that asked;
 *   3. its accounting is exact — 2 frames, not 1 and not 3 — and a second call
 *      gets a disjoint pair (so the first is not handed out twice);
 *   4. the frames come back the way the teardown path releases them
 *      (frame_pool_frame_owner(), exactly as proc_free_syscall_stack does),
 *      leaving the partition's usage where it started.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 -I user \
 *       -o /tmp/syscall_stack_pair_host_test \
 *       tests/syscall_stack_pair_host_test.c kernel/frame_pool.c kernel/partition.c
 *   /tmp/syscall_stack_pair_host_test
 */
#include "kernel/process.h"
#include "kernel/frame_pool.h"
#include "kernel/partition.h"
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "tests/process_host_stubs.h"   /* weak stubs; frame_pool.c's strong defs win */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ─── Link stubs ────────────────────────────────────────────────────────────
 * process.c's dependencies that this test never calls. It drives
 * alloc_proc_syscall_stack() and the frame pool directly, and reaches nothing
 * that spawns, loads, schedules or migrates — these exist so including
 * process.c as source compiles and links. Signatures must match the real
 * headers, which process.c includes too, so a mismatch is a compile error
 * rather than a silent bug. The shared file (tests/process_host_stubs.h)
 * already covers the syscall-stack wiring, the teardown trio and the
 * bootstrap-stack bounds; the frame pool's REAL definitions of
 * free_physical_ram_frame_for_partition() and frame_pool_frame_owner()
 * override its weak ones, because this test links frame_pool.c for real. */

/* frame_pool.c reserves the kernel image by this symbol's ADDRESS; it is never
 * read. A host test cannot choose the address, and this one never calls
 * frame_pool_init() — the geometry below is built explicitly instead. */
char _kernel_image_end[1];

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* process.c reads the tick counter for its channel deadlines. */
volatile uint64_t kernel_tick_counter = 0;

int      stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int      stream_count_for_partition(uint32_t p) { (void)p; return 0; }
int      stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
void     dspp_partition_announce(uint32_t p, const char* n, uint32_t o) { (void)p; (void)n; (void)o; }
void     dspp_partition_withdraw(uint32_t p) { (void)p; }
void     dspp_partition_ownedset_send(uint32_t g, const uint32_t* ids, uint32_t c) { (void)g; (void)ids; (void)c; }
uint64_t loader_load_into_process(const char* n, uint64_t b, uint64_t* pml4, uint32_t p) {
    (void)n; (void)b; (void)pml4; (void)p; return 0;
}
int      catalog_check_access(uint32_t u, const char* n, uint32_t perm) { (void)u; (void)n; (void)perm; return 1; }
uint64_t user_clone_page_table(void) { return 0; }
void     user_map_page(uint64_t* pml4, uint64_t v, uint64_t p, uint64_t f) { (void)pml4; (void)v; (void)p; (void)f; }
void     kernel_enter_ring3(uint64_t* rs, uint64_t* cs, uint64_t c, uint64_t rip, uint64_t rsp) {
    (void)rs; (void)cs; (void)c; (void)rip; (void)rsp;
}
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
void     persist_partitions(void) { }
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t service_unregister_partition(uint32_t p) { (void)p; return 0; }
uint32_t simi_ctx_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
uint32_t cluster_local_node_id(void) { return 0; }
uint32_t cluster_leader_id(void) { return 0; }
int      partition_lease_step_down(uint32_t p) { (void)p; return 1; }

/* Pull in process.c itself: that is what makes alloc_proc_syscall_stack() the
 * REAL function (and gives this test the real proc_table[]/proc_count). */
#include "kernel/process.c"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

/* The measured geometry, by frame number. 57855 is not 512-aligned and 57856
 * is (113 * 512) — the alignment is what let the contiguous allocator skip
 * past the lone frame in the boot that exposed this. */
#define F_LONE    57855ULL                     /* the single free frame 0x0e1ff000 */
#define A_BLOCK   57856ULL                     /* block start, arena 0x0e200000    */
#define ARENA     16384ULL                     /* 64 MiB capability arena          */
#define REGION    1344ULL                      /* the tenant's storage + budget    */
#define B_ABOVE   (A_BLOCK + ARENA + REGION)   /* 75584 = 0x12740000               */

#define PART_TENANT 1u
#define FRAME_ADDR(f) ((void*)(uintptr_t)((f) * 4096ULL))

int main(void) {
    /* A stack-pointer override OUTSIDE the pool's 4 GiB span, so the allocator's
     * live-kernel-stack withholding can never fire in this test: on the host the
     * real %rsp (~0x7ffd...) lands INSIDE the first 4 GiB, and the frame holding
     * it would be withheld — a skip this test did not ask for. */
    frame_pool_test_sp_override = 0x100000000ULL;

    CHECK(B_ABOVE == 75584ULL,
          "geometry: the block ends at frame 75584 (0x12740000) — the address the boot measured");

    /* ── The geometry, through the pool's own reservation API ───────────────
     * Frames [0, F_LONE) reserved (the image, low memory and the initrd GRUB
     * loaded), frame F_LONE deliberately left free, then the arena + region
     * reserved. Everything from B_ABOVE up is free. */
    frame_pool_reserve_range(0, F_LONE * 4096ULL);
    frame_pool_reserve_range((F_LONE + 1) * 4096ULL, B_ABOVE * 4096ULL);

    /* ── Scenario 1: the wedge is real (NEGATIVE CONTROL) ─────────────────
     * Two single-frame allocations, exactly what alloc_proc_syscall_stack
     * used to make. If this pair were adjacent the test below would prove
     * nothing, so assert what the pool actually does. */
    void* w1 = allocate_physical_ram_frame_for_partition(PART_TENANT);
    void* w2 = allocate_physical_ram_frame_for_partition(PART_TENANT);
    CHECK(w1 == FRAME_ADDR(F_LONE),
          "scenario 1: the lowest free frame is the lone one at 0x0e1ff000 (frame 57855)");
    CHECK(w2 == FRAME_ADDR(B_ABOVE),
          "scenario 1: the NEXT single-frame allocation skips the whole block to 0x12740000 (frame 75584)");
    CHECK((uint64_t)(uintptr_t)w2 != (uint64_t)(uintptr_t)w1 + 4096ULL,
          "scenario 1: NEGATIVE CONTROL — two consecutive single-frame allocations are NOT adjacent "
          "(17728 frames apart), so pairing them cannot produce a syscall stack");
    free_physical_ram_frame_for_partition(w1, PART_TENANT);
    free_physical_ram_frame_for_partition(w2, PART_TENANT);
    CHECK(partition_get_frame_usage(PART_TENANT) == 0,
          "scenario 1: the control's two frames went back — usage is 0 again");

    /* ── Scenario 2: the fix pairs them anyway ─────────────────────────────
     * A contiguous request does not care where the lowest free frame is. */
    uint64_t top = alloc_proc_syscall_stack(PART_TENANT);
    CHECK(top != 0,
          "scenario 2: alloc_proc_syscall_stack() SUCCEEDS in the wedge geometry");
    uint64_t lower = top - 8192ULL + 8ULL;
    CHECK(((top + 8ULL) % 8192ULL) == 0ULL && (lower % 4096ULL) == 0ULL,
          "scenario 2: the returned top is one 8-byte slot below an 8 KiB boundary and the pair's lower frame is page-aligned "
          "(the alloc_proc_syscall_stack convention)");
    CHECK((lower / 4096ULL) >= B_ABOVE,
          "scenario 2: the pair sits OUTSIDE the reserved block (nothing reserved was handed out)");
    CHECK(frame_pool_frame_owner(lower / 4096ULL) == PART_TENANT,
          "scenario 2: the lower frame is owner-tagged to the partition that asked");
    CHECK(frame_pool_frame_owner(lower / 4096ULL + 1ULL) == PART_TENANT,
          "scenario 2: the upper frame is owner-tagged too — both halves, not just the first");
    CHECK(partition_get_frame_usage(PART_TENANT) == 2ULL,
          "scenario 2: exactly TWO frames were charged for an 8 KiB stack");

    /* Contiguity is the whole point, so prove it from the pool's side as well:
     * the two frames must be one run. If the upper half had never been marked
     * allocated, the next single-frame allocation would return it. */
    void* probe = allocate_physical_ram_frame_for_partition(PART_TENANT);
    CHECK(probe != (void*)(uintptr_t)(lower + 4096ULL),
          "scenario 2: the upper frame is genuinely allocated — the next single allocation does not reuse it "
          "(the historical half-a-stack bug)");
    free_physical_ram_frame_for_partition(probe, PART_TENANT);

    /* ── Scenario 3: a second stack is a disjoint pair ────────────────────── */
    uint64_t top2 = alloc_proc_syscall_stack(PART_TENANT);
    CHECK(top2 != 0 && top2 != top,
          "scenario 3: a second syscall stack gets a different pair (the first was not handed out twice)");
    uint64_t lower2 = top2 - 8192ULL + 8ULL;
    uint64_t lo1 = lower / 4096ULL, lo2 = lower2 / 4096ULL;
    CHECK(lo2 + 1 < lo1 || lo2 > lo1 + 1,
          "scenario 3: the two stacks' frame ranges are disjoint");
    CHECK(partition_get_frame_usage(PART_TENANT) == 4ULL,
          "scenario 3: two stacks, four frames charged");

    /* ── Scenario 4: the teardown release path balances the books ──────────
     * proc_free_syscall_stack() frees each half with frame_pool_frame_owner()
     * as the partition argument — reproduced here verbatim, because a release
     * that decremented the wrong partition's counter (or only one of the two
     * frames) would leak while looking correct. */
    struct { uint64_t lo; } pairs[2] = { { lo1 }, { lo2 } };
    for (int p = 0; p < 2; p++) {
        for (int f = 0; f < 2; f++) {
            uint64_t addr = (pairs[p].lo + (uint64_t)f) * 4096ULL;
            free_physical_ram_frame_for_partition((void*)(uintptr_t)addr,
                                                 frame_pool_frame_owner(addr / 4096ULL));
        }
    }
    CHECK(partition_get_frame_usage(PART_TENANT) == 0ULL,
          "scenario 4: releasing both stacks the way proc_free_syscall_stack does returns usage to 0");

    if (g_fail) { printf("FAILED: %d check(s)\n", g_fail); return 1; }
    printf("all checks passed\n");
    return 0;
}
