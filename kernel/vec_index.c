/*
 * vec_index.c — Vector Store Roadmap Phase 6 implementation (HNSW). See
 * vec_index.h for the full design writeup.
 */
#include "vec_index.h"
#include "object_catalog.h"
#include "kernel_io.h"   // Gap Remediation Phase C -- sys_sls_vec_index_list()
#include "persist.h"     // Gap Remediation Phase D -- persist_vec_index_defs()
#include "../user/permissions.h"
#include <stddef.h>

struct VecIndexNode vec_index_nodes[VEC_INDEX_MAX_NODES];
uint32_t             vec_index_next_free_node = 0;
struct VecIndex       vec_indexes[VEC_INDEX_MAX];

// ─── String helpers (no libc -- vi_* here, matching this codebase's
// established per-file convention). ─────────────────────────────────────
static void vi_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static int vi_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ─── PRNG -- xorshift32. No PRNG existed anywhere in this kernel before
// this phase (confirmed by grep before writing this); a hand-rolled one
// is genuinely new machinery, same category as Phase 3's JSON parser or
// Phase 2's vs_sqrtf(). Reset to a fixed seed by vec_index_init() so a
// given sequence of operations produces a reproducible graph shape run to
// run -- useful for this phase's own host test assertions, and harmless
// for a first-cut approximate index that never claimed cryptographic or
// even statistically rigorous randomness. ───────────────────────────────
static uint32_t vi_rng_state = 0x9E3779B9u;   // nonzero seed -- xorshift's fixed point is all-zero, never reached from here
static uint32_t vi_rand32(void) {
    uint32_t x = vi_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vi_rng_state = x;
    return x;
}

// Discrete geometric layer assignment -- see vec_index.h's header comment
// point 1 on why this replaces the paper's ln()-based formula.
static uint32_t vi_random_layer(void) {
    uint32_t layer = 0;
    while (layer < VEC_INDEX_MAX_LAYERS - 1 && (vi_rand32() % VEC_INDEX_M) == 0) layer++;
    return layer;
}

static int vi_find_index_slot(const char* index_name) {
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++)
        if (vec_indexes[i].active && vi_streq(vec_indexes[i].index_name, index_name)) return (int)i;
    return -1;
}

// Mirrors vecstore.c's own find_active_collection() -- duplicated rather
// than exported, matching this codebase's usual per-file convention (this
// lookup, unlike distance/top-K math, carries no cross-file correctness-
// drift risk -- see vecstore.h's own comment on why THOSE were exported
// instead).
static int vi_find_collection_index(const char* collection_name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (vi_streq(object_catalog[i].name, collection_name)) return (int)i;
    }
    return -1;
}

// ─── Distance helpers -- both re-fetch vector values through vecstore_
// get() rather than caching a local copy (see header comment). A failed
// fetch (deleted/stale VecId, denied access) reports an effectively-
// infinite distance so it can never win a comparison, rather than
// crashing or silently treating it as "closest." ────────────────────────
#define VI_DISTANCE_INFINITY 3.4e38f

static int vi_fetch_values(struct VecIndex* idx, uint32_t caller_uid, uint32_t node_idx, struct VecValues* out) {
    uint64_t ext;
    return vecstore_get(caller_uid, idx->collection_name, vec_index_nodes[node_idx].vec_id, &ext, out);
}

// Gap Remediation (post-roadmap x86 boot-build fix): out-parameter
// convention, not a by-value float return -- see vecstore.c's own header
// comment (the real x86-64 cross-build disables SSE, which breaks float
// BY-VALUE RETURN specifically; parameters and internal math are fine).
static void vi_distance(float* out, struct VecIndex* idx, uint32_t caller_uid, const struct VecValues* query, uint32_t node_idx) {
    struct VecValues v;
    if (vi_fetch_values(idx, caller_uid, node_idx, &v) != 0) { *out = VI_DISTANCE_INFINITY; return; }
    if (idx->metric == VEC_METRIC_L2) vs_distance_l2(out, query, &v, idx->dimension);
    else                              vs_distance_cosine(out, query, &v, idx->dimension);
}

static void vi_node_distance(float* out, struct VecIndex* idx, uint32_t caller_uid, uint32_t a, uint32_t b) {
    struct VecValues va, vb;
    if (vi_fetch_values(idx, caller_uid, a, &va) != 0) { *out = VI_DISTANCE_INFINITY; return; }
    if (vi_fetch_values(idx, caller_uid, b, &vb) != 0) { *out = VI_DISTANCE_INFINITY; return; }
    if (idx->metric == VEC_METRIC_L2) vs_distance_l2(out, &va, &vb, idx->dimension);
    else                              vs_distance_cosine(out, &va, &vb, idx->dimension);
}

