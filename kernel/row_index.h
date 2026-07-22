/*
 * row_index.h — Phase 17 (relational layer): B-tree indexing for row-set
 * tables (kernel/rowstore.h, Phase 16). See docs/AeroSLS-RDBMS-Roadmap-v0.1.md
 * §4 for the scoped design and the "Findings addendum" for what's built here.
 *
 * This is a NEW capability alongside index_mgr.c, not a replacement of it.
 * index_mgr.c's sorted-array-with-O(n)-shifts remains exactly as it was for
 * every legacy single-record table (object_records[] path) — those callers
 * (constraint.c's REFERENCE checks, cursor.c's order_index, mqt.c) are
 * unaffected and unmodified by this phase, matching Phase 16's own posture
 * of adding a parallel system for opted-in tables rather than touching the
 * legacy path. Row-set tables (rowstore.h) get real B-tree indexes instead;
 * legacy tables keep the array index. See the Findings addendum for the
 * explicit per-caller audit confirming this split is safe.
 *
 * ─── Design summary (see roadmap doc for the full writeup) ──────────────────
 * A fixed-fanout B+-tree (BTREE_ORDER=4 — small on purpose: makes multi-level
 * splitting easy to force and verify in a host test, matching this project's
 * "small first cut, not the tuned/general case" posture; real systems tune
 * fanout to a disk page/cache-line size, which is meaningless for this
 * first-cut, unpersisted, in-RAM structure). Nodes live in one shared, fixed,
 * array-backed pool (bump allocator, no reclaim — same "no reclaim in first
 * cut" posture as rowstore.c's page pool and frame_pool.c's quota system).
 *
 * Keys are a canonical, fixed-width, memcmp-comparable byte encoding derived
 * from the column's typed value (UINT64: 8-byte big-endian; FLOAT: 8-byte
 * big-endian with the standard sign/bit-flip transform that makes IEEE-754
 * doubles memcmp-orderable; BOOL: 1 byte; STRING: up to 64 bytes, zero-padded
 * — zero-padding at the end preserves correct lexicographic order for text
 * without embedded NULs). All keys for one index share the same encoded
 * width; comparisons always memcmp the full BTREE_MAX_KEY_BYTES buffer since
 * both operands are zero-padded identically, so a uniform-length compare is
 * always correct without tracking per-key lengths.
 *
 * Duplicate values: leaf entries are keyed by DISTINCT value (not one array
 * slot per row) — each leaf key slot holds a small fixed-size array of up to
 * BTREE_MAX_DUPES_PER_KEY row_ids sharing that value. This is a deliberate
 * design choice, not an oversight: the alternative (one array slot per row,
 * duplicates as plain repeated keys) makes splitting correctness-critical
 * (a run of equal keys straddling a split boundary can silently strand
 * entries on the wrong side of later lookups) and was rejected specifically
 * to avoid that silent-miss failure mode. The chosen design instead fails
 * LOUDLY and narrowly: inserting a value's 17th duplicate onto one index
 * returns/logs a clear "index full for this key" outcome rather than being
 * silently dropped or silently unfindable — "denial looks like absence" is
 * exactly the failure mode this design avoids. Low-cardinality columns
 * (e.g. a BOOL column on a large table) are consequently a poor indexing
 * candidate under this cap, same as in most real query planners, which
 * generally skip indexes on low-selectivity columns anyway.
 *
 * Deletion in this first cut is a tombstone within a key's duplicate array —
 * no leaf/tree rebalancing or merging on delete (matching every other
 * "no reclaim in first cut" precedent in this project). Space is not
 * reclaimed; correctness (an entry, once tombstoned, is never returned by
 * lookup/range-scan again) is unaffected.
 *
 * Persistence: explicitly OUT OF SCOPE this phase. Indexes are a derived,
 * in-RAM performance structure over already-persisted row data
 * (kernel/rowstore.c persists the rows themselves); rebuilding an index from
 * its table via row_index_create() is always correct and cheap enough at
 * this project's scale. A future phase can add NVMe persistence if rebuild
 * cost ever becomes a real problem — not assumed here.
 */
#ifndef ROW_INDEX_H
#define ROW_INDEX_H

