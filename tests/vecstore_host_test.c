/*
 * vecstore_host_test.c — Vector Store Roadmap Phase 1 verification: a
 * standalone host-buildable round-trip test for kernel/vecstore.c's new
 * vector collection storage engine, linked against the REAL, unmodified
 * kernel/vecstore.c — not a reimplementation.
 *
 * vecstore.c's own dependencies (see vecstore.h's header comment on why
 * this phase is RAM-only) are lighter than rowstore.c's: object_catalog.h's
 * types/globals, frame_pool.h (proven shallow and host-testable since
 * Phase 13), and catalog_check_access() (a real function in
 * object_catalog.c, NOT linked here for the same "heavy dependency graph"
 * reason rowstore_host_test.c already established -- stubbed, call-tracked
 * so this test can still verify vecstore.c gates every CRUD call on it).
 *
 * Gap Remediation Phase D update: vecstore.c's page pool now lazy-loads
 * from / eagerly flushes to NVMe (see vecstore.h's own header comment) and
 * calls persist_vecstore_headers() after every mutating call -- this test
 * still has zero interest in exercising real persistence (that's persist_
 * rdbms_vecstore_host_test.c's job), so both are stubbed at their own real
 * API boundary rather than linking kernel/persist.c: a minimal fake in-
 * memory NVMe map (matching every other host test's identical precedent)
 * for nvme_read_sync()/nvme_write_sync(), and a no-op persist_vecstore_
 * headers() (the one function this test would otherwise need the whole of
 * persist.c, and therefore object_catalog[]/role_table[]/etc.'s full
 * surface, just to link).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/vecstore_host_test tests/vecstore_host_test.c \
 *       kernel/vecstore.c kernel/vec_index.c
 *   /tmp/vecstore_host_test
 *
 * Gap Remediation Phase D note: this link line was missing kernel/vec_
 * index.c -- a THIRD instance of the same pre-existing Phase 6 gap already
 * found and fixed in vec_join_host_test.c (Phase B) and vecstore_syscall_
 * host_test.c (Phase C): vecstore_insert()/vecstore_delete() have called
 * vec_index_notify_insert()/_delete() unconditionally since Phase 6, but
 * this file's own documented build line was never updated to match, and
 * (unlike the other two) this gap had apparently never surfaced before
 * because nothing had rebuilt this specific test since. Found during
 * Phase D's own regression sweep, fixed the same one-line way.
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/vecstore.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ─── catalog_check_access() stub — call-tracked, mirroring
 * rowstore_host_test.c's own precedent exactly: confirms vecstore.c gates
 * every CRUD call on PERM_READ (get/scan) or PERM_WRITE (insert/delete),
 * and lets one scenario force a denial. ──────────────────────────────────── */
static int      g_access_calls = 0;
static uint32_t g_access_last_perm = 0;
static int      g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name;
    g_access_calls++;
    g_access_last_perm = needed_perm;
    return g_access_force_deny ? 0 : 1;
}

void* allocate_physical_ram_frame(void) { return malloc(4096); }

/* ollama_embed() stub -- Phase 4 gave vecstore.c's sys_sls_vec_embed_insert()
 * a real link-time dependency on net/ollama_client.c's ollama_embed(), but
 * this suite is scoped to Phase 1's own storage engine and never exercises
 * the embed-insert path -- stubbed just to satisfy the linker, matching
 * this file's own established "stub the heavy dependency at its own real
 * API boundary" pattern for catalog_check_access() above. Real embed-insert
 * marshaling logic is covered by vecstore_syscall_host_test.c instead. */
int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp) {
    (void)req; (void)resp;
    return -1;
}

/* ─── Gap Remediation Phase D stubs -- see this file's own top comment ──── */
void persist_vecstore_headers(void) { /* no-op for this test -- see top comment */ }
void persist_vec_index_defs(void) { /* no-op -- vec_index.c is linked only to satisfy
                                        vecstore.c's own vec_index_notify_insert/delete()
                                        calls; this test never calls vec_index_create() */ }

#define FAKE_NVME_MAX_FRAMES 128
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

static void make_object(uint32_t idx, const char* name, uint64_t object_id) {
    memset(&object_catalog[idx], 0, sizeof(object_catalog[idx]));
    strcpy(object_catalog[idx].name, name);
    object_catalog[idx].type      = OBJ_TYPE_DB_TABLE;
    object_catalog[idx].object_id = object_id;
    object_catalog[idx].active    = 1;
}

