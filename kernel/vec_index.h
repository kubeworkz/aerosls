/*
 * vec_index.h — Vector Store Roadmap Phase 6: approximate nearest-neighbor
 * indexing (HNSW). See docs/AeroSLS-VectorStore-Roadmap-v0.1.md §8 for the
 * full design writeup and the "Findings addendum" for what's built here.
 *
 * ─── This phase started ahead of its own stated gate — named, not hidden ──
 * §8's own text says: "Revisit only once brute-force search (Phase 2) is
 * demonstrably too slow for a real workload this project actually runs —
 * not before." No such workload exists yet; brute-force search has not
 * been shown to be a real bottleneck anywhere in this project. This phase
 * was started anyway on an explicit, direct user request ("let's go for
 * phase 6"), overriding that gate deliberately rather than by oversight —
 * worth recording here so a future reader doesn't mistake this for the
 * roadmap's own stated trigger condition having been met. Framed as an
 * exploratory/architectural phase (HNSW's graph-of-neighbors shape is a
 * close fit for AeroSLS's own pointer/object-graph-friendly SLS design
 * philosophy — the roadmap's own original motivating observation), not a
 * performance fix for a real, measured problem.
 *
 * ─── A new, third parallel structure — same relationship to vecstore.c
 * that row_index.c has to rowstore.c ────────────────────────────────────
 * vec_index.c does NOT store vector values itself. Every distance
 * computation re-fetches the vector via vecstore_get() (a direct
 * (page_id, slot_index) read, not a scan — see vecstore.h's own VecId
 * comment), rather than caching a second copy of the data locally. This
 * is a deliberate reuse-over-raw-speed choice: real HNSW implementations
 * typically keep vectors resident in the graph's own memory for
 * traversal speed, but this project has repeatedly favored "one source of
 * truth, reuse already-tested storage" over that (Phase 2's own O(n*k)
 * top-K choice over a heap is the most directly analogous precedent) —
 * vecstore.c remains the sole owner of vector VALUES, exactly as
 * rowstore.c remains the sole owner of row VALUES with row_index.c only
 * ever storing keys/RowIds, never a second copy of a row.
 *
 * ─── A single shared, bump-allocated node pool — matching row_index.h's
 * own BTreeNode precedent exactly, not a naive per-index array ─────────
 * `vec_index_nodes[VEC_INDEX_MAX_NODES]` is ONE flat pool shared across
 * every defined VecIndex (mirrors row_index.h's own `btree_nodes[
 * BTREE_MAX_NODES]`), not a `VEC_INDEX_MAX_ELEMENTS`-sized array eagerly
 * reserved inside every single VecIndex struct — the latter would waste
 * multiple megabytes of BSS per UNUSED index slot. No reclaim on delete
 * (same "no reclaim in first cut" posture row_index.h's own header
 * comment already names for rowstore.c's page pool and frame_pool.c's
 * quota system). Graph edges (`neighbors[layer][i]`) are pool-wide node
 * indices; `index_id` on each node exists only so vec_index_notify_delete
 * can find "the node for VecId X belonging to index Y" via a full pool
 * scan — ordinary graph TRAVERSAL never needs to check `index_id` at all,
 * since edges are only ever created between nodes of the SAME index by
 * construction (connectivity never crosses between different indexes
 * sharing the pool).
 *
 * ─── Auto-maintenance, mirroring row_index.c's real precedent exactly ────
 * vecstore_insert()/vecstore_delete() call vec_index_notify_insert()/
 * _delete() automatically after a successful mutation (see vecstore.c) —
 * the SAME "base storage layer calls the optional index layer
 * unconditionally; the index layer decides whether there's anything to
 * do" pattern rowstore.c already established for row_index.c
 * (row_index_notify_insert() etc.). This is a deliberate choice, not
 * default behavior copied blindly: an approximate index that silently
 * falls out of sync with its own source data is worse than no index at
 * all — it would return confidently WRONG results while looking
 * healthy, a failure mode this project hasn't named before (every prior
 * "denial looks like absence" case at least failed by omission, not by
 * quiet fabrication) — auto-maintenance from day one is how this phase
 * avoids ever creating that failure mode, rather than requiring every
 * future vecstore_insert() caller to remember a second call.
 *
 * ─── Real, named simplifications versus the textbook HNSW paper ─────────
 * 1. Layer assignment: the paper's continuous formula (floor(-ln(U) * mL),
 *    mL = 1/ln(M)) needs a natural log this freestanding kernel doesn't
 *    have anywhere (confirmed by the same "no libc math" audit Phase 2's
 *    own vs_sqrtf() comment already names). vi_random_layer() below uses
 *    the equivalent discrete construction instead: flip an M-sided
 *    "coin" (via a hand-rolled xorshift32 PRNG, since no PRNG existed
 *    anywhere in this kernel before this phase either) and climb one
 *    layer per success, capped at VEC_INDEX_MAX_LAYERS-1 — this produces
 *    the identical geometric P(layer >= k) = (1/M)^k decay the paper's
 *    continuous formula targets, via integer arithmetic only.
 * 2. Neighbor selection: simple "keep the M/M0 closest," not the paper's
 *    heuristic-with-diversification selectNeighbors algorithm — a real,
 *    common simplification, applied in vi_connect() (see vec_index.c).
 * 3. Deletion is a tombstone, not a real removal with neighbor re-linking
 *    — mirrors row_index.c's own first-cut B-tree deletion stance. A
 *    MORE SEVERE limitation than row_index.c's own precedent, though, and
 *    named honestly rather than assumed equivalent: a B-tree's routing
 *    keys are independent of its leaves' duplicate-tombstone lists, so
 *    deleting a row never affects the tree's ability to route to OTHER
 *    rows. An HNSW node's edges ARE the navigation structure itself — a
 *    node whose underlying vecstore entry was deleted becomes an
 *    effectively infinite-distance dead end during traversal (vecstore_
 *    get() fails for a deleted VecId, so vi_distance() can no longer
 *    score it), which can fragment the graph or leave dead-end paths that
 *    degrade recall for OTHER, unrelated queries that would have routed
 *    through that node — not just a "can't find the deleted thing"
 *    limitation. The Findings addendum states what this phase's own host
 *    test could and couldn't confirm about that risk empirically.
 * 4. Neighbor arrays are sized VEC_INDEX_M0 (the layer-0 worst case) at
 *    EVERY layer, even though layers above 0 only ever use VEC_INDEX_M —
 *    wastes some space for simpler fixed-offset addressing, matching
 *    row_index.h's own BTreeNode precedent ("simple fixed array over a
 *    jagged one").
 * 5. RAM-only, no persistence — matching Phase 1's own precedent and
 *    row_index.c's own identical stance for exactly the same reason (a
 *    derived, rebuildable-from-source-data structure).
 * 6. No syscall/shell surface this phase — a deliberate scope decision
 *    matching Phase 5's own identical call: this is a plain, fully
 *    host-testable kernel structure with no reachability blocker forcing
 *    a live surface (unlike Phase 4's SYS_SLS_VEC_CREATE, which fixed a
 *    real, hard gap). Revisit only when a real caller needs it.
 * 7. vec_index_create() does NOT backfill an already-populated collection
 *    — only vectors inserted AFTER the index is created are indexed (via
 *    the auto-maintenance hook). A real, named scope cut, not an
 *    oversight: this phase's own verification builds the index
 *    incrementally from an empty collection, the more common real-world
 *    case for an index defined at collection-creation time; backfilling
 *    an existing collection is straightforward to add later (a
 *    vecstore_collection_scan() over the collection, feeding each entry
 *    through the same insert path) if a real caller needs it.
 */
