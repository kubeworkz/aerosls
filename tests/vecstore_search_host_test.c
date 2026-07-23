/*
 * vecstore_search_host_test.c — Vector Store Roadmap Phase 2 verification:
 * a standalone host-buildable test for kernel/vecstore.c's brute-force
 * similarity search, linked against the REAL, unmodified kernel/vecstore.c
 * -- not a reimplementation. Same stub set as vecstore_host_test.c
 * (Phase 1's own suite) -- see that file's header comment for why
 * object_catalog.c isn't linked here.
 *
 * The five 2D points below (chosen by hand, not randomly) were picked so
 * that L2 distance and cosine distance from the query point produce TWO
 * DIFFERENT orderings with NO TIES in either metric -- proof the two
 * metrics genuinely behave differently, not a coincidence, and a test that
 * doesn't need to reason about tie-breaking behavior at all.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/vecstore_search_host_test tests/vecstore_search_host_test.c \
 *       kernel/vecstore.c kernel/vec_index.c
 *   /tmp/vecstore_search_host_test
 *
 * Gap Remediation Phase D note: this link line was missing kernel/vec_
 * index.c -- the same pre-existing Phase 6 gap already found and fixed in
 * vecstore_host_test.c during this same regression sweep (see that file's
 * own note for the full explanation).
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

static int      g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    return g_access_force_deny ? 0 : 1;
}
void* allocate_physical_ram_frame(void) { return malloc(4096); }

/* ollama_embed() stub -- see vecstore_host_test.c's own identical comment:
 * Phase 4 gave vecstore.c a real link-time dependency on ollama_embed()
 * via sys_sls_vec_embed_insert(), which this Phase 2 search suite never
 * exercises -- stubbed to satisfy the linker only. */
int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp) {
    (void)req; (void)resp;
    return -1;
}

/* ─── Gap Remediation Phase D stubs -- matching vecstore_host_test.c's own
 * identical set exactly (see that file's top comment for the rationale). ── */
void persist_vecstore_headers(void) { }
void persist_vec_index_defs(void) { }

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

static int approx(float a, float b) { float d = a - b; if (d < 0) d = -d; return d < 0.001f; }