struct scan_ctx { uint32_t count; uint64_t ext_ids[64]; float first_vals[64]; };
static void scan_cb(struct VecId id, uint64_t external_id, const struct VecValues* v, void* ctxp) {
    (void)id;
    struct scan_ctx* ctx = (struct scan_ctx*)ctxp;
    if (ctx->count < 64) {
        ctx->ext_ids[ctx->count] = external_id;
        ctx->first_vals[ctx->count] = v->values[0];
        ctx->count++;
    }
}

int main(void) {
    vecstore_init();

    /* ── Scenario 1: create_collection against a nonexistent object fails. ── */
    CHECK(vecstore_create_collection(1, "no_such_object", 4) == 1,
          "s1: create_collection against a nonexistent catalog object fails");

    make_object(0, "embeddings", 0xC001);
    object_catalog_count = 1;

    /* ── Scenario 2: dimension bounds are enforced. ─────────────────────────── */
    CHECK(vecstore_create_collection(1, "embeddings", 0) == 1, "s2: dimension=0 is rejected");
    CHECK(vecstore_create_collection(1, "embeddings", VECSTORE_MAX_DIMENSION + 1) == 1,
          "s2: dimension > VECSTORE_MAX_DIMENSION is rejected");

    /* ── Scenario 3: a real create_collection succeeds and computes a
     * sane layout. ──────────────────────────────────────────────────────────── */
    CHECK(vecstore_create_collection(1, "embeddings", 4) == 0, "s3: create_collection(dimension=4) succeeds");
    CHECK(vector_collections[0].active == 1, "s3: vector_collections[0].active is set");
    CHECK(vector_collections[0].dimension == 4, "s3: dimension recorded correctly");
    CHECK(vector_collections[0].entry_width == 1 + 8 + 4 * 4, "s3: entry_width = tombstone + external_id + 4 floats");
    CHECK(vector_collections[0].entries_per_page == (VECSTORE_PAGE_SIZE - 4) / vector_collections[0].entry_width,
          "s3: entries_per_page computed correctly");

    /* ── Scenario 4: re-promoting the same object fails (idempotency, no
     * catalog flag involved -- see vecstore.h's own header comment). ──────── */
    CHECK(vecstore_create_collection(1, "embeddings", 8) == 1, "s4: re-promoting an already-promoted object fails");

    /* ── Scenario 5: insert rejects a values->count mismatch. ───────────────── */
    struct VecValues bad = { .count = 3, .values = {1.0f, 2.0f, 3.0f} };
    struct VecId dummy_id;
    CHECK(vecstore_insert(1, "embeddings", 100, &bad, &dummy_id) == 4,
          "s5: insert with values->count != dimension is rejected");

    /* ── Scenario 6: a real insert succeeds and a real get round-trips the
     * exact values AND the external_id. ────────────────────────────────────── */
    struct VecValues v1 = { .count = 4, .values = {1.5f, -2.25f, 0.0f, 3.125f} };
    struct VecId id1;
    CHECK(vecstore_insert(1, "embeddings", 42, &v1, &id1) == 0, "s6: insert succeeds");
    CHECK(vector_collections[0].entry_count == 1, "s6: entry_count is now 1");

    uint64_t got_ext;
    struct VecValues got;
    CHECK(vecstore_get(1, "embeddings", id1, &got_ext, &got) == 0, "s6: get succeeds");
    CHECK(got_ext == 42, "s6: external_id round-trips exactly");
    CHECK(got.count == 4 &&
          got.values[0] == 1.5f && got.values[1] == -2.25f &&
          got.values[2] == 0.0f && got.values[3] == 3.125f,
          "s6: all 4 vector components round-trip exactly (bit-exact float storage)");

    /* ── Scenario 7: a bad/stale VecId fails cleanly. ────────────────────────── */
    struct VecId bad_id = { 9999, 0 };
    CHECK(vecstore_get(1, "embeddings", bad_id, NULL, NULL) == 3, "s7: get with an out-of-range page_id fails (3)");

    /* ── Scenario 8: delete tombstones the entry; a subsequent get fails. ───── */
    CHECK(vecstore_delete(1, "embeddings", id1) == 0, "s8: delete succeeds");
    CHECK(vector_collections[0].entry_count == 0, "s8: entry_count is back to 0");
    CHECK(vecstore_get(1, "embeddings", id1, NULL, NULL) == 3, "s8: get on a deleted entry fails (3)");
    CHECK(vecstore_delete(1, "embeddings", id1) == 3, "s8: deleting an already-deleted entry fails (3)");

    /* ── Scenario 9: scan visits every active entry with correct data,
     * skipping tombstoned ones. ─────────────────────────────────────────────── */
    struct VecValues v2 = { .count = 4, .values = {10.0f, 20.0f, 30.0f, 40.0f} };
    struct VecValues v3 = { .count = 4, .values = {100.0f, 200.0f, 300.0f, 400.0f} };
    struct VecId id2, id3;
    vecstore_insert(1, "embeddings", 200, &v2, &id2);
    vecstore_insert(1, "embeddings", 300, &v3, &id3);
    // id1's slot is tombstoned (deleted in s8) -- scan must skip it.

    struct scan_ctx ctx = {0};
    uint32_t visited = vecstore_collection_scan(1, "embeddings", scan_cb, &ctx);
    CHECK(visited == 2, "s9: scan visits exactly the 2 live entries, skipping the tombstoned one");
    CHECK(ctx.count == 2 &&
          ((ctx.ext_ids[0] == 200 && ctx.ext_ids[1] == 300) || (ctx.ext_ids[0] == 300 && ctx.ext_ids[1] == 200)),
          "s9: scan reports the correct external_ids");

    /* ── Scenario 10: permission denial blocks every CRUD entry point. ──────── */
    g_access_force_deny = 1;
    CHECK(vecstore_insert(1, "embeddings", 999, &v2, &dummy_id) == 2, "s10: insert is denied when catalog_check_access() refuses");
    CHECK(vecstore_get(1, "embeddings", id2, NULL, NULL) == 2, "s10: get is denied when catalog_check_access() refuses");
    CHECK(vecstore_delete(1, "embeddings", id2) == 2, "s10: delete is denied when catalog_check_access() refuses");
    CHECK(vecstore_collection_scan(1, "embeddings", scan_cb, &ctx) == 0, "s10: scan returns 0 when catalog_check_access() refuses");
    g_access_force_deny = 0;
    // The sequence above ends with the scan call, which gates on PERM_READ
    // (not the delete before it, which gated on PERM_WRITE) -- confirming
    // the LAST call's perm reflects scan's own real gate, not a stale value.
    CHECK(g_access_calls > 0 && g_access_last_perm == PERM_READ, "s10: the last gated call (scan) correctly requested PERM_READ");

    /* ── Scenario 11: operating against an unknown collection name fails
     * cleanly (1), not a crash. ─────────────────────────────────────────────── */
    CHECK(vecstore_insert(1, "no_such_collection", 1, &v2, &dummy_id) == 1, "s11: insert against an unknown collection fails (1)");
    CHECK(vecstore_get(1, "no_such_collection", id2, NULL, NULL) == 1, "s11: get against an unknown collection fails (1)");
    CHECK(vecstore_delete(1, "no_such_collection", id2) == 1, "s11: delete against an unknown collection fails (1)");
    CHECK(vecstore_collection_scan(1, "no_such_collection", scan_cb, &ctx) == 0, "s11: scan against an unknown collection visits 0");

    /* ── Scenario 12: multi-page storage -- a "wide" collection (large
     * dimension) whose entries_per_page is deliberately 1, so every insert
     * forces a brand-new page, real coverage of the page-chain-linking path
     * without needing hundreds of inserts to trigger it. ───────────────────── */
    make_object(1, "wide_vectors", 0xC002);
    object_catalog_count = 2;
    CHECK(vecstore_create_collection(1, "wide_vectors", 800) == 0, "s12: create_collection(dimension=800) succeeds");
    CHECK(vector_collections[1].entries_per_page == 1, "s12: a dimension=800 collection packs exactly 1 entry/page");

    struct VecValues wide_a, wide_b, wide_c;
    wide_a.count = wide_b.count = wide_c.count = 800;
    for (uint32_t i = 0; i < 800; i++) { wide_a.values[i] = (float)i; wide_b.values[i] = (float)(i * 2); wide_c.values[i] = (float)(i * 3); }
    struct VecId wa, wb, wc;
    CHECK(vecstore_insert(1, "wide_vectors", 1, &wide_a, &wa) == 0, "s12: insert 1 of 3 (forces page 1)");
    CHECK(vecstore_insert(1, "wide_vectors", 2, &wide_b, &wb) == 0, "s12: insert 2 of 3 (forces page 2, chains to page 1)");
    CHECK(vecstore_insert(1, "wide_vectors", 3, &wide_c, &wc) == 0, "s12: insert 3 of 3 (forces page 3, chains to page 2)");
    CHECK(vector_collections[1].page_count == 3, "s12: 3 pages were allocated, one per entry");

    uint64_t wb_ext; struct VecValues wb_got;
    CHECK(vecstore_get(1, "wide_vectors", wb, &wb_ext, &wb_got) == 0 && wb_ext == 2 && wb_got.values[799] == 799.0f * 2,
          "s12: the middle page's entry round-trips correctly (proves the page chain, not just the head page)");

    struct scan_ctx wide_ctx = {0};
    uint32_t wide_visited = vecstore_collection_scan(1, "wide_vectors", scan_cb, &wide_ctx);
    CHECK(wide_visited == 3, "s12: scanning a 3-page collection visits all 3 entries across the whole chain");

    /* ── Scenario 13: the two collections are fully independent -- deleting
     * from one doesn't touch the other. ─────────────────────────────────────── */
    vecstore_delete(1, "wide_vectors", wa);
    CHECK(vector_collections[1].entry_count == 2, "s13: wide_vectors' own entry_count reflects its own delete");
    CHECK(vector_collections[0].entry_count == 2, "s13: embeddings' entry_count is unaffected by wide_vectors' delete");

    /* ── Scenario 14 (VectorStore Gap Analysis §1.3): opt-in external_id
     * uniqueness -- off by default, toggled on/off, dedup rejects a repeat,
     * a tombstoned external_id is free to reuse, and toggling off again
     * un-blocks it without touching any existing rows. ──────────────────── */
    make_object(2, "unique_test", 0xC003);
    object_catalog_count = 3;
    CHECK(vecstore_create_collection(1, "unique_test", 4) == 0, "s14: create_collection(unique_test) succeeds");
    CHECK(vector_collections[2].unique_external_id == 0,
          "s14: unique_external_id defaults to 0 (off) on a freshly created collection");

    struct VecValues u1 = { 4, {1,1,1,1} };
    struct VecId u1id;
    CHECK(vecstore_insert(1, "unique_test", 7, &u1, &u1id) == 0,
          "s14: insert with external_id=7 succeeds while uniqueness is off");
    CHECK(vecstore_insert(1, "unique_test", 7, &u1, &u1id) == 0,
          "s14: a second insert with the SAME external_id=7 also succeeds while uniqueness is off (pre-existing, unchanged default behavior)");

    CHECK(vecstore_set_unique_external_id(1, "no_such_object", 1) == 1,
          "s14: set_unique_external_id against a nonexistent collection fails (1)");
    CHECK(vecstore_set_unique_external_id(1, "unique_test", 1) == 0,
          "s14: set_unique_external_id(unique_test, on) succeeds");
    CHECK(vector_collections[2].unique_external_id == 1, "s14: unique_external_id is now 1 (on)");

    CHECK(vecstore_insert(1, "unique_test", 7, &u1, &u1id) == 6,
          "s14: inserting external_id=7 AGAIN now that uniqueness is on is rejected (6) -- two copies already exist from before it was turned on");
    struct VecValues u2 = { 4, {2,2,2,2} };
    struct VecId u2id;
    CHECK(vecstore_insert(1, "unique_test", 8, &u2, &u2id) == 0,
          "s14: inserting a genuinely new external_id=8 still succeeds with uniqueness on");
    CHECK(vecstore_insert(1, "unique_test", 8, &u2, &u2id) == 6,
          "s14: re-inserting external_id=8 is rejected (6)");

    CHECK(vecstore_delete(1, "unique_test", u2id) == 0, "s14: delete of external_id=8's entry succeeds");
    CHECK(vecstore_insert(1, "unique_test", 8, &u2, &u2id) == 0,
          "s14: external_id=8 is free to reuse immediately after its only live entry was tombstoned");

    g_access_force_deny = 1;
    CHECK(vecstore_set_unique_external_id(1, "unique_test", 0) == 2,
          "s14: set_unique_external_id is denied when catalog_check_access() refuses (2)");
    g_access_force_deny = 0;
    CHECK(vecstore_set_unique_external_id(1, "unique_test", 0) == 0, "s14: set_unique_external_id(unique_test, off) succeeds");
    CHECK(vector_collections[2].unique_external_id == 0, "s14: unique_external_id is back to 0 (off)");
    CHECK(vecstore_insert(1, "unique_test", 7, &u1, &u1id) == 0,
          "s14: external_id=7 (already duplicated twice above) can be inserted again once uniqueness is back off -- turning it off never scans or deletes existing rows");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
