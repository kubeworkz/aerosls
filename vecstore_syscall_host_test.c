/*
 * vecstore_syscall_host_test.c — Vector Store Roadmap Phase 4 verification:
 * a standalone host-buildable test for kernel/vecstore.c's new syscall
 * adapters (sys_sls_vec_create/insert/embed_insert/search), linked against
 * the REAL, unmodified kernel/vecstore.c -- not a reimplementation.
 *
 * Unlike sys_sls_sql_execute() (a genuine one-line pass-through, per the
 * RDBMS roadmap's own Phase 22 findings), these adapters carry real,
 * new marshaling logic worth testing directly rather than trusting by
 * analogy: sys_sls_vec_search()'s k-capping + truncated flag,
 * sys_sls_vec_embed_insert()'s two-stage ollama_status/insert_status
 * reporting and its defensive dimension cap. ollama_embed() itself is
 * stubbed here, controllable per-scenario -- mirroring
 * catalog_check_access()'s own stub in vecstore_host_test.c -- since this
 * suite is about the ADAPTER logic, not Ollama's real wire protocol
 * (already covered by ollama_client_host_test.c) or the storage engine's
 * own CRUD correctness (already covered by vecstore_host_test.c /
 * vecstore_search_host_test.c).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -I drivers -I net \
 *       -o /tmp/vecstore_syscall_host_test vecstore_syscall_host_test.c kernel/vecstore.c
 *   /tmp/vecstore_syscall_host_test
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

int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    return 1;   // this suite isn't about permission gating -- vecstore_host_test.c already covers that
}
void* allocate_physical_ram_frame(void) { return malloc(4096); }

/* ── Controllable ollama_embed() stub ────────────────────────────────────
 * Real net/ollama_client.c is deliberately NOT linked here (same "stub the
 * heavy dependency at its own real API boundary" reasoning as every prior
 * host test in this project) -- this test controls exactly what
 * ollama_embed() returns per scenario, which is what lets it exercise
 * sys_sls_vec_embed_insert()'s two failure paths (Ollama itself failing
 * vs. Ollama succeeding but the subsequent insert failing) precisely,
 * something a real network call couldn't do deterministically anyway. */
static int      g_ollama_force_fail = 0;
static uint32_t g_ollama_dim = 0;
static float    g_ollama_vals[OLLAMA_MAX_EMBED_DIM];
static int      g_ollama_calls = 0;

