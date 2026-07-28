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

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
