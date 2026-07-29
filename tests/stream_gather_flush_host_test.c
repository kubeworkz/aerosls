/*
 * stream_gather_flush_host_test.c — verification for kernel/stream.c's
 * gather-batched stream flush, linked against the REAL, unmodified
 * kernel/stream.c.
 *
 * stream_write_chunk()'s is_last flush used to issue one 4 KiB NVMe command
 * per frame -- up to STREAM_MAX_FRAMES (16,384) synchronous submit-and-poll
 * round trips for a single 64 MiB stream. A stream's pages are separately
 * allocated frame-pool frames, so they are scattered in memory and the
 * contiguous multi-page path cannot describe them; a PRP list, however, is
 * natively a scatter list, so nvme_write_pages_gather_sync() describes them
 * to the controller directly with no copying.
 *
 * ─── Why this test is not optional ──────────────────────────────────────
 * Batching a loop that writes to computed LBAs is exactly the kind of change
 * where an off-by-one does not crash -- it silently writes a frame to the
 * wrong disk address, and the damage only surfaces when someone reads the
 * stream back. The first draft of this batching had precisely that bug: the
 * frame that filled a batch was flushed with the batch AND re-seeded into the
 * next run, so it was written twice and every later frame landed one slot too
 * early. It was caught by writing this test.
 *
 * So the assertions here are about WHERE bytes land, not just how many
 * commands were issued: every scenario reconstructs the stream from the fake
 * disk and compares it against what was written, frame by frame, at the exact
 * LBAs stream.c computed.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/stream_gather_flush_host_test \
 *       tests/stream_gather_flush_host_test.c kernel/stream.c
 *   /tmp/stream_gather_flush_host_test
 */
#include "kernel/stream.h"
#include "kernel/object_catalog.h"
#include "drivers/nvme_io.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Stubs for stream.c's dependencies ─────────────────────────────────── */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
uint64_t sys_sls_valloc(struct SLSVallocRequest* r) { (void)r; return 0; }
uint64_t sys_sls_insert(struct SLSRecordRequest* r) { (void)r; return 0; }
uint64_t sys_sls_update(struct SLSRecordRequest* r) { (void)r; return 0; }
uint32_t partition_get_for_uid(uint32_t uid) { (void)uid; return 0; }
void* allocate_physical_ram_frame_for_partition(uint32_t p) { (void)p; return 0; }
void stream_persist_directory(void);
uint32_t cluster_local_node_id(void) { return 0; }
void dspp_migrate_send_begin(uint64_t t, uint32_t d, uint32_t p, const char* n,
                             const char* m, uint64_t s, uint32_t f, uint32_t o) {
    (void)t;(void)d;(void)p;(void)n;(void)m;(void)s;(void)f;(void)o;
}
void dspp_migrate_send_page(uint64_t t, uint32_t d, uint32_t p, uint32_t i,
                            const uint8_t* pg) { (void)t;(void)d;(void)p;(void)i;(void)pg; }

/* ─── Fake NVMe that records WHERE each page landed ─────────────────────── */
#define FAKE_MAX 4096
static struct { uint64_t lba; uint8_t data[4096]; int used; } disk[FAKE_MAX];
static uint32_t g_cmds = 0;      /* NVMe commands issued */
static uint32_t g_pages = 0;     /* 4 KiB pages placed */

void* io_sq = (void*)1;
void* io_cq = (void*)1;

static int slot(uint64_t lba) {
    for (int i = 0; i < FAKE_MAX; i++) if (disk[i].used && disk[i].lba == lba) return i;
    for (int i = 0; i < FAKE_MAX; i++) if (!disk[i].used) { disk[i].used = 1; disk[i].lba = lba; return i; }
    return -1;
}
static int put_page(uint64_t lba, const void* buf) {
    int s = slot(lba); if (s < 0) return 1;
    memcpy(disk[s].data, buf, 4096); g_pages++; return 0;
}
int nvme_write_sync(uint64_t lba, const void* buf) { g_cmds++; return put_page(lba, buf); }
int nvme_read_sync(uint64_t lba, void* buf) {
    for (int i = 0; i < FAKE_MAX; i++)
        if (disk[i].used && disk[i].lba == lba) { memcpy(buf, disk[i].data, 4096); return 0; }
    return 1;
}
int nvme_write_pages_sync(uint64_t slba, const void* buf, uint32_t n) {
    g_cmds++;
    for (uint32_t i = 0; i < n; i++)
        if (put_page(slba + (uint64_t)i*8, (const uint8_t*)buf + (size_t)i*4096)) return 1;
    return 0;
}
int nvme_read_pages_sync(uint64_t slba, void* buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (nvme_read_sync(slba + (uint64_t)i*8, (uint8_t*)buf + (size_t)i*4096)) return 1;
    return 0;
}
/* The one under test: honours the gather list, one command, pages laid down
 * at consecutive LBAs from slba. */
