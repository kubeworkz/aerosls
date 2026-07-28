/*
 * persist_lba_layout_host_test.c — guards kernel/persist.h's NVMe snapshot
 * LBA layout against the region-overlap bug class.
 *
 * ─── Why this test exists ────────────────────────────────────────────────
 * This is not a hypothetical. The capacity-sizing pass (Multitenant Isolation
 * Gap Analysis §18) grew PARTITION_MAX, PARTITION_ASSIGN_MAX, DATABASE_MAX,
 * DATABASE_GRANT_MAX and TENANT_MAX, and rewrote persist.h's layout table in
 * comments -- carefully, with sizeof()-verified frame counts and a deliberate
 * 1-frame safety gap after every region. It did not update the actual
 * #define values. Twenty-four of thirty-five constants were left describing
 * the OLD layout, producing ten pairs of overlapping regions that silently
 * corrupted each other on every write: persist_databases() overwrote two of
 * the three frames holding databases[] with database_grants[] data, and
 * persist_partitions() overwrote the row-store table headers.
 *
 * That went undetected because every existing test asserts only on
 * low-indexed entries (databases[0], database_grants[2]) which happen to live
 * in the one frame per overlap that survives. The suite passed on luck. It
 * was found only when checksums were added to the persistence layer and
 * verification started failing on regions that were genuinely corrupt.
 *
 * persist.h's own §18 comment predicted exactly this: "a silent on-disk
 * data-corruption time bomb the moment any of those constants grew." The
 * constants grew. Nothing was watching. This file is what watches.
 *
 * ─── What it checks, and why these specific invariants ───────────────────
 * Everything is derived from the REAL #define values and REAL sizeof()s, so
 * the test tracks the code rather than a transcription of it. Changing any
 * governing constant (PARTITION_MAX, DATABASE_GRANT_MAX, RECORD_VAL_LEN, ...)
 * without moving the LBAs to match will fail here instead of corrupting a
 * user's disk.
 *
 * No .c file is linked: sizeof() on an extern array with a known bound is a
 * compile-time constant, so the headers alone are enough. That keeps this
 * test fast and free of the persistence layer's dependency graph.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/persist_lba_layout_host_test tests/persist_lba_layout_host_test.c
 *   /tmp/persist_lba_layout_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/row_constraint.h"
#include "kernel/row_journal.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "kernel/tenant.h"
#include "kernel/service_registry.h"
#include "kernel/database.h"
#include "kernel/view.h"
#include "kernel/stream.h"
#include "kernel/persist.h"
#include <stdio.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

#define SECTORS_PER_FRAME 8ULL
#define FRAMES_FOR(bytes) (((uint64_t)(bytes) + 4095ULL) / 4096ULL)

/* One contiguous on-disk extent: a header frame, or one persisted array. */
struct Extent {
    const char* name;
    uint64_t    lba;
    uint64_t    sectors;
};

/* A region == one header plus the arrays its persist_*() writes. Mirrors
 * kernel/persist.c's own region table; the two must agree, and a divergence
 * shows up as an overlap or gap failure below. */
struct Region {
    const char*    name;
    int            gap_required;   /* persist.h documents a 1-frame gap from PART onward */
    struct Extent  ext[4];
    int            next;
};

#define HDR(n, lba)        { n " hdr", lba, SECTORS_PER_FRAME }
#define ARR(n, lba, arr)   { n, lba, FRAMES_FOR(sizeof(arr)) * SECTORS_PER_FRAME }

