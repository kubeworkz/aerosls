/*
 * vecstore.c — Vector Store Roadmap Phase 1 vector collection storage.
 * See vecstore.h for the full design writeup.
 */
#include "vecstore.h"
#include "object_catalog.h"
#include "frame_pool.h"
#include "kernel_io.h"
#include "persist.h"     // Gap Remediation Phase D -- persist_vecstore_headers()
#include "../user/permissions.h"
#include "../drivers/nvme_io.h"   // Gap Remediation Phase D -- real page persistence
#include "vec_index.h"   // Vector Store Roadmap Phase 6 -- auto-maintenance hooks only, see
                          // vecstore_insert()/vecstore_delete() below and vec_index.h's own
                          // header comment on why this mirrors row_index.c/rowstore.c's
                          // established precedent
#include <stddef.h>

struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;

// RAM-cached page pointers -- NULL = not resident this boot. Gap
// Remediation Phase D: this is no longer purely RAM-only (see vecstore.h's
// header comment) -- a page either lives here already, or is lazily loaded
// from NVMe on first access via vecstore_load_page(), exactly mirroring
// rowstore.c's row_pages[]/rowstore_load_page() precedent.
static uint8_t* vec_pages[VECSTORE_MAX_PAGES];

// ─── String / memory helpers (no libc, vs_* here, matching this
// codebase's established per-file convention: rs_* in rowstore.c, ri_* in
// row_index.c, rc_* in row_constraint.c, rj_* in row_journal.c). ──────────
static int vs_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void vs_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void vs_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d; while (n--) *p++ = v;
}

// ─── Collection lookup ──────────────────────────────────────────────────────
// -1 if not found / not a vector collection. Deliberately checks
// vector_collections[]'s OWN .active flag, not any object_catalog[] field
// -- see vecstore.h's header comment on why there is no catalog-level
// "uses_vectorstore" flag in this phase.
static int find_active_collection(const char* name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!vector_collections[i].active) continue;
        if (vs_streq(object_catalog[i].name, name)) return (int)i;
    }
    return -1;
}

// ─── Page pool ───────────────────────────────────────────────────────────────
// Allocates a brand-new page from the shared bump-allocator pool. No
// reclaim in this first cut, matching rowstore.c's identical posture.
// Gap Remediation Phase D: does NOT flush the fresh frame to NVMe itself --
// same crash-window trade-off rowstore_alloc_page() already accepts (the
// page becomes "real" on NVMe only once vecstore_flush_page() is first
// called on it, matching this file's own new insert-path ordering below).
static uint32_t vecstore_alloc_page(void) {
    if (vecstore_next_free_page_id >= VECSTORE_MAX_PAGES) return VECSTORE_INVALID_PAGE;
    uint32_t id = vecstore_next_free_page_id;

    uint8_t* frame = (uint8_t*)allocate_physical_ram_frame();
    if (!frame) return VECSTORE_INVALID_PAGE;   // don't advance the cursor on failure
    vs_memset(frame, 0, VECSTORE_PAGE_SIZE);
    uint32_t invalid = VECSTORE_INVALID_PAGE;
    vs_memcpy(frame, &invalid, 4);
    vec_pages[id] = frame;

    vecstore_next_free_page_id++;
    return id;
}

// Gap Remediation Phase D: loads page_id into RAM (allocating a fresh frame
// first if needed), lazily reading it from NVMe if it's a previously-
// allocated page this boot hasn't touched yet -- mirrors rowstore.c's
// rowstore_load_page() (and, one level further back, stream.c's
// stream_lazy_load_frame()) exactly. Replaces the old, purely-RAM
// vecstore_page() accessor this file had through Phase 6.
static uint8_t* vecstore_load_page(uint32_t page_id) {
    if (page_id >= VECSTORE_MAX_PAGES) return 0;
    if (vec_pages[page_id]) return vec_pages[page_id];

    uint8_t* frame = (uint8_t*)allocate_physical_ram_frame();
    if (!frame) return 0;

    if (page_id < vecstore_next_free_page_id) {
        // Previously allocated (this boot or a prior one) -- may have real
        // data on NVMe. Errors are swallowed the same way rowstore.c's
        // identical path does: the caller sees whatever's in the zeroed
        // frame, detectable as "no valid entries" rather than a crash.
        vs_memset(frame, 0, VECSTORE_PAGE_SIZE);
        nvme_read_sync(VECSTORE_LBA_BASE + (uint64_t)page_id * 8, frame);
    } else {
        vs_memset(frame, 0, VECSTORE_PAGE_SIZE);
        uint32_t invalid = VECSTORE_INVALID_PAGE;
        vs_memcpy(frame, &invalid, 4);
    }
    vec_pages[page_id] = frame;
    return frame;
}

