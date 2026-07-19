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
 *       kernel/vec_index.c kernel/vecstore.c
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
     * data exists -- matching vec_index_create()'s own documented "no
     * backfill" contract (vec_index.h header comment point 7): the index
     * is meant to be created up front and populated via the auto-
     * maintenance hook as data is inserted, not retrofitted onto an
     * already-populated collection. ─────────────────────────────────────── */
    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "cluster_points");
    object_catalog[0].type = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xF001;
    object_catalog[0].active = 1;
    object_catalog_count = 1;
    CHECK(vecstore_create_collection("cluster_points", DIM) == 0, "setup: collection created");
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
     * tagging keeps the two graphs independent. ─────────────────────────────── */
    {
        CHECK(vec_index_create(1, "idx_cosine", "cluster_points", VEC_METRIC_COSINE) == 0,
              "s5: a second index on the same collection, different metric, is created");
        struct VecValues v;
        v.count = DIM;
        for (int d = 0; d < DIM; d++) v.values[d] = 2.0f * CLUSTER_SEP + test_jitter(JITTER_MAG);
        struct VecId new_id;
        CHECK(vecstore_insert(1, "cluster_points", 9999, &v, &new_id) == 0,
              "s5: a new insert after the second index exists succeeds");
        // Both indexes should have picked up the new point via the SAME
        // auto-maintenance call. idx_cluster's own active_count nets back
        // to exactly N_POINTS here, not N_POINTS+1 -- scenario 4 already
        // deleted one point from idx_cluster (300 -> 299) before this new
        // insert (299 -> 300); idx_cosine didn't exist yet during that
        // deletion, so it's unaffected by it.
        CHECK(vec_indexes[0].active_count == N_POINTS, "s5: idx_cluster (L2) also grew by the new insert (net of scenario 4's earlier deletion)");
        int cosine_slot = -1;
        for (int i = 0; i < VEC_INDEX_MAX; i++) if (vec_indexes[i].active && strcmp(vec_indexes[i].index_name, "idx_cosine") == 0) cosine_slot = i;
        CHECK(cosine_slot >= 0 && vec_indexes[cosine_slot].active_count == 1,
              "s5: idx_cosine (empty until now) grew to exactly 1 -- both indexes maintained independently from one insert");

        struct VecMatch cos_result[1];
        CHECK(vec_index_search(1, "idx_cosine", &v, 1, 50, cos_result) == 1 && cos_result[0].external_id == 9999,
              "s5: idx_cosine's own search finds the point inserted into it, independent of idx_cluster");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