int nvme_write_pages_gather_sync(uint64_t slba, const void* const* pages, uint32_t n) {
    if (n == 0) return 0;
    if (n > NVME_MAX_PAGES_PER_XFER) return 1;   /* mirrors the real cap */
    g_cmds++;
    for (uint32_t i = 0; i < n; i++) {
        if (!pages[i]) return 1;
        if (put_page(slba + (uint64_t)i*8, pages[i])) return 1;
    }
    return 0;
}
int nvme_read_pages_gather_sync(uint64_t slba, void* const* pages, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (nvme_read_sync(slba + (uint64_t)i*8, pages[i])) return 1;
    return 0;
}
int nvme_flush_sync(void) { return 0; }

/* ─── Helpers ───────────────────────────────────────────────────────────── */
static uint8_t* make_frame(uint8_t fill) {
    uint8_t* f = NULL;
    if (posix_memalign((void**)&f, 4096, 4096) != 0) return NULL;
    memset(f, fill, 4096);
    return f;
}
/* Reads back the frame stream.c should have placed for index fi and checks
 * its fill byte -- i.e. that this frame landed at THIS LBA and no other. */
static int frame_at(struct StreamEntry* se, uint32_t fi, uint8_t expect) {
    uint8_t buf[4096];
    if (nvme_read_sync(se->lba_base + (uint64_t)fi * 8, buf) != 0) return 0;
    for (int i = 0; i < 4096; i++) if (buf[i] != expect) return 0;
    return 1;
}
static void reset_disk(void) { memset(disk, 0, sizeof(disk)); g_cmds = 0; g_pages = 0; }

/* Drives the real flush by calling stream_write_chunk() with is_last -- but
 * the frames are placed directly, because allocate_physical_ram_frame_for_
 * partition() is stubbed out (this test is about the flush, not allocation). */
static struct StreamEntry* prep(uint32_t nframes, int hole_at, uint8_t base) {
    struct StreamEntry* se = &stream_store[0];
    memset(se, 0, sizeof(*se));
    se->active = 1;
    se->lba_base = STREAM_DATA_LBA_BASE;
    se->frames_used = nframes;
    for (uint32_t i = 0; i < nframes; i++)
        se->frames[i] = ((int)i == hole_at) ? NULL : make_frame((uint8_t)(base + (i & 0x3F)));
    return se;
}

/* ─── Retransmission stubs ────────────────────────────────────────────────
 * stream_migrate_send_partition() now waits for a per-page PAGE_ACK and
 * abandons the transfer -- leaving the source intact -- if none arrives.
 *
 * FAITHFUL as "always acknowledged", not merely convenient: this file does
 * not link net/dspp.c and has no wire, so modelling a silent peer would make
 * every send here abandon and turn this into an accidental test of the
 * give-up path. Loss, retransmission and give-up are covered directly by
 * tests/cross_node_migration_host_test.c against a real lossy destination. */
void dspp_migrate_send_frag(uint64_t t, uint32_t n, uint32_t p, uint32_t pg,
                            uint32_t f, const uint8_t* d) {
    (void)t; (void)n; (void)p; (void)pg; (void)f; (void)d;
}
void dspp_migrate_arm_page(uint64_t t, uint32_t p) { (void)t; (void)p; }
void dspp_migrate_arm_begin(uint64_t t) { (void)t; }
int  dspp_migrate_begin_acked(void) { return 1; }   /* FAITHFUL: see the note above */
int  dspp_migrate_page_acked(void) { return 1; }
int  dspp_migrate_frag_acked(uint32_t f) { (void)f; return 1; }
int  dspp_migrate_nacked(void)     { return 0; }
void dspp_migrate_disarm(void)     { }

