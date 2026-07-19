/*
 * rowstore_host_test.c — Phase 16 (relational layer) verification: a
 * standalone host-buildable round-trip test for kernel/rowstore.c's new
 * row-set storage engine, linked against the REAL, unmodified
 * kernel/rowstore.c AND kernel/persist.c — not a reimplementation.
 *
 * rowstore.c's real dependencies turned out to be exactly what the design
 * investigation predicted: frame_pool.h (RAM pages, proven shallow and
 * host-testable in Phase 13), a stream.c-shaped NVMe read/write pair
 * (proven fakeable behind a small in-memory LBA map in Phase 10's
 * persist_partition_host_test.c), object_catalog.h's types/globals, and
 * catalog_check_access() (a real function in object_catalog.c, which is
 * NOT linked here — object_catalog.c's own dependency graph is exactly as
 * heavy as Phase 9 found process.c's to be, so it's stubbed, call-tracked
 * so this test can still verify rowstore.c calls it correctly).
 *
 * persist.c IS linked for real, same as Phase 10's own test — its
 * dependency surface (kernel_io.h's two logging functions, nvme_read_sync/
 * nvme_write_sync, and five OTHER subsystems' plain data arrays it also
 * touches but this test doesn't exercise) is cheap enough to fake. Linking
 * it for real gives genuine coverage of persist_rowstore_headers() and
 * restore block 6, not just rowstore.c's in-RAM logic.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -I drivers \
 *       -o /tmp/rowstore_host_test \
 *       rowstore_host_test.c kernel/rowstore.c kernel/persist.c
 *   /tmp/rowstore_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/persist.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Dummy definitions for subsystems persist.c also touches but this
 * test doesn't exercise (see persist_partition_host_test.c precedent) ──── */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
void catalog_after_restore(void) { /* no-op for this test */ }

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* Phase 17 (relational layer): rowstore.c's insert/update/delete (added
 * this phase) unconditionally call these three row_index.c hooks after a
 * successful mutation. This test has no interest in exercising indexing —
 * see row_index_host_test.c for the real, call-tracked coverage of that —
 * so they're stubbed as no-ops purely so the linker resolves them, matching
 * this file's own established "no interest in this subsystem" pattern for
 * catalog_check_access() and the five persist.c-only arrays above. */
void row_index_notify_insert(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)values; (void)layout;
}
void row_index_notify_update(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* old_values, const struct RowValues* new_values,
                             const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)old_values; (void)new_values; (void)layout;
}
void row_index_notify_delete(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)values; (void)layout;
}

/* ─── catalog_check_access() stub — call-tracked, not just a fixed return.
 * rowstore.c's own header comment (a deliberate design decision, not an
 * oversight) says every row CRUD call must gate on this with PERM_READ
 * (get/scan) or PERM_WRITE (insert/update/delete) — this stub lets the
 * test confirm that actually happens, and lets one scenario force a
 * denial. ─────────────────────────────────────────────────────────────── */
static int      g_access_calls = 0;
static uint32_t g_access_last_perm = 0;
static int      g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name;
    g_access_calls++;
    g_access_last_perm = needed_perm;
    return g_access_force_deny ? 0 : 1;
}

/* ─── frame_pool.h stub — real per-call heap allocations (not a fixed
 * address) since rowstore.c genuinely needs distinct, independently
 * writable 4 KiB buffers per page, unlike simpler stubs elsewhere in this
 * project that can get away with returning one fixed dummy pointer. */
void* allocate_physical_ram_frame(void) { return malloc(4096); }

/* ─── Fake NVMe: an in-memory map from frame-aligned LBA to 4KiB bytes ──
 * Sized larger than Phase 10's own 64-frame test: table_headers[] alone
 * needs ~38 frames, plus a handful of row-page frames, plus persist_
 * catalog()'s ~5 frames when the full-round-trip scenario exercises it. */
#define FAKE_NVME_MAX_FRAMES 256
static struct { uint64_t lba; uint8_t data[4096]; int used; } g_fake_nvme[FAKE_NVME_MAX_FRAMES];

void* io_sq = (void*)1;
void* io_cq = (void*)1;

