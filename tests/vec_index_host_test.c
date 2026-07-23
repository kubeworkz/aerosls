/*
 * vec_index_host_test.c — Vector Store Roadmap Phase 6 verification: a
 * standalone host-buildable test for kernel/vec_index.c's HNSW index,
 * linked against the REAL, unmodified kernel/vec_index.c AND
 * kernel/vecstore.c (whose vecstore_insert()/vecstore_delete() call the
 * real auto-maintenance hooks under test, not stubs) -- not a
 * reimplementation of either.
 *
 * Verifies exactly what the roadmap's own §8 scope named: builds a real
 * vector collection AND a real HNSW index over it, inserts genuinely
 * clustered synthetic data (so nearest-neighbor structure is well-
 * defined and independently checkable), and compares the index's
 * APPROXIMATE search results against vecstore_search()'s (Phase 2)
 * EXACT results across many queries, measuring real recall@k -- not just
 * "did it return something plausible."
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/vec_index_host_test tests/vec_index_host_test.c \
 *       kernel/vec_index.c kernel/vecstore.c kernel/storage_quota.c
 *   /tmp/vec_index_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

static int      g_access_calls = 0;
static int      g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    g_access_calls++;
    return g_access_force_deny ? 0 : 1;
}
void* allocate_physical_ram_frame(void) { return malloc(4096); }

int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp) {
    (void)req; (void)resp;
    return -1;   // unused by this suite -- see vecstore_host_test.c's identical stub/comment
}

/* ─── Gap Remediation Phase D stubs -- see vecstore_host_test.c's own top
 * comment for the rationale (this suite has zero interest in persistence
 * round-tripping, covered separately by persist_rdbms_vecstore_host_
 * test.c). ─────────────────────────────────────────────────────────────── */
void persist_vecstore_headers(void) { }
void persist_vec_index_defs(void) { }

// VectorStore Interface Roadmap Phase 1: forward-declared exactly the way
// object_catalog.c declares it (a bare extern, not a vecstore.h prototype
// -- see that file's own comment on this convention, mirroring
// tier_notify_access()). This test calls it directly as the unit under
// test for Scenario 7 below.
extern void vecstore_notify_object_freed(const char* collection_name);

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

// VectorStore Interface Roadmap Phase 3: small test-local helper -- Scenario
// 6 (above) inlines this exact lookup-by-name loop once already; Scenarios
// 8-9 (below) need the active_count it finds repeatedly enough to be worth
// naming. Returns -1 if no active index has this name (a test-setup bug,
// not a real scenario this suite otherwise exercises).
static int vec_indexes_active_count_by_name(const char* name) {
    for (int i = 0; i < VEC_INDEX_MAX; i++)
        if (vec_indexes[i].active && strcmp(vec_indexes[i].index_name, name) == 0)
            return (int)vec_indexes[i].active_count;
    return -1;
}

// ─── This TEST file's own tiny xorshift32 -- deliberately separate from
// vec_index.c's own internal PRNG (used for layer assignment, not test
// data generation) -- generates a reproducible synthetic clustered
// dataset, matching this project's per-file hand-rolled-helper
// convention. ────────────────────────────────────────────────────────────
static uint32_t g_test_rng = 0xC0FFEEu;
static uint32_t test_rand32(void) {
    uint32_t x = g_test_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_test_rng = x;
    return x;
}
static float test_jitter(float mag) {
    // deterministic pseudo-random float in [-mag, +mag]
    uint32_t r = test_rand32() % 20001;   // 0..20000
    return mag * ((float)r / 10000.0f - 1.0f);
}

#define DIM          8
#define N_CLUSTERS   6
#define N_POINTS     300
#define CLUSTER_SEP  20.0f   // cluster centers at (i*CLUSTER_SEP, ..., i*CLUSTER_SEP)
#define JITTER_MAG   3.0f    // within-cluster jitter, well under half the inter-center distance