#ifndef VEC_INDEX_H
#define VEC_INDEX_H

#include <stdint.h>
#include "vecstore.h"

// ─── Limits ─────────────────────────────────────────────────────────────
#define VEC_INDEX_MAX              16     // max simultaneously-defined indexes (mirrors ROW_INDEX_MAX)
#define VEC_INDEX_MAX_NODES        4096   // shared node pool across every defined index (mirrors
                                           // row_index.h's own BTREE_MAX_NODES precedent exactly)
#define VEC_INDEX_MAX_LAYERS       8      // generous headroom: at M=16, expected top layer for even
                                           // VEC_INDEX_MAX_NODES nodes concentrated on one index is
                                           // ~log_16(4096) ~= 3
#define VEC_INDEX_M                16     // max neighbors per node, per layer, for layers > 0
#define VEC_INDEX_M0               32     // max neighbors per node at layer 0 (2*M, standard HNSW ratio)
#define VEC_INDEX_EF_CONSTRUCTION  64     // candidate list size explored while inserting
#define VEC_INDEX_MAX_EF           256    // upper bound on caller-supplied search-time ef
#define VEC_INDEX_INVALID          0xFFFFFFFFu

// ─── One graph node — corresponds 1:1 with one already-stored vecstore
// entry (vec_id), living in the ONE shared pool below (see header
// comment). Holds NO vector values of its own -- every distance
// computation re-fetches via vecstore_get(). ────────────────────────────
struct VecIndexNode {
    uint32_t     index_id;                                        // which vec_indexes[] slot owns this node
    struct VecId vec_id;
    uint64_t     external_id;                                     // cached -- avoids a vecstore_get() just to report it in results
    uint32_t     top_layer;                                       // this node exists on layers [0, top_layer]
    uint8_t      active;                                          // 0 = tombstoned (see header comment)
    uint32_t     neighbor_count[VEC_INDEX_MAX_LAYERS];
    uint32_t     neighbors[VEC_INDEX_MAX_LAYERS][VEC_INDEX_M0];    // pool-wide node indices, not VecIds
};

extern struct VecIndexNode vec_index_nodes[VEC_INDEX_MAX_NODES];
extern uint32_t            vec_index_next_free_node;