// ─── Bounded candidate list -- structurally the same insertion-sorted-
// ascending-array shape as vecstore.c's own vs_topk_insert() (reused
// there directly for the FINAL top-K, see header comment on why that one
// was exported instead of duplicated), but carrying a pool-wide node
// index rather than a VecMatch, since graph traversal needs to re-enter
// vec_index_nodes[] -- VecMatch alone (external_id + VecId) can't do
// that. A structural analog of vs_topk_insert(), not a literal reuse. ───
struct vi_cand { uint32_t node_idx; float distance; };
struct vi_beam { struct vi_cand items[VEC_INDEX_MAX_EF]; uint32_t count; };

static void vi_beam_insert(struct vi_beam* beam, uint32_t cap, struct vi_cand cand) {
    if (cap > VEC_INDEX_MAX_EF) cap = VEC_INDEX_MAX_EF;
    if (beam->count < cap) {
        uint32_t pos = beam->count;
        while (pos > 0 && beam->items[pos - 1].distance > cand.distance) {
            beam->items[pos] = beam->items[pos - 1];
            pos--;
        }
        beam->items[pos] = cand;
        beam->count++;
    } else if (cap > 0 && cand.distance < beam->items[cap - 1].distance) {
        uint32_t pos = cap - 1;
        while (pos > 0 && beam->items[pos - 1].distance > cand.distance) {
            beam->items[pos] = beam->items[pos - 1];
            pos--;
        }
        beam->items[pos] = cand;
    }
}

// Reset per call (see header comment on why this matches the textbook
// SEARCH-LAYER's own per-invocation-fresh visited set -- upper-layer
// greedy descent doesn't use a visited set at all, so this scratch buffer
// is only ever touched during an actual beam search, one layer at a
// time). Static, not stack-allocated -- matching this codebase's
// established "static working buffer, not deep per-call stack usage"
// idiom (net/inference.c's static request/response buffers are the
// direct precedent) -- safe because neither vecstore.c nor vec_index.c
// support concurrent access from multiple callers, a posture this whole
// project has held since Phase 1.
static uint8_t vi_visited[VEC_INDEX_MAX_NODES];

// One layer's ef-bounded greedy beam search from entry_points[]. Standard
// HNSW SEARCH-LAYER: explore the frontier nearest-first, stop once the
// nearest unexplored candidate is already farther than the worst kept
// result and the result set is full. `result` comes back sorted
// ascending by distance, capped at `ef` entries.
static void vi_search_layer(struct VecIndex* idx, uint32_t caller_uid,
                            const struct VecValues* query, uint32_t layer,
                            const uint32_t* entry_points, uint32_t n_entry,
                            uint32_t ef, struct vi_beam* result) {
    for (uint32_t i = 0; i < VEC_INDEX_MAX_NODES; i++) vi_visited[i] = 0;
    result->count = 0;

    struct vi_beam frontier;   // candidates still to explore, ascending by distance
    frontier.count = 0;

    for (uint32_t i = 0; i < n_entry; i++) {
        uint32_t node_idx = entry_points[i];
        if (node_idx >= VEC_INDEX_MAX_NODES || vi_visited[node_idx]) continue;
        vi_visited[node_idx] = 1;
        float d; vi_distance(&d, idx, caller_uid, query, node_idx);
        struct vi_cand c = { node_idx, d };
        vi_beam_insert(&frontier, VEC_INDEX_MAX_EF, c);
        vi_beam_insert(result, ef, c);
    }

    while (frontier.count > 0) {
        struct vi_cand cur = frontier.items[0];
        for (uint32_t i = 1; i < frontier.count; i++) frontier.items[i - 1] = frontier.items[i];
        frontier.count--;

        if (result->count >= ef && cur.distance > result->items[result->count - 1].distance) {
            break;   // nothing left in the frontier can improve `result` -- standard early exit
        }

        struct VecIndexNode* node = &vec_index_nodes[cur.node_idx];
        if (layer > node->top_layer) continue;   // defensive; shouldn't happen by construction
        for (uint32_t i = 0; i < node->neighbor_count[layer]; i++) {
            uint32_t nb = node->neighbors[layer][i];
            if (nb >= VEC_INDEX_MAX_NODES || vi_visited[nb]) continue;
            vi_visited[nb] = 1;
            float d; vi_distance(&d, idx, caller_uid, query, nb);
            if (result->count < ef || d < result->items[result->count - 1].distance) {
                struct vi_cand c = { nb, d };
                vi_beam_insert(&frontier, VEC_INDEX_MAX_EF, c);
                vi_beam_insert(result, ef, c);
            }
        }
    }
}