// Gap Remediation Phase D: mirrors rowstore.c's rowstore_flush_page().
static void vecstore_flush_page(uint32_t page_id) {
    if (page_id >= VECSTORE_MAX_PAGES || !vec_pages[page_id]) return;
    nvme_write_sync(VECSTORE_LBA_BASE + (uint64_t)page_id * 8, vec_pages[page_id]);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────
void vecstore_init(void) {
    vs_memset(vector_collections, 0, sizeof(vector_collections));
    vs_memset(vec_pages, 0, sizeof(vec_pages));
    vecstore_next_free_page_id = 0;
    kernel_serial_print("[VECSTORE] Vector store engine initialised.\n");
}

int vecstore_create_collection(const char* collection_name, uint32_t dimension) {
    if (dimension == 0 || dimension > VECSTORE_MAX_DIMENSION) return 1;

    int idx = -1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (vs_streq(object_catalog[i].name, collection_name)) { idx = (int)i; break; }
    }
    if (idx < 0) return 1;
    if (vector_collections[idx].active) return 1;   // already a vector collection

    struct VecCollectionHeader* h = &vector_collections[idx];
    vs_memset(h, 0, sizeof(*h));
    h->object_id     = object_catalog[idx].object_id;
    h->active         = 1;
    h->dimension      = dimension;
    h->entry_width    = 1 + 8 + dimension * 4;   // tombstone + external_id + float[dimension]
    h->entries_per_page = (VECSTORE_PAGE_SIZE - 4) / h->entry_width;
    h->first_page_id   = VECSTORE_INVALID_PAGE;
    h->last_page_id     = VECSTORE_INVALID_PAGE;

    if (h->entries_per_page == 0) { vs_memset(h, 0, sizeof(*h)); return 1; }   // entry too wide for even one per page

    persist_vecstore_headers();   // Gap Remediation Phase D

    kernel_serial_printf(
        "[VECSTORE] '%s' enabled: dimension=%u, entry_width=%u, %u entr(y/ies)/page.\n",
        collection_name, h->dimension, h->entry_width, h->entries_per_page);
    return 0;
}

// ─── Entry (de)serialization ─────────────────────────────────────────────────
// Entry layout: [0]=tombstone/active byte, [1..8]=external_id (u64, raw
// bytes), [9..]=dimension floats (raw bytes) -- no text round-trip at all,
// unlike rowstore.c's serialize_row()/deserialize_row() (see vecstore.h's
// header comment on why the API boundary here is numeric, not text).
static void vs_write_entry(uint8_t* slot, uint64_t external_id, const struct VecValues* values, uint32_t dimension) {
    slot[0] = 1;
    vs_memcpy(slot + 1, &external_id, 8);
    vs_memcpy(slot + 9, values->values, dimension * 4);
}
static void vs_read_entry(const uint8_t* slot, uint64_t* out_external_id, struct VecValues* out, uint32_t dimension) {
    vs_memcpy(out_external_id, slot + 1, 8);
    out->count = dimension;
    vs_memcpy(out->values, slot + 9, dimension * 4);
}

// ─── Vector CRUD ─────────────────────────────────────────────────────────────
int vecstore_insert(uint32_t caller_uid, const char* collection_name,
                    uint64_t external_id, const struct VecValues* values,
                    struct VecId* out_id) {
    if (!collection_name || !values) return 3;
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 1;
    struct VecCollectionHeader* h = &vector_collections[idx];
    if (!catalog_check_access(caller_uid, collection_name, PERM_WRITE)) return 2;
    if (values->count != h->dimension) return 4;

    if (h->first_page_id == VECSTORE_INVALID_PAGE ||
        h->entries_in_last_page >= h->entries_per_page) {
        uint32_t new_page = vecstore_alloc_page();
        if (new_page == VECSTORE_INVALID_PAGE) return 5;

        if (h->first_page_id == VECSTORE_INVALID_PAGE) {
            h->first_page_id = new_page;
        } else {
            uint8_t* prev = vecstore_load_page(h->last_page_id);
            if (prev) { vs_memcpy(prev, &new_page, 4); vecstore_flush_page(h->last_page_id); }
        }
        h->last_page_id = new_page;
        h->entries_in_last_page = 0;
        h->page_count++;
    }

    uint8_t* page = vecstore_load_page(h->last_page_id);
    if (!page) return 5;
    uint32_t slot_idx = h->entries_in_last_page;
    uint8_t* slot = page + 4 + slot_idx * h->entry_width;
    vs_write_entry(slot, external_id, values, h->dimension);
    vecstore_flush_page(h->last_page_id);   // Gap Remediation Phase D

    h->entries_in_last_page++;
    h->entry_count++;

    struct VecId new_id = { h->last_page_id, slot_idx };
    if (out_id) *out_id = new_id;
    persist_vecstore_headers();   // Gap Remediation Phase D

    // Phase 6: auto-maintenance -- see vec_index.h's own header comment on
    // why this call is unconditional here (vec_index_notify_insert() is
    // the one that decides whether any index actually needs updating,
    // mirroring row_index_notify_insert()'s identical role in
    // rowstore.c). Not separately access-gated -- catalog_check_access()
    // already ran above.
    vec_index_notify_insert(caller_uid, collection_name, new_id, external_id, values);
    return 0;
}

