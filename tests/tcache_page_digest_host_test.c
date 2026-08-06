/*
 * tcache_page_digest_host_test.c — the page-digest layer, linked against the
 * REAL, unmodified kernel/qemu_sls_tcache.c.
 *
 * ─── What this guards, and why it is not a style test ──────────────────────
 * The digest layer exists so a restored translation cache survives its own
 * loader. sls-launcher.c rewrites the guest image into guest RAM on every
 * launch; guest RAM is not persisted while the cache is, so on a fresh boot
 * the loader used to find every page different, copy it, and flush -- which
 * invalidated every restored TB. Measured across four boots: one flush of
 * page 0 per boot, generation climbing 2, 3, 4, TCACHE 0 hit / 8 miss on
 * every first launch after a reboot.
 *
 * The fix lets the loader SKIP that flush when the bytes it is about to write
 * are the ones the cached TBs were compiled from. That is a loosening of a
 * cache-validity check, and it is the dangerous direction: a wrong "yes" here
 * does not fail a test or raise a fault. It jumps into host machine code
 * compiled against assumptions that no longer hold.
 *
 * So the assertions below are mostly about when the answer must be NO.
 *
 * ─── The invariant that bounds the whole thing ─────────────────────────────
 * qemu_sls_tcache_flush_page() clears the digest for the page it flushes.
 * Every invalidation that does not come from the loader -- a guest store
 * caught by qemu_sls_mmu_shadow_fault(), a DMA completion -- therefore drops
 * the digest, and the next load cannot match against it. Only a page whose
 * digest the loader itself recorded, AFTER its own flush, is ever skipped.
 * If exactly one assertion in this file is worth keeping, it is that one.
 *
 * ─── Mutation resistance, measured rather than asserted ────────────────────
 * Eight mutations were applied to kernel/qemu_sls_tcache.c and this file run
 * against each. Six were killed:
 *
 *   page_matches always returns 1        killed (3 assertions fail)
 *   page_matches always returns 0        killed (6 fail)
 *   flush_page does not clear the digest killed (1 fail -- the invariant)
 *   sync does not write the digest table killed (1 fail)
 *   init does not read the digest table  killed (1 fail)
 *   slot key drops the +1 on page number killed (2 fail -- page 0)
 *
 * Two survived, and neither is a gap to paper over:
 *
 *   "digest ignores length" -- the length term in digest_bytes() is defence
 *   in depth, not load-bearing. FNV over a prefix already differs from FNV
 *   over the full buffer, so the truncation assertion below passes with or
 *   without it. The implementation comment was overstating this and has been
 *   corrected; inventing an artificial assertion to kill the mutant would
 *   have been worse than recording the truth.
 *
 *   "match ignores the cleared-digest short circuit" -- an EQUIVALENT mutant.
 *   Dropping `!d->digest` still returns 0, because the comparison then reads
 *   `0 == digest_bytes(...)` and digest_bytes never returns 0 (`h ? h : 1`).
 *   The short circuit is redundant *given that guard*, and would become
 *   load-bearing the moment someone removed it. That coupling is the thing
 *   worth knowing, and no test can express it from outside the file.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -DAEROSLS_BUILD_ID='"aerosls-digest-test"' \
 *       -o /tmp/tcache_page_digest_host_test \
 *       tests/tcache_page_digest_host_test.c kernel/qemu_sls_tcache.c
 *   /tmp/tcache_page_digest_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "kernel/qemu_sls_tcache.h"

/* ─── stubs for everything qemu_sls_tcache.c reaches outside itself ─────────
 * A backing store rather than no-ops, so the sync -> init round trip below is
 * a real round trip: if the digest table were written to the wrong LBA, or
 * not read back at all, these assertions would still pass against no-ops and
 * fail against this.
 *
 * Covers LBA 10000 (header) .. 19128 (end of the digest table), which is
 * every region the tcache touches. */
#define FAKE_LBA_BASE   10000ULL
#define FAKE_LBA_COUNT  9128ULL           /* through QEMU_TCACHE_PDIG end */
static uint8_t *disk;                     /* FAKE_LBA_COUNT * 512 bytes */