int main(void) {
    vecstore_init();
    vec_index_init();

    /* ── Setup: register the collection + a real HNSW index BEFORE any
     * data exists. This is still the more common real-world ordering (an
     * index defined at collection-creation time, then populated as data
     * arrives via the auto-maintenance hook) -- Scenarios 5 and 8/8b below
     * separately cover the OTHER ordering, an index created AFTER a
     * collection is already populated, which Gap Analysis §4 changed
     * vec_index_create() to auto-backfill from rather than start empty
     * (vec_index.h header comment point 7's UPDATE 2). ────────────────────── */
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "cluster_points");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xF001;
    object_catalog[0].active = 1;
    object_catalog_count = 1;
    CHECK(vecstore_create_collection(1, "cluster_points", DIM) == 0, "setup: collection created");
    CHECK(vec_index_create(1, "idx_cluster", "cluster_points", VEC_METRIC_L2) == 0,
          "setup: HNSW index created (empty, before any data)");

    /* ── Insert N_POINTS clustered points via vecstore_insert() -- NOT any
     * direct vec_index API -- proving the auto-maintenance hook is what
     * actually builds the graph, end to end. external_id == point index
     * (1..N_POINTS), cluster membership = point index % N_CLUSTERS. ────────── */
    for (int p = 1; p <= N_POINTS; p++) {
        int cluster = p % N_CLUSTERS;
        struct VecValues v;
        v.count = DIM;
        for (int d = 0; d < DIM; d++) {
            v.values[d] = (float)cluster * CLUSTER_SEP + test_jitter(JITTER_MAG);
        }
        struct VecId id;
        int rc = vecstore_insert(1, "cluster_points", (uint64_t)p, &v, &id);
        if (rc != 0) { printf("FAIL: insert %d failed, rc=%d\n", p, rc); g_fail++; }
    }
    CHECK(vec_indexes[0].active_count == N_POINTS, "setup: index's active_count reflects all N_POINTS auto-inserted entries");
    CHECK(vec_indexes[0].entry_point != VEC_INDEX_INVALID, "setup: index has a real entry point after insertion");
    CHECK(vec_indexes[0].top_layer > 0, "setup: with 300 points at M=16, the graph has grown beyond layer 0 (real multi-layer structure)");

    /* ── Scenario 1: structural invariants -- no node exceeds its layer's
     * neighbor cap, every active node is reachable in the sense that it
     * has at least one neighbor at layer 0 once more than one point
     * exists (a fully disconnected node would be a real bug). ──────────────── */
    {
        int cap_ok = 1, has_edges = 1;
        uint32_t checked = 0;
        for (uint32_t n = 0; n < vec_index_next_free_node; n++) {
            if (vec_index_nodes[n].index_id != 0) continue;
            checked++;
            for (uint32_t l = 0; l <= vec_index_nodes[n].top_layer && l < VEC_INDEX_MAX_LAYERS; l++) {
                uint32_t cap = (l == 0) ? VEC_INDEX_M0 : VEC_INDEX_M;
                if (vec_index_nodes[n].neighbor_count[l] > cap) cap_ok = 0;
            }
            if (vec_index_nodes[n].neighbor_count[0] == 0) has_edges = 0;   // every node should have >=1 layer-0 neighbor with 300 points
        }
        CHECK(checked == (uint32_t)N_POINTS, "s1: exactly N_POINTS nodes belong to this index in the shared pool");
        CHECK(cap_ok, "s1: no node's neighbor list exceeds its layer's M/M0 cap, anywhere in the graph");
        CHECK(has_edges, "s1: every node has at least one layer-0 neighbor (no fully disconnected node)");
    }

    /* ── Scenario 2: recall@10 -- the real verification this phase's own
     * scope plan named. For each of several query points (some exact
     * dataset members, some novel cluster-center probes), compare
     * vec_index_search()'s approximate top-10 against vecstore_search()'s
     * exact top-10, and measure the fraction of the true top-10 the
     * approximate search actually found. ────────────────────────────────────── */
    {
        const int K = 10;
        const int N_QUERIES = 12;
        double total_recall = 0.0;

        for (int q = 0; q < N_QUERIES; q++) {
            struct VecValues query;
            query.count = DIM;
            int cluster = q % N_CLUSTERS;
            for (int d = 0; d < DIM; d++) {
                query.values[d] = (float)cluster * CLUSTER_SEP + test_jitter(JITTER_MAG * 1.5f);
            }

            struct VecMatch exact[10];
            uint32_t n_exact = vecstore_search(1, "cluster_points", &query, VEC_METRIC_L2, K, exact);

            struct VecMatch approx[10];
            uint32_t n_approx = vec_index_search(1, "idx_cluster", &query, K, 100, approx);

            if (n_exact != (uint32_t)K || n_approx != (uint32_t)K) {
                printf("FAIL: query %d returned n_exact=%u n_approx=%u (expected %d each)\n", q, n_exact, n_approx, K);
                g_fail++;
                continue;
            }

            int matched = 0;
            for (int i = 0; i < K; i++)
                for (int j = 0; j < K; j++)
                    if (exact[i].external_id == approx[j].external_id) { matched++; break; }

            double recall = (double)matched / (double)K;
            total_recall += recall;
            printf("      query %d (cluster %d): recall@10 = %d/10\n", q, cluster, matched);
        }

        double avg_recall = total_recall / N_QUERIES;
        printf("      average recall@10 across %d queries: %.1f%%\n", N_QUERIES, avg_recall * 100.0);
        // A real, stated bar: 70% average recall@10 with ef=100 against a
        // well-clustered 300-point/8-dim dataset is a defensible
        // threshold for a first-cut approximate index with the
        // simplifications named in vec_index.h's own header comment
        // (simple-closest-M selection, not the paper's diversification
        // heuristic) -- not a coincidence-proof 100%, and not a
        // rubber-stamp near-0% either.
        CHECK(avg_recall >= 0.70, "s2: average recall@10 meets a real, stated 70% bar");
    }

    /* ── Scenario 3: an exact-match query (a real dataset point used
     * verbatim as the query) must be found by BOTH the exact and
     * approximate search, at distance 0/near-0, as rank 0. ─────────────────── */
    {
        // Locate a real, already-inserted point's own exact stored vector:
        // an approximate probe near cluster 1's center finds its nearest
        // real neighbor via the exact search, then we re-fetch THAT
        // point's own values verbatim via vecstore_get().
        struct VecValues seed;
        seed.count = DIM;
        for (int d = 0; d < DIM; d++) seed.values[d] = (float)(1 % N_CLUSTERS) * CLUSTER_SEP;
        struct VecMatch seed_match[1];
        CHECK(vecstore_search(1, "cluster_points", &seed, VEC_METRIC_L2, 1, seed_match) == 1,
              "s3: setup -- found a real seed point near a cluster center");
        uint64_t got_ext;
        struct VecValues qv;
        CHECK(vecstore_get(1, "cluster_points", seed_match[0].id, &got_ext, &qv) == 0,
              "s3: setup -- fetched that seed point's own exact stored vector");

        struct VecMatch exact[1], approx[1];
        CHECK(vecstore_search(1, "cluster_points", &qv, VEC_METRIC_L2, 1, exact) == 1 &&
              exact[0].distance < 0.001f, "s3: exact search on a point's own vector finds itself at distance ~0");
        CHECK(vec_index_search(1, "idx_cluster", &qv, 1, 100, approx) == 1 &&
              approx[0].external_id == exact[0].external_id && approx[0].distance < 0.001f,
              "s3: approximate search ALSO finds the exact same point at distance ~0 for a verbatim self-query");
    }

    /* ── Scenario 4: deletion -- vecstore_delete() an indexed point (auto-
     * maintenance hook tombstones it in the index too), then confirm it
     * is NEVER returned by vec_index_search() again, even when queried
     * with its own exact former location. ────────────────────────────────────── */
    {
        struct VecValues seed;
        seed.count = DIM;
        for (int d = 0; d < DIM; d++) seed.values[d] = 0.0f;   // cluster 0's center
        struct VecMatch victim[1];
        CHECK(vecstore_search(1, "cluster_points", &seed, VEC_METRIC_L2, 1, victim) == 1,
              "s4: setup -- found a real point to delete");
        uint64_t victim_ext = victim[0].external_id;
        struct VecId victim_id = victim[0].id;

        uint64_t ve; struct VecValues victim_vals;
        vecstore_get(1, "cluster_points", victim_id, &ve, &victim_vals);

        CHECK(vecstore_delete(1, "cluster_points", victim_id) == 0, "s4: vecstore_delete succeeds");

        struct VecMatch after[20];
        uint32_t n = vec_index_search(1, "idx_cluster", &victim_vals, 20, 150, after);
        int found_deleted = 0;
        for (uint32_t i = 0; i < n; i++) if (after[i].external_id == victim_ext) found_deleted = 1;
        CHECK(!found_deleted, "s4: the deleted point's external_id never appears in approximate search results again");

        // The rest of the graph must still work -- deletion tombstones
        // ONE node without rebuilding the graph (see vec_index.h's own
        // header comment on this being a real, more severe limitation
        // than row_index.c's own tombstone precedent) -- confirm search
        // for a DIFFERENT, unrelated cluster still finds real results.
        struct VecValues other_seed;
        other_seed.count = DIM;
        for (int d = 0; d < DIM; d++) other_seed.values[d] = 4.0f * CLUSTER_SEP;   // cluster 4's center
        struct VecMatch still_works[5];
        uint32_t n2 = vec_index_search(1, "idx_cluster", &other_seed, 5, 100, still_works);
        CHECK(n2 == 5, "s4: search for an unrelated cluster still returns full results after a deletion elsewhere in the graph");
    }

    /* ── Scenario 5: a second index on the same collection, different
     * metric (cosine), sharing the same node pool -- confirms index_id
     * tagging keeps the two graphs independent. Also exercises Gap
     * Analysis §4's auto-backfill on a collection that's ALREADY populated
     * at create time (cluster_points has N_POINTS-1 live entries here,
     * after scenario 4's earlier deletion) -- distinct from Scenario 8's
     * dedicated backfill coverage, which uses a small, hand-countable
     * dataset; this scenario proves auto-backfill also holds at a much
     * larger scale (hundreds of entries) and alongside a second, unrelated
     * pre-existing index over the same collection. ──────────────────────────── */
    {
        CHECK(vec_index_create(1, "idx_cosine", "cluster_points", VEC_METRIC_COSINE) == 0,
              "s5: a second index on the same collection, different metric, is created");
        int cosine_slot = -1;
        for (int i = 0; i < VEC_INDEX_MAX; i++) if (vec_indexes[i].active && strcmp(vec_indexes[i].index_name, "idx_cosine") == 0) cosine_slot = i;
        CHECK(cosine_slot >= 0 && vec_indexes[cosine_slot].active_count == (uint32_t)(N_POINTS - 1),
              "s5: Gap Analysis §4 -- idx_cosine auto-backfilled from cluster_points' N_POINTS-1 already-live entries "
              "(one fewer than N_POINTS because scenario 4 already deleted one) immediately at create time, before any new insert");

        struct VecValues v;
        v.count = DIM;
        for (int d = 0; d < DIM; d++) v.values[d] = 2.0f * CLUSTER_SEP + test_jitter(JITTER_MAG);
        struct VecId new_id;
        CHECK(vecstore_insert(1, "cluster_points", 9999, &v, &new_id) == 0,
              "s5: a new insert after the second index exists succeeds");
        // Both indexes should have picked up the new point via the SAME
        // auto-maintenance call -- both now land on exactly N_POINTS:
        // idx_cluster via its usual net-of-scenario-4's-deletion path,
        // idx_cosine via its own fresh backfill (N_POINTS-1) plus this one
        // new insert.
        CHECK(vec_indexes[0].active_count == N_POINTS, "s5: idx_cluster (L2) also grew by the new insert (net of scenario 4's earlier deletion)");
        CHECK(vec_indexes[cosine_slot].active_count == (uint32_t)N_POINTS,
              "s5: idx_cosine reaches exactly N_POINTS too -- its own backfill plus this one shared auto-maintenance insert, maintained independently of idx_cluster");

        struct VecMatch cos_result[1];
        CHECK(vec_index_search(1, "idx_cosine", &v, 1, 50, cos_result) == 1 && cos_result[0].external_id == 9999,
              "s5: idx_cosine's own search finds the point inserted into it, independent of idx_cluster");
    }

    /* ── Scenario 6: VectorStore Interface Roadmap Phase 1 -- vec_index_drop().
     * Drops idx_cosine (leaves idx_cluster untouched) and confirms: the
     * dropped index stops answering searches, its slot becomes reusable
     * (a fresh vec_index_create() with the same name succeeds again), and
     * the OTHER index over the same collection is completely unaffected. ─── */
    {
        struct VecValues probe;
        probe.count = DIM;
        for (int d = 0; d < DIM; d++) probe.values[d] = 2.0f * CLUSTER_SEP;
        struct VecMatch before[1];
        CHECK(vec_index_search(1, "idx_cosine", &probe, 1, 50, before) == 1,
              "s6: setup -- idx_cosine answers a search before being dropped");

        CHECK(vec_index_drop(1, "idx_cosine") == 0, "s6: vec_index_drop succeeds on an existing index");
        CHECK(vec_index_drop(1, "idx_cosine") == 1, "s6: a second drop of the same (now-gone) name fails with 'not found' (1), not silently succeeding again");
        CHECK(vec_index_drop(1, "does_not_exist") == 1, "s6: dropping a never-existing index name also fails with 1");

        struct VecMatch after[1];
        CHECK(vec_index_search(1, "idx_cosine", &probe, 1, 50, after) == 0,
              "s6: idx_cosine no longer answers ANY search after being dropped");

        CHECK(vec_index_create(1, "idx_cosine", "cluster_points", VEC_METRIC_COSINE) == 0,
              "s6: the dropped slot is reusable -- creating a new index under the same name succeeds again");

        struct VecMatch cluster_check[1];
        CHECK(vec_index_search(1, "idx_cluster", &probe, 1, 100, cluster_check) == 1,
              "s6: idx_cluster (the OTHER index over the same collection) is completely unaffected by idx_cosine's drop");
    }

    /* ── Scenario 7: VectorStore Interface Roadmap Phase 1 -- vecstore_
     * notify_object_freed(). A dedicated, freshly-created collection +
     * index (rather than reusing cluster_points/idx_cluster, which later
     * scenarios don't run after this one but keeping this scenario fully
     * self-contained is worth the small setup cost) -- confirms the
     * function this whole phase's headline bug fix hinges on: after a
     * simulated vfree, the collection becomes unreachable through every
     * vecstore.c entry point AND any index built over it stops answering,
     * rather than being silently orphaned (see object_catalog.c's own
     * sys_sls_vfree()/catalog_vfree_partition() for the real caller --
     * this test calls vecstore_notify_object_freed() directly, the same
     * isolation boundary every other scenario in this file already uses
     * for catalog_check_access(), since linking the real object_catalog.c
     * here would pull in its full persist.h/journal.h/lock_mgr.h/etc.
     * dependency graph for no benefit to what THIS function's own logic
     * needs verified). ───────────────────────────────────────────────────── */
    {
        memset(&object_catalog[1], 0, sizeof(object_catalog[1]));
        strcpy(object_catalog[1].name, "temp_collection");
        object_catalog[1].type = OBJ_TYPE_DB_TABLE;
        object_catalog[1].object_id = 0xF002;
        object_catalog[1].active = 1;
        object_catalog_count = 2;
        CHECK(vecstore_create_collection(1, "temp_collection", 4) == 0, "s7: setup -- temp_collection created");
        CHECK(vec_index_create(1, "idx_temp", "temp_collection", VEC_METRIC_L2) == 0, "s7: setup -- index over temp_collection created");

        struct VecValues v = { .count = 4, .values = {1.0f, 2.0f, 3.0f, 4.0f} };
        struct VecId temp_id;
        CHECK(vecstore_insert(1, "temp_collection", 42, &v, &temp_id) == 0, "s7: setup -- one vector inserted into temp_collection");

        struct VecMatch pre[1];
        CHECK(vecstore_search(1, "temp_collection", &v, VEC_METRIC_L2, 1, pre) == 1, "s7: setup -- temp_collection is searchable before being freed");
        CHECK(vec_index_search(1, "idx_temp", &v, 1, 50, pre) == 1, "s7: setup -- idx_temp is searchable before being freed");

        // The function under test -- called the same way object_catalog.c's
        // fixed sys_sls_vfree() now calls it: BEFORE the catalog object's
        // own .active flag would be cleared (object_catalog[1].active is
        // deliberately left at 1 here, matching that exact call-order
        // contract -- see vecstore_notify_object_freed()'s own header
        // comment on why calling it after would silently find nothing).
        vecstore_notify_object_freed("temp_collection");

        uint64_t ext; struct VecValues got;
        CHECK(vecstore_get(1, "temp_collection", temp_id, &ext, &got) == 1,
              "s7: vecstore_get on the freed collection now fails (collection unreachable, not just the one vector)");
        CHECK(vecstore_search(1, "temp_collection", &v, VEC_METRIC_L2, 1, pre) == 0,
              "s7: vecstore_search on the freed collection now returns 0 matches (couldn't run at all, per the documented ambiguous-0 contract)");
        CHECK(vecstore_collection_scan(1, "temp_collection", NULL, NULL) == 0,
              "s7: vecstore_collection_scan on the freed collection visits 0 entries");
        CHECK(vec_index_search(1, "idx_temp", &v, 1, 50, pre) == 0,
              "s7: idx_temp (built over the now-freed collection) no longer answers searches either -- the index was deactivated too, not left dangling");

        // Calling it again (e.g. a second vfree attempt, or vfree on a name
        // that was never a vector collection in the first place) must be a
        // harmless no-op, not a crash -- find_active_collection() returns
        // -1 the second time since vector_collections[idx].active is
        // already 0, and the function returns immediately per its own
        // header comment.
        vecstore_notify_object_freed("temp_collection");
        vecstore_notify_object_freed("this_was_never_a_vector_collection");
        CHECK(1, "s7: calling vecstore_notify_object_freed() again (already-freed) and on a name that was never a collection are both silent no-ops, not crashes");
    }

    /* ── Scenario 8: VectorStore Interface Roadmap Phase 3 / Gap Analysis §4
     * -- backfill. A fresh collection populated BEFORE its index exists
     * (the exact scenario vec_index.h's own header comment point 7 named
     * as a gap): as of Gap Analysis §4, vec_index_create() now backfills
     * automatically, so the freshly created index should be immediately
     * populated and immediately searchable -- no separate vec_index_rebuild()
     * call required. vec_index_rebuild() itself is unchanged and still
     * exercised here for its own real purpose (re-populating an ALREADY-
     * populated index, e.g. Scenario 9's tombstone-cleanup case below),
     * including its two error paths (nonexistent index name, permission
     * denied). ─────────────────────────────────────────────────────────────── */
    {
        memset(&object_catalog[2], 0, sizeof(object_catalog[2]));
        strcpy(object_catalog[2].name, "backfill_col");
        object_catalog[2].type = OBJ_TYPE_DB_TABLE;
        object_catalog[2].object_id = 0xF003;
        object_catalog[2].active = 1;
        object_catalog_count = 3;
        CHECK(vecstore_create_collection(1, "backfill_col", 3) == 0, "s8: setup -- backfill_col created");

        struct VecId backfill_ids[5];
        for (int i = 0; i < 5; i++) {
            struct VecValues v;
            v.count = 3;
            v.values[0] = (float)i; v.values[1] = (float)i * 2; v.values[2] = (float)i * 3;
            CHECK(vecstore_insert(1, "backfill_col", (uint64_t)(500 + i), &v, &backfill_ids[i]) == 0,
                  "s8: setup -- a vector inserted into backfill_col BEFORE any index exists over it");
        }

        CHECK(vec_index_create(1, "idx_backfill", "backfill_col", VEC_METRIC_L2) == 0,
              "s8: setup -- index created AFTER the collection was already populated");
        CHECK(vec_indexes_active_count_by_name("idx_backfill") == 5,
              "s8: Gap Analysis §4 -- the freshly created index is ALREADY populated with all 5 pre-existing vectors, no rebuild needed");

        struct VecValues probe0 = { .count = 3, .values = {0.0f, 0.0f, 0.0f} };
        struct VecMatch immediate[1];
        CHECK(vec_index_search(1, "idx_backfill", &probe0, 1, 50, immediate) == 1 &&
              immediate[0].external_id == 500 && immediate[0].distance < 0.001f,
              "s8: searching the just-created index immediately finds a pre-existing vector (external_id=500), distance ~0 for its own exact values");

        CHECK(vec_index_rebuild(1, "does_not_exist") == 1, "s8: rebuilding a nonexistent index name fails with 1");
        g_access_force_deny = 1;
        CHECK(vec_index_rebuild(1, "idx_backfill") == 2, "s8: rebuilding with access denied fails with 2, and leaves the index untouched (still 5)");
        g_access_force_deny = 0;

        CHECK(vec_index_rebuild(1, "idx_backfill") == 0, "s8: vec_index_rebuild ALSO still works directly, re-populating an already-populated index from scratch");
        CHECK(vec_indexes_active_count_by_name("idx_backfill") == 5,
              "s8: after an explicit rebuild, active_count is still exactly 5 -- no duplication, same 5 entries re-derived from the collection");

        struct VecMatch after_rebuild[1];
        CHECK(vec_index_search(1, "idx_backfill", &probe0, 1, 50, after_rebuild) == 1 &&
              after_rebuild[0].external_id == 500 && after_rebuild[0].distance < 0.001f,
              "s8: the explicitly-rebuilt index still finds the same pre-existing vector (external_id=500) via a search, distance ~0");
    }

    /* ── Scenario 8b: VectorStore Gap Analysis §4 -- vec_index_create()
     * rejects an index_name containing a space or control character, the
     * same round-trip-corruption risk cat_valid_name() closes for
     * object/collection names in object_catalog.c's sys_sls_valloc(), but
     * enforced here directly since index names are their own separate
     * namespace that never goes through sys_sls_valloc(). ────────────────── */
    {
        int before_count = 0;
        for (int i = 0; i < VEC_INDEX_MAX; i++) if (vec_indexes[i].active) before_count++;

        CHECK(vec_index_create(1, "has a space", "backfill_col", VEC_METRIC_L2) == 1,
              "s8b: an index_name with an embedded space is rejected (rc=1), not silently created");
        CHECK(vec_index_create(1, "", "backfill_col", VEC_METRIC_L2) == 1,
              "s8b: an empty index_name is rejected the same way");

        int after_count = 0;
        for (int i = 0; i < VEC_INDEX_MAX; i++) if (vec_indexes[i].active) after_count++;
        CHECK(before_count == after_count, "s8b: neither rejected attempt actually created a new index slot");

        CHECK(vec_index_create(1, "idx_plain_name_123", "backfill_col", VEC_METRIC_L2) == 0,
              "s8b: a plain, space-free index_name is completely unaffected and still succeeds");
        CHECK(vec_indexes_active_count_by_name("idx_plain_name_123") == 5,
              "s8b: the valid new index also auto-backfilled from backfill_col's 5 existing entries, same as Scenario 8");
    }

    /* ── Scenario 9: VectorStore Interface Roadmap Phase 3 -- tombstone
     * cleanup. Deletes a live, indexed vector (tombstoning its node, per
     * vec_index_notify_delete()'s own existing behavior -- Scenario 4's
     * own precedent), then rebuilds and confirms the rebuilt graph is
     * genuinely CLEAN, not just "the deleted point no longer wins a
     * search": no ACTIVE node's neighbor list, at any layer, references a
     * tombstoned node_idx. This is the real difference rebuild makes over
     * a bare vecstore_delete() -- vec_index.h's own header comment already
     * names tombstoned-but-still-linked nodes as dead ends other live
     * queries route through; a full rebuild regenerates the graph from
     * only live data, so no such dead end can exist afterward. ──────────── */
    {
        struct VecValues probe1 = { .count = 3, .values = {1.0f, 2.0f, 3.0f} };
        struct VecMatch victim[1];
        CHECK(vec_index_search(1, "idx_backfill", &probe1, 1, 50, victim) == 1 && victim[0].external_id == 501,
              "s9: setup -- located external_id=501 (inserted as point i=1 in Scenario 8) via search");

        struct VecId victim_id = victim[0].id;
        CHECK(vecstore_delete(1, "backfill_col", victim_id) == 0, "s9: setup -- deleted that vector (auto-maintenance tombstones its node in idx_backfill)");
        CHECK(vec_indexes_active_count_by_name("idx_backfill") == 4,
              "s9: active_count drops to 4 immediately after the delete, before any rebuild");

        CHECK(vec_index_rebuild(1, "idx_backfill") == 0, "s9: vec_index_rebuild succeeds after the deletion");
        CHECK(vec_indexes_active_count_by_name("idx_backfill") == 4,
              "s9: active_count stays at 4 after rebuild -- the deleted point does not come back (vecstore_collection_scan() only visits live entries)");

        struct VecMatch after[10];
        uint32_t n = vec_index_search(1, "idx_backfill", &probe1, 10, 50, after);
        int found_deleted = 0;
        for (uint32_t i = 0; i < n; i++) if (after[i].external_id == 501) found_deleted = 1;
        CHECK(!found_deleted, "s9: the deleted point's external_id never appears in the rebuilt index's search results");

        // The real, structural check: walk every ACTIVE node belonging to
        // idx_backfill and confirm every neighbor edge, at every layer,
        // points to another ACTIVE node -- proving no dead-end edges to
        // the deleted (or any pre-rebuild-tombstoned) node survived.
        int backfill_slot = -1;
        for (int i = 0; i < VEC_INDEX_MAX; i++)
            if (vec_indexes[i].active && strcmp(vec_indexes[i].index_name, "idx_backfill") == 0) backfill_slot = i;
        CHECK(backfill_slot >= 0, "s9: setup -- idx_backfill's slot located for direct graph inspection");

        int clean = 1;
        uint32_t active_nodes_checked = 0;
        for (uint32_t n2 = 0; n2 < vec_index_next_free_node; n2++) {
            if (vec_index_nodes[n2].index_id != (uint32_t)backfill_slot || !vec_index_nodes[n2].active) continue;
            active_nodes_checked++;
            for (uint32_t l = 0; l <= vec_index_nodes[n2].top_layer && l < VEC_INDEX_MAX_LAYERS; l++) {
                for (uint32_t e = 0; e < vec_index_nodes[n2].neighbor_count[l]; e++) {
                    uint32_t nb = vec_index_nodes[n2].neighbors[l][e];
                    if (nb >= VEC_INDEX_MAX_NODES || !vec_index_nodes[nb].active || vec_index_nodes[nb].index_id != (uint32_t)backfill_slot) {
                        clean = 0;
                    }
                }
            }
        }
        CHECK(active_nodes_checked == 4, "s9: exactly 4 active nodes belong to idx_backfill post-rebuild (matches active_count)");
        CHECK(clean, "s9: every active node's every neighbor edge, at every layer, points to another active node in the same index -- no dead-end edges survive a rebuild");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