static int find_or_alloc_frame(uint64_t lba) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) return i;
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (!g_fake_nvme[i].used) { g_fake_nvme[i].used = 1; g_fake_nvme[i].lba = lba; return i; }
    return -1;
}
int nvme_write_sync(uint64_t lba, const void* buf) {
    int idx = find_or_alloc_frame(lba);
    if (idx < 0) return 1;
    memcpy(g_fake_nvme[idx].data, buf, 4096);
    return 0;
}
int nvme_read_sync(uint64_t lba, void* buf) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++) {
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) {
            memcpy(buf, g_fake_nvme[i].data, 4096);
            return 0;
        }
    }
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

/* Scan callback (scenario 5) — flags if the tombstoned row (id "20",
 * deleted in scenario 4) is ever visited; it shouldn't be. */
static void scan_cb(struct RowId id, const struct RowValues* values, void* ctx) {
    (void)id;
    int* saw_deleted = (int*)ctx;
    if (strcmp(values->values[0], "20") == 0) *saw_deleted = 1;
}

/* ─── Test fixture: one "employees" table, 3 columns ────────────────────
 * id UINT64 (8B), name STRING (64B), active BOOL (1B) -> row_width =
 * 1(tombstone)+8+64+1 = 74B -> rows_per_page = (4096-4)/74 = 55. */
static void make_employees_table(void) {
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "employees");
    object_catalog[0].type      = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xE401;
    object_catalog[0].active    = 1;
    object_catalog_count = 1;

    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    struct { const char* k; SLSFieldType t; } cols[3] = {
        {"id", FIELD_TYPE_UINT64}, {"name", FIELD_TYPE_STRING}, {"active", FIELD_TYPE_BOOL}
    };
    for (int i = 0; i < 3; i++) {
        strcpy(object_schemas[0].fields[i].key, cols[i].k);
        object_schemas[0].fields[i].type   = cols[i].t;
        object_schemas[0].fields[i].active = 1;
    }
    object_schemas[0].field_count = 3;
}

static struct RowValues row_of(uint64_t id, const char* name, int active) {
    struct RowValues v; memset(&v, 0, sizeof(v));
    v.count = 3;
    snprintf(v.values[0], RECORD_VAL_LEN, "%llu", (unsigned long long)id);
    strncpy(v.values[1], name, RECORD_VAL_LEN - 1);
    strcpy(v.values[2], active ? "true" : "false");
    return v;
}

