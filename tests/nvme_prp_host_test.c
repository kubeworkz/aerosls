/*
 * nvme_prp_host_test.c — verification for drivers/nvme_io.c's
 * nvme_build_prp(), the PRP-address arithmetic behind multi-page NVMe
 * transfers, exercised as the REAL function (this file compiles
 * drivers/nvme_io.c itself) rather than a reimplementation.
 *
 * Why this function is separated out and tested while the rest of the
 * driver is not: nvme_io.c's submit path writes MMIO doorbells and polls a
 * hardware completion queue, so it cannot run on a host at all -- no test
 * in this suite has ever linked this file. PRP construction is the part
 * where a mistake is genuinely dangerous rather than merely broken: the
 * controller DMAs to whatever addresses the list names, so a wrong entry
 * silently scribbles over unrelated memory instead of failing loudly. It is
 * pure arithmetic over caller-supplied addresses, so it can be checked
 * exhaustively here, leaving the untestable remainder as thin as it already
 * was.
 *
 * Rather than link drivers/nvme_io.c (whose other functions pull in the
 * NVMe controller globals, the frame pool, and kernel_io), this file
 * #includes the .c directly with the few externs it needs stubbed first --
 * the same "#include the real .c" technique tests/scheduler_fairness_host_
 * test.c already uses for kernel/process.c, chosen for the identical reason
 * (the function under test is static-adjacent and the file's other
 * dependencies are irrelevant to it).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/nvme_prp_host_test tests/nvme_prp_host_test.c
 *   /tmp/nvme_prp_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ─── Stubs for the handful of externs nvme_io.c reaches for ────────────
 * None are touched by nvme_build_prp() itself; they exist so the
 * translation unit links. */
static uint8_t g_fake_frame[4096] __attribute__((aligned(4096)));
void* allocate_physical_ram_frame(void) { return g_fake_frame; }
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* nvme.h's controller global + the admin submit path nvme_io_init() calls.
 * nvme_build_prp() touches neither. */
#include "drivers/nvme.h"
#include "drivers/nvme_admin.h"
struct nvme_controller nvme_ctrl;
struct NVMeCqe nvme_submit_admin_cmd(struct NVMeCmd cmd) {
    (void)cmd; struct NVMeCqe c; memset(&c, 0, sizeof(c)); return c;
}

#include "drivers/nvme_io.c"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

#define PAGE 4096ULL