int vecstore_get(uint32_t caller_uid, const char* collection_name,
                 struct VecId id, uint64_t* out_external_id, struct VecValues* out) {
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 1;
    struct VecCollectionHeader* h = &vector_collections[idx];
    if (!catalog_check_access(caller_uid, collection_name, PERM_READ)) return 2;
    if (id.page_id >= vecstore_next_free_page_id) return 3;
    if (id.slot_index >= h->entries_per_page) return 3;

    uint8_t* page = vecstore_load_page(id.page_id);
    if (!page) return 3;
    uint8_t* slot = page + 4 + id.slot_index * h->entry_width;
    if (!slot[0]) return 3;   // tombstoned / never written

    uint64_t ext_id;
    struct VecValues vals;
    vs_read_entry(slot, &ext_id, &vals, h->dimension);
    if (out_external_id) *out_external_id = ext_id;
    if (out) *out = vals;
    return 0;
}

int vecstore_delete(uint32_t caller_uid, const char* collection_name, struct VecId id) {
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 1;
    struct VecCollectionHeader* h = &vector_collections[idx];
    if (!catalog_check_access(caller_uid, collection_name, PERM_WRITE)) return 2;
    if (id.page_id >= vecstore_next_free_page_id) return 3;
    if (id.slot_index >= h->entries_per_page) return 3;

    uint8_t* page = vecstore_load_page(id.page_id);
    if (!page) return 3;
    uint8_t* slot = page + 4 + id.slot_index * h->entry_width;
    if (!slot[0]) return 3;   // already deleted / never written

    slot[0] = 0;   // tombstone -- slot is NOT reclaimed for reuse in this first cut
    vecstore_flush_page(id.page_id);   // Gap Remediation Phase D
    h->entry_count--;
    persist_vecstore_headers();        // Gap Remediation Phase D

    // Phase 6: auto-maintenance -- see the identical comment in
    // vecstore_insert() above.
    vec_index_notify_delete(collection_name, id);
    return 0;
}

// ─── Scan ────────────────────────────────────────────────────────────────────
uint32_t vecstore_collection_scan(uint32_t caller_uid, const char* collection_name,
                                  VecScanCb cb, void* ctx) {
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 0;
    struct VecCollectionHeader* h = &vector_collections[idx];
    if (!catalog_check_access(caller_uid, collection_name, PERM_READ)) return 0;

    uint32_t visited = 0;
    uint32_t page_id = h->first_page_id;
    while (page_id != VECSTORE_INVALID_PAGE) {
        uint8_t* page = vecstore_load_page(page_id);
        if (!page) break;
        uint32_t next_page; vs_memcpy(&next_page, page, 4);
        uint32_t limit = (page_id == h->last_page_id) ? h->entries_in_last_page : h->entries_per_page;

        for (uint32_t s = 0; s < limit; s++) {
            uint8_t* slot = page + 4 + s * h->entry_width;
            if (!slot[0]) continue;
            uint64_t ext_id;
            struct VecValues vals;
            vs_read_entry(slot, &ext_id, &vals, h->dimension);
            struct VecId id = { page_id, s };
            if (cb) cb(id, ext_id, &vals, ctx);
            visited++;
        }
        page_id = next_page;
    }
    return visited;
}

// ─── Phase 2: brute-force similarity search ─────────────────────────────────