int ollama_embed(const struct OllamaEmbedRequest* req, struct OllamaEmbedResponse* resp) {
    (void)req;
    g_ollama_calls++;
    if (g_ollama_force_fail) return -1;
    resp->http_status = 200;
    resp->dimension = g_ollama_dim;
    uint32_t n = g_ollama_dim > OLLAMA_MAX_EMBED_DIM ? OLLAMA_MAX_EMBED_DIM : g_ollama_dim;
    for (uint32_t i = 0; i < n; i++) resp->embedding[i] = g_ollama_vals[i];
    return 0;
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

int main(void) {
    vecstore_init();

    make_object(0, "points", 0xE001);
    make_object(1, "tiny",   0xE002);
    object_catalog_count = 2;

    /* ── Scenario 1: sys_sls_vec_create ──────────────────────────────────── */
    {
        struct SLSVecCreateRequest req;
        req.caller_uid = 1;
        strcpy(req.collection_name, "points");
        req.dimension = 4;
        uint64_t rc = sys_sls_vec_create(&req);
        CHECK(rc == 0 && req.status == 0, "s1: sys_sls_vec_create succeeds for a valid, valloc'd object");

        struct SLSVecCreateRequest bad;
        bad.caller_uid = 1;
        strcpy(bad.collection_name, "no_such_object");
        bad.dimension = 4;
        rc = sys_sls_vec_create(&bad);
        CHECK(rc != 0 && bad.status != 0, "s1: sys_sls_vec_create fails cleanly for a nonexistent object");

        strcpy(req.collection_name, "tiny");
        req.dimension = 2;
        rc = sys_sls_vec_create(&req);
        CHECK(rc == 0 && req.status == 0, "s1: sys_sls_vec_create also succeeds for a second, independent collection");
    }

    /* ── Scenario 2: sys_sls_vec_insert ──────────────────────────────────── */
    struct VecId points_id_a;
    {
        struct SLSVecInsertRequest req;
        req.caller_uid = 1;
        strcpy(req.collection_name, "points");
        req.external_id = 42;
        req.values.count = 4;
        req.values.values[0] = 1.0f; req.values.values[1] = 2.0f;
        req.values.values[2] = 3.0f; req.values.values[3] = 4.0f;
        uint64_t rc = sys_sls_vec_insert(&req);
        CHECK(rc == 0 && req.status == 0, "s2: sys_sls_vec_insert succeeds for a matching-dimension vector");
        points_id_a = req.out_id;

        uint64_t got_ext; struct VecValues got_vals;
        CHECK(vecstore_get(1, "points", points_id_a, &got_ext, &got_vals) == 0 &&
              got_ext == 42 && got_vals.values[0] == 1.0f && got_vals.values[3] == 4.0f,
              "s2: the inserted vector round-trips through vecstore_get() with the exact values sent");

        struct SLSVecInsertRequest bad = req;
        bad.values.count = 3;   // wrong dimension for "points" (4)
        rc = sys_sls_vec_insert(&bad);
        CHECK(rc != 0 && bad.status == 4, "s2: sys_sls_vec_insert reports status 4 (dimension mismatch) cleanly, no crash");
    }

    /* ── Scenario 3: sys_sls_vec_search, including k-capping ────────────────── */
    {
        // Insert a few more points so there's something real to rank.
        struct SLSVecInsertRequest ins;
        ins.caller_uid = 1;
        strcpy(ins.collection_name, "points");
        ins.values.count = 4;
        for (int i = 0; i < 3; i++) {
            ins.external_id = (uint64_t)(100 + i);
            ins.values.values[0] = (float)i; ins.values.values[1] = 0;
            ins.values.values[2] = 0; ins.values.values[3] = 0;
            sys_sls_vec_insert(&ins);
        }

        struct SLSVecSearchRequest req;
        req.caller_uid = 1;
        strcpy(req.collection_name, "points");
        req.query.count = 4;
        req.query.values[0] = 1.0f; req.query.values[1] = 2.0f;
        req.query.values[2] = 3.0f; req.query.values[3] = 4.0f;
        req.metric = VEC_METRIC_L2;
        req.k = 2;
        uint64_t rc = sys_sls_vec_search(&req);
        CHECK(rc == 0, "s3: sys_sls_vec_search always reports syscall-level success (0)");
        CHECK(req.match_count == 2, "s3: k=2 (under the cap) returns exactly 2 matches");
        CHECK(req.truncated == 0, "s3: truncated flag is 0 when k is under VEC_SEARCH_MAX_K");
        CHECK(req.matches[0].external_id == 42 && req.matches[0].distance < 0.001f,
              "s3: closest match is external_id=42 (the exact point), distance ~0");

        // Now request more than VEC_SEARCH_MAX_K -- must be capped internally,
        // never overflow req.matches[VEC_SEARCH_MAX_K], and the caller must be
        // told via req.truncated that k was reduced.
        struct SLSVecSearchRequest big = req;
        big.k = VEC_SEARCH_MAX_K + 1000;
        rc = sys_sls_vec_search(&big);
        CHECK(rc == 0, "s4: an oversized k still returns syscall-level success, not a crash");
        CHECK(big.truncated == 1, "s4: truncated flag is set to 1 when k exceeds VEC_SEARCH_MAX_K");
        CHECK(big.match_count <= VEC_SEARCH_MAX_K, "s4: match_count never exceeds VEC_SEARCH_MAX_K regardless of requested k");
    }

    /* ── Scenario 5: sys_sls_vec_embed_insert -- the happy path ─────────────── */
    {
        g_ollama_force_fail = 0;
        g_ollama_dim = 4;
        g_ollama_vals[0] = 9.0f; g_ollama_vals[1] = 8.0f;
        g_ollama_vals[2] = 7.0f; g_ollama_vals[3] = 6.0f;

        struct SLSVecEmbedInsertRequest req;
        req.caller_uid = 1;
        strcpy(req.collection_name, "points");
        req.external_id = 777;
        strcpy(req.ollama_req.endpoint_ip, "127.0.0.1");
        req.ollama_req.port = 11434;
        strcpy(req.ollama_req.model, "nomic-embed-text");
        strcpy(req.ollama_req.prompt, "a test sentence");

        int calls_before = g_ollama_calls;
        uint64_t rc = sys_sls_vec_embed_insert(&req);
        CHECK(g_ollama_calls == calls_before + 1, "s5: sys_sls_vec_embed_insert calls ollama_embed() exactly once");
        CHECK(rc == 0 && req.ollama_status == 0 && req.insert_status == 0,
              "s5: happy path reports success on both ollama_status and insert_status");

        uint64_t got_ext; struct VecValues got_vals;
        CHECK(vecstore_get(1, "points", req.out_id, &got_ext, &got_vals) == 0 &&
              got_ext == 777 && got_vals.values[0] == 9.0f && got_vals.values[3] == 6.0f,
              "s5: the embedded-then-inserted vector round-trips with Ollama's exact reported values");
    }

    /* ── Scenario 6: sys_sls_vec_embed_insert -- Ollama itself fails ────────── */
    {
        g_ollama_force_fail = 1;
        struct SLSVecEmbedInsertRequest req;
        req.caller_uid = 1;
        strcpy(req.collection_name, "points");
        req.external_id = 888;
        strcpy(req.ollama_req.endpoint_ip, "127.0.0.1");
        req.ollama_req.port = 11434;
        strcpy(req.ollama_req.model, "nomic-embed-text");
        strcpy(req.ollama_req.prompt, "unreachable");

        uint64_t rc = sys_sls_vec_embed_insert(&req);
        CHECK(rc != 0 && req.ollama_status != 0, "s6: sys_sls_vec_embed_insert reports failure when ollama_embed() fails");
        CHECK(req.insert_status == -1, "s6: insert_status is -1 (never attempted) when Ollama itself failed -- distinguishable from a real vecstore_insert() code");
        g_ollama_force_fail = 0;
    }

    /* ── Scenario 7: sys_sls_vec_embed_insert -- Ollama succeeds, but the
     * subsequent insert fails (dimension mismatch against a real, smaller
     * collection) -- the two status fields must disagree, not collapse. ───── */
    {
        g_ollama_dim = 4;   // "tiny" collection's real dimension is 2
        struct SLSVecEmbedInsertRequest req;
        req.caller_uid = 1;
        strcpy(req.collection_name, "tiny");
        req.external_id = 999;
        strcpy(req.ollama_req.endpoint_ip, "127.0.0.1");
        req.ollama_req.port = 11434;
        strcpy(req.ollama_req.model, "nomic-embed-text");
        strcpy(req.ollama_req.prompt, "dimension mismatch case");

        uint64_t rc = sys_sls_vec_embed_insert(&req);
        CHECK(rc != 0, "s7: sys_sls_vec_embed_insert reports overall failure when the insert step fails");
        CHECK(req.ollama_status == 0, "s7: ollama_status is 0 -- Ollama itself succeeded");
        CHECK(req.insert_status == 4, "s7: insert_status is 4 (dimension mismatch), distinct from ollama_status, not collapsed into one code");
    }

    /* ── Scenario 8: the defensive dimension cap -- a stub reporting an
     * (impossible in real Ollama usage, but not structurally prevented)
     * oversized dimension must not overflow VecValues.values[], and must
     * fail cleanly through the ordinary dimension-mismatch path rather
     * than corrupting memory. ────────────────────────────────────────────── */
    {
        g_ollama_dim = OLLAMA_MAX_EMBED_DIM;   // the max a real OllamaEmbedResponse can structurally report
        for (uint32_t i = 0; i < OLLAMA_MAX_EMBED_DIM; i++) g_ollama_vals[i] = 1.0f;

        struct SLSVecEmbedInsertRequest req;
        req.caller_uid = 1;
        strcpy(req.collection_name, "tiny");   // dimension 2 -- guaranteed mismatch against 2048
        req.external_id = 1000;
        strcpy(req.ollama_req.endpoint_ip, "127.0.0.1");
        req.ollama_req.port = 11434;
        strcpy(req.ollama_req.model, "big-model");
        strcpy(req.ollama_req.prompt, "max-dimension case");

        uint64_t rc = sys_sls_vec_embed_insert(&req);
        CHECK(rc != 0 && req.ollama_status == 0 && req.insert_status == 4,
              "s8: a max-sized (2048-dim) embedding is handled without crashing, fails cleanly on dimension mismatch");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