#include <stdint.h>
#include "rowstore.h"

// ─── Limits ─────────────────────────────────────────────────────────────────
#define ROW_INDEX_MAX            16       // max defined B-tree indexes (mirrors index_mgr.c's INDEX_MAX)
#define BTREE_ORDER               4       // max children per internal node; max distinct keys per leaf
                                           // before a split (small on purpose -- see header comment)
#define BTREE_MAX_KEY_BYTES       64      // matches ROWSTORE_STRING_LEN, the widest indexable column type
#define BTREE_MAX_DUPES_PER_KEY   16      // max row_ids sharing one distinct indexed value, per index
#define BTREE_MAX_NODES           16384   // shared node pool across every row-set index (first cut)
#define BTREE_INVALID_NODE        0xFFFFFFFFu

// ─── One B-tree node — array-backed, index-addressed (no pointers), matching
// this codebase's established idiom (frame_pool.c's frames[], rowstore.c's
// row_pages[]). A node is either a leaf (is_leaf=1: keys[]/ids[]/id_count[]
// meaningful, children[] unused) or internal (is_leaf=0: keys[] are routing
// separators, children[] meaningful, ids[]/id_count[] unused). ────────────────
struct BTreeNode {
    uint8_t      is_leaf;
    uint8_t      active;                                  // this pool slot is in use
    uint32_t     key_count;
    uint8_t      keys[BTREE_ORDER][BTREE_MAX_KEY_BYTES];
    // internal-node fields (key_count keys, key_count+1 children)
    uint32_t     children[BTREE_ORDER + 1];
    // leaf-node fields (key_count distinct keys, each with its own duplicate list)
    struct RowId ids[BTREE_ORDER][BTREE_MAX_DUPES_PER_KEY];
    uint8_t      id_active[BTREE_ORDER][BTREE_MAX_DUPES_PER_KEY];
    uint32_t     id_count[BTREE_ORDER];
    uint8_t      key_capped[BTREE_ORDER];                  // Phase 25: 1 if this key's duplicate
                                                             // list ever hit BTREE_MAX_DUPES_PER_KEY
                                                             // and a later insert for the same key
                                                             // was silently dropped -- see
                                                             // row_index_lookup_checked() below
    uint32_t     next_leaf;                                // leaf chain, for range scans; BTREE_INVALID_NODE = none
};

// ─── One defined index ───────────────────────────────────────────────────────
struct RowIndex {
    char           index_name[OBJECT_NAME_LEN];
    uint64_t       table_object_id;
    uint32_t       column_index;     // which column in the table's RowTableLayout
    SLSFieldType   column_type;      // cached from the layout at creation time
    uint32_t       root_node;        // BTREE_INVALID_NODE if empty
    uint32_t       entry_count;      // number of (key, row_id) pairs currently indexed (active only)
    uint8_t        active;
};

extern struct RowIndex row_indexes[ROW_INDEX_MAX];

// ─── Lifecycle ────────────────────────────────────────────────────────────
void row_index_init(void);

// Creates a B-tree index on table_name's column_name (table must already be
// a row-set table — rowstore_create_table() already called). Builds the
// index by scanning every currently-active row via rowstore_table_scan().
// Returns 0 on success. Non-zero: 1 = table not found / not a row-set table,
// 2 = permission denied (caller_uid needs PERM_WRITE on table_name).
// 3 = column not found in the table's layout. 4 = index_name already used
// or ROW_INDEX_MAX exhausted. 5 = a row's column value was unencodable
// (shouldn't happen for a well-formed row-set table; guarded anyway).
int row_index_create(uint32_t caller_uid, const char* index_name,
                     const char* table_name, const char* column_name);

// Phase 5 (SQL Feature-Parity Roadmap, DDL): drops a previously-created
// index. Deactivates the row_indexes[] slot (bump-allocated like every
// other table here -- ROW_INDEX_MAX, ROW_CONSTRAINT_MAX -- "no reclaim in
// first cut" is this whole subsystem's pre-existing, named posture, not a
// new limitation introduced here: the B-tree node pool entries this
// index's nodes occupied are simply abandoned, exactly like row_indexes[]
// slots and row_constraints[] slots already are on every other "remove"
// path in this codebase). Does NOT touch the underlying table's rows.
// Returns 0 on success. Non-zero: 1 = index not found (or its table no
// longer exists). 2 = permission denied (caller_uid needs PERM_WRITE on
// the underlying table, matching row_index_create()'s own gate).
int row_index_drop(uint32_t caller_uid, const char* index_name);