// Newton-Raphson sqrt -- no libc math exists in this freestanding kernel
// (confirmed by direct audit before this phase started: no sqrt/newton/
// isqrt anywhere in the tree), matching every other numeric helper in this
// codebase's own hand-rolled-per-file convention. Globally convergent for
// any x > 0 from a positive starting guess; the starting guess below (1.0
// for x < 1, x itself otherwise) just shortens the number of iterations
// needed, it doesn't affect correctness. Bounded at 20 iterations with an
// early exit on convergence (float precision reaches its fixed point in
// under 10 iterations for any realistic embedding-magnitude input) rather
// than looping until exact equality, which float rounding can make loop
// forever for some inputs.
float vs_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float guess = (x < 1.0f) ? 1.0f : x;
    for (int i = 0; i < 20; i++) {
        float next = 0.5f * (guess + x / guess);
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

float vs_dot(const struct VecValues* a, const struct VecValues* b, uint32_t n) {
    float s = 0.0f;
    for (uint32_t i = 0; i < n; i++) s += a->values[i] * b->values[i];
    return s;
}

// 1.0f - cosine similarity -- see vecstore.h's header comment on why this
// (not raw similarity) is what this file calls "cosine distance."
float vs_distance_cosine(const struct VecValues* a, const struct VecValues* b, uint32_t n) {
    float na = vs_sqrtf(vs_dot(a, a, n));
    float nb = vs_sqrtf(vs_dot(b, b, n));
    if (na == 0.0f || nb == 0.0f) return 1.0f;   // undefined direction -- neutral, not min or max
    float sim = vs_dot(a, b, n) / (na * nb);
    if (sim > 1.0f) sim = 1.0f;     // clamp -- float rounding can push a tiny bit outside [-1,1]
    if (sim < -1.0f) sim = -1.0f;
    return 1.0f - sim;
}

float vs_distance_l2(const struct VecValues* a, const struct VecValues* b, uint32_t n) {
    float s = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float d = a->values[i] - b->values[i];
        s += d * d;
    }
    return vs_sqrtf(s);
}

// Bounded top-K insertion: out[0..*found) stays sorted ascending by
// distance at all times. O(k) worst case per candidate (a shift, not a
// full re-sort) -- see vecstore.h's header comment on the O(n*k) vs.
// O(n*log k) trade-off this makes deliberately.
void vs_topk_insert(struct VecMatch* out, uint32_t* found, uint32_t k, struct VecMatch cand) {
    if (*found < k) {
        uint32_t pos = *found;
        while (pos > 0 && out[pos - 1].distance > cand.distance) {
            out[pos] = out[pos - 1];
            pos--;
        }
        out[pos] = cand;
        (*found)++;
    } else if (k > 0 && cand.distance < out[k - 1].distance) {
        uint32_t pos = k - 1;
        while (pos > 0 && out[pos - 1].distance > cand.distance) {
            out[pos] = out[pos - 1];
            pos--;
        }
        out[pos] = cand;
    }
    // else: candidate is worse than every match already kept -- discarded,
    // matching a bounded top-K's whole point (never grows past k).
}

struct vs_search_ctx {
    const struct VecValues* query;
    VecMetric                metric;
    uint32_t                 dimension;
    uint32_t                 k;
    struct VecMatch*         out;
    uint32_t                 found;
};

static void vs_search_cb(struct VecId id, uint64_t external_id, const struct VecValues* v, void* ctxp) {
    struct vs_search_ctx* ctx = (struct vs_search_ctx*)ctxp;
    float dist = (ctx->metric == VEC_METRIC_L2)
                 ? vs_distance_l2(ctx->query, v, ctx->dimension)
                 : vs_distance_cosine(ctx->query, v, ctx->dimension);
    struct VecMatch cand = { external_id, id, dist };
    vs_topk_insert(ctx->out, &ctx->found, ctx->k, cand);
}

uint32_t vecstore_search(uint32_t caller_uid, const char* collection_name,
                         const struct VecValues* query, VecMetric metric,
                         uint32_t k, struct VecMatch* out) {
    if (!query || !out || k == 0) return 0;
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 0;
    if (query->count != vector_collections[idx].dimension) return 0;

    struct vs_search_ctx ctx = { query, metric, vector_collections[idx].dimension, k, out, 0 };
    // Reuses vecstore_collection_scan()'s own catalog_check_access() gate
    // and page-chain traversal -- no parallel permission path or duplicate
    // scan logic, matching this codebase's established "one real choke
    // point" convention (predicate_table_scan/rowstore_table_scan's own
    // relationship in the RDBMS roadmap is the direct precedent).
    vecstore_collection_scan(caller_uid, collection_name, vs_search_cb, &ctx);
    return ctx.found;
}