// Single-best-neighbor greedy hill-climb through layers strictly above
// `down_to_layer`, down to and including `down_to_layer + 1` (i.e. stops
// once layer `down_to_layer` would be next) -- the upper-layer half of
// both insertion and search, matching the textbook algorithm's own
// ef=1 shortcut for every layer above the graph's actual working layer.
static uint32_t vi_greedy_descend(struct VecIndex* idx, uint32_t caller_uid,
                                  const struct VecValues* query,
                                  uint32_t entry_node, uint32_t from_layer, uint32_t down_to_layer) {
    uint32_t current = entry_node;
    float current_dist; vi_distance(&current_dist, idx, caller_uid, query, current);
    for (uint32_t layer = from_layer; layer > down_to_layer; layer--) {
        int improved = 1;
        while (improved) {
            improved = 0;
            struct VecIndexNode* node = &vec_index_nodes[current];
            if (layer > node->top_layer) break;   // defensive
            for (uint32_t i = 0; i < node->neighbor_count[layer]; i++) {
                uint32_t nb = node->neighbors[layer][i];
                float d; vi_distance(&d, idx, caller_uid, query, nb);
                if (d < current_dist) { current = nb; current_dist = d; improved = 1; }
            }
        }
    }
    return current;
}

// Bidirectional edge from `from` to `to` at `layer`. If `from`'s neighbor
// list at this layer is already at capacity (M0 at layer 0, M above),
// replaces its current furthest neighbor only if `to` is genuinely closer
// -- the "simple keep-M-closest" selection named in vec_index.h's header
// comment, not the paper's diversification heuristic.
static void vi_connect(struct VecIndex* idx, uint32_t caller_uid, uint32_t from, uint32_t to, uint32_t layer) {
    struct VecIndexNode* node = &vec_index_nodes[from];
    uint32_t max_m = (layer == 0) ? VEC_INDEX_M0 : VEC_INDEX_M;

    for (uint32_t i = 0; i < node->neighbor_count[layer]; i++)
        if (node->neighbors[layer][i] == to) return;   // already connected

    if (node->neighbor_count[layer] < max_m) {
        node->neighbors[layer][node->neighbor_count[layer]++] = to;
        return;
    }

    float new_dist; vi_node_distance(&new_dist, idx, caller_uid, from, to);
    uint32_t worst_i = 0;
    float worst_dist; vi_node_distance(&worst_dist, idx, caller_uid, from, node->neighbors[layer][0]);
    for (uint32_t i = 1; i < node->neighbor_count[layer]; i++) {
        float d; vi_node_distance(&d, idx, caller_uid, from, node->neighbors[layer][i]);
        if (d > worst_dist) { worst_dist = d; worst_i = i; }
    }
    if (new_dist < worst_dist) node->neighbors[layer][worst_i] = to;
}

// ─── Insertion ────────────────────────────────────────────────────────────
static uint32_t vi_entry_scratch[VEC_INDEX_MAX_EF];

static void vi_insert_into(struct VecIndex* idx, uint32_t idx_slot, uint32_t caller_uid,
                           struct VecId id, uint64_t external_id, const struct VecValues* values) {
    if (vec_index_next_free_node >= VEC_INDEX_MAX_NODES) return;   // shared pool exhausted -- see
                                                                    // header comment point 3's sibling
                                                                    // concern: the vecstore insert this
                                                                    // is reacting to has ALREADY
                                                                    // succeeded and is not undone; the
                                                                    // entry simply isn't indexed,
                                                                    // matching row_index.c's own
                                                                    // BTREE_MAX_DUPES_PER_KEY-exhaustion
                                                                    // precedent exactly.
    uint32_t new_idx = vec_index_next_free_node++;
    struct VecIndexNode* node = &vec_index_nodes[new_idx];
    node->index_id = idx_slot;
    node->vec_id = id;
    node->external_id = external_id;
    node->active = 1;
    node->top_layer = vi_random_layer();
    for (uint32_t l = 0; l < VEC_INDEX_MAX_LAYERS; l++) node->neighbor_count[l] = 0;
    idx->node_count++;
    idx->active_count++;

    if (idx->entry_point == VEC_INDEX_INVALID) {
        idx->entry_point = new_idx;
        idx->top_layer = node->top_layer;
        return;
    }

    uint32_t entry = idx->entry_point;
    if (idx->top_layer > node->top_layer) {
        entry = vi_greedy_descend(idx, caller_uid, values, entry, idx->top_layer, node->top_layer);
    }

    uint32_t start_layer = (idx->top_layer < node->top_layer) ? idx->top_layer : node->top_layer;
    vi_entry_scratch[0] = entry;
    uint32_t n_entry = 1;

    for (int32_t layer = (int32_t)start_layer; layer >= 0; layer--) {
        struct vi_beam candidates;
        vi_search_layer(idx, caller_uid, values, (uint32_t)layer, vi_entry_scratch, n_entry,
                        VEC_INDEX_EF_CONSTRUCTION, &candidates);

        uint32_t max_m = (layer == 0) ? VEC_INDEX_M0 : VEC_INDEX_M;
        uint32_t connect_count = candidates.count < max_m ? candidates.count : max_m;
        for (uint32_t i = 0; i < connect_count; i++) {
            uint32_t nb_idx = candidates.items[i].node_idx;
            vi_connect(idx, caller_uid, new_idx, nb_idx, (uint32_t)layer);
            vi_connect(idx, caller_uid, nb_idx, new_idx, (uint32_t)layer);
        }

        n_entry = candidates.count < VEC_INDEX_MAX_EF ? candidates.count : VEC_INDEX_MAX_EF;
        for (uint32_t i = 0; i < n_entry; i++) vi_entry_scratch[i] = candidates.items[i].node_idx;
        if (n_entry == 0) { vi_entry_scratch[0] = entry; n_entry = 1; }   // defensive: never let the
                                                                            // next layer start with zero
                                                                            // entry points
    }

    if (node->top_layer > idx->top_layer) {
        idx->entry_point = new_idx;
        idx->top_layer = node->top_layer;
    }
}