/* Non-NULL, and it matters. qemu_sls_tcache_sync() and flush_page() both
 * early-return on `!io_sq || !io_cq`, so with NULL queues the persistence
 * assertion below would be checking a function that returned immediately and
 * wrote nothing -- passing or failing for reasons having nothing to do with
 * digests. They only need to be non-NULL; the fake disk above is what the
 * I/O actually lands in. */
static int fake_queue_sq, fake_queue_cq;
void *io_sq = &fake_queue_sq;
void *io_cq = &fake_queue_cq;

static uint8_t *disk_at(uint64_t slba) {
    if (slba < FAKE_LBA_BASE || slba >= FAKE_LBA_BASE + FAKE_LBA_COUNT) return NULL;
    return disk + (slba - FAKE_LBA_BASE) * 512;
}

int nvme_read_sync(uint64_t slba, void *buf) {
    uint8_t *p = disk_at(slba);
    if (!p) return -1;
    memcpy(buf, p, 4096);
    return 0;
}
int nvme_write_sync(uint64_t slba, const void *buf) {
    uint8_t *p = disk_at(slba);
    if (!p) return -1;
    memcpy(p, buf, 4096);
    return 0;
}
int nvme_read_pages_sync(uint64_t slba, void *buf, uint32_t page_count) {
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_read_sync(slba + i * 8, (uint8_t *)buf + i * 4096) != 0) return -1;
    return 0;
}
int nvme_write_pages_sync(uint64_t slba, const void *buf, uint32_t page_count) {
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_write_sync(slba + i * 8, (const uint8_t *)buf + i * 4096) != 0) return -1;
    return 0;
}
int nvme_flush_sync(void) { return 0; }

/* qemu_sls_tcache_insert() marks the TB region dirty for the delta
 * checkpointer. Not under test here, and a no-op cannot mask a digest bug. */
void ckpt_mark_dirty(uint32_t region) { (void)region; }

/* Quiet by default; the guards under test print on refusal paths. */
static int verbose = 0;
void kernel_serial_print(const char *s) { if (verbose) fputs(s, stdout); }
void kernel_serial_printf(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void kernel_serial_print_hex64(uint64_t v) { if (verbose) printf("%016llx", (unsigned long long)v); }

/* ─── harness ───────────────────────────────────────────────────────────── */

static int checks_passed = 0, checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* A 4 KiB page of plausible guest code, deterministic per seed. */
static void fill_page(uint8_t *p, uint32_t len, unsigned seed) {
    for (uint32_t i = 0; i < len; i++)
        p[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 3));
}