int main(void) {
    /* ── Scenario 1: create_table validates and computes the right layout ── */
    make_employees_table();
    CHECK(rowstore_create_table("nonexistent") == 1, "scenario 1: creating a row-set table on a nonexistent object fails cleanly");
    CHECK(rowstore_create_table("employees") == 0, "scenario 1: rowstore_create_table succeeds for a real, schema-set object");
    CHECK(object_catalog[0].uses_rowstore == 1, "scenario 1: uses_rowstore flag set on the catalog entry");
    CHECK(table_headers[0].active == 1, "scenario 1: table header marked active");
    CHECK(table_headers[0].layout.column_count == 3, "scenario 1: layout has 3 columns");
    CHECK(table_headers[0].layout.row_width == 74, "scenario 1: row_width computed correctly (1 + 8 + 64 + 1 = 74)");
    CHECK(table_headers[0].layout.rows_per_page == 55, "scenario 1: rows_per_page computed correctly ((4096-4)/74 = 55)");
    CHECK(rowstore_create_table("employees") == 1, "scenario 1: creating it a second time fails (already a row-set table)");

    /* ── Scenario 2: insert enough rows to span two pages, confirming real
     * page-chaining, not just single-page correctness. ──────────────────── */
    struct RowId ids[60];
    int inserted = 0;
    for (int i = 0; i < 60; i++) {
        struct RowValues v = row_of((uint64_t)i, "alice", i % 2);
        int rc = rowstore_row_insert(1, "employees", &v, &ids[i]);
        if (rc == 0) inserted++;
    }
    CHECK(inserted == 60, "scenario 2: all 60 inserts succeed");
    CHECK(table_headers[0].row_count == 60, "scenario 2: row_count reflects all 60");
    CHECK(table_headers[0].page_count == 2, "scenario 2: 60 rows at 55/page correctly spans exactly 2 pages");
    CHECK(ids[0].page_id == ids[54].page_id, "scenario 2: rows 0-54 (55 rows) share the first page");
    CHECK(ids[55].page_id != ids[0].page_id, "scenario 2: row 55 lands on a NEW page, not overflowing the first");
    CHECK(g_access_calls > 0 && g_access_last_perm == PERM_WRITE, "scenario 2: insert gates on catalog_check_access() with PERM_WRITE");

    /* ── Scenario 3: get every row back, confirm round-trip correctness
     * across all three column types (UINT64/STRING/BOOL). ───────────────── */
    int all_match = 1;
    for (int i = 0; i < 60; i++) {
        struct RowValues out;
        int rc = rowstore_row_get(1, "employees", ids[i], &out);
        if (rc != 0) { all_match = 0; break; }
        char expect_id[32]; snprintf(expect_id, sizeof(expect_id), "%d", i);
        if (strcmp(out.values[0], expect_id) != 0) { all_match = 0; break; }
        if (strcmp(out.values[1], "alice") != 0) { all_match = 0; break; }
        if (strcmp(out.values[2], (i % 2) ? "true" : "false") != 0) { all_match = 0; break; }
    }
    CHECK(all_match, "scenario 3: all 60 rows read back with correct id/name/active values, both pages");

    /* ── Scenario 4: update in place, delete, and confirm both take effect
     * without disturbing neighboring rows. ───────────────────────────────── */
    struct RowValues updated = row_of(999, "bob", 1);
    CHECK(rowstore_row_update(1, "employees", ids[10], &updated) == 0, "scenario 4: update on row 10 succeeds");
    struct RowValues check10;
    rowstore_row_get(1, "employees", ids[10], &check10);
    CHECK(strcmp(check10.values[0], "999") == 0 && strcmp(check10.values[1], "bob") == 0,
          "scenario 4: row 10's new values read back correctly");
    struct RowValues check9;
    rowstore_row_get(1, "employees", ids[9], &check9);
    CHECK(strcmp(check9.values[1], "alice") == 0, "scenario 4: neighboring row 9 untouched by row 10's update");

    CHECK(rowstore_row_delete(1, "employees", ids[20]) == 0, "scenario 4: delete on row 20 succeeds");
    CHECK(rowstore_row_get(1, "employees", ids[20], NULL) == 3, "scenario 4: getting a deleted row returns 3 (not found)");
    CHECK(table_headers[0].row_count == 59, "scenario 4: row_count decremented after the delete");
    CHECK(rowstore_row_delete(1, "employees", ids[20]) == 3, "scenario 4: deleting the same row again fails cleanly, not a double-free crash");

    /* ── Scenario 5: full-table scan visits exactly the active rows, in
     * physical order, and skips the tombstoned one. ─────────────────────── */
    int scan_saw_deleted = 0;
    uint32_t visited = rowstore_table_scan(1, "employees", scan_cb, &scan_saw_deleted);
    CHECK(visited == 59, "scenario 5: table scan visits exactly 59 rows (60 inserted minus 1 deleted)");
    CHECK(!scan_saw_deleted, "scenario 5: the tombstoned row (deleted in scenario 4) is never visited");

    /* ── Scenario 6: error paths — value count mismatch, bad type parse,
     * oversized string, permission denial, unknown table, bad row id. ──── */
    struct RowValues wrong_count = row_of(1, "x", 0); wrong_count.count = 2;
    CHECK(rowstore_row_insert(1, "employees", &wrong_count, NULL) == 4, "scenario 6: value count mismatch rejected (4)");

    struct RowValues bad_uint = row_of(0, "x", 0); strcpy(bad_uint.values[0], "not-a-number");
    CHECK(rowstore_row_insert(1, "employees", &bad_uint, NULL) == 5, "scenario 6: unparseable UINT64 rejected (5)");

    struct RowValues long_str = row_of(1, "x", 0);
    memset(long_str.values[1], 'a', RECORD_VAL_LEN - 1); long_str.values[1][RECORD_VAL_LEN - 1] = '\0';
    CHECK(rowstore_row_insert(1, "employees", &long_str, NULL) == 5, "scenario 6: oversized STRING (>= 64 bytes) rejected, not silently truncated (5)");

    struct RowValues ok_row = row_of(1, "x", 0);
    g_access_force_deny = 1;
    CHECK(rowstore_row_insert(1, "employees", &ok_row, NULL) == 2, "scenario 6: permission denial propagates (2)");
    g_access_force_deny = 0;

    CHECK(rowstore_row_insert(1, "no_such_table", &ok_row, NULL) == 1, "scenario 6: unknown table rejected (1)");

    struct RowId bad_id = { 999999, 0 };
    CHECK(rowstore_row_get(1, "employees", bad_id, NULL) == 3, "scenario 6: out-of-range page_id in a RowId rejected (3)");

    /* ── Scenario 7: page pool exhaustion, forced deterministically rather
     * than by actually allocating a quarter million pages. ──────────────── */
    make_employees_table();
    strcpy(object_catalog[0].name, "tiny");
    object_catalog[0].object_id = 0xE402;
    rowstore_create_table("tiny");
    uint32_t saved_cursor = rowstore_next_free_page_id;
    rowstore_next_free_page_id = ROWSTORE_MAX_PAGES;   /* simulate a full pool */
    struct RowValues v7 = row_of(1, "x", 0);
    CHECK(rowstore_row_insert(1, "tiny", &v7, NULL) == 6, "scenario 7: insert into an empty table with an exhausted page pool fails cleanly (6)");
    rowstore_next_free_page_id = saved_cursor;   /* restore for the round-trip scenario below */

    /* ── Scenario 8: restart-simulating round trip — write real rows,
     * flip the persisted uses_rowstore flag via a real persist_catalog()
     * call, wipe ALL in-memory state (catalog + rowstore, exactly like a
     * fresh boot's BSS-zero would), then restore from the fake NVMe and
     * confirm the table identity, its layout, AND its row data all come
     * back — the strongest check this test can offer, matching Phase 10/
     * 11/13's own "real execution, restart round trip" precedent. ───────── */
    make_employees_table();
    rowstore_create_table("employees");
    persist_catalog();   /* real function from persist.c -- persists uses_rowstore */
    struct RowId rid_a, rid_b;
    struct RowValues va = row_of(42, "carol", 1);
    struct RowValues vb = row_of(43, "dave",  0);
    rowstore_row_insert(1, "employees", &va, &rid_a);
    rowstore_row_insert(1, "employees", &vb, &rid_b);

    /* Wipe every bit of in-memory state a fresh boot's BSS-zero would. */
    memset(object_catalog, 0, sizeof(object_catalog));
    memset(object_schemas, 0, sizeof(object_schemas));
    object_catalog_count = 0;
    rowstore_init();

    CHECK(object_catalog_count == 0, "scenario 8: post-'reboot', pre-restore: catalog genuinely wiped");
    CHECK(table_headers[0].active == 0, "scenario 8: post-'reboot', pre-restore: table header genuinely wiped");

    persist_restore_all();   /* real function from persist.c */

    CHECK(object_catalog_count >= 1, "scenario 8: catalog restored, at least the employees table present");
    int found_idx = -1;
    for (uint32_t i = 0; i < object_catalog_count; i++)
        if (object_catalog[i].active && strcmp(object_catalog[i].name, "employees") == 0) { found_idx = (int)i; break; }
    CHECK(found_idx == 0, "scenario 8: 'employees' restored at its original catalog index");
    CHECK(found_idx >= 0 && object_catalog[found_idx].uses_rowstore == 1,
          "scenario 8: uses_rowstore flag survived the round trip (via persist_catalog(), a separate array from table_headers[])");
    CHECK(table_headers[0].active == 1 && table_headers[0].row_count == 2,
          "scenario 8: row-store table header restored with row_count=2");

    struct RowValues out_a, out_b;
    int rc_a = rowstore_row_get(1, "employees", rid_a, &out_a);
    int rc_b = rowstore_row_get(1, "employees", rid_b, &out_b);
    CHECK(rc_a == 0 && strcmp(out_a.values[0], "42") == 0 && strcmp(out_a.values[1], "carol") == 0,
          "scenario 8: row A's actual data survived the round trip, read via a lazily-restored page from the fake NVMe");
    CHECK(rc_b == 0 && strcmp(out_b.values[0], "43") == 0 && strcmp(out_b.values[1], "dave") == 0,
          "scenario 8: row B's actual data survived the round trip too");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