// ─── Public API ───────────────────────────────────────────────────────────

void vec_index_init(void) {
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
        vec_indexes[i].active = 0;
        vec_indexes[i].node_count = 0;
        vec_indexes[i].active_count = 0;
        vec_indexes[i].entry_point = VEC_INDEX_INVALID;
        vec_indexes[i].top_layer = 0;
    }
    vec_index_next_free_node = 0;
    vi_rng_state = 0x9E3779B9u;   // reset to a fixed, reproducible seed -- see header comment
}

int vec_index_create(uint32_t caller_uid, const char* index_name,
                     const char* collection_name, VecMetric metric) {
    if (!index_name || !collection_name) return 1;

    int cidx = vi_find_collection_index(collection_name);
    if (cidx < 0 || !vector_collections[cidx].active) return 1;

    if (catalog_check_access(caller_uid, collection_name, PERM_READ) == 0) return 2;

    int free_slot = -1;
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
        if (vec_indexes[i].active && vi_streq(vec_indexes[i].index_name, index_name)) return 3;
        if (!vec_indexes[i].active && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot < 0) return 3;

    struct VecIndex* idx = &vec_indexes[free_slot];
    vi_strcpy(idx->index_name, index_name, OBJECT_NAME_LEN);
    vi_strcpy(idx->collection_name, collection_name, OBJECT_NAME_LEN);
    idx->dimension = vector_collections[cidx].dimension;
    idx->metric = metric;
    idx->entry_point = VEC_INDEX_INVALID;
    idx->top_layer = 0;
    idx->node_count = 0;
    idx->active_count = 0;
    idx->active = 1;

    persist_vec_index_defs();   // Gap Remediation Phase D
    return 0;
}

void vec_index_notify_insert(uint32_t caller_uid, const char* collection_name,
                             struct VecId id, uint64_t external_id,
                             const struct VecValues* values) {
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
        if (!vec_indexes[i].active) continue;
        if (!vi_streq(vec_indexes[i].collection_name, collection_name)) continue;
        vi_insert_into(&vec_indexes[i], i, caller_uid, id, external_id, values);
    }
}

void vec_index_notify_delete(const char* collection_name, struct VecId id) {
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
        if (!vec_indexes[i].active) continue;
        if (!vi_streq(vec_indexes[i].collection_name, collection_name)) continue;
        for (uint32_t n = 0; n < vec_index_next_free_node; n++) {
            struct VecIndexNode* node = &vec_index_nodes[n];
            if (node->index_id != i || !node->active) continue;
            if (node->vec_id.page_id == id.page_id && node->vec_id.slot_index == id.slot_index) {
                node->active = 0;
                if (vec_indexes[i].active_count > 0) vec_indexes[i].active_count--;
            }
        }
    }
}

// ─── VectorStore Interface Roadmap Phase 1: bulk deactivation ─────────────
// See vec_index.h's own comment on this function. Same loop shape as
// vec_index_notify_delete() above, generalized from "one matching VecId"
// to "every node this index owns" -- reused by both
// vecstore_notify_object_freed() (whole collection freed) and, indirectly,
// vec_index_drop() below (single named index dropped) share this exact
// per-index tombstone-everything body.
static void vi_deactivate_index(uint32_t index_slot) {
    for (uint32_t n = 0; n < vec_index_next_free_node; n++) {
        struct VecIndexNode* node = &vec_index_nodes[n];
        if (node->index_id != index_slot) continue;
        node->active = 0;
    }
    vec_indexes[index_slot].active = 0;
    vec_indexes[index_slot].active_count = 0;
    persist_vec_index_defs();   // Gap Remediation Phase D
}