/* The ACK wait times against the LAPIC tick. Nothing waits here (the stub
 * above acknowledges at once), so a frozen clock is faithful. */
volatile uint64_t kernel_tick_counter = 0;

int main(void) {
    printf("=== stream flush: scatter-gather batching ===\n\n");

    /* ── Scenario 1: a short contiguous run ─────────────────────────────── */
    reset_disk();
    struct StreamEntry* se = prep(5, -1, 0x10);
    stream_flush_frames(se);
    int all = 1;
    for (uint32_t i = 0; i < 5; i++) if (!frame_at(se, i, (uint8_t)(0x10 + i))) all = 0;
    CHECK(all, "5 contiguous frames each land at their own LBA");
    CHECK(g_cmds == 1, "...in exactly ONE gather command, not five");

    /* ── Scenario 2: exactly the batch limit ────────────────────────────── */
    reset_disk();
    se = prep(NVME_MAX_PAGES_PER_XFER, -1, 0x20);
    stream_flush_frames(se);
    all = 1;
    for (uint32_t i = 0; i < NVME_MAX_PAGES_PER_XFER; i++)
        if (!frame_at(se, i, (uint8_t)(0x20 + (i & 0x3F)))) all = 0;
    CHECK(all, "a run of exactly NVME_MAX_PAGES_PER_XFER frames lands correctly");
    CHECK(g_cmds == 1, "...in one command (it fits exactly)");

    /* ── Scenario 3: THE regression -- one past the batch limit ──────────
     * This is the case the first draft got wrong: the frame that filled the
     * batch was written twice and everything after it shifted one LBA early.
     * Checking placement (not command count) is what catches that. */
    reset_disk();
    uint32_t n = NVME_MAX_PAGES_PER_XFER + 1;
    se = prep(n, -1, 0x30);
    stream_flush_frames(se);
    all = 1;
    int firstbad = -1;
    for (uint32_t i = 0; i < n; i++)
        if (!frame_at(se, i, (uint8_t)(0x30 + (i & 0x3F)))) { all = 0; if (firstbad < 0) firstbad = (int)i; }
    CHECK(all, "REGRESSION GUARD: 33 frames (one past the batch limit) each land at the correct LBA -- no frame written twice, nothing shifted");
    if (!all) printf("      (first misplaced frame: %d)\n", firstbad);
    CHECK(g_cmds == 2, "...split across exactly two commands");
    CHECK(g_pages == n, "...and exactly 33 pages were placed, not 34");

    /* ── Scenario 4: a hole splits the run ───────────────────────────────
     * A NULL frame must END the run, not be skipped over: writing across a
     * hole would shift every later frame onto the wrong LBA. */
    reset_disk();
    se = prep(6, 3, 0x40);
    stream_flush_frames(se);
    all = 1;
    for (uint32_t i = 0; i < 6; i++) {
        if (i == 3) continue;                       /* the hole */
        if (!frame_at(se, i, (uint8_t)(0x40 + i))) all = 0;
    }
    CHECK(all, "a hole splits the run and every surrounding frame still lands at its own LBA");
    CHECK(g_cmds == 2, "...as two commands, one per side of the hole");
    uint8_t tmp[4096];
    CHECK(nvme_read_sync(se->lba_base + 3 * 8, tmp) != 0,
          "...and nothing at all was written where the hole is");

    /* ── Scenario 5: many batches ────────────────────────────────────────── */
    reset_disk();
    n = NVME_MAX_PAGES_PER_XFER * 3 + 7;
    se = prep(n, -1, 0x50);
    stream_flush_frames(se);
    all = 1;
    for (uint32_t i = 0; i < n; i++)
        if (!frame_at(se, i, (uint8_t)(0x50 + (i & 0x3F)))) all = 0;
    CHECK(all, "103 frames across four batches all land correctly");
    CHECK(g_cmds == 4, "...in four commands rather than 103");
    printf("      (%u frames flushed in %u commands = %.1fx fewer round trips)\n",
           n, g_cmds, (double)n / (double)g_cmds);

    /* ── Scenario 6: nothing to flush ────────────────────────────────────── */
    reset_disk();
    se = prep(0, -1, 0x60);
    stream_flush_frames(se);
    CHECK(g_cmds == 0, "an empty stream issues no commands at all");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
