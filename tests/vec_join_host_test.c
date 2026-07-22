/*
 * vec_join_host_test.c — Vector Store Roadmap Phase 5 verification: the
 * first host test in this project to link kernel/vecstore.c AND the full
 * relational stack (kernel/sql_exec.c, sql_parser.c, predicate.c,
 * row_index.c, rowstore.c, persist.c, cursor.c, mvcc.c, row_constraint.c,
 * row_journal.c) together in one binary, plus the real, unmodified
 * kernel/vec_join.c under test -- confirmed by direct grep before writing
 * this file that no existing host test combines both subsystems (every
 * prior phase kept them deliberately separate).
 *
 * Creates a real vector collection AND a real relational table, inserts
 * genuinely related data into both (an employee's row id doubles as the
 * external_id on its embedding), runs a real vecstore_search(), and
 * confirms vec_join_resolve() correctly resolves the search's external_ids
 * back to the right relational rows via real sql_execute() calls --
 * exactly the verification plan the roadmap's own §7 named.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/vec_join_host_test tests/vec_join_host_test.c \
 *       kernel/vec_join.c kernel/vecstore.c kernel/vec_index.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c kernel/database.c
 *   /tmp/vec_join_host_test
 *
 * Gap Remediation Phase B note: this link line was missing kernel/vec_index.c
 * (found while regression-sweeping Phase B's own changes) -- Phase 6, built
 * after this test, added vec_index_notify_insert()/_delete() calls into
 * vecstore.c's vecstore_insert()/_delete() but never updated this file's own
 * documented build line to match, leaving it unable to link since. Not a
 * Phase B regression; a pre-existing Phase 6 gap this pass happened to
 * surface and fixes alongside its own work.
 *
 * Gap Remediation Phase C note: Scenarios 4-6 (appended below) cover the new
 * syscall adapters this file's own link line already supports linking --
 * sys_sls_vec_index_create()/sys_sls_vec_index_search() (kernel/vec_index.c),
 * sys_sls_vec_join() (kernel/vec_join.c), and the smoke-tested
 * sys_sls_vec_list()/sys_sls_vec_index_list() (kernel/vecstore.c,
 * kernel/vec_index.c). Extended in place rather than as a new file, matching
 * Phase B's own precedent of extending sql_exec_host_test.c rather than
 * forking -- this file already links the full stack every new scenario
 * needs (vec_join.c + vecstore.c + vec_index.c + the relational stack), so a
 * new file would only duplicate that setup for no real isolation benefit.
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/predicate.h"
#include "kernel/sql_exec.h"
#include "kernel/cursor.h"
#include "kernel/index_mgr.h"
#include "kernel/persist.h"
#include "kernel/vecstore.h"
#include "kernel/vec_join.h"
#include "kernel/vec_index.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Same combined stub set sql_exec_host_test.c already established,
 * plus vecstore.c's own ollama_embed() stub (see vecstore_host_test.c's
 * identical precedent) -- vec_join.c itself adds no new stub
 * requirements, confirming vec_join.h's own header comment that this file
 * is a pure consumer of both already-tested subsystems, not a third one
 * with its own dependency surface. ──────────────────────────────────────── */
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSIndex        index_store[INDEX_MAX];
uint32_t               index_count = 0;
void catalog_after_restore(void) { /* no-op for this test */ }
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }
// Database Namespace & Access Roadmap Phase 2: kernel/database.c's
// database_drop() calls catalog_get_role() for its permission gate -- this
// test never calls CREATE/DROP DATABASE, so the return value is a pure
// linkability stub, not exercised by any scenario below.
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }

int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    return 1;   // this suite is about vec_join.c's own correlation logic, not permission gating
}
void* allocate_physical_ram_frame(void) { return malloc(4096); }

