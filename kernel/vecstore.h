/*
 * vecstore.h — Vector Store Roadmap Phase 1: fixed-dimension vector
 * collection storage. See docs/AeroSLS-VectorStore-Roadmap-v0.1.md §3 for
 * the full design writeup and the "Findings addendum" for what's built
 * here.
 *
 * ─── Why this is smaller than rowstore.c, not a copy of it ───────────────
 * `rowstore.c` earns its complexity from per-table, schema-derived,
 * multi-column row layouts (`RowTableLayout`, up to 16 columns, four
 * different `SLSFieldType`s). A vector collection has exactly ONE column,
 * of ONE fixed type (`float`), whose width is set once at collection-
 * creation time (the embedding dimension). This file reuses `rowstore.c`'s
 * page-pool/fixed-slot-addressing MECHANISM (a bump-allocated pool of 4 KiB
 * pages, a stable id of (page_id, slot_index), a singly-linked page chain
 * via a 4-byte next_page_id header) without any of its column/schema
 * machinery — there is no `VecTableLayout` because there is nothing to
 * derive one from beyond a single integer (dimension).
 *
 * ─── Why entries are `float` (4 bytes), not `double` (8 bytes) ───────────
 * Real embedding models (the whole reason this subsystem exists — see the
 * roadmap's own Vector Store Phase 3) produce float32 vectors, not
 * float64. Storing them as `float` here is not a "narrower on purpose"
 * space trade-off the way `ROWSTORE_STRING_LEN=64` (vs. legacy 256) was —
 * it's simply the correct, native representation for the data this
 * subsystem actually stores, confirmed against what Ollama's own
 * `/api/embeddings` response shape produces (plain JSON numbers, which
 * this codebase's own float parsing conventions already treat as
 * `double`-precision text round-tripped through `float`-precision storage
 * — acceptable loss for a similarity metric, not for exact reconstruction).
 *
 * ─── Why there is NO catalog flag (no `uses_vectorstore` on
 * `SLSObjectEntry`), unlike `uses_rowstore` ──────────────────────────────
 * `uses_rowstore` exists because MULTIPLE subsystems outside `rowstore.c`
 * itself need to ask "is this object a row-set table" — `mvcc.c`,
 * `sql_exec.c`, and (Phase 24) `object_catalog.c`'s own legacy-path guard
 * all consult it. No such external caller exists yet for vector
 * collections in this phase; the only place that needs to know "is this
 * already a vector collection" is `vecstore_create_collection()`'s own
 * idempotency check, which can and does ask ITS OWN `vector_collections[]`
 * array directly (checking `.active`), exactly the way `row_index.c`
 * (Phase 17) never touches `object_catalog[]`'s flags at all and instead
 * keeps its own independent notion of "does this table have an index."
 * Adding an unused, persisted flag to `SLSObjectEntry` now would ALSO
 * create a real, avoidable data-integrity hazard: `persist_catalog()`
 * snapshots the whole `object_catalog[]` array as raw bytes (confirmed by
 * reading `persist.c`'s own implementation), so a persisted flag would
 * survive an NVMe restore while this phase's RAM-only collection data (see
 * below) would NOT — leaving a restored kernel with a flag claiming a
 * collection exists and no data backing it. Not adding the flag at all
 * sidesteps that hazard entirely rather than requiring a restore-time
 * reconciliation step this phase doesn't otherwise need. If a future phase
 * (SQL-level vector search, most likely) needs an external caller to ask
 * this question, that's the point to revisit this decision — the same
 * "decide the real design question when a real caller needs it" posture
 * Phase 22 used to justify adding `mvcc_txn_is_active()` only once
 * `sql_exec.c`'s own test actually needed it.
 *
 * ─── Why this phase is RAM-only, with no NVMe persistence ────────────────
 * A real, explicit scope cut, not an oversight: `row_index.c` (Phase 17)
 * already established the precedent of a RAM-only first cut for a new
 * storage-adjacent subsystem in this codebase ("no persistence this phase"
 * — see its own header comment). Adding NVMe paging here would mean a new
 * `PERSIST_MAGIC_VECSTORE` region, a new reserved LBA range (mirroring
 * `ROWSTORE_LBA_BASE`), and lazy-load/eager-flush plumbing identical in
 * shape to `rowstore.c`'s own — real, mechanical work that this first
 * phase defers rather than silently skips. `vecstore_init()` simply zeroes
 * everything at boot every time, the same as `row_index_init()` does.
 *
 * ─── Registration: object must already exist via `sys_sls_valloc`, no
 * schema step ───────────────────────────────────────────────────────────
 * Mirrors `rowstore_create_table()`'s own precondition that the catalog
 * object already exists — but does NOT require `sys_sls_schema_set()`
 * first (unlike `rowstore_create_table()`, which derives its column layout
 * FROM the schema). A vector collection's only "schema" is its dimension,
 * passed directly to `vecstore_create_collection()` as an argument — there
 * is no per-column concept to define via the schema API, so requiring one
 * would be forcing a fake, meaningless schema-definition step onto a
 * caller for no real reason.
 */