// ─── One defined index ─────────────────────────────────────────────────
struct VecIndex {
    char      index_name[OBJECT_NAME_LEN];
    char      collection_name[OBJECT_NAME_LEN];
    uint32_t  dimension;
    VecMetric metric;           // fixed at creation -- see header comment: search is only meaningful
                                 // under the SAME metric the graph's edges were built with
    uint32_t  entry_point;      // pool-wide node index of the current entry point; VEC_INDEX_INVALID if empty
    uint32_t  top_layer;        // highest layer currently in use across all of THIS index's nodes
    uint32_t  node_count;       // nodes belonging to this index ever inserted (includes tombstoned)
    uint32_t  active_count;     // active (non-tombstoned) node count for this index
    uint8_t   active;           // this VecIndex slot itself is defined
};

extern struct VecIndex vec_indexes[VEC_INDEX_MAX];

// ─── Lifecycle ────────────────────────────────────────────────────────────
void vec_index_init(void);

// Creates a new, empty HNSW index over an already-created vector
// collection (vecstore_create_collection() already called). Does NOT
// backfill existing entries -- see header comment point 7.
// Returns 0 on success. 1 = collection not found, 2 = permission denied
// (PERM_READ on collection_name), 3 = index_name already used or
// VEC_INDEX_MAX exhausted.
int vec_index_create(uint32_t caller_uid, const char* index_name,
                     const char* collection_name, VecMetric metric);

// ─── Auto-maintenance hooks -- called by vecstore.c's vecstore_insert()/
// vecstore_delete() after a successful mutation on ANY collection (see
// header comment). Self-guarding: a fast linear scan over vec_indexes[]
// for any index whose collection_name matches; a true no-op if none
// exists. NOT separately access-gated -- the vecstore_insert()/
// vecstore_delete() call that triggered this already passed
// catalog_check_access() itself, matching row_index_notify_insert()'s own
// identical posture. ─────────────────────────────────────────────────────
void vec_index_notify_insert(uint32_t caller_uid, const char* collection_name,
                             struct VecId id, uint64_t external_id,
                             const struct VecValues* values);
void vec_index_notify_delete(const char* collection_name, struct VecId id);

// ─── Query ────────────────────────────────────────────────────────────────
// Approximate top-K nearest-neighbor search. ef controls the candidate
// list size explored at layer 0 (raised to k if smaller; capped
// internally at VEC_INDEX_MAX_EF) -- higher ef trades search time for
// higher recall against vecstore_search()'s exact result. Returns the
// number of matches written to out[] (0..k). Returns 0 if the index
// doesn't exist, the caller lacks PERM_READ on the underlying collection,
// the index is empty, or query->count doesn't match the index's
// dimension -- same "0 is ambiguous between empty and couldn't-run"
// contract vecstore_search()/vecstore_collection_scan() already have (see
// those functions' own header comments on why this ambiguity is a
// deliberate, accepted, consistent tradeoff, not repeated here without
// comment). out must point to at least k struct VecMatch slots.
uint32_t vec_index_search(uint32_t caller_uid, const char* index_name,
                          const struct VecValues* query, uint32_t k, uint32_t ef,
                          struct VecMatch* out);

// ─── Gap Remediation Phase C: live reachability ────────────────────────────
// Point 6 of this header's own simplifications list named "no syscall/shell
// surface this phase" as a deliberate cut, revisit only when a real caller
// needs it -- a frontend wanting to demonstrate the HNSW index is that real
// caller. caller_uid travels inside each request struct, matching every
// other Phase 22+/Vector-Store-Phase-4+ syscall's own convention.
#define SYS_SLS_VEC_INDEX_CREATE 227
#define SYS_SLS_VEC_INDEX_SEARCH 228

struct SLSVecIndexCreateRequest {
    uint32_t  caller_uid;
    char      index_name[OBJECT_NAME_LEN];
    char      collection_name[OBJECT_NAME_LEN];   // must already be a vector collection
    VecMetric metric;
    int       status;   // vec_index_create()'s own return code (0 = success)
};

struct SLSVecIndexSearchRequest {
    uint32_t         caller_uid;
    char             index_name[OBJECT_NAME_LEN];
    struct VecValues query;
    uint32_t         k;                            // capped to VEC_SEARCH_MAX_K internally
    uint32_t         ef;
    struct VecMatch  matches[VEC_SEARCH_MAX_K];     // filled in by the call
    uint32_t         match_count;                    // filled in by the call
    uint8_t          truncated;                      // 1 if the caller's k exceeded VEC_SEARCH_MAX_K
};

// Thin adapters, matching sys_sls_vec_create()/sys_sls_vec_search()'s own
// shape exactly (vecstore.c) -- unpack the request struct, call straight
// into the already-built, already-host-tested engine function, return a
// 0/nonzero status.
uint64_t sys_sls_vec_index_create(struct SLSVecIndexCreateRequest* req);
uint64_t sys_sls_vec_index_search(struct SLSVecIndexSearchRequest* req);

// ─── Gap Remediation Phase C: index enumeration ───────────────────────────
// Mirrors SYS_SLS_VEC_LIST's own shape exactly (vecstore.h) -- before this,
// a caller had to already know an index's name; there was no way to ask
// "what HNSW indexes exist."
#define SYS_SLS_VEC_INDEX_LIST 230
void sys_sls_vec_index_list(void);

#endif /* VEC_INDEX_H */
