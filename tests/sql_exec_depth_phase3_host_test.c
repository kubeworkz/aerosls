/*
 * sql_exec_depth_phase3_host_test.c — SQL Query-Surface Roadmap Phase 3
 * verification: a standalone host-buildable test for kernel/sql_exec.c's
 * depth-indexed scratch banking (g_stmt_scratch/g_select_scratch/
 * g_join_scratch_b/g_agg_buckets/g_join_probe_pred, each now
 * `_bank[SQL_EXEC_MAX_DEPTH]` behind an unchanged-name macro) and the new
 * g_exec_depth counter + sql_exec_depth_enter()/_leave() pair. Linked
 * against the REAL, unmodified full stack -- sql_exec.c, sql_parser.c,
 * predicate.c, row_index.c, rowstore.c, persist.c, cursor.c, mvcc.c,
 * row_constraint.c, row_journal.c, database.c -- not a reimplementation.
 * Mirrors sql_group_phase1_host_test.c's own fixture/stub scaffolding
 * exactly (same stack, same reasons).
 *
 * Phase 3 itself adds no real nested call site (that's Phase 4's UNION,
 * Phase 5/6's view/CTE expansion) -- this test drives the banking directly
 * through sql_exec.c's own TEST-ONLY internal hooks
 * (sql_exec_test_phase3_nesting()/sql_exec_test_phase3_depth_exceeded(),
 * declared in sql_exec.h, not part of the public SQL surface), which is
 * exactly what the roadmap's Phase 3 scope calls for: "an internal-only
 * test (a nested dispatch_stmt call proving the outer statement survives)".
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/sql_exec_depth_phase3_host_test tests/sql_exec_depth_phase3_host_test.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/view.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/sql_exec_depth_phase3_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/predicate.h"
#include "kernel/sql_exec.h"
#include "kernel/cursor.h"
#include "kernel/index_mgr.h"
#include "kernel/persist.h"
#include "user/permissions.h"
#include "kernel/tenant.h"      // Multitenant Isolation Gap Analysis §5 item 1 -- persist.c now references tenants[]/tenant_next_id; this test doesn't exercise tenant_create() itself so the bare storage (not kernel/tenant.c's functions) is enough to satisfy the linker,
// the same "declare the extern array directly" convention this file already uses for object_catalog[] etc. above.
struct SLSTenantEntry tenants[TENANT_MAX];
uint32_t              tenant_next_id = 1;
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];   /* Multi-Node Partition Scaling Roadmap Phase 2 */
struct SLSIndex        index_store[INDEX_MAX];
uint32_t               index_count = 0;
void catalog_after_restore(void) { /* no-op for this test */ }
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
int group_contains_uid(const char* name, uint32_t uid) { (void)name; (void)uid; return 0; }

struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
struct VecIndex             vec_indexes[VEC_INDEX_MAX];
int vec_index_create(uint32_t caller_uid, const char* index_name,
                     const char* collection_name, VecMetric metric) {
    (void)caller_uid; (void)index_name; (void)collection_name; (void)metric;
    return 1;
}
uint32_t vecstore_collection_scan(uint32_t caller_uid, const char* collection_name,
                                  VecScanCb cb, void* ctx) {
    (void)caller_uid; (void)collection_name; (void)cb; (void)ctx;
    return 0;
}
void vec_index_notify_insert(uint32_t caller_uid, const char* collection_name,
                             struct VecId id, uint64_t external_id,
                             const struct VecValues* values) {
    (void)caller_uid; (void)collection_name; (void)id; (void)external_id; (void)values;
}

int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    return 1;
}
void* allocate_physical_ram_frame(void) { return malloc(4096); }

#define FAKE_NVME_MAX_FRAMES 512
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
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) { memcpy(buf, g_fake_nvme[i].data, 4096); return 0; }
    return 1;
}