// ─── Vector Store Roadmap Phase 4: syscall adapters ("make it live") ─────
// Thin, one-line-of-real-logic adapters -- see vecstore.h's own comment on
// why sys_sls_vec_embed_insert() below is the one place this file includes
// ollama_client.h (already pulled in transitively via vecstore.h), and why
// SYS_SLS_VEC_CREATE exists even though the roadmap's own §6 scope bullet
// only named insert/embed-insert/search.

uint64_t sys_sls_vec_create(struct SLSVecCreateRequest* req) {
    if (!req) return 1;
    req->status = vecstore_create_collection(req->collection_name, req->dimension);
    return (uint64_t)req->status;
}

uint64_t sys_sls_vec_insert(struct SLSVecInsertRequest* req) {
    if (!req) return 1;
    req->status = vecstore_insert(req->caller_uid, req->collection_name,
                                  req->external_id, &req->values, &req->out_id);
    return (uint64_t)req->status;
}

// Embeds req->ollama_req.prompt via a real network round-trip to Ollama,
// then stores the resulting embedding -- two independent failure points,
// reported separately (ollama_status vs. insert_status) rather than
// collapsed into one code, so a caller can tell "Ollama never answered"
// apart from "Ollama answered fine but the collection/dimension was
// wrong" -- the same "don't let denial look like absence" discipline named
// in this struct's own header comment, applied to failure REPORTING this
// time rather than reachability.
uint64_t sys_sls_vec_embed_insert(struct SLSVecEmbedInsertRequest* req) {
    if (!req) return 1;

    struct OllamaEmbedResponse resp;
    req->ollama_status = ollama_embed(&req->ollama_req, &resp);
    if (req->ollama_status != 0) {
        req->insert_status = -1;   // never attempted -- distinct from any real vecstore_insert() code (0-5)
        return 1;
    }

    // resp.dimension is already bounded by OLLAMA_MAX_EMBED_DIM, which
    // equals VECSTORE_MAX_DIMENSION (independently declared in each
    // header -- see ollama_client.h's own comment on why they don't share
    // one) -- this cap is belt-and-suspenders, not expected to trigger.
    uint32_t n = resp.dimension;
    if (n > VECSTORE_MAX_DIMENSION) n = VECSTORE_MAX_DIMENSION;

    struct VecValues values;
    values.count = n;
    for (uint32_t i = 0; i < n; i++) values.values[i] = resp.embedding[i];

    req->insert_status = vecstore_insert(req->caller_uid, req->collection_name,
                                         req->external_id, &values, &req->out_id);
    return (uint64_t)req->insert_status;
}

uint64_t sys_sls_vec_search(struct SLSVecSearchRequest* req) {
    if (!req) return 1;

    uint32_t k = req->k;
    req->truncated = (k > VEC_SEARCH_MAX_K) ? 1 : 0;
    if (k > VEC_SEARCH_MAX_K) k = VEC_SEARCH_MAX_K;

    // vecstore_search()'s own documented ambiguity (0 means either "ran
    // and found nothing" or "couldn't run at all") is deliberately
    // preserved here rather than "fixed" at the syscall boundary -- see
    // vecstore_search()'s own header comment for why that's an accepted,
    // named tradeoff, not an oversight. This adapter always returns
    // syscall-success (0); match_count carries the real signal.
    req->match_count = vecstore_search(req->caller_uid, req->collection_name,
                                       &req->query, req->metric, k, req->matches);
    return 0;
}

// ─── Gap Remediation Phase C: collection enumeration ────────────────────────
// Mirrors sys_sls_obj_list()'s own shape exactly (object_catalog.c) -- see
// vecstore.h's own comment on this syscall.
void sys_sls_vec_list(void) {
    // kernel_serial_print() takes exactly one argument (const char*, no
    // format processing at all -- kernel_io.c's own definition) --
    // kernel_serial_printf() is the variadic one. Worth noting: this file's
    // own established precedent, sys_sls_obj_list() (object_catalog.c),
    // calls kernel_serial_print() with a format string PLUS several extra
    // string arguments for its header row -- a real, pre-existing bug (see
    // that file's own Gap Remediation Phase C fix), not a pattern to copy.
    kernel_serial_printf(
        "\n[VECSTORE] Collection Directory\n"
        " %-24s %-6s %-8s %s\n"
        " %-24s %-6s %-8s %s\n",
        "Name", "Dim", "Entries", "Pages",
        "------------------------", "------", "--------", "-----");

    uint32_t shown = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active || !vector_collections[i].active) continue;
        struct VecCollectionHeader* h = &vector_collections[i];
        kernel_serial_printf(" %-24s %-6u %-8u %u\n",
                             object_catalog[i].name, h->dimension, h->entry_count, h->page_count);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no vector collections defined)\n");
}
