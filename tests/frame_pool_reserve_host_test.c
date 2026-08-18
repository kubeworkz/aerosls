/*
 * frame_pool_reserve_host_test.c — the allocator must not hand out the
 * kernel's own memory.
 *
 * ─── The bug this locks down ──────────────────────────────────────────
 * physical_memory_bitmap[] lives in .bss, so it boots all-zero: every
 * frame marked FREE, including the frames the kernel itself occupies.
 * Nothing reserved them. alloc_raw_frame() starts at physical frame 1 and
 * walks upward, and paging is an identity map for 0-4 GiB -- so the
 * caller's write went straight through the returned pointer into the
 * running image.
 *
 * With the real image ending at ~119.5 MiB (dominated by 117 MiB of
 * .bss), allocation #256 returned 0x100000: the kernel's own .text.
 *
 * It was found by linking the whole kernel for the first time and
 * comparing where the image ends against where the allocator starts. It
 * had been latent for the entire life of the project; the growth of .bss
 * across recent work widened it rather than caused it.
 *
 * ─── What this file asserts ───────────────────────────────────────────
 * Scenario 1 is a NEGATIVE CONTROL and is the reason the rest means
 * anything: it reproduces the unreserved allocator and shows it really
 * does return addresses inside the image. Without that, "allocations are
 * above the kernel now" would not distinguish a fix from a coincidence.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/frame_pool_reserve_host_test \
 *       tests/frame_pool_reserve_host_test.c kernel/frame_pool.c
 *   /tmp/frame_pool_reserve_host_test
 */
#include "kernel/frame_pool.h"
#include "kernel/partition.h"
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include "tests/process_host_stubs.h"   /* stack_bottom/stack_top (frame_pool_init reservation bounds) */

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* The real one is provided by arch/x86/linker.ld and its ADDRESS is the
 * value. A host test cannot place a symbol at a chosen address, which is
 * exactly why frame_pool_reserve_below() takes the boundary as an
 * argument -- this definition only exists to satisfy the link. */
char _kernel_image_end[1];

/* arch/x86/boot.asm exports these as the two ends of one 64 KiB region, an
 * adjacency the linker script establishes and that C cannot reproduce: two
 * separate objects have no guaranteed order or spacing. They exist here only
 * to satisfy the link -- the symbols themselves (stack_bottom/stack_top) now
 * come from tests/process_host_stubs.h, included above.
 *
 * So frame_pool_init() is deliberately NOT exercised by this test, and that is
 * a real gap stated rather than papered over -- a stub where stack_top happens
 * to land below stack_bottom would make frame_pool_reserve_range() take its
 * inverted-range early return and the test would pass having reserved nothing.
 * What IS tested is frame_pool_reserve_range() against explicit bounds, which
 * is the whole substance of the change; frame_pool_init() only supplies the
 * two addresses. The wiring itself is checked at runtime by the [FRAME] ERROR
 * line it prints when the image-end reservation fails to cover the stack. */

/* Mirrors the real image layout closely enough to be meaningful: the
 * measured link put the end at 0x7781000 (119.5 MiB). */
#define FAKE_IMAGE_END   0x7781000ULL
#define FAKE_END_FRAME   (FAKE_IMAGE_END / 4096)   /* 30593 */

