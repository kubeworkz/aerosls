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
 *   gcc -Wall -Wextra -std=c11 -I kernel -I drivers -I net \
 *       -o /tmp/vec_join_host_test vec_join_host_test.c \
 *       kernel/vec_join.c kernel/vecstore.c \
 *       kernel/sql_exec.c kernel/sql_parser.c kernel/predicate.c \
 *       kernel/row_index.c kernel/rowstore.c kernel/persist.c kernel/cursor.c \
 *       kernel/mvcc.c kernel/row_constraint.c kernel/row_journal.c
 *   /tmp/vec_join_host_test
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

int main(void) {
    row_index_init();
    rowstore_init();
    cursor_mgr_init();
    mvcc_init();
    vecstore_init();

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

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