void vec_index_notify_collection_freed(const char* collection_name) {
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
        if (!vec_indexes[i].active) continue;
        if (!vi_streq(vec_indexes[i].collection_name, collection_name)) continue;
        vi_deactivate_index(i);
    }
}

int vec_index_drop(uint32_t caller_uid, const char* index_name) {
    int slot = vi_find_index_slot(index_name);
    if (slot < 0) return 1;
    struct VecIndex* idx = &vec_indexes[slot];

    if (catalog_check_access(caller_uid, idx->collection_name, PERM_WRITE) == 0) return 2;

    vi_deactivate_index((uint32_t)slot);
    return 0;
}

// ─── VectorStore Interface Roadmap Phase 3: rebuild/backfill ──────────────
// Callback shape matches vecstore.c's own vs_search_cb -- one static ctx
// struct, one static callback, one vecstore_collection_scan() call, same
// three-piece pattern that file's search adapter already established for
// "walk every live entry, do something with it."
struct vi_rebuild_ctx {
    struct VecIndex* idx;
    uint32_t          idx_slot;
    uint32_t          caller_uid;
};

static void vi_rebuild_cb(struct VecId id, uint64_t external_id,
                          const struct VecValues* values, void* ctxp) {
    struct vi_rebuild_ctx* ctx = (struct vi_rebuild_ctx*)ctxp;
    vi_insert_into(ctx->idx, ctx->idx_slot, ctx->caller_uid, id, external_id, values);
}

// See this function's own prototype comment (vec_index.h) for the full
// contract. Clear step reuses vi_deactivate_index()'s per-node tombstone
// LOOP SHAPE but not that function itself -- vi_deactivate_index() also
// sets vec_indexes[slot].active = 0 (a real drop), which rebuild must NOT
// do, so this inlines the same loop rather than calling it and then
// un-deactivating the index back to active (which would round-trip
// through persist_vec_index_defs() twice and briefly make the index
// invisible to any concurrent reader between the two calls -- avoided
// entirely by never actually dropping it in the first place).
int vec_index_rebuild(uint32_t caller_uid, const char* index_name) {
    int slot = vi_find_index_slot(index_name);
    if (slot < 0) return 1;
    struct VecIndex* idx = &vec_indexes[slot];

    if (catalog_check_access(caller_uid, idx->collection_name, PERM_WRITE) == 0) return 2;

    for (uint32_t n = 0; n < vec_index_next_free_node; n++) {
        struct VecIndexNode* node = &vec_index_nodes[n];
        if (node->index_id != (uint32_t)slot) continue;
        node->active = 0;
    }
    // Reset graph-entry state so the first re-inserted node below becomes
    // the new entry point, exactly as it would for a freshly created,
    // empty index (vec_index_create()'s own initial values) -- node_count
    // is deliberately NOT reset here; see this function's own header
    // comment (vec_index.h) for why.
    idx->entry_point = VEC_INDEX_INVALID;
    idx->top_layer = 0;
    idx->active_count = 0;

    struct vi_rebuild_ctx ctx = { idx, (uint32_t)slot, caller_uid };
    vecstore_collection_scan(caller_uid, idx->collection_name, vi_rebuild_cb, &ctx);

    persist_vec_index_defs();   // Gap Remediation Phase D
    return 0;
}

uint32_t vec_index_search(uint32_t caller_uid, const char* index_name,
                          const struct VecValues* query, uint32_t k, uint32_t ef,
                          struct VecMatch* out) {
    if (!query || !out || k == 0) return 0;
    int slot = vi_find_index_slot(index_name);
    if (slot < 0) return 0;
    struct VecIndex* idx = &vec_indexes[slot];

    if (catalog_check_access(caller_uid, idx->collection_name, PERM_READ) == 0) return 0;
    if (query->count != idx->dimension) return 0;
    if (idx->entry_point == VEC_INDEX_INVALID) return 0;

    if (ef < k) ef = k;
    if (ef > VEC_INDEX_MAX_EF) ef = VEC_INDEX_MAX_EF;

    uint32_t entry = idx->entry_point;
    if (idx->top_layer > 0) {
        entry = vi_greedy_descend(idx, caller_uid, query, entry, idx->top_layer, 0);
    }

    uint32_t entry_points[1] = { entry };
    struct vi_beam candidates;
    vi_search_layer(idx, caller_uid, query, 0, entry_points, 1, ef, &candidates);

    uint32_t found = 0;
    for (uint32_t i = 0; i < candidates.count && found < k; i++) {
        uint32_t node_idx = candidates.items[i].node_idx;
        if (!vec_index_nodes[node_idx].active) continue;   // tombstoned -- excluded from results, see header comment
        out[found].external_id = vec_index_nodes[node_idx].external_id;
        out[found].id = vec_index_nodes[node_idx].vec_id;
        out[found].distance = candidates.items[i].distance;
        found++;
    }
    return found;
}