static struct Region regions[] = {
    { "catalog", 0, {
        HDR("catalog", PERSIST_CAT_HDR_LBA),
        ARR("object_catalog", PERSIST_CAT_ENT_LBA, object_catalog),
        ARR("role_table",     PERSIST_ROLE_ENT_LBA, role_table) }, 3 },
    { "records", 0, {
        HDR("records", PERSIST_REC_HDR_LBA),
        ARR("object_records", PERSIST_REC_ENT_LBA, object_records) }, 2 },
    { "schemas", 0, {
        HDR("schemas", PERSIST_SCH_HDR_LBA),
        ARR("object_schemas", PERSIST_SCH_ENT_LBA, object_schemas) }, 2 },
    { "programs", 0, {
        HDR("programs", PERSIST_PROG_HDR_LBA),
        ARR("service_binaries", PERSIST_PROG_DAT_LBA, service_binaries) }, 2 },
    { "partitions", 1, {
        HDR("partitions", PERSIST_PART_HDR_LBA),
        ARR("partition_table",        PERSIST_PART_ENT_LBA,    partition_table),
        ARR("partition_assign_table", PERSIST_PART_ASSIGN_LBA, partition_assign_table),
        ARR("partition_owner_table",  PERSIST_PART_OWNER_LBA,  partition_owner_table) }, 4 },
    { "rowstore", 1, {
        HDR("rowstore", PERSIST_ROWSTORE_HDR_LBA),
        ARR("table_headers", PERSIST_ROWSTORE_ENT_LBA, table_headers) }, 2 },
    { "row_constraints", 1, {
        HDR("row_constraints", PERSIST_ROW_CONSTRAINT_HDR_LBA),
        ARR("row_constraints", PERSIST_ROW_CONSTRAINT_ENT_LBA, row_constraints) }, 2 },
    { "row_index", 1, {
        HDR("row_index", PERSIST_ROW_INDEX_HDR_LBA),
        ARR("row_indexes", PERSIST_ROW_INDEX_ENT_LBA, row_indexes) }, 2 },
    { "vecstore", 1, {
        HDR("vecstore", PERSIST_VECSTORE_HDR_LBA),
        ARR("vector_collections", PERSIST_VECSTORE_ENT_LBA, vector_collections) }, 2 },
    { "vec_index", 1, {
        HDR("vec_index", PERSIST_VEC_INDEX_HDR_LBA),
        ARR("vec_indexes", PERSIST_VEC_INDEX_ENT_LBA, vec_indexes) }, 2 },
    { "row_journal", 1, {
        HDR("row_journal", PERSIST_ROW_JOURNAL_HDR_LBA),
        ARR("row_journal_buffer",      PERSIST_ROW_JOURNAL_ENT_LBA,    row_journal_buffer),
        ARR("row_journal_attachments", PERSIST_ROW_JOURNAL_ATTACH_LBA, row_journal_attachments) }, 3 },
    { "databases", 1, {
        HDR("databases", PERSIST_DATABASE_HDR_LBA),
        ARR("databases",       PERSIST_DATABASE_ENT_LBA,   databases),
        ARR("database_grants", PERSIST_DATABASE_GRANT_LBA, database_grants) }, 3 },
    { "views", 1, {
        HDR("views", PERSIST_VIEW_HDR_LBA),
        ARR("views", PERSIST_VIEW_ENT_LBA, views) }, 2 },
    { "tenants", 1, {
        HDR("tenants", PERSIST_TENANT_HDR_LBA),
        ARR("tenants", PERSIST_TENANT_ENT_LBA, tenants) }, 2 },
    { "rowstore_partcursor", 1, {
        ARR("rowstore_partition_cursor", PERSIST_ROWSTORE_PARTCURSOR_LBA, rowstore_partition_cursor) }, 1 },
    { "vecstore_partcursor", 1, {
        ARR("vecstore_partition_cursor", PERSIST_VECSTORE_PARTCURSOR_LBA, vecstore_partition_cursor) }, 1 },
    { "services", 1, {
        HDR("services", PERSIST_SERVICE_HDR_LBA),
        ARR("services_registry", PERSIST_SERVICE_ENT_LBA, services_registry) }, 2 },
};
#define NREGIONS ((int)(sizeof(regions)/sizeof(regions[0])))

static uint64_t region_start(const struct Region* r) {
    uint64_t lo = r->ext[0].lba;
    for (int i = 1; i < r->next; i++) if (r->ext[i].lba < lo) lo = r->ext[i].lba;
    return lo;
}
static uint64_t region_end(const struct Region* r) {
    uint64_t hi = r->ext[0].lba + r->ext[0].sectors;
    for (int i = 1; i < r->next; i++) {
        uint64_t e = r->ext[i].lba + r->ext[i].sectors;
        if (e > hi) hi = e;
    }
    return hi;
}