int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp) {
    (void)req; (void)resp;
    return -1;   // unused by this suite -- see vecstore_host_test.c's identical stub/comment
}

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

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static void make_employees_table(void) {
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "employees");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xE801;
    object_catalog[0].active = 1;
    object_catalog_count = 1;   // must be set before rowstore_create_table() -- it locates the
                                 // object by scanning object_catalog[0..object_catalog_count), same
                                 // ordering requirement sql_exec_host_test.c's own precedent follows
    memset(&object_schemas[0], 0, sizeof(object_schemas[0]));
    strcpy(object_schemas[0].fields[0].key, "id");     object_schemas[0].fields[0].type = FIELD_TYPE_UINT64; object_schemas[0].fields[0].active = 1;
    strcpy(object_schemas[0].fields[1].key, "name");   object_schemas[0].fields[1].type = FIELD_TYPE_STRING; object_schemas[0].fields[1].active = 1;
    object_schemas[0].field_count = 2;
    rowstore_create_table("employees");
}

static void make_embeddings_collection(uint32_t idx, uint32_t dimension) {
    memset(&object_catalog[idx], 0, sizeof(object_catalog[idx]));
    strcpy(object_catalog[idx].name, "employee_embeddings");
    object_catalog[idx].type = OBJ_TYPE_DB_TABLE;
    object_catalog[idx].object_id = 0xE802;
    object_catalog[idx].active = 1;
    vecstore_create_collection("employee_embeddings", dimension);
}

/* ─── The collector this test exercises vec_join_resolve() with -- pairs
 * each resolved row with the VecMatch that produced it, so the assertions
 * below can check BOTH the relational data (name) AND the vector-search
 * metadata (distance, external_id) travelled through correctly together. ── */
struct resolved_row { uint64_t external_id; float distance; char name[64]; char id_text[32]; };
struct collect_ctx { struct resolved_row rows[64]; uint32_t count; };
static void collect_cb(const struct VecMatch* match, const struct RowValues* row, void* ctxp) {
    struct collect_ctx* ctx = (struct collect_ctx*)ctxp;
    if (ctx->count >= 64) return;
    struct resolved_row* r = &ctx->rows[ctx->count++];
    r->external_id = match->external_id;
    r->distance = match->distance;
    r->name[0] = '\0';
    r->id_text[0] = '\0';
    // "employees" schema order is id, name -- but this test deliberately
    // doesn't hardcode that positional assumption anywhere in vec_join.c
    // itself (it looks id_column up by name at runtime); it's fine to rely
    // on it here in the TEST's own assertions since we defined the schema
    // ourselves, just above.
    if (row->count >= 2) {
        strncpy(r->id_text, row->values[0], sizeof(r->id_text) - 1);
        strncpy(r->name, row->values[1], sizeof(r->name) - 1);
    }
}
static int find_resolved(struct collect_ctx* ctx, uint64_t ext_id) {
    for (uint32_t i = 0; i < ctx->count; i++)
        if (ctx->rows[i].external_id == ext_id) return (int)i;
    return -1;
}