#ifndef VECSTORE_H
#define VECSTORE_H

#include <stdint.h>
#include "object_catalog.h"
#include "../net/ollama_client.h"   // Phase 4 only -- see the §6 syscall-surface
                                      // comment below for why this is the one
                                      // place vecstore.c/.h is allowed to know
                                      // ollama_client.h exists

// ─── Limits ─────────────────────────────────────────────────────────────────
#define VECSTORE_MAX_COLLECTIONS  CATALOG_MAX_OBJECTS  // index-aligned with object_catalog[]
#define VECSTORE_MAX_DIMENSION    2048   // generous cap covering real embedding models (see
                                          // Vector Store Roadmap Phase 3's own model survey --
                                          // nomic-embed-text=768, mxbai-embed-large=1024,
                                          // all-minilm=384, bge-large=1024 -- revisit only if a
                                          // future model genuinely needs more, not speculatively
#define VECSTORE_PAGE_SIZE        4096
#define VECSTORE_MAX_PAGES        65536   // RAM-only (see header comment) -- 256 MiB ceiling at
                                           // VECSTORE_PAGE_SIZE, a real, stated cap not a silent one
#define VECSTORE_INVALID_PAGE     0xFFFFFFFFu

// Max bytes one serialized entry can occupy: 1 tombstone byte + 8-byte
// external_id + up to VECSTORE_MAX_DIMENSION floats. Used to size a stack
// scratch buffer in vecstore.c -- never persisted directly (this phase
// persists nothing -- see header comment).
#define VECSTORE_MAX_ENTRY_BYTES (1 + 8 + VECSTORE_MAX_DIMENSION * 4)

// ─── Vector values at the API boundary ─────────────────────────────────────
// Unlike RowValues (text in, text out, matching the legacy KV path's own
// string-based convention), vectors are numeric at the boundary too --
// there is no meaningful "text representation" of an embedding component
// worth forcing a parse/format round-trip on, and Phase 2's distance
// functions need raw floats directly.
struct VecValues {
    uint32_t count;                           // must equal the collection's dimension
    float    values[VECSTORE_MAX_DIMENSION];
};

// A vector entry's stable address: which page, which fixed-width slot
// within it -- same shape as rowstore.h's own struct RowId, deliberately
// (see header comment on why the ADDRESSING mechanism is reused even
// though the schema machinery is not).
struct VecId {
    uint32_t page_id;
    uint32_t slot_index;
};

// ─── Per-collection header — index-aligned with object_catalog[], same
// idiom as table_headers[]. Small and fixed-size, but NOT persisted this
// phase (see header comment). ────────────────────────────────────────────
struct VecCollectionHeader {
    uint64_t object_id;
    uint8_t  active;             // this catalog slot is a vector collection
    uint32_t dimension;          // fixed at vecstore_create_collection() time
    uint32_t entry_width;        // 1 (tombstone) + 8 (external_id) + dimension*4
    uint32_t entries_per_page;   // (VECSTORE_PAGE_SIZE - 4) / entry_width
    uint32_t entry_count;        // active (non-tombstoned) entries
    uint32_t page_count;         // pages currently allocated to this collection
    uint32_t first_page_id;      // VECSTORE_INVALID_PAGE = none yet
    uint32_t last_page_id;       // append target
    uint32_t entries_in_last_page;
};

extern struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];