int main(void) {
    printf("=== persist.h NVMe LBA layout: overlap / envelope guard ===\n\n");

    /* ── 1. No two extents anywhere may overlap ──────────────────────────
     * The check that would have caught the real bug directly. Compares every
     * extent against every other, across region boundaries, headers
     * included -- the original corruption had a data array landing on top of
     * both another array AND, elsewhere, a neighbouring region's header. */
    int overlaps = 0;
    for (int i = 0; i < NREGIONS; i++) {
        for (int ii = 0; ii < regions[i].next; ii++) {
            for (int j = 0; j < NREGIONS; j++) {
                for (int jj = 0; jj < regions[j].next; jj++) {
                    if (i == j && ii == jj) continue;
                    if (i > j || (i == j && ii > jj)) continue;   /* each pair once */
                    uint64_t as = regions[i].ext[ii].lba, ae = as + regions[i].ext[ii].sectors;
                    uint64_t bs = regions[j].ext[jj].lba, be = bs + regions[j].ext[jj].sectors;
                    if (as < be && bs < ae) {
                        printf("      OVERLAP: %s [%llu,%llu) vs %s [%llu,%llu)\n",
                               regions[i].ext[ii].name, (unsigned long long)as, (unsigned long long)ae,
                               regions[j].ext[jj].name, (unsigned long long)bs, (unsigned long long)be);
                        overlaps++;
                    }
                }
            }
        }
    }
    CHECK(overlaps == 0,
          "no two persisted extents overlap -- the exact bug class that silently corrupted databases[] and table_headers[] before this test existed");

    /* ── 2. Everything stays inside the intended envelope ────────────────
     * Below STREAM_DIR_LBA, at or above the first region. An overflow past
     * the stream directory would corrupt a completely different subsystem. */
    int below = 1, above = 1;
    uint64_t highest = 0;
    for (int i = 0; i < NREGIONS; i++) {
        for (int j = 0; j < regions[i].next; j++) {
            uint64_t s = regions[i].ext[j].lba, e = s + regions[i].ext[j].sectors;
            if (e > STREAM_DIR_LBA) { below = 0;
                printf("      PAST STREAM_DIR_LBA: %s ends at %llu\n",
                       regions[i].ext[j].name, (unsigned long long)e); }
            if (s < PERSIST_CAT_HDR_LBA) { above = 0;
                printf("      BELOW FIRST REGION: %s starts at %llu\n",
                       regions[i].ext[j].name, (unsigned long long)s); }
            if (e > highest) highest = e;
        }
    }
    CHECK(below, "every persisted extent ends below STREAM_DIR_LBA -- no collision with the stream subsystem");
    CHECK(above, "no extent starts below the first region's LBA");
    printf("      (layout occupies up to LBA %llu; STREAM_DIR_LBA is %llu -- %llu sectors / %llu frames of margin)\n",
           (unsigned long long)highest, (unsigned long long)STREAM_DIR_LBA,
           (unsigned long long)(STREAM_DIR_LBA - highest),
           (unsigned long long)((STREAM_DIR_LBA - highest) / SECTORS_PER_FRAME));

    /* ── 3. The documented 1-frame safety gap is really there ────────────
     * persist.h states that every region from PERSIST_PART_HDR_LBA onward
     * carries a deliberate 1-frame gap before the next region's header, so a
     * future resize cannot silently spill into its neighbour. Asserting it
     * keeps that promise honest -- zero-slack adjacency is what made the
     * original bug possible in the first place. */
    int gaps_ok = 1;
    for (int i = 0; i < NREGIONS; i++) {
        if (!regions[i].gap_required) continue;
        uint64_t end = region_end(&regions[i]);
        /* find the nearest region starting at or after this one's end */
        uint64_t nearest = STREAM_DIR_LBA; const char* who = "(end of layout)";
        for (int j = 0; j < NREGIONS; j++) {
            if (j == i) continue;
            uint64_t s = region_start(&regions[j]);
            if (s >= end && s < nearest) { nearest = s; who = regions[j].name; }
        }
        if (nearest < STREAM_DIR_LBA && nearest - end < SECTORS_PER_FRAME) {
            printf("      NO GAP: region '%s' ends at %llu, '%s' starts at %llu (need >= %llu sectors)\n",
                   regions[i].name, (unsigned long long)end, who,
                   (unsigned long long)nearest, (unsigned long long)SECTORS_PER_FRAME);
            gaps_ok = 0;
        }
    }
    CHECK(gaps_ok,
          "every region from partitions onward keeps persist.h's documented 1-frame safety gap before the next region");

    /* ── 4. Spot-check the two regions that were actually corrupt ────────
     * Named explicitly so a regression reads as the specific historical bug
     * rather than an anonymous overlap. */
    uint64_t db_end    = PERSIST_DATABASE_ENT_LBA + FRAMES_FOR(sizeof(databases)) * SECTORS_PER_FRAME;
    CHECK(PERSIST_DATABASE_GRANT_LBA >= db_end,
          "regression guard: database_grants[] starts at or after databases[] ends (it started 16 sectors INSIDE it)");
    uint64_t assign_end = PERSIST_PART_ASSIGN_LBA + FRAMES_FOR(sizeof(partition_assign_table)) * SECTORS_PER_FRAME;
    CHECK(PERSIST_ROWSTORE_ENT_LBA >= assign_end,
          "regression guard: table_headers[] starts after partition_assign_table[] ends (the assign table used to run straight through it)");

    /* ── 5. Report the layout, so a failure is diagnosable from output ─── */
    printf("\n      layout (derived from the real #defines and sizeof()s):\n");
    for (int i = 0; i < NREGIONS; i++) {
        for (int j = 0; j < regions[i].next; j++) {
            uint64_t s = regions[i].ext[j].lba;
            printf("        %-30s lba=%-6llu frames=%-4llu end=%llu\n",
                   regions[i].ext[j].name, (unsigned long long)s,
                   (unsigned long long)(regions[i].ext[j].sectors / SECTORS_PER_FRAME),
                   (unsigned long long)(s + regions[i].ext[j].sectors));
        }
    }

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