int main(void) {
    printf("=== Frame allocator must not hand out the kernel image ===\n\n");
    printf("      (simulated image end 0x%llx = %.1f MiB = frame %llu)\n\n",
           (unsigned long long)FAKE_IMAGE_END, FAKE_IMAGE_END / 1048576.0,
           (unsigned long long)FAKE_END_FRAME);

    /* ═══ Scenario 1: THE NEGATIVE CONTROL ════════════════════════════
     * The unreserved allocator, exactly as it behaved before the fix. */
    printf("-- Scenario 1: unreserved, the allocator returns kernel memory --\n");
    {
        void* first = allocate_physical_ram_frame();
        CHECK(first == (void*)0x1000,
              "the very first allocation is physical 0x1000 -- the allocator starts at frame 1");

        /* Walk to the frame that lands on the image. 0x100000 is 1 MiB,
         * where the kernel is loaded. */
        void* f = 0;
        for (int i = 1; i < 256; i++) f = allocate_physical_ram_frame();
        CHECK(f == (void*)0x100000,
              "*** allocation #256 returns 0x100000 -- the kernel's own .text ***");

        /* And it keeps going, straight through the image. */
        for (int i = 0; i < 1000; i++) f = allocate_physical_ram_frame();
        CHECK((uint64_t)(uintptr_t)f < FAKE_IMAGE_END,
              "*** and a thousand more are all still inside the image ***");
        printf("      (allocation #1256 = 0x%llx, %.1f MiB into the kernel)\n",
               (unsigned long long)(uintptr_t)f, (uintptr_t)f / 1048576.0);
    }

    /* ═══ Scenario 2: reserved ════════════════════════════════════════ */
    printf("\n-- Scenario 2: after reserving, allocations start above the image --\n");
    {
        frame_pool_reset();          /* fresh bitmap, as at boot */
        CHECK(frame_pool_reserved_count() == 0, "a fresh pool has reserved nothing");

        frame_pool_reserve_below(FAKE_IMAGE_END);
        CHECK(frame_pool_reserved_count() == FAKE_END_FRAME,
              "reserving marks exactly every frame below the image end");

        void* first = allocate_physical_ram_frame();
        CHECK((uint64_t)(uintptr_t)first >= FAKE_IMAGE_END,
              "*** the first allocation is now ABOVE the kernel image ***");
        CHECK(first == (void*)FAKE_IMAGE_END,
              "...and is the very next frame, so nothing usable was wasted");
        printf("      (first allocation = 0x%llx)\n", (unsigned long long)(uintptr_t)first);

        /* Nothing below the boundary is ever returned, however many we take. */
        int below = 0;
        for (int i = 0; i < 5000; i++) {
            void* p = allocate_physical_ram_frame();
            if (p && (uint64_t)(uintptr_t)p < FAKE_IMAGE_END) below++;
        }
        CHECK(below == 0, "*** 5000 further allocations, none inside the image ***");
    }

    /* ═══ Scenario 3: the specific hazards below 1 MiB ════════════════
     * Reserving one contiguous span below the image end covers these by
     * construction, but they are the reason the span starts at 0 rather
     * than at 1 MiB, so they are asserted by name. */
    printf("\n-- Scenario 3: low-memory hazards are covered --\n");
    {
        frame_pool_reset();
        frame_pool_reserve_below(FAKE_IMAGE_END);
        struct { uint64_t addr; const char* what; } hazards[] = {
            { 0x00000, "frame 0 (NULL)" },
            { 0xB8000, "the VGA text buffer vga.c writes to" },
            { 0xA0000, "the start of the VGA framebuffer hole" },
            { 0xC0000, "BIOS ROM shadow -- not RAM at all" },
            { 0xFF000, "the last frame below 1 MiB" },
            { 0x100000,"the kernel load address" },
        };
        for (unsigned i = 0; i < sizeof(hazards)/sizeof(hazards[0]); i++)
            CHECK(frame_pool_is_reserved(hazards[i].addr / 4096),
                  hazards[i].what);
    }

    /* ═══ Scenario 4: bounding the top of RAM ═════════════════════════
     * The bitmap spans a fixed 4 GiB whatever the machine actually has. */
    printf("\n-- Scenario 4: memory the machine does not have --\n");
    {
        frame_pool_reset();
        frame_pool_reserve_below(FAKE_IMAGE_END);
        const uint64_t ram_top = 256ULL * 1024 * 1024;   /* a 256 MiB machine */
        frame_pool_limit_ram(ram_top);

        CHECK(!frame_pool_is_reserved((ram_top - 4096) / 4096),
              "the last real frame is still available");
        CHECK(frame_pool_is_reserved(ram_top / 4096),
              "*** the first frame past the end of RAM is reserved ***");
        CHECK(frame_pool_is_reserved((3ULL * 1024 * 1024 * 1024) / 4096),
              "...as is a frame at 3 GiB on a 256 MiB machine");

        int outside = 0;
        for (int i = 0; i < 20000; i++) {
            void* p = allocate_physical_ram_frame();
            if (!p) break;
            uint64_t a = (uint64_t)(uintptr_t)p;
            if (a < FAKE_IMAGE_END || a >= ram_top) outside++;
        }
        CHECK(outside == 0,
              "*** every allocation lands between the image end and the end of RAM ***");

        frame_pool_reset();
        frame_pool_limit_ram(0);
        CHECK(frame_pool_reserved_count() == 0,
              "a limit of 0 means 'unknown' and reserves nothing -- it does not lock the pool");
    }

    /* ═══ Scenario 5: rounding and idempotence ════════════════════════ */
    printf("\n-- Scenario 5: rounding and idempotence --\n");
    {
        frame_pool_reset();
        /* An image ending part-way through a frame still occupies it. */
        frame_pool_reserve_below(0x100001ULL);
        CHECK(frame_pool_is_reserved(0x100000 / 4096),
              "*** a partially-occupied final frame is reserved, not rounded away ***");
        CHECK(frame_pool_reserved_count() == 0x101,
              "...rounding UP by exactly one frame");

        uint64_t before = frame_pool_reserved_count();
        frame_pool_reserve_below(0x100001ULL);
        CHECK(frame_pool_reserved_count() == before,
              "reserving twice is idempotent -- the count does not double");

        frame_pool_reset();
        frame_pool_reserve_below(0);
        CHECK(frame_pool_reserved_count() == 0,
              "an end address of 0 reserves nothing rather than wrapping");
    }

    /* ─── A reservation is not an allocation ───────────────────────────────
     * frame_owner[] is a uint8_t and PARTITION_MAX is 256, so every value it
     * can hold names a real partition and there is no spare tag for "the
     * machine owns this." Boot reservations therefore leave the default 0,
     * which reads as PARTITION_SYSTEM, and fp_mark_used() sets the bitmap bit
     * -- so a reserved kernel frame satisfies BOTH of the conditions
     * partition_reclaim_all_frames() uses to decide a frame is free-able.
     *
     * Reclaiming PARTITION_SYSTEM would hand back the kernel's own .text,
     * .data, .bss, page tables and 64 KiB bootstrap stack. Both call sites
     * refuse partition 0 today, so this is a landmine rather than a live bug,
     * and this test is what keeps it defused if a third call site appears. */
    {
        frame_pool_reset();
        frame_pool_reserve_below(64 * 1024);      /* frames 0..15 = "the kernel" */
        frame_pool_limit_ram(1024 * 1024);        /* frames 256.. = "no RAM there" */

        uint64_t reserved_before = frame_pool_reserved_count();
        CHECK(frame_pool_frame_is_machine_owned(0) &&
              frame_pool_frame_is_machine_owned(15),
              "*** frames inside the kernel image are machine-owned ***");
        CHECK(!frame_pool_frame_is_machine_owned(16) &&
              !frame_pool_frame_is_machine_owned(255),
              "...allocatable frames between the two watermarks are not");
        CHECK(frame_pool_frame_is_machine_owned(256) &&
              frame_pool_frame_is_machine_owned(1000),
              "*** and so is memory the machine does not physically have ***");

        /* Hand one real frame to a tenant so the reclaim has honest work. */
        void* tenant = allocate_physical_ram_frame_for_partition(7);
        CHECK(tenant != 0, "a tenant frame was allocated from the free span");
        uint64_t tenant_idx = (uint64_t)(uintptr_t)tenant / FRAME_SIZE;

        uint32_t freed = partition_reclaim_all_frames(PARTITION_SYSTEM);
        CHECK(freed == 0,
              "*** reclaiming PARTITION_SYSTEM frees NOTHING when it owns nothing -- "
              "the boot reservation is not its allocation ***");
        CHECK(frame_pool_reserved_count() == reserved_before,
              "...and the reserved count is unchanged");
        CHECK(frame_pool_is_reserved(0) && frame_pool_is_reserved(15),
              "*** the kernel image is STILL reserved after the reclaim ***");
        CHECK(frame_pool_is_reserved(256) && frame_pool_is_reserved(1000),
              "...and so is the nonexistent memory above the RAM top");

        /* The assertion that actually matters: the allocator must not now be
         * able to hand out the kernel. Drain a few frames and check every one
         * came from the free span. */
        int handed_out_kernel = 0;
        for (int i = 0; i < 32; i++) {
            void* f = allocate_physical_ram_frame();
            if (!f) break;
            uint64_t idx = (uint64_t)(uintptr_t)f / FRAME_SIZE;
            if (frame_pool_frame_is_machine_owned(idx)) handed_out_kernel = 1;
        }
        CHECK(!handed_out_kernel,
              "*** after reclaiming PARTITION_SYSTEM the allocator still never returns "
              "a frame from the kernel image or from absent RAM ***");

        /* A real tenant reclaim must still work -- the guard must not have
         * turned reclamation into a no-op across the board. */
        CHECK(!frame_pool_frame_is_machine_owned(tenant_idx),
              "the tenant's frame is in the allocatable span");
        CHECK(partition_reclaim_all_frames(7) == 1,
              "*** reclaiming a real tenant still frees exactly its frame ***");
        CHECK(!frame_pool_is_reserved(tenant_idx),
              "...and that frame really is back in the pool");
    }

    /* ─── Seed Kernel Phase 2: the contiguous reservation (arena) must not
     * make the ALLOCATABLE frames below it machine-owned ───────────────────
     * Historical bug: frame_pool_reserve_contiguous() folded its range into
     * the reserved_below watermark, which then covered every frame between
     * the image and the arena -- and this kernel's processes and streams
     * all live there -- so partition_reclaim_all_frames() skipped them all
     * and every partition destroy leaked. The reservation must be tracked
     * as its OWN range: the arena frames are machine-owned, the frames
     * below it are not, and reclaiming a tenant below the arena works. */
    {
        frame_pool_reset();
        frame_pool_reserve_below(64 * 1024);        /* frames 0..15 = "the kernel" */
        frame_pool_limit_ram(1024 * 1024);          /* frames 256.. = "no RAM there" */
        /* Carve a 16-frame contiguous reservation at frame 64 (2 MiB-
         * aligned in real boots; any power-of-two align works here) -- the
         * cap arena's carve. */
        uint64_t reserved_before_arena = frame_pool_reserved_count();
        uint64_t arena = frame_pool_reserve_contiguous(16, 64);
        CHECK(arena == 64 * 4096,
              "the contiguous reservation lands at frame 64");
        CHECK(frame_pool_frame_is_machine_owned(64) &&
              frame_pool_frame_is_machine_owned(79),
              "*** the contiguous reservation itself is machine-owned ***");
        CHECK(!frame_pool_frame_is_machine_owned(16) &&
              !frame_pool_frame_is_machine_owned(63),
              "*** frames between the image and the reservation are NOT "
              "machine-owned -- the historical bug marked them so ***");
        CHECK(frame_pool_reserved_count() == reserved_before_arena + 16,
              "the contiguous reservation added exactly its 16 frames to the "
              "reserved count");

        /* A real tenant allocation below the arena must be reclaimable. */
        void* tenant = allocate_physical_ram_frame_for_partition(9);
        CHECK(tenant != 0, "a tenant frame was allocated");
        uint64_t t_idx = (uint64_t)(uintptr_t)tenant / FRAME_SIZE;
        CHECK(t_idx < 64,
              "the allocator handed out a frame BELOW the arena (the range "
              "the historical watermark wrongly machine-owned)");
        CHECK(frame_pool_frame_is_machine_owned(t_idx) == 0,
              "*** the tenant's own frame is not machine-owned ***");
        CHECK(partition_reclaim_all_frames(9) == 1,
              "*** reclaiming partition 9 frees the frame below the arena -- "
              "the watermark fix ***");
        CHECK(!frame_pool_is_reserved(t_idx),
              "...and that frame is truly back in the pool");

        /* The reservation itself must survive the reclaim untouched. */
        CHECK(frame_pool_is_reserved(64) && frame_pool_is_reserved(79),
              "*** the contiguous reservation is still reserved after the "
              "reclaim ***");
        CHECK(frame_pool_reserved_count() == reserved_before_arena + 16,
              "...and the reserved count is unchanged by the tenant reclaim "
              "(frames_reserved tracks reservations only)");
    }

    /* ─── Reserving the stack by its own bounds ────────────────────────────
     * frame_pool_init() reserves below _kernel_image_end and then reserves
     * [stack_bottom, stack_top) AGAIN. The second reservation is redundant
     * exactly as long as the linker script keeps .bootstrap_stack inside .bss
     * below the image end -- a property of one line in one file that nothing
     * verified, whose cost when wrong is the allocator handing out the memory
     * the kernel is running on. */
    {
        frame_pool_reset();
        /* A stack deliberately placed ABOVE the "image end", i.e. the layout
         * the redundant reservation exists to survive. */
        frame_pool_reserve_below(64 * 1024);              /* frames 0..15  */
        frame_pool_reserve_range(128 * 1024, 192 * 1024); /* frames 32..47 */

        CHECK(frame_pool_is_reserved(32) && frame_pool_is_reserved(47),
              "*** an explicit range reserves every frame it overlaps ***");
        CHECK(!frame_pool_is_reserved(31) && !frame_pool_is_reserved(48),
              "...and nothing outside it");

        /* Partial frames at either end are still occupied. */
        frame_pool_reset();
        frame_pool_reserve_range(4096 * 5 + 100, 4096 * 7 + 1);
        CHECK(frame_pool_is_reserved(5) && frame_pool_is_reserved(6) &&
              frame_pool_is_reserved(7),
              "*** a range starting mid-frame and ending mid-frame reserves both "
              "partial frames, not just the whole ones between ***");
        CHECK(!frame_pool_is_reserved(4) && !frame_pool_is_reserved(8),
              "...and stops there");

        frame_pool_reset();
        frame_pool_reserve_range(8192, 8192);
        CHECK(frame_pool_reserved_count() == 0, "an empty range reserves nothing");
        frame_pool_reserve_range(16384, 8192);
        CHECK(frame_pool_reserved_count() == 0,
              "*** an inverted range spanning frames reserves nothing rather than "
              "wrapping to a gigantic loop ***");
        /* The case the loop bound does NOT cover on its own: inverted, but both
         * ends inside one frame, so rounding gives first == 5, last == 6 and a
         * range describing no memory would reserve a frame. */
        frame_pool_reserve_range(4096 * 5 + 100, 4096 * 5 + 50);
        CHECK(frame_pool_reserved_count() == 0,
              "*** an inverted range WITHIN a single frame reserves nothing -- the "
              "early return is load-bearing, not decorative ***");
    }

    /* ─── Never hand out the frame we are standing on ──────────────────────
     * The last line of defence. If the reservations are right this is dead
     * code; when they are not, it is the only thing between a tenant's upload
     * and the kernel's own return addresses -- which is precisely the failure
     * that produced 1760 bytes of payload where the live stack used to be. */
    {
        CHECK(fp_frame_contains(0x1000, 0x1000) &&
              fp_frame_contains(0x1000, 0x1FFF),
              "*** a frame contains its own first and last byte ***");
        CHECK(!fp_frame_contains(0x1000, 0x0FFF) &&
              !fp_frame_contains(0x1000, 0x2000),
              "*** ...and neither the byte below nor the first byte of the next "
              "frame -- both off-by-ones would misreport which frame holds the stack ***");
        CHECK(!fp_frame_contains(0x1000, 0),
              "a zero address matches nothing, so a platform that cannot read its "
              "stack pointer disables the check instead of withholding frame 0");

        /* The real assertion, and it needs the seam. The fake allocator returns
         * addresses derived from the frame index while the host's actual rsp is
         * far above them, so checking against the true stack pointer can never
         * fail -- an earlier version of this did exactly that and a mutation
         * deleting the guard sailed through. Point the allocator at an address
         * inside a frame it is about to hand out, and the guard becomes
         * reachable. */
        frame_pool_reset();
        frame_pool_reserve_below(0);          /* nothing reserved: worst case */
        uint64_t pretend_sp = 3 * 4096 + 0x100;      /* inside frame 3 */
        frame_pool_test_sp_override = pretend_sp;
        uint64_t withheld_before = frame_pool_live_stack_withheld;

        int handed_out_own_stack = 0, saw_frame_2 = 0, saw_frame_4 = 0;
        for (int i = 0; i < 8; i++) {
            void* f = allocate_physical_ram_frame();
            if (!f) break;
            uint64_t base = (uint64_t)(uintptr_t)f;
            if (fp_frame_contains(base, pretend_sp)) handed_out_own_stack = 1;
            if (base == 2 * 4096) saw_frame_2 = 1;
            if (base == 4 * 4096) saw_frame_4 = 1;
        }
        frame_pool_test_sp_override = 0;

        CHECK(!handed_out_own_stack,
              "*** with NOTHING reserved, the allocator still never returns the frame "
              "holding the live stack ***");
        CHECK(frame_pool_live_stack_withheld == withheld_before + 1,
              "*** ...it counted the withholding, so the refusal is reported and not "
              "silent ***");
        CHECK(saw_frame_2 && saw_frame_4,
              "*** ...and it skipped ONLY that frame -- the neighbours on both sides "
              "were still handed out, so the guard is surgical rather than a stall ***");
        CHECK(frame_pool_is_reserved(3),
              "...the withheld frame stays marked used, so the scan does not return "
              "to it on the next allocation and spin");
    }

    /* ─── "No error at boot" made assertable ───────────────────────────────
     * The run that finally succeeded printed neither a WITHHELD line nor a
     * [FRAME] ERROR line -- which is exactly as consistent with "both guards
     * are healthy" as with "neither ran". An absence cannot tell those apart,
     * so the state is now a value. */
    {
        frame_pool_reset();
        /* Frame 0 deliberately reserved. Without that the uninitialised
         * lo == hi == 0 sweep would check frame 0, find it free and return
         * "not reserved" for the wrong reason -- and a mutation deleting the
         * has-init-run guard passed a 54-check suite exactly that way. With
         * frame 0 reserved, only the guard can produce the right answer. */
        frame_pool_reserve_below(64 * 1024);
        CHECK(frame_pool_is_reserved(0), "frame 0 is reserved, so the sweep would say yes");
        CHECK(!frame_pool_stack_still_reserved(),
              "*** before init has run, the predicate reports NOT reserved rather "
              "than vacuously true -- an unrun check must not read as a pass ***");

        /* Stand in for what frame_pool_init() does with the real symbols. */
        frame_pool_stack_lo_frame = 4;
        frame_pool_stack_hi_frame = 15;
        CHECK(frame_pool_stack_still_reserved(),
              "*** with the stack frames reserved, it reports reserved ***");

        /* The detection that matters: something clears one of them later. */
        frame_pool_reset();
        frame_pool_reserve_range(4 * 4096, 16 * 4096);
        frame_pool_stack_lo_frame = 4;
        frame_pool_stack_hi_frame = 15;
        CHECK(frame_pool_stack_still_reserved(), "...still reserved after an explicit range");
        free_physical_ram_frame((void*)(uintptr_t)(9 * 4096));  /* un-reserve one, mid-range */
        CHECK(!frame_pool_stack_still_reserved(),
              "*** freeing ONE frame in the middle of the stack is detected -- the "
              "check covers every frame, not just the ends ***");

        frame_pool_reset();
        frame_pool_reserve_range(4 * 4096, 16 * 4096);
        frame_pool_stack_lo_frame = 4; frame_pool_stack_hi_frame = 15;
        free_physical_ram_frame((void*)(uintptr_t)(15 * 4096)); /* the last one */
        CHECK(!frame_pool_stack_still_reserved(),
              "...including the topmost frame, which an exclusive bound would miss");

        /* State must not leak across a reset. Every other check in this block
         * sets the bounds itself immediately after resetting, so a reset that
         * left them stale would never show -- which is how the mutation
         * deleting that line survived. Here the bounds are set BEFORE the
         * reset and never re-set, so only a reset that clears them gives the
         * right answer. */
        frame_pool_reserve_range(4 * 4096, 16 * 4096);
        frame_pool_stack_lo_frame = 4; frame_pool_stack_hi_frame = 15;
        CHECK(frame_pool_stack_still_reserved(), "bounds set and frames reserved");
        frame_pool_reset();
        frame_pool_reserve_below(64 * 1024);   /* frames 0..15 reserved again */
        CHECK(frame_pool_is_reserved(4) && frame_pool_is_reserved(15),
              "...and after a reset those same frames are reserved once more");
        CHECK(!frame_pool_stack_still_reserved(),
              "*** but the predicate still reports NOT reserved: reset cleared the "
              "bounds, so this is a fresh pool whose init has not run rather than "
              "the previous pool's answer ***");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