// ─── Auto-maintenance hooks — called by kernel/rowstore.c after a successful
// row mutation on a table that has one or more indexes defined. Not
// separately access-gated: the mutation that triggered the call already
// passed catalog_check_access() in rowstore.c. Best-effort: if the
// duplicate cap (BTREE_MAX_DUPES_PER_KEY) is hit, the row mutation itself
// still succeeds (it already committed) but the new entry is not added to
// the index -- see the header comment above for why this is a deliberate,
// documented, "fails loud and narrow" limitation rather than a silent one. ──
void row_index_notify_insert(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values,
                             const struct RowTableLayout* layout);
void row_index_notify_update(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* old_values,
                             const struct RowValues* new_values,
                             const struct RowTableLayout* layout);
void row_index_notify_delete(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values,
                             const struct RowTableLayout* layout);

// ─── Query ────────────────────────────────────────────────────────────────
// Exact-match lookup. Writes up to max_ids matching row_ids into out_ids
// (ascending insertion order within the matched key's duplicate list).
// Returns the number of matches found (may exceed max_ids -- compare
// against max_ids to detect truncation, same convention as out_ids-style
// APIs elsewhere in this project). Returns 0 if the index doesn't exist,
// the caller lacks PERM_READ on the underlying table, or there's no match.
uint32_t row_index_lookup(uint32_t caller_uid, const char* index_name,
                          const char* value, struct RowId* out_ids, uint32_t max_ids);

// Phase 25 (index-assisted SQL planning): identical to row_index_lookup(),
// plus *out_complete tells the caller whether the result can be TRUSTED as
// the full, current match set. row_index_lookup() alone can't answer that:
// its returned count is however many currently-active entries this key's
// duplicate list holds, and that number looks the same whether the key
// genuinely has that many matches or whether BTREE_MAX_DUPES_PER_KEY was
// hit at some point and a later legitimate insert for this exact value was
// silently dropped (row_index.h's own documented best-effort duplicate-cap
// behavior) -- churn (inserts then deletes) can even leave the active count
// LOW while the cap is still permanently exhausted for that key. This
// function is the one place that distinguishes the two: *out_complete is 0
// if this key's duplicate list ever hit the cap (a sticky, permanent flag
// once set -- matching this whole subsystem's "no reclaim in first cut"
// posture), 1 otherwise. A caller planning to use this index result INSTEAD
// OF a full table scan (rather than merely as a candidate set it will
// re-verify row-by-row) must treat *out_complete == 0 as "don't trust this,
// fall back." out_complete must be non-NULL.
uint32_t row_index_lookup_checked(uint32_t caller_uid, const char* index_name,
                                  const char* value, struct RowId* out_ids, uint32_t max_ids,
                                  uint8_t* out_complete);

// Phase 19 (relational layer): planner support. Finds the first active
// index defined on (table_object_id, column_index) -- "first applicable
// index wins," matching this roadmap's own trivial-planner scope for
// Phase 19 (no cost model, no index preference beyond existence). Writes
// its name into index_name_out (must be at least OBJECT_NAME_LEN bytes).
// Returns 1 if found, 0 otherwise (no matching index -- caller falls back
// to a full table scan).
int row_index_find_for_column(uint64_t table_object_id, uint32_t column_index,
                              char* index_name_out);

// Range scan over [lo, hi] inclusive (text bounds, same convention as
// RowValues). Either bound may be NULL for "unbounded on that side".
// Returns matching row_ids in ascending key order (and ascending duplicate-
// insertion order within a key), writing up to max_ids into out_ids.
// Returns the total match count (may exceed max_ids).
uint32_t row_index_range_scan(uint32_t caller_uid, const char* index_name,
                              const char* lo, const char* hi,
                              struct RowId* out_ids, uint32_t max_ids);

#endif /* ROW_INDEX_H */