// ─── Gap Remediation Phase C: live reachability adapters ───────────────────
// Thin adapters, matching sys_sls_vec_create()/sys_sls_vec_search()'s own
// shape exactly (kernel/vecstore.c) -- see vec_index.h's own comment.
uint64_t sys_sls_vec_index_create(struct SLSVecIndexCreateRequest* req) {
    if (!req) return 1;
    req->status = vec_index_create(req->caller_uid, req->index_name,
                                   req->collection_name, req->metric);
    return (uint64_t)req->status;
}

uint64_t sys_sls_vec_index_search(struct SLSVecIndexSearchRequest* req) {
    if (!req) return 1;

    uint32_t k = req->k;
    req->truncated = (k > VEC_SEARCH_MAX_K) ? 1 : 0;
    if (k > VEC_SEARCH_MAX_K) k = VEC_SEARCH_MAX_K;

    // Same documented 0-is-ambiguous contract vec_index_search()/
    // vecstore_search() already have, deliberately preserved rather than
    // "fixed" at the syscall boundary -- see sys_sls_vec_search()'s own
    // adapter (vecstore.c) for the identical, already-established posture.
    req->match_count = vec_index_search(req->caller_uid, req->index_name,
                                        &req->query, k, req->ef, req->matches);
    return 0;
}

// ─── VectorStore Interface Roadmap Phase 1: index drop ────────────────────
// Thin adapter, same shape as the two above -- one line into vec_index_drop().
uint64_t sys_sls_vec_index_drop(struct SLSVecIndexDropRequest* req) {
    if (!req) return 1;
    req->status = vec_index_drop(req->caller_uid, req->index_name);
    return (uint64_t)req->status;
}

// ─── VectorStore Interface Roadmap Phase 2: semantic (embed-then-search) ──
// HNSW counterpart to vecstore.c's own sys_sls_vec_embed_search() -- same
// embed-first shape, calling vec_index_search() instead of vecstore_search()
// as the final step. See that adapter's own comment for why this is a
// deliberate second copy of the embed-and-convert prefix rather than a
// shared helper.
uint64_t sys_sls_vec_index_embed_search(struct SLSVecIndexEmbedSearchRequest* req) {
    if (!req) return 1;

    struct OllamaEmbedResponse resp;
    req->ollama_status = ollama_embed(&req->ollama_req, &resp);
    if (req->ollama_status != 0) {
        req->match_count = 0;   // never attempted -- distinct from "attempted, found nothing"
        return 1;
    }

    uint32_t n = resp.dimension;
    if (n > VECSTORE_MAX_DIMENSION) n = VECSTORE_MAX_DIMENSION;

    struct VecValues query;
    query.count = n;
    for (uint32_t i = 0; i < n; i++) query.values[i] = resp.embedding[i];

    uint32_t k = req->k;
    req->truncated = (k > VEC_SEARCH_MAX_K) ? 1 : 0;
    if (k > VEC_SEARCH_MAX_K) k = VEC_SEARCH_MAX_K;

    req->match_count = vec_index_search(req->caller_uid, req->index_name,
                                        &query, k, req->ef, req->matches);
    return 0;
}

// ─── VectorStore Interface Roadmap Phase 3: rebuild/backfill ──────────────
// Thin adapter, same shape as sys_sls_vec_index_drop() -- one line into
// vec_index_rebuild().
uint64_t sys_sls_vec_index_rebuild(struct SLSVecIndexRebuildRequest* req) {
    if (!req) return 1;
    req->status = vec_index_rebuild(req->caller_uid, req->index_name);
    return (uint64_t)req->status;
}