int main(void) {
    printf("tcache_page_digest_host_test\n============================\n\n");

    disk = calloc(FAKE_LBA_COUNT, 512);
    if (!disk) { printf("FAIL: out of memory\n"); return 1; }

    /* Cold start: no magic on the fake disk, so init() takes the
     * "no snapshot" path and leaves an empty digest table. */
    qemu_sls_tcache_init();

    static uint8_t page_a[4096], page_b[4096], page_a_short[4096];
    fill_page(page_a, sizeof(page_a), 1);
    fill_page(page_b, sizeof(page_b), 2);
    memcpy(page_a_short, page_a, sizeof(page_a));

    const uint64_t GPA0 = 0x0;        /* the page the loader actually writes */
    const uint64_t GPA1 = 0x1000;

    printf("-- before anything is recorded --\n");
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_a, sizeof(page_a)) == 0,
          "*** a page with no recorded digest does NOT match, so a cache "
          "restored without digests cannot authorise skipping a flush ***");

    printf("\n-- record, then match --\n");
    qemu_sls_tcache_record_page(GPA0, page_a, sizeof(page_a));
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_a, sizeof(page_a)) == 1,
          "the same bytes match after being recorded (the case that makes a "
          "cross-reboot warm start possible at all)");
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_b, sizeof(page_b)) == 0,
          "*** different bytes at the same page do NOT match ***");

    printf("\n-- page 0 must be representable --\n");
    /* The table stores page+1 so that 0 means "empty". Page 0 is the page the
     * loader writes for a guest image at GPA 0, so an off-by-one here would
     * break precisely the case this whole mechanism exists for, while every
     * other page kept working. */
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_a, sizeof(page_a)) == 1,
          "*** page 0 is a real entry, not read as an empty slot -- the guest "
          "image lives at GPA 0, so this is the case that matters ***");

    printf("\n-- a truncated read does not match --\n");
    /* Note what this does and does not prove. It asserts that a 2048-byte
     * read of the same prefix is rejected, which is the property that
     * matters. It does NOT prove the length term in digest_bytes() is doing
     * that work: mutation testing showed this assertion still passes with the
     * length term removed, because FNV over a prefix already differs from FNV
     * over the full buffer. The term is defence in depth and the comment at
     * digest_bytes() now says so. */
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_a_short, 2048) == 0,
          "*** a shorter read with an identical prefix does NOT match, so a "
          "truncated image cannot pass as the full one ***");

    printf("\n-- pages are independent --\n");
    CHECK(qemu_sls_tcache_page_matches(GPA1, page_a, sizeof(page_a)) == 0,
          "*** recording page 0 does not authorise page 1, even for identical "
          "bytes ***");
    qemu_sls_tcache_record_page(GPA1, page_b, sizeof(page_b));
    CHECK(qemu_sls_tcache_page_matches(GPA1, page_b, sizeof(page_b)) == 1 &&
          qemu_sls_tcache_page_matches(GPA0, page_a, sizeof(page_a)) == 1,
          "two pages hold distinct digests simultaneously");

    printf("\n-- THE SAFETY INVARIANT: flush clears the digest --\n");
    /* This is what bounds the loosening. A guest store or DMA reaches
     * flush_page(), not record_page(), so the digest must not survive it. */
    qemu_sls_tcache_flush_page(GPA0);
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_a, sizeof(page_a)) == 0,
          "*** flush_page() clears the digest, so an invalidation from OUTSIDE "
          "the loader (guest store via shadow_fault, DMA) cannot be skipped on "
          "the next launch. This is the assertion the design rests on ***");
    CHECK(qemu_sls_tcache_page_matches(GPA1, page_b, sizeof(page_b)) == 1,
          "*** ...and it clears only the flushed page, not the whole table ***");

    printf("\n-- re-record after a flush restores matching --\n");
    /* The loader's order is flush, then record. If record before flush were
     * enough, the invariant above would be defeated by the loader itself. */
    qemu_sls_tcache_record_page(GPA0, page_a, sizeof(page_a));
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_a, sizeof(page_a)) == 1,
          "recording after the flush restores the match, which is the order "
          "sls-launcher.c uses");

    printf("\n-- generation still advances on flush --\n");
    uint32_t gen_before = qemu_sls_page_gen[0];
    qemu_sls_tcache_flush_page(GPA0);
    CHECK(qemu_sls_page_gen[0] == gen_before + 1,
          "flush_page() still bumps the generation counter -- the digest is an "
          "addition to invalidation, not a replacement for it");

    printf("\n-- persistence round trip --\n");
    /* Re-record, sync to the fake disk, wipe memory by re-initialising, and
     * confirm the digest came back. Against no-op NVMe stubs this assertion
     * would pass while nothing was written; against a backing store it fails
     * if the table goes to the wrong LBA or is never read. */
    qemu_sls_tcache_record_page(GPA0, page_a, sizeof(page_a));
    qemu_sls_tcache_sync();
    qemu_sls_tcache_init();          /* re-reads header, gens, TBs, digests */
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_a, sizeof(page_a)) == 1,
          "*** a digest recorded before a sync survives re-initialisation, so "
          "the table is written to and read from the same place ***");
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_b, sizeof(page_b)) == 0,
          "...and the restored digest still discriminates, rather than "
          "matching anything");

    printf("\n-- a zero-length or null read is never a match --\n");
    CHECK(qemu_sls_tcache_page_matches(GPA0, page_a, 0) == 0 &&
          qemu_sls_tcache_page_matches(GPA0, NULL, sizeof(page_a)) == 0,
          "*** degenerate inputs answer NO rather than defaulting to yes ***");

    printf("\n============================\n");
    printf("passed %d, failed %d\n", checks_passed, checks_failed);
    free(disk);
    return checks_failed == 0 ? 0 : 1;
}