// The page-pool bump allocator's cursor -- own pool, separate from
// rowstore.c's rowstore_next_free_page_id (two independent subsystems,
// two independent pools, matching the roadmap's own "new parallel
// subsystem, not an extension" principle all the way down to allocator
// state, not just the top-level API).
extern uint32_t vecstore_next_free_page_id;

// ─── Lifecycle ────────────────────────────────────────────────────────────
// Zeroes vector_collections[]/the in-RAM page-pointer cache and resets the
// bump allocator. Called once at boot (kernel.c). No restore step --
// RAM-only this phase.
void vecstore_init(void);

// Enables vector-collection storage for an already-valloc'd catalog object
// (no schema step required -- see header comment). dimension must be in
// [1, VECSTORE_MAX_DIMENSION]. Returns 0 on success; 1 if the object
// doesn't exist, already has a vector collection, or dimension is out of
// range.
int vecstore_create_collection(const char* collection_name, uint32_t dimension);

// Vector CRUD. Every call resolves collection_name to a catalog entry and
// gates on catalog_check_access() first (PERM_READ for get/scan,
// PERM_WRITE for insert/delete) -- same posture rowstore.h's own header
// comment already established for row-set tables, reused here rather than
// re-argued.
//
// Return codes, shared across all three:
//   0 = success
//   1 = collection not found / not a vector collection
//   2 = permission denied
//   3 = entry not found (bad/stale VecId, or already deleted)
//   4 = values->count doesn't match the collection's dimension
//   5 = page pool exhausted (insert only)
//
// external_id is caller-supplied and opaque to this subsystem -- the
// correlation key a caller uses to join a search result back to a
// relational row (Vector Store Roadmap Phase 5). Not validated or
// interpreted here in any way -- any uint64_t is accepted, including 0
// and duplicate values across different entries (uniqueness, if wanted,
// is the caller's responsibility, matching this whole roadmap's "first
// cut, not the general case" posture -- a real constraint layer here,
// mirroring row_constraint.c, is future work if it turns out to matter).
int vecstore_insert(uint32_t caller_uid, const char* collection_name,
                    uint64_t external_id, const struct VecValues* values,
                    struct VecId* out_id);
int vecstore_get(uint32_t caller_uid, const char* collection_name,
                 struct VecId id, uint64_t* out_external_id, struct VecValues* out);
int vecstore_delete(uint32_t caller_uid, const char* collection_name, struct VecId id);

// Full-collection scan in physical (page, then slot) order, invoking cb()
// for every active (non-tombstoned) entry -- the primitive Phase 2's
// brute-force similarity search is built on, mirroring
// rowstore_table_scan()'s own role for predicate.c. Returns the number of
// entries visited (0 if the collection doesn't exist, isn't a vector
// collection, or access is denied).
typedef void (*VecScanCb)(struct VecId id, uint64_t external_id,
                          const struct VecValues* values, void* ctx);
uint32_t vecstore_collection_scan(uint32_t caller_uid, const char* collection_name,
                                  VecScanCb cb, void* ctx);

// ─── Vector Store Roadmap Phase 2: brute-force similarity search ─────────
//
// ─── Metric choice: both return a "smaller is more similar" distance ─────
// Cosine SIMILARITY is naturally "higher is more similar" ([-1, 1], 1 =
// identical direction), the opposite sense from L2 (0 = identical, larger
// = farther). Rather than making top-K selection metric-aware (a min-heap
// for one metric, a max-heap for the other), VEC_METRIC_COSINE reports
// `1.0f - cosine_similarity` as its distance -- 0 = identical direction, 2
// = opposite direction -- so BOTH metrics share one "smallest distance
// wins" selection rule. If either vector has zero magnitude (undefined
// direction), cosine distance is reported as a neutral 1.0f rather than
// crashing on a divide-by-zero or arbitrarily favoring/penalizing it.
typedef enum {
    VEC_METRIC_COSINE = 0,
    VEC_METRIC_L2      = 1,
} VecMetric;

struct VecMatch {
    uint64_t     external_id;
    struct VecId id;
    float        distance;   // metric-dependent; always "smaller = more similar" -- see above
};