// ─── Gap Remediation Phase C: index enumeration ─────────────────────────────
// Mirrors sys_sls_vec_list()'s own shape exactly (vecstore.c) -- see
// vec_index.h's own comment on this syscall. Uses kernel_serial_printf()
// (the variadic one), not kernel_serial_print() -- see the header comment
// object_catalog.c's sys_sls_obj_list() fix carries for why that
// distinction matters.
void sys_sls_vec_index_list(void) {
    kernel_serial_printf(
        "\n[VEC_INDEX] Index Directory\n"
        " %-24s %-24s %-8s %-8s %s\n"
        " %-24s %-24s %-8s %-8s %s\n",
        "Name", "Collection", "Metric", "Active", "Nodes",
        "------------------------", "------------------------", "--------", "--------", "-----");

    uint32_t shown = 0;
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
        if (!vec_indexes[i].active) continue;
        struct VecIndex* idx = &vec_indexes[i];
        kernel_serial_printf(" %-24s %-24s %-8s %-8u %u\n",
                             idx->index_name, idx->collection_name,
                             idx->metric == VEC_METRIC_L2 ? "l2" : "cosine",
                             idx->active_count, idx->node_count);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no vector indexes defined)\n");
}

// ─── VectorStore Interface Roadmap follow-on: collection/index definition
// export/import. See vec_index.h's own header comment for the full "why
// this format, why this file, what's a named gap" writeup. ────────────────
static int vse_append(char* buf, uint32_t* pos, uint32_t max, const char* src) {
    uint32_t i = 0;
    while (src[i]) {
        if (*pos + 1 >= max) return 0;
        buf[(*pos)++] = src[i++];
    }
    buf[*pos] = '\0';
    return 1;
}

static int vse_append_uint(char* buf, uint32_t* pos, uint32_t max, uint32_t v) {
    char digits[12];
    int  n = 0;
    if (v == 0) digits[n++] = '0';
    while (v > 0) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    char rev[12];
    for (int i = 0; i < n; i++) rev[i] = digits[n - 1 - i];
    rev[n] = '\0';
    return vse_append(buf, pos, max, rev);
}

uint32_t vec_schema_export(uint32_t caller_uid, char* out, uint32_t max) {
    if (!out || max == 0) return 0;
    uint32_t pos = 0;
    out[0] = '\0';
    int stop = 0;

    if (!vse_append(out, &pos, max,
        "# vector-store schema export -- collection/index DEFINITIONS ONLY, no vector data\n"))
        return pos;

    for (uint32_t i = 0; i < object_catalog_count && !stop; i++) {
        if (!object_catalog[i].active || !vector_collections[i].active) continue;
        if (!catalog_check_access(caller_uid, object_catalog[i].name, PERM_READ)) continue;
        struct VecCollectionHeader* h = &vector_collections[i];

        if (!vse_append(out, &pos, max, "COLLECTION ") ||
            !vse_append(out, &pos, max, object_catalog[i].name) ||
            !vse_append(out, &pos, max, " DIM ") ||
            !vse_append_uint(out, &pos, max, h->dimension) ||
            !vse_append(out, &pos, max, "\n")) { stop = 1; break; }

        for (uint32_t k = 0; k < VEC_INDEX_MAX; k++) {
            if (!vec_indexes[k].active) continue;
            if (!vi_streq(vec_indexes[k].collection_name, object_catalog[i].name)) continue;
            if (!vse_append(out, &pos, max, "INDEX ") ||
                !vse_append(out, &pos, max, vec_indexes[k].index_name) ||
                !vse_append(out, &pos, max, " ON ") ||
                !vse_append(out, &pos, max, vec_indexes[k].collection_name) ||
                !vse_append(out, &pos, max, " METRIC ") ||
                !vse_append(out, &pos, max, vec_indexes[k].metric == VEC_METRIC_L2 ? "l2" : "cosine") ||
                !vse_append(out, &pos, max, "\n")) { stop = 1; break; }
        }
    }
    return pos;
}

// Advances past leading whitespace in `line` starting at `pos`, copies the
// next whitespace-delimited token into `outbuf` (bounded, NUL-terminated),
// and returns the new position. No quoting -- see vec_index.h's own header
// comment for why that's an honest simplification here, not an oversight.
static uint32_t vse_next_token(const char* line, uint32_t pos, char* outbuf, uint32_t outmax) {
    while (line[pos] == ' ' || line[pos] == '\t') pos++;
    uint32_t o = 0;
    while (line[pos] && line[pos] != ' ' && line[pos] != '\t' && line[pos] != '\r') {
        if (o + 1 < outmax) outbuf[o++] = line[pos];
        pos++;
    }
    outbuf[o] = '\0';
    return pos;
}