// ─── Phase 5 (SQL Feature-Parity Roadmap, DDL) stand-ins -- sql_exec.c's
// new exec_create_table() now unconditionally calls sys_sls_valloc()/
// sys_sls_schema_set(), and rowstore.c's new rowstore_drop_table() now
// unconditionally calls sys_sls_vfree() (real object_catalog.c cleanup
// this test doesn't link). This test never exercises CREATE/DROP TABLE
// via SQL text at runtime, so failure-code no-ops are safe here -- see
// tests/sql_ddl_phase5_host_test.c for the real coverage of these paths. ──
uint64_t sys_sls_valloc(struct SLSVallocRequest* req) { (void)req; return 0; }
uint64_t sys_sls_schema_set(struct SLSSchemaRequest* req) { (void)req; return 1; }
uint64_t sys_sls_vfree(const char* name) { (void)name; return 1; }

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();
    vecstore_init();
    vec_index_init();

    make_employees_table();   // sets object_catalog_count = 1 internally

    /* ── Setup: 5 employees, each with a 3-dim embedding whose external_id
     * equals its own "id" column -- the real correlation key this whole
     * phase is about. Vectors are placed along one axis at controlled
     * distances so nearest-neighbor ordering is unambiguous and hand-
     * verifiable (0,1,0), (1,1,0), (3,1,0), (6,1,0), (10,1,0) for ids
     * 1..5 -- distances from a query at (0,1,0) grow strictly with id. ──── */
    struct SqlResult r;
    for (int id = 1; id <= 5; id++) {
        char q[256];
        snprintf(q, sizeof(q), "INSERT INTO employees (id, name) VALUES (%d, 'employee%d')", id, id);
        CHECK(sql_execute(1, q, &r) == 0, "setup: employee row inserted");
    }
    object_catalog_count = 2;
    make_embeddings_collection(1, 3);

    struct VecId vid;
    float xs[5] = {0.0f, 1.0f, 3.0f, 6.0f, 10.0f};
    for (int i = 0; i < 5; i++) {
        struct VecValues v = { 3, { xs[i], 1.0f, 0.0f } };
        CHECK(vecstore_insert(1, "employee_embeddings", (uint64_t)(i + 1), &v, &vid) == 0,
              "setup: embedding inserted with external_id == employee id");
    }
    // One more embedding whose external_id has NO corresponding employee
    // row -- a real, honest possibility this phase's own contract must
    // handle without crashing (see vec_join.h's own "may be less than
    // match_count" comment).
    {
        struct VecValues v = { 3, { 100.0f, 1.0f, 0.0f } };
        vecstore_insert(1, "employee_embeddings", 9999, &v, &vid);
    }

    /* ── Scenario 1: search + join, end to end ───────────────────────────── */
    {
        struct VecValues query = { 3, { 0.0f, 1.0f, 0.0f } };
        struct VecMatch matches[6];
        uint32_t n = vecstore_search(1, "employee_embeddings", &query, VEC_METRIC_L2, 3, matches);
        CHECK(n == 3, "s1: vecstore_search returns the requested top-3");
        CHECK(matches[0].external_id == 1 && matches[1].external_id == 2 && matches[2].external_id == 3,
              "s1: nearest-neighbor order is employees 1, 2, 3 (closest to the query point)");

        struct collect_ctx ctx = { {{0}}, 0 };
        uint32_t delivered = vec_join_resolve(1, "employees", "id", matches, n, collect_cb, &ctx);
        CHECK(delivered == 3, "s1: vec_join_resolve delivers exactly 3 (match, row) pairs for 3 real matches");
        CHECK(ctx.count == 3, "s1: the collector callback fired exactly 3 times");

        int i1 = find_resolved(&ctx, 1), i2 = find_resolved(&ctx, 2), i3 = find_resolved(&ctx, 3);
        CHECK(i1 >= 0 && strcmp(ctx.rows[i1].name, "employee1") == 0, "s1: external_id=1 resolved to the real 'employee1' row");
        CHECK(i2 >= 0 && strcmp(ctx.rows[i2].name, "employee2") == 0, "s1: external_id=2 resolved to the real 'employee2' row");
        CHECK(i3 >= 0 && strcmp(ctx.rows[i3].name, "employee3") == 0, "s1: external_id=3 resolved to the real 'employee3' row");
        CHECK(ctx.rows[i1].distance < ctx.rows[i2].distance && ctx.rows[i2].distance < ctx.rows[i3].distance,
              "s1: each resolved row still carries its ORIGINAL search distance, in the correct relative order");
    }

    /* ── Scenario 2: an external_id with no corresponding relational row --
     * must be silently omitted, not crash and not fabricate a row. ────────── */
    {
        struct VecValues query = { 3, { 100.0f, 1.0f, 0.0f } };
        struct VecMatch matches[6];
        uint32_t n = vecstore_search(1, "employee_embeddings", &query, VEC_METRIC_L2, 1, matches);
        CHECK(n == 1 && matches[0].external_id == 9999, "s2: search finds the orphan embedding (external_id=9999, no employee row)");

        struct collect_ctx ctx = { {{0}}, 0 };
        uint32_t delivered = vec_join_resolve(1, "employees", "id", matches, n, collect_cb, &ctx);
        CHECK(delivered == 0, "s2: an external_id with no matching row resolves to 0 delivered pairs, not a crash");
    }

    /* ── Scenario 3: batching -- more matches than fit in one
     * VEC_JOIN_MAX_IDS_PER_QUERY-sized OR-chain, forcing vec_join_resolve()
     * to issue multiple underlying sql_execute() calls and still resolve
     * every one correctly. 20 more employees + embeddings, searched with
     * k=20 (> VEC_JOIN_MAX_IDS_PER_QUERY=16), so this genuinely exercises
     * the multi-batch path, not just a single query. ───────────────────────── */
    {
        for (int id = 10; id <= 29; id++) {
            char q[256];
            snprintf(q, sizeof(q), "INSERT INTO employees (id, name) VALUES (%d, 'batch%d')", id, id);
            sql_execute(1, q, &r);
            struct VecValues v = { 3, { (float)(1000 + id), 1.0f, 0.0f } };
            struct VecId bid;
            vecstore_insert(1, "employee_embeddings", (uint64_t)id, &v, &bid);
        }

        struct VecValues query = { 3, { 1000.0f, 1.0f, 0.0f } };
        struct VecMatch matches[20];
        uint32_t n = vecstore_search(1, "employee_embeddings", &query, VEC_METRIC_L2, 20, matches);
        CHECK(n == 20, "s3: search returns 20 matches (all 20 batch-inserted embeddings, closest to this query)");

        struct collect_ctx ctx = { {{0}}, 0 };
        uint32_t delivered = vec_join_resolve(1, "employees", "id", matches, n, collect_cb, &ctx);
        CHECK(delivered == 20, "s3: all 20 matches resolve correctly across multiple internal batches (> VEC_JOIN_MAX_IDS_PER_QUERY)");
        int all_correct = 1;
        for (uint32_t i = 0; i < n; i++) {
            int idx = find_resolved(&ctx, matches[i].external_id);
            char expect[64];
            snprintf(expect, sizeof(expect), "batch%llu", (unsigned long long)matches[i].external_id);
            if (idx < 0 || strcmp(ctx.rows[idx].name, expect) != 0) all_correct = 0;
        }
        CHECK(all_correct, "s3: every one of the 20 batched matches resolved to its own correct employee row");
    }

    /* ── Scenario 4: Gap Remediation Phase C -- sys_sls_vec_index_create()/
     * sys_sls_vec_index_search(), the HNSW syscall adapters. vec_index_create()
     * does NOT backfill an already-populated collection (vec_index.h's own
     * header comment, point 7), so the 26 embeddings inserted above are
     * deliberately NOT expected to appear in this index -- fresh, controlled-
     * position vectors are inserted AFTER index creation instead, exercising
     * the real auto-maintenance path (vecstore_insert() -> vec_index_notify_
     * insert()) rather than assuming it works. ─────────────────────────────── */
    {
        struct SLSVecIndexCreateRequest creq;
        memset(&creq, 0, sizeof(creq));
        creq.caller_uid = 1;
        strcpy(creq.index_name, "emp_hnsw");
        strcpy(creq.collection_name, "employee_embeddings");
        creq.metric = VEC_METRIC_L2;
        uint64_t crc = sys_sls_vec_index_create(&creq);
        CHECK(crc == 0 && creq.status == 0, "s4: sys_sls_vec_index_create creates an HNSW index over employee_embeddings");

        // Positions deliberately far from every other embedding already in
        // this shared collection (employee ids 1-5 at x in {0,1,3,6,10}, the
        // orphan at x=100, the 20 batch entries at x in [1010,1029]) -- a
        // real bug caught during verification: an earlier version of this
        // scenario reused x=0/2/5/9, which collided with employee id=1's own
        // (0,1,0) vector and skewed Scenario 5's later top-3 search. Distinct
        // ranges keep each scenario's own nearest-neighbor assertions
        // unambiguous.
        float hxs[4] = {50.0f, 52.0f, 55.0f, 59.0f};
        for (int i = 0; i < 4; i++) {
            struct VecValues v = { 3, { hxs[i], 1.0f, 0.0f } };
            struct VecId hid;
            CHECK(vecstore_insert(1, "employee_embeddings", (uint64_t)(500 + i), &v, &hid) == 0,
                  "s4: setup: post-index embedding inserted (auto-indexed via vec_index_notify_insert)");
        }

        struct SLSVecIndexSearchRequest sreq;
        memset(&sreq, 0, sizeof(sreq));
        sreq.caller_uid = 1;
        strcpy(sreq.index_name, "emp_hnsw");
        sreq.query.count = 3;
        sreq.query.values[0] = 50.0f; sreq.query.values[1] = 1.0f; sreq.query.values[2] = 0.0f;
        sreq.k = 2;
        sreq.ef = 64;
        uint64_t src = sys_sls_vec_index_search(&sreq);
        CHECK(src == 0, "s4: sys_sls_vec_index_search returns success status");
        CHECK(sreq.match_count == 2, "s4: HNSW search returns the requested top-2");
        CHECK(sreq.truncated == 0, "s4: k=2 well under VEC_SEARCH_MAX_K, not truncated");
        // Only 4 well-separated post-index points and ef=64 (>> node count) --
        // recall should be exact here, verified against the known layout
        // rather than asserting HNSW's general approximate contract (see
        // vec_index.h's own header comment on why exactness isn't guaranteed
        // in general).
        int found_500 = 0;
        for (uint32_t i = 0; i < sreq.match_count; i++)
            if (sreq.matches[i].external_id == 500) found_500 = 1;
        CHECK(found_500, "s4: nearest post-index point (external_id=500, exactly at the query) is in the top-2");
    }

    /* ── Scenario 5: Gap Remediation Phase C -- sys_sls_vec_join(), the join
     * syscall adapter. Feeds it real matches from a real vecstore_search()
     * call (mirroring how a caller would copy SLSVecSearchRequest.matches[]
     * across in practice) and confirms it resolves exactly like the already-
     * verified vec_join_resolve() does in Scenario 1. ───────────────────────── */
    {
        struct VecValues query = { 3, { 0.0f, 1.0f, 0.0f } };
        struct VecMatch matches[3];
        uint32_t n = vecstore_search(1, "employee_embeddings", &query, VEC_METRIC_L2, 3, matches);
        CHECK(n == 3, "s5: setup: exact search still returns top-3 (unaffected by the HNSW index existing alongside)");
        CHECK(matches[0].external_id == 1 && matches[1].external_id == 2 && matches[2].external_id == 3,
              "s5: setup: top-3 is still employees 1, 2, 3 (Scenario 4's own probe points live far away at x>=50, no collision)");

        struct SLSVecJoinRequest jreq;
        memset(&jreq, 0, sizeof(jreq));
        jreq.caller_uid = 1;
        strcpy(jreq.table_name, "employees");
        strcpy(jreq.id_column, "id");
        memcpy(jreq.matches, matches, sizeof(struct VecMatch) * n);
        jreq.match_count = n;
        uint64_t jrc = sys_sls_vec_join(&jreq);
        CHECK(jrc == 0, "s5: sys_sls_vec_join always returns 0 (per its own documented contract)");
        CHECK(jreq.result_count == 3, "s5: sys_sls_vec_join resolves all 3 matches to real employee rows");
        CHECK(jreq.truncated == 0, "s5: result_count (3) is well under VEC_JOIN_MAX_RESULTS, not truncated");

        int all_match = 1;
        for (uint32_t i = 0; i < jreq.result_count && i < VEC_JOIN_MAX_RESULTS; i++) {
            char expect[32];
            snprintf(expect, sizeof(expect), "employee%llu", (unsigned long long)jreq.results[i].match.external_id);
            if (jreq.results[i].row.count < 2 || strcmp(jreq.results[i].row.values[1], expect) != 0) all_match = 0;
        }
        CHECK(all_match, "s5: every resolved row's name matches its own match's external_id, via the syscall path");
    }

    /* ── Scenario 6: Gap Remediation Phase C -- sys_sls_vec_list()/
     * sys_sls_vec_index_list() smoke tests. Both are console-dump functions
     * with no return value, matching this project's established precedent
     * of not asserting on sys_sls_obj_list()-style console output -- this
     * just confirms they run without crashing against a real, populated
     * collection/index set (26+4 embeddings, 1 HNSW index). ─────────────────── */
    {
        sys_sls_vec_list();
        sys_sls_vec_index_list();
        CHECK(1, "s6: sys_sls_vec_list()/sys_sls_vec_index_list() run without crashing against real data");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