int main(void) {
    printf("=== nvme_build_prp: multi-page PRP construction ===\n\n");

    uint64_t list[512];
    uint64_t prp1, prp2;
    const uint64_t BUF = 0x200000ULL;   /* arbitrary, page-aligned */

    /* ── Single page: prp1 only, prp2 unused ───────────────────────────── */
    memset(list, 0xEE, sizeof(list));
    prp1 = prp2 = 0xDEAD;
    CHECK(nvme_build_prp(BUF, 1, list, &prp1, &prp2) == 0, "1 page: accepted");
    CHECK(prp1 == BUF, "1 page: prp1 is the buffer");
    CHECK(prp2 == 0, "1 page: prp2 is zero -- a single page needs no second entry");
    CHECK(list[0] == 0xEEEEEEEEEEEEEEEEULL,
          "1 page: the PRP list page is left completely untouched");

    /* ── Two pages: prp2 is the second page DIRECTLY, not a list ────────
     * This is the case most easily got wrong -- pointing prp2 at a list for
     * a 2-page transfer makes the controller read the list page's first 8
     * bytes as data. */
    memset(list, 0xEE, sizeof(list));
    CHECK(nvme_build_prp(BUF, 2, list, &prp1, &prp2) == 0, "2 pages: accepted");
    CHECK(prp1 == BUF, "2 pages: prp1 is the first page");
    CHECK(prp2 == BUF + PAGE,
          "2 pages: prp2 is the SECOND PAGE ITSELF, not a pointer to a list");
    CHECK(list[0] == 0xEEEEEEEEEEEEEEEEULL,
          "2 pages: still no list page used");

    /* ── Three pages: the smallest case that needs a real list ─────────── */
    memset(list, 0, sizeof(list));
    CHECK(nvme_build_prp(BUF, 3, list, &prp1, &prp2) == 0, "3 pages: accepted");
    CHECK(prp1 == BUF, "3 pages: prp1 is the first page");
    CHECK(prp2 == (uint64_t)(uintptr_t)list, "3 pages: prp2 points at the list page");
    CHECK(list[0] == BUF + 1 * PAGE, "3 pages: list[0] is the second page");
    CHECK(list[1] == BUF + 2 * PAGE, "3 pages: list[1] is the third page");
    CHECK(list[2] == 0, "3 pages: list[2] untouched -- exactly page_count-1 entries written");

    /* ── Max batch: every entry correct, none past the end ─────────────── */
    memset(list, 0, sizeof(list));
    CHECK(nvme_build_prp(BUF, NVME_MAX_PAGES_PER_XFER, list, &prp1, &prp2) == 0,
          "32 pages (the cap): accepted");
    int all_ok = 1;
    for (uint32_t i = 0; i + 1 < NVME_MAX_PAGES_PER_XFER; i++)
        if (list[i] != BUF + (uint64_t)(i + 1) * PAGE) all_ok = 0;
    CHECK(all_ok, "32 pages: all 31 list entries are consecutive page addresses");
    CHECK(list[NVME_MAX_PAGES_PER_XFER - 1] == 0,
          "32 pages: nothing written past entry 30 -- no off-by-one overrun into the next entry");
    int all_aligned = 1;
    for (uint32_t i = 0; i + 1 < NVME_MAX_PAGES_PER_XFER; i++)
        if (list[i] & (PAGE - 1)) all_aligned = 0;
    CHECK(all_aligned,
          "32 pages: every list entry is page-aligned -- NVMe requires zero offset on all but the first PRP");

    /* ── Rejections ─────────────────────────────────────────────────────
     * Each of these would be a silent memory-corruption bug if it were
     * accepted and handed to the controller. */
    CHECK(nvme_build_prp(BUF, 0, list, &prp1, &prp2) != 0,
          "rejects page_count 0");
    CHECK(nvme_build_prp(BUF, NVME_MAX_PAGES_PER_XFER + 1, list, &prp1, &prp2) != 0,
          "rejects page_count above the cap rather than silently truncating the transfer");
    CHECK(nvme_build_prp(BUF + 1, 4, list, &prp1, &prp2) != 0,
          "rejects an unaligned buffer -- a PRP list cannot describe one, so this must fail loudly");
    CHECK(nvme_build_prp(BUF + 2048, 2, list, &prp1, &prp2) != 0,
          "rejects a half-page-offset buffer even in the 2-page no-list case");
    CHECK(nvme_build_prp(BUF, 4, NULL, &prp1, &prp2) != 0,
          "rejects >2 pages with no list page supplied (the io_prp_list allocation-failed path)");
    CHECK(nvme_build_prp(BUF, 1, NULL, &prp1, &prp2) == 0,
          "...but 1 page with no list page is fine -- none is needed");
    CHECK(nvme_build_prp(BUF, 2, NULL, &prp1, &prp2) == 0,
          "...and so is 2 pages");
    CHECK(nvme_build_prp(BUF, 4, list, NULL, &prp2) != 0, "rejects a NULL out_prp1");
    CHECK(nvme_build_prp(BUF, 4, list, &prp1, NULL) != 0, "rejects a NULL out_prp2");

    /* ── The multi-page entry points refuse work when the queue is down ──
     * io_sq/io_cq are NULL here (nvme_io_init() was never called), which is
     * exactly the "NVMe MMIO above the 4 GiB identity map" boot this
     * driver's own comments describe. They must fail rather than ring a
     * doorbell at a bogus address. */
    static uint8_t aligned_buf[2 * 4096] __attribute__((aligned(4096)));
    CHECK(io_sq == 0 && io_cq == 0, "setup: I/O queues are down (nvme_io_init never called)");
    CHECK(nvme_write_pages_sync(0, aligned_buf, 2) != 0,
          "nvme_write_pages_sync fails cleanly when the I/O queue was never created");
    CHECK(nvme_read_pages_sync(0, aligned_buf, 2) != 0,
          "nvme_read_pages_sync fails cleanly too");
    CHECK(nvme_write_pages_sync(0, aligned_buf, 0) == 0,
          "a zero-page transfer is a successful no-op, not an error");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