// ─── Phase 5 (SQL Feature-Parity Roadmap, DDL) stand-ins -- sql_exec.c's
// exec_create_table() unconditionally calls sys_sls_valloc()/
// sys_sls_schema_set(), and rowstore.c's rowstore_drop_table() unconditionally
// calls sys_sls_vfree() (real object_catalog.c cleanup this test doesn't
// link). This test never exercises CREATE/DROP TABLE via SQL text at
// runtime, so failure-code no-ops are safe here. ──────────────────────────
uint64_t sys_sls_valloc(struct SLSVallocRequest* req) { (void)req; return 0; }
uint64_t sys_sls_schema_set(struct SLSSchemaRequest* req) { (void)req; return 1; }
uint64_t sys_sls_vfree(const char* name) { (void)name; return 1; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static int catalog_slot = 0;
static void make_table(const char* name, uint64_t object_id, int ncols,
                       const char* colnames[], SLSFieldType coltypes[]) {
    int slot = catalog_slot++;
    memset(&object_catalog[slot], 0, sizeof(object_catalog[slot]));
    strcpy(object_catalog[slot].name, name);
    object_catalog[slot].type = OBJ_TYPE_DB_TABLE;
    object_catalog[slot].object_id = object_id;
    object_catalog[slot].active = 1;
    object_catalog_count = (uint32_t)(slot + 1);

    memset(&object_schemas[slot], 0, sizeof(object_schemas[slot]));
    for (int i = 0; i < ncols; i++) {
        strcpy(object_schemas[slot].fields[i].key, colnames[i]);
        object_schemas[slot].fields[i].type = coltypes[i];
        object_schemas[slot].fields[i].active = 1;
    }
    object_schemas[slot].field_count = (uint32_t)ncols;
    rowstore_create_table(name);
}

#define COLLECT_MAX_COLS 4
#define COLLECT_MAX_ROWS 32
struct collectN_ctx {
    uint32_t idx[COLLECT_MAX_COLS];
    int      ncols;
    char     vals[COLLECT_MAX_ROWS][COLLECT_MAX_COLS][64];
    uint32_t count;
};
static void collectN_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct collectN_ctx* ctx = (struct collectN_ctx*)ctxp;
    if (ctx->count >= COLLECT_MAX_ROWS) { ctx->count++; return; }
    for (int c = 0; c < ctx->ncols; c++) {
        if (v->count > ctx->idx[c]) { strncpy(ctx->vals[ctx->count][c], v->values[ctx->idx[c]], 63); ctx->vals[ctx->count][c][63] = '\0'; }
        else ctx->vals[ctx->count][c][0] = '\0';
    }
    ctx->count++;
}

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();

    /* ── Fixture ────────────────────────────────────────────────────────────
     * widgets(id, name, group_id): 3 rows, 2 in group 1, 1 in group 2.
     * wgroups(id, label): 2 rows -- the JOIN target for widgets.group_id.
     * gadgets(tag): 3 rows, one tagged 'x' -- a small, wholly unrelated
     * table for the nested/inner statement, so a bank mixup between depth
     * 0 and depth 1 would show up as wrong rows/counts, not just a crash. */
    {
        const char* wcols[3] = {"id", "name", "group_id"};
        SLSFieldType wtypes[3] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING, FIELD_TYPE_UINT64};
        make_table("widgets", 0xD101, 3, wcols, wtypes);
        const char* gcols[2] = {"id", "label"};
        SLSFieldType gtypes[2] = {FIELD_TYPE_UINT64, FIELD_TYPE_STRING};
        make_table("wgroups", 0xD102, 2, gcols, gtypes);
        const char* tcols[1] = {"tag"};
        SLSFieldType ttypes[1] = {FIELD_TYPE_STRING};
        make_table("gadgets", 0xD103, 1, tcols, ttypes);
    }

    struct SqlResult r;
    sql_execute(1, "INSERT INTO wgroups (id, label) VALUES (1, 'alpha')", &r);
    sql_execute(1, "INSERT INTO wgroups (id, label) VALUES (2, 'beta')", &r);
    sql_execute(1, "INSERT INTO widgets (id, name, group_id) VALUES (1, 'w1', 1)", &r);
    sql_execute(1, "INSERT INTO widgets (id, name, group_id) VALUES (2, 'w2', 1)", &r);
    sql_execute(1, "INSERT INTO widgets (id, name, group_id) VALUES (3, 'w3', 2)", &r);
    sql_execute(1, "INSERT INTO gadgets (tag) VALUES ('x')", &r);
    sql_execute(1, "INSERT INTO gadgets (tag) VALUES ('y')", &r);
    sql_execute(1, "INSERT INTO gadgets (tag) VALUES ('z')", &r);
    CHECK(r.error == SQL_ERR_NONE, "setup: 2 wgroups, 3 widgets, 3 gadgets inserted");

    uint64_t txn_id = sql_tx_begin();
    CHECK(txn_id != 0, "setup: transaction opened for the nesting tests");

    /* ── Scenario 1: outer is a JOIN+GROUP BY query (exercises
     * g_select_scratch, g_join_scratch_b, g_join_probe_pred, AND
     * g_agg_buckets at depth 0 during its own dispatch); a wholly
     * unrelated plain SELECT runs to completion at depth 1 in between the
     * outer's parse and its dispatch. The outer's real, correct result --
     * fetched via its own cursor afterward -- is the actual proof: if
     * depth-0's banks had been aliased with depth-1's, this would surface
     * as wrong rows, not just a crash. ─────────────────────────────────── */
    {
        struct SqlResult outer_out, inner_out;
        int ok = sql_exec_test_phase3_nesting(
            txn_id, 1,
            "SELECT w.group_id, COUNT(*) FROM widgets w JOIN wgroups g ON w.group_id = g.id GROUP BY w.group_id",
            "SELECT tag FROM gadgets WHERE tag = 'x'",
            &outer_out, &inner_out);
        CHECK(ok == 1, "scenario 1: nested dispatch reports overall success");
        CHECK(inner_out.error == SQL_ERR_NONE && inner_out.row_count == 1,
              "scenario 1: the depth-1 inner SELECT ran to completion correctly (exactly gadget 'x')");
        CHECK(outer_out.error == SQL_ERR_NONE && outer_out.row_count == 2,
              "scenario 1: the depth-0 outer JOIN+GROUP BY still produced its correct 2 groups after the nested call");
        struct collectN_ctx ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ncols = 2; ctx.idx[0] = 0; ctx.idx[1] = 1;
        cursor_fetch_rows(outer_out.cursor_id, 100, collectN_cb, &ctx);
        int saw_group1_count2 = 0, saw_group2_count1 = 0;
        for (uint32_t i = 0; i < ctx.count; i++) {
            if (strcmp(ctx.vals[i][0], "1") == 0 && strcmp(ctx.vals[i][1], "2") == 0) saw_group1_count2 = 1;
            if (strcmp(ctx.vals[i][0], "2") == 0 && strcmp(ctx.vals[i][1], "1") == 0) saw_group2_count1 = 1;
        }
        CHECK(saw_group1_count2, "scenario 1: group 1's COUNT(*) is exactly 2 (w1, w2) -- not corrupted by the nested gadgets query");
        CHECK(saw_group2_count1, "scenario 1: group 2's COUNT(*) is exactly 1 (w3) -- not corrupted by the nested gadgets query");
        cursor_close(outer_out.cursor_id);
    }

    /* ── Scenario 2: roles swapped -- outer is the plain SELECT, inner is
     * the JOIN+GROUP BY -- proving the banking isn't order-dependent. ──── */
    {
        struct SqlResult outer_out, inner_out;
        int ok = sql_exec_test_phase3_nesting(
            txn_id, 1,
            "SELECT tag FROM gadgets WHERE tag = 'z'",
            "SELECT w.group_id, COUNT(*) FROM widgets w JOIN wgroups g ON w.group_id = g.id GROUP BY w.group_id",
            &outer_out, &inner_out);
        CHECK(ok == 1, "scenario 2: nested dispatch reports overall success (roles swapped)");
        CHECK(inner_out.error == SQL_ERR_NONE && inner_out.row_count == 2,
              "scenario 2: the depth-1 inner JOIN+GROUP BY ran to completion correctly (2 groups)");
        CHECK(outer_out.error == SQL_ERR_NONE && outer_out.row_count == 1,
              "scenario 2: the depth-0 outer plain SELECT still produced its correct 1 row after the nested call");
        cursor_close(outer_out.cursor_id);
    }

    /* ── Scenario 3: a would-be third nesting level (depth 1 -> 2) fails
     * loud with SQL_ERR_NESTING_TOO_DEEP, never silently reusing bank[1]
     * while it's still the live depth-1 bank. ──────────────────────────── */
    {
        struct SqlResult scratch;
        int ok = sql_exec_test_phase3_depth_exceeded(&scratch);
        CHECK(ok == 1, "scenario 3: entering a 3rd nesting level is rejected exactly as designed");
        CHECK(scratch.error == SQL_ERR_NESTING_TOO_DEEP, "scenario 3: the rejection's error code is SQL_ERR_NESTING_TOO_DEEP");
    }

    /* ── Scenario 4: after the depth-exceeded probe unwinds itself, an
     * ordinary top-level query still works normally -- no leaked depth
     * state from scenario 3 poisons later queries. ─────────────────────── */
    CHECK(sql_execute(1, "SELECT * FROM gadgets", &r) == 0 && r.row_count == 3,
          "scenario 4: a normal top-level query after the depth-exceeded probe still sees all 3 gadgets");
    cursor_close(r.cursor_id);

    /* ── Scenario 5: plain single-level dispatch (no nesting touched at
     * all) is still byte-identical to pre-Phase-3 behavior -- the banking
     * macros resolve to bank[0] exactly like the old unbanked globals did. */
    CHECK(sql_execute(1, "SELECT w.group_id, COUNT(*) FROM widgets w JOIN wgroups g ON w.group_id = g.id GROUP BY w.group_id", &r) == 0 &&
          r.row_count == 2,
          "scenario 5: single-level JOIN+GROUP BY dispatch (no nesting) is unaffected by the banking change");
    cursor_close(r.cursor_id);

    sql_tx_commit(txn_id);

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