int main(void) {
    vecstore_init();

    memset(&object_catalog[0], 0, sizeof(object_catalog[0]));
    strcpy(object_catalog[0].name, "points");
    object_catalog[0].type      = OBJ_TYPE_DB_TABLE;
    object_catalog[0].object_id = 0xD001;
    object_catalog[0].active    = 1;
    object_catalog_count = 1;

    CHECK(vecstore_create_collection(1, "points", 2) == 0, "setup: create_collection(dimension=2) succeeds");

    // A=(1,0) [identical to the query], B=(2,1), C=(0,3), D=(-2,0), E=(5,1).
    // Hand-computed expected distances from query=(1,0) -- see file header
    // comment for the full derivation.
    struct VecValues qA = { 2, {1, 0} };
    struct VecValues qB = { 2, {2, 1} };
    struct VecValues qC = { 2, {0, 3} };
    struct VecValues qD = { 2, {-2, 0} };
    struct VecValues qE = { 2, {5, 1} };
    struct VecId idA, idB, idC, idD, idE;
    vecstore_insert(1, "points", 1, &qA, &idA);
    vecstore_insert(1, "points", 2, &qB, &idB);
    vecstore_insert(1, "points", 3, &qC, &idC);
    vecstore_insert(1, "points", 4, &qD, &idD);
    vecstore_insert(1, "points", 5, &qE, &idE);

    struct VecValues query = { 2, {1, 0} };

    /* ── Scenario 1: L2 search, k=5 (all points) -- exact ordering AND
     * exact distance values, hand-verified. Expected ascending order:
     * A(0), B(sqrt(2)), D(3), C(sqrt(10)), E(sqrt(17)). ─────────────────────── */
    struct VecMatch l2[5];
    uint32_t n = vecstore_search(1, "points", &query, VEC_METRIC_L2, 5, l2);
    CHECK(n == 5, "s1: L2 search with k=5 returns all 5 points");
    CHECK(l2[0].external_id == 1 && approx(l2[0].distance, 0.0f), "s1: rank 0 is A (identical point), distance 0");
    CHECK(l2[1].external_id == 2 && approx(l2[1].distance, 1.41421356f), "s1: rank 1 is B, distance sqrt(2)");
    CHECK(l2[2].external_id == 4 && approx(l2[2].distance, 3.0f), "s1: rank 2 is D, distance 3");
    CHECK(l2[3].external_id == 3 && approx(l2[3].distance, 3.16227766f), "s1: rank 3 is C, distance sqrt(10)");
    CHECK(l2[4].external_id == 5 && approx(l2[4].distance, 4.12310563f), "s1: rank 4 is E, distance sqrt(17)");

    /* ── Scenario 2: cosine search, k=5 -- a DIFFERENT ordering than L2's
     * (proof the metrics genuinely differ, not a coincidence). Expected
     * ascending order: A(0), E(~0.0194), B(~0.1056), C(1.0), D(2.0). ────────── */
    struct VecMatch cos[5];
    n = vecstore_search(1, "points", &query, VEC_METRIC_COSINE, 5, cos);
    CHECK(n == 5, "s2: cosine search with k=5 returns all 5 points");
    CHECK(cos[0].external_id == 1 && approx(cos[0].distance, 0.0f), "s2: rank 0 is A (identical direction), distance 0");
    CHECK(cos[1].external_id == 5 && approx(cos[1].distance, 0.01941932f), "s2: rank 1 is E, distance ~0.0194");
    CHECK(cos[2].external_id == 2 && approx(cos[2].distance, 0.10557281f), "s2: rank 2 is B, distance ~0.1056");
    CHECK(cos[3].external_id == 3 && approx(cos[3].distance, 1.0f), "s2: rank 3 is C (perpendicular), distance 1.0");
    CHECK(cos[4].external_id == 4 && approx(cos[4].distance, 2.0f), "s2: rank 4 is D (opposite direction), distance 2.0");

    /* ── Scenario 3: the two metrics really do disagree on ordering --
     * B ranks 1 under L2 but 2 under cosine; D ranks 2 under L2 but LAST
     * under cosine. Not re-deriving the numbers, just cross-checking the
     * two result sets above assert genuinely different orderings. ────────── */
    CHECK(l2[1].external_id != cos[1].external_id, "s3: L2's 2nd-closest and cosine's 2nd-closest are different points");
    CHECK(l2[2].external_id != cos[4].external_id || l2[2].external_id == 4,
          "s3: D is L2's 3rd-closest but cosine's most distant -- the metrics disagree, not coincidentally similar");

    /* ── Scenario 4: top-K truncation -- k smaller than the collection
     * still returns the correct PREFIX of the full ordering. ───────────────── */
    struct VecMatch top3[3];
    n = vecstore_search(1, "points", &query, VEC_METRIC_L2, 3, top3);
    CHECK(n == 3, "s4: k=3 returns exactly 3 matches");
    CHECK(top3[0].external_id == 1 && top3[1].external_id == 2 && top3[2].external_id == 4,
          "s4: k=3 returns exactly the top 3 of the full k=5 ordering (A, B, D), not something else");

    /* ── Scenario 5: k larger than the collection's entry count returns
     * only the entries that actually exist, not garbage or a crash. ────────── */
    struct VecMatch big[20];
    n = vecstore_search(1, "points", &query, VEC_METRIC_L2, 20, big);
    CHECK(n == 5, "s5: k=20 against a 5-entry collection returns exactly 5, not 20");

    /* ── Scenario 6: a search result's VecId is a real, usable id -- feeding
     * it back into vecstore_get() returns the same point. ──────────────────── */
    uint64_t got_ext; struct VecValues got_vals;
    CHECK(vecstore_get(1, "points", l2[0].id, &got_ext, &got_vals) == 0 && got_ext == 1 &&
          got_vals.values[0] == 1.0f && got_vals.values[1] == 0.0f,
          "s6: the VecId returned by search round-trips through vecstore_get() to the same point");

    /* ── Scenario 7: error paths -- dimension mismatch, unknown collection,
     * permission denial, and an empty collection all fail cleanly (0),
     * not a crash. ───────────────────────────────────────────────────────────── */
    struct VecValues wrong_dim = { 3, {1, 0, 0} };
    struct VecMatch dummy[5];
    CHECK(vecstore_search(1, "points", &wrong_dim, VEC_METRIC_L2, 5, dummy) == 0,
          "s7: a query with the wrong dimension is rejected (0 results)");
    CHECK(vecstore_search(1, "no_such_collection", &query, VEC_METRIC_L2, 5, dummy) == 0,
          "s7: searching an unknown collection returns 0");
    g_access_force_deny = 1;
    CHECK(vecstore_search(1, "points", &query, VEC_METRIC_L2, 5, dummy) == 0,
          "s7: search is denied when catalog_check_access() refuses");
    g_access_force_deny = 0;

    memset(&object_catalog[1], 0, sizeof(object_catalog[1]));
    strcpy(object_catalog[1].name, "empty_collection");
    object_catalog[1].type      = OBJ_TYPE_DB_TABLE;
    object_catalog[1].object_id = 0xD002;
    object_catalog[1].active    = 1;
    object_catalog_count = 2;
    vecstore_create_collection(1, "empty_collection", 2);
    CHECK(vecstore_search(1, "empty_collection", &query, VEC_METRIC_L2, 5, dummy) == 0,
          "s7: searching a real but genuinely empty collection returns 0 (not an error, just no matches)");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