// Brute-force top-K nearest-neighbor search: scans every active entry in
// the collection (built directly on vecstore_collection_scan() -- same
// catalog_check_access() gate, no parallel permission path, matching this
// codebase's established "reuse the one real choke point" convention) and
// keeps the k closest matches, ascending by distance (out[0] is the
// closest). Selection is a bounded insertion-sorted array of size k, not a
// heap -- O(n*k) rather than O(n*log k), a deliberate simplicity-over-
// asymptotic-optimality choice for k values realistic in this first cut
// (tens, not thousands); see the roadmap's own findings addendum for why.
//
// Returns the number of matches actually written to out[] (0..k -- fewer
// than k if the collection has fewer than k active entries; 0 if the
// collection doesn't found/isn't a vector collection/access is denied/
// query->count doesn't match the collection's dimension -- callers that
// need to distinguish "genuinely empty result" from "search couldn't run
// at all" should check those preconditions themselves first, the same
// contract vecstore_collection_scan()/rowstore_table_scan() already have).
// out must point to at least k struct VecMatch slots.
uint32_t vecstore_search(uint32_t caller_uid, const char* collection_name,
                         const struct VecValues* query, VecMetric metric,
                         uint32_t k, struct VecMatch* out);

// ─── Vector Store Roadmap Phase 6: shared distance/top-K primitives ──────
// Exported (no longer `static` in vecstore.c) specifically so
// kernel/vec_index.c (Phase 6's HNSW index) reuses this EXACT, already
// host-tested math rather than an independently hand-rolled second copy.
// This is a deliberate, narrow exception to this codebase's usual "each
// file keeps its own small hand-rolled helpers" convention (see e.g.
// net/ollama_client.c's own oc_* string helpers, kept separate from
// net/inference.c's near-identical ones on purpose) -- that convention
// exists because small differences in string/buffer helpers don't matter.
// Small differences in distance math or top-K selection WOULD matter here:
// Phase 6's own verification plan compares approximate (HNSW) search
// results against exact (Phase 2 vecstore_search()) results to measure
// recall, and that comparison is only meaningful if both paths agree on
// what "distance" and "closest" mean bit-for-bit. Reuse, not duplication,
// is what makes that comparison trustworthy.
float vs_sqrtf(float x);
float vs_dot(const struct VecValues* a, const struct VecValues* b, uint32_t n);
float vs_distance_cosine(const struct VecValues* a, const struct VecValues* b, uint32_t n);
float vs_distance_l2(const struct VecValues* a, const struct VecValues* b, uint32_t n);
void  vs_topk_insert(struct VecMatch* out, uint32_t* found, uint32_t k, struct VecMatch cand);

// ─── Vector Store Roadmap Phase 4: syscall surface ("make it live") ──────
//
// ─── Why a 4th syscall (VEC_CREATE) beyond this phase's own three named
// deliverables (insert/embed-insert/search) ───────────────────────────────
// The roadmap's own §6 scope bullet named only insert, embed-insert, and
// search as this phase's syscalls -- written before implementation noticed
// a real reachability gap: vecstore_create_collection() (Phase 1) had NO
// syscall wrapper of its own, and per this header's own "Registration"
// comment above, every insert requires a collection to already exist.
// Without exposing vecstore_create_collection() too, the three "live"
// syscalls this phase adds would be live in name only -- reachable from
// dispatch, but with no reachable way to ever get a collection into a
// state where insert/search succeed. This is the same "denial looks like
// absence" failure class named and fixed four times across the RDBMS
// roadmap (Phase 19 WHERE-column validation, Phase 22 mvcc bad-txn-id
// ambiguity, Phase 23 swallowed constraint violations, Phase 24 design
// notes) -- applied here to reachability rather than error handling, but
// the same real mistake to avoid. SYS_SLS_VEC_CREATE (224) closes it. Note
// this is also, incidentally, a gap the RDBMS roadmap itself still has and
// has never closed: rowstore_create_table() has no syscall wrapper
// anywhere in this codebase either (confirmed by grep -- it's reachable
// only from host tests), and sql_exec.c's grammar has no CREATE TABLE
// statement. That gap is out of scope to fix here (it belongs to the
// RDBMS roadmap, not this one) but is worth naming rather than silently
// repeating without comment.
//
// ─── uid convention ────────────────────────────────────────────────────────
// caller_uid travels inside each request struct, exactly matching
// SYS_SLS_SQL_EXECUTE's own SLSSqlRequest.caller_uid convention (itself
// modeled on SLSVallocRequest.owner_uid) -- do_syscall()'s single opaque
// void* arg has no uid context of its own to supply, so the shell (the
// only caller of do_syscall() in this codebase) stamps current_session_uid
// into the struct before the call, same as every other syscall that needs
// a uid.
#define SYS_SLS_VEC_CREATE       221
#define SYS_SLS_VEC_INSERT       222
#define SYS_SLS_VEC_EMBED_INSERT 223
#define SYS_SLS_VEC_SEARCH       224