void vec_schema_import(uint32_t caller_uid, const char* text, struct VecSchemaImportResult* out) {
    if (!out) return;
    out->total = 0; out->succeeded = 0; out->failed = 0;
    for (uint32_t i = 0; i < VEC_SCHEMA_IMPORT_MAX_LINES; i++) {
        out->lines[i].ok = 0;
        out->lines[i].offset = 0;
        out->lines[i].error_msg[0] = '\0';
    }
    if (!text) return;

    uint32_t i = 0;
    while (text[i]) {
        uint32_t line_start = i;
        uint32_t j = i;
        while (text[j] && text[j] != '\n') j++;
        uint32_t line_len = j - i;

        char line[256];
        uint32_t copy_len = line_len < sizeof(line) - 1 ? line_len : (uint32_t)sizeof(line) - 1;
        for (uint32_t k = 0; k < copy_len; k++) line[k] = text[line_start + k];
        line[copy_len] = '\0';
        i = text[j] ? j + 1 : j;

        uint32_t p = 0;
        while (line[p] == ' ' || line[p] == '\t' || line[p] == '\r') p++;
        if (line[p] == '\0' || line[p] == '#') continue;   // blank or comment line -- not counted

        out->total++;
        uint32_t slot = out->total - 1;
        struct VecSchemaImportLineResult* lr =
            (slot < VEC_SCHEMA_IMPORT_MAX_LINES) ? &out->lines[slot] : 0;
        if (lr) lr->offset = line_start;

        char kw[OBJECT_NAME_LEN];
        p = vse_next_token(line, p, kw, sizeof(kw));

        int ok = 0;
        const char* err = "";

        if (vi_streq(kw, "COLLECTION")) {
            char name[OBJECT_NAME_LEN], dimkw[16], dimval[16];
            p = vse_next_token(line, p, name, sizeof(name));
            p = vse_next_token(line, p, dimkw, sizeof(dimkw));
            p = vse_next_token(line, p, dimval, sizeof(dimval));
            if (name[0] == '\0' || !vi_streq(dimkw, "DIM") || dimval[0] == '\0') {
                err = "malformed COLLECTION line (expected: COLLECTION <name> DIM <n>)";
            } else {
                uint32_t dim = 0; int bad_digit = 0;
                for (uint32_t k = 0; dimval[k]; k++) {
                    if (dimval[k] < '0' || dimval[k] > '9') { bad_digit = 1; break; }
                    dim = dim * 10 + (uint32_t)(dimval[k] - '0');
                }
                if (bad_digit) {
                    err = "malformed DIM value (not a non-negative integer)";
                } else if (vecstore_create_collection(caller_uid, name, dim) == 0) {
                    ok = 1;
                } else {
                    // VectorStore Gap Analysis §1.2 (closed): vecstore_
                    // create_collection() can now also fail with 2 (access
                    // denied) rather than only 1 (bad name/dimension/already
                    // exists) -- the message stays generic here since this
                    // parser doesn't thread the numeric return code out
                    // separately, matching this loop's own existing
                    // one-error-message-per-line-kind posture.
                    err = "vecstore_create_collection() failed (bad name/dimension, access denied, or a collection with this name already exists)";
                }
            }
        } else if (vi_streq(kw, "INDEX")) {
            char name[OBJECT_NAME_LEN], onkw[8], coll[OBJECT_NAME_LEN], metrickw[16], metricval[16];
            p = vse_next_token(line, p, name, sizeof(name));
            p = vse_next_token(line, p, onkw, sizeof(onkw));
            p = vse_next_token(line, p, coll, sizeof(coll));
            p = vse_next_token(line, p, metrickw, sizeof(metrickw));
            p = vse_next_token(line, p, metricval, sizeof(metricval));
            if (name[0] == '\0' || !vi_streq(onkw, "ON") || coll[0] == '\0' ||
                !vi_streq(metrickw, "METRIC") || metricval[0] == '\0') {
                err = "malformed INDEX line (expected: INDEX <name> ON <collection> METRIC <cosine|l2>)";
            } else {
                VecMetric metric = vi_streq(metricval, "l2") ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
                if (vec_index_create(caller_uid, name, coll, metric) == 0) {
                    ok = 1;
                } else {
                    err = "vec_index_create() failed (collection not found, permission denied, or index name already used)";
                }
            }
        } else {
            err = "unrecognized line (expected a line starting with COLLECTION or INDEX)";
        }

        if (lr) { lr->ok = (uint8_t)ok; vi_strcpy(lr->error_msg, err, sizeof(lr->error_msg)); }
        if (ok) out->succeeded++; else out->failed++;
    }
}

uint64_t sys_sls_vec_schema_export(struct SLSVecSchemaExportRequest* req) {
    if (!req) return 1;
    req->bytes_written = vec_schema_export(req->caller_uid, req->out, sizeof(req->out));
    return 0;
}

uint64_t sys_sls_vec_schema_import(struct SLSVecSchemaImportRequest* req) {
    if (!req) return 1;
    vec_schema_import(req->caller_uid, req->text, &req->result);
    return req->result.failed == 0 ? 0 : 1;
}