// A fixed cap on how many matches one search syscall can return, embedded
// directly in the request struct so the whole result travels back across
// the void* arg boundary in one allocation -- mirrors SqlResult's own
// CURSOR_MAX_ROWSET_ROWS-plus-`truncated`-flag shape (sql_exec.h). 64 is
// generous for this first cut's realistic k values (see Phase 2's own
// "tens, not thousands" framing) without needing a syscall-level unbounded
// result mechanism (cursors, in the SQL engine's case) that this roadmap's
// Phase 4 scope never asked for.
#define VEC_SEARCH_MAX_K 64

struct SLSVecCreateRequest {
    uint32_t caller_uid;
    char     collection_name[OBJECT_NAME_LEN];   // must already exist via sys_sls_valloc
    uint32_t dimension;
    int      status;   // vecstore_create_collection()'s own return code (0 = success)
};

struct SLSVecInsertRequest {
    uint32_t          caller_uid;
    char              collection_name[OBJECT_NAME_LEN];
    uint64_t          external_id;
    struct VecValues  values;
    struct VecId      out_id;   // filled in on success
    int               status;   // vecstore_insert()'s own return code (0 = success)
};

// Embeds ollama_req.prompt via ollama_embed() first, then stores the
// resulting embedding via vecstore_insert() -- the one place in this whole
// roadmap where vecstore.c and net/ollama_client.c are deliberately joined
// (see this header's own top-of-file "new parallel subsystem" principle,
// and the Phase 3 findings addendum's "no shared code" statement -- this
// struct, and the thin adapter that uses it, are the intentional exception
// Phase 4's own scope exists to add, not a violation of that principle).
struct SLSVecEmbedInsertRequest {
    uint32_t                  caller_uid;
    char                      collection_name[OBJECT_NAME_LEN];
    uint64_t                  external_id;
    struct OllamaEmbedRequest ollama_req;    // endpoint_ip/port/model/prompt -- the text to embed
    struct VecId              out_id;        // filled in only if ollama_status == 0 && insert_status == 0
    int                       ollama_status;  // ollama_embed()'s own return code; nonzero means insert was never attempted
    int                       insert_status;  // vecstore_insert()'s own return code; only meaningful if ollama_status == 0
};

struct SLSVecSearchRequest {
    uint32_t         caller_uid;
    char              collection_name[OBJECT_NAME_LEN];
    struct VecValues  query;
    VecMetric         metric;
    uint32_t          k;                          // capped to VEC_SEARCH_MAX_K internally
    struct VecMatch   matches[VEC_SEARCH_MAX_K];   // filled in by the call
    uint32_t          match_count;                 // filled in by the call
    uint8_t           truncated;                   // 1 if the caller's k exceeded VEC_SEARCH_MAX_K
};

// Thin adapters, one line of real logic each, matching sys_sls_sql_execute()'s
// own shape in sql_exec.c -- unpack the request struct, call straight into
// the already-built (and already host-tested) engine function(s), return a
// 0/nonzero status. Defined in vecstore.c (see that file for why the embed-
// insert adapter is the one place vecstore.c includes ollama_client.h).
uint64_t sys_sls_vec_create(struct SLSVecCreateRequest* req);
uint64_t sys_sls_vec_insert(struct SLSVecInsertRequest* req);
uint64_t sys_sls_vec_embed_insert(struct SLSVecEmbedInsertRequest* req);
uint64_t sys_sls_vec_search(struct SLSVecSearchRequest* req);

#endif /* VECSTORE_H */
