/*
 * rowstore.h — Phase 16 (relational layer): row-set storage engine.
 * See docs/AeroSLS-RDBMS-Roadmap-v0.1.md §3 for the full design writeup and
 * the investigation that led to it.
 *
 * The legacy object_records[] model (object_catalog.c) pairs one catalog
 * object 1:1 with exactly one SLSObjectRecord, capped at RECORD_MAX_FIELDS
 * (32) fields TOTAL — not per row, total, forever. That's fine for a
 * config-blob-style KV store and is the wall every relational ambition in
 * this codebase hits immediately. This file replaces it, for tables that
 * opt in, with real unbounded row sets.
 *
 * Design, in one paragraph: a row-set table's rows live in fixed-size
 * (4 KiB) pages drawn from ONE shared, monotonically-growing page pool
 * (a bump allocator, no reclaim in this first cut) common to every
 * row-set table — not a per-table fixed NVMe region the way stream.c
 * reserves 64 MiB per named stream slot, which doesn't generalize to many
 * tables. Row width is fixed and schema-derived, computed once at
 * rowstore_create_table() time from the table's existing SLSObjectSchema
 * (STRING columns get a ROWSTORE_STRING_LEN=64-byte inline slot,
 * deliberately narrower than the legacy RECORD_VAL_LEN=256, so a
 * reasonable number of rows actually fit in a page; UINT64/FLOAT = 8
 * bytes; BOOL = 1 byte — no TOAST/overflow support in this first cut).
 * Each page holds a 4-byte next_page_id header (forming a singly-linked
 * list per table, so table_headers[] stays a small, fixed-size,
 * persistable struct rather than needing a dynamically-growing per-table
 * page-id array) followed by as many fixed-width row slots as fit. A
 * row_id is (page_id, slot_index) — O(1) lookup, no directory scan.
 *
 * Pages are RAM-cached and lazily loaded from / eagerly flushed to NVMe,
 * mirroring stream.c's frames[] pattern exactly (same mechanism, different
 * allocation policy — see the design doc for why). table_headers[] itself
 * (small, fixed-size, index-aligned with object_catalog[] like
 * object_records[]/object_schemas[] already are) persists via persist.c's
 * ordinary magic-tagged-header + array-snapshot pattern.
 *
 * Row-level CRUD is gated by catalog_check_access() — a deliberate choice,
 * not a mechanical copy of the legacy path: sys_sls_select/insert/update/
 * delete (object_catalog.c) do NOT call catalog_check_access() themselves
 * today (confirmed by inspection, not assumed), so the legacy KV record
 * path has no partition or permission boundary at that layer at all — a
 * real, pre-existing gap this phase does not fix (out of scope; noted for
 * a future phase, the same "explicitly not in scope" treatment Phase 9
 * gave its own analogous gap in process spawning before Phase 9 fixed it).
 * Row-set tables are new functionality with no existing callers to break,
 * so they get the check from day one instead.
 *
 * ─── SQL Feature-Parity Roadmap Phase 4: real NULL + BLOB ──────────────────
 * (docs/AeroSLS-SQL-Feature-Parity-Roadmap-v0.1.md) Before this phase, every
 * column always held SOME text -- "no NULL/default support yet" was
 * `exec_insert()`'s own comment (sql_exec.c), and `ROW_CONSTRAINT_NOT_NULL`
 * (row_constraint.c) actually checked `strlen(val) == 0`, an empty-string
 * convention indistinguishable from a real empty STRING value. This phase
 * adds a genuine per-column NULL flag: `struct RowValues` gains a
 * `null_mask` (bit c set => column c's `values[c]` text is meaningless,
 * the column IS NULL), and the on-disk row layout (`serialize_row()`/
 * `deserialize_row()` in rowstore.c) persists that mask directly instead of
 * inventing a magic string sentinel a real column value could collide with.
 *
 * Two extra bytes are reserved in every row slot right after the tombstone
 * byte (`offset` starts at 3, not 1) to hold the mask -- a real row-width
 * change, not additive-only, since it moves every column's on-disk byte
 * offset. This is a one-way format change with no migration path for any
 * previously-persisted `sls_storage.img`: acceptable because this is a
 * research/dev codebase with no real deployed data (matching this whole
 * roadmap's precedent -- Phase 2's JOIN struct-layout change and multiple
 * Vector Store roadmap phases made the same call before it, always named
 * explicitly rather than silently).
 *
 * NULL participates in comparisons/constraints the standard SQL way,
 * collapsed to this codebase's existing boolean (not tri-valued) WHERE
 * evaluation: a NULL column compared, LIKE-matched, or used as an
 * arithmetic operand always evaluates the containing comparison to false
 * ("fail closed", predicate.c) -- the one exception being the new `IS
 * NULL`/`IS NOT NULL` operators, which exist specifically to test the flag
 * directly. `NOT_NULL`/`RANGE`/`UNIQUE`/`REFERENCE` constraints
 * (row_constraint.c) all now check the real flag instead of the old
 * `strlen(val) == 0` convention -- notably, `UNIQUE` now correctly treats a
 * real empty STRING value as comparable-for-uniqueness (only a true NULL is
 * exempt, matching standard SQL), a genuine behavior fix this phase makes
 * as a side effect of no longer conflating the two.
 *
 * Explicitly OUT OF SCOPE this phase (see the roadmap doc's own Phase 4
 * writeup and this file's implementation for what was actually decided,
 * not assumed): `LEFT JOIN`'s unmatched-row sentinel padding
 * (`""`/`"0"`/`"false"`, sql_exec.c Phase 2) is NOT retrofitted to use real
 * NULL here -- Phase 2 named that as a future Phase-4 cleanup, but
 * revisiting already-tested JOIN code was judged higher regression risk
 * than benefit for this phase's scope, so it's named again as still not
 * done, not silently dropped. Aggregate functions (`SUM`/`AVG`/`COUNT`,
 * Phase 1) do not skip NULL columns the standard SQL way -- they still
 * treat every row's value textually exactly as before; NULL-aware
 * aggregation is a real, separate piece of work not attempted here.
 * Frontend (`slsos-sim`) NULL rendering is untouched -- the JSON API now
 * emits a real `null` for a NULL column (see `net/http.c`'s Phase 4 note)
 * and that is this phase's actual round-trip guarantee; how a UI chooses to
 * display that `null` is a separate concern this SQL-engine-scoped roadmap
 * doesn't reach into, matching every prior phase's kernel-only footprint.
 */
#ifndef ROWSTORE_H
#define ROWSTORE_H

#include <stdint.h>
#include "object_catalog.h"
#include "partition.h"   // Storage Isolation Roadmap Phase 3 -- PARTITION_MAX sizes the per-partition page sub-ranges below

// ─── Limits ─────────────────────────────────────────────────────────────────
#define ROWSTORE_MAX_TABLES   CATALOG_MAX_OBJECTS  // index-aligned with object_catalog[]
#define ROWSTORE_MAX_COLUMNS  16                    // narrower than SCHEMA_MAX_FIELDS=32 —
                                                     // see design doc: keeps row width small
                                                     // enough to pack a real number of rows/page
#define ROWSTORE_STRING_LEN   64                    // deliberately narrower than legacy
                                                     // RECORD_VAL_LEN=256 — page-packing density
#define ROWSTORE_PAGE_SIZE    4096
#define ROWSTORE_MAX_PAGES    262144   // 1 GiB of row data reserved, first cut (see LBA layout below)
#define ROWSTORE_INVALID_PAGE 0xFFFFFFFFu

// Max bytes one serialized row can occupy: 1 tombstone byte + 2 null-mask
// bytes (Phase 4) + up to 16 columns of up to 64 bytes each (STRING/BLOB
// are the widest types). Used to size a stack scratch buffer in
// rowstore.c — never persisted directly.
#define ROWSTORE_MAX_ROW_BYTES (3 + ROWSTORE_MAX_COLUMNS * ROWSTORE_STRING_LEN)

// ─── On-disk layout (sls_storage.img) — bulk row page data ─────────────────
// Separate from persist.h's small-struct-array regions on purpose (this is
// large and sparse, not a small fixed struct array) — same separation
// stream.c's own STREAM_DATA_LBA_BASE has from persist.h's regions.
// Placed well clear of every existing region (persist.c's regions end at
// LBA 5816; stream.c's end at LBA 1,114,112); the real disk image is 10 GiB
// (see Makefile), not the 1 GiB stream.h's own comment claims, so there is
// ample room. table_headers[] itself (small metadata) persists via
// persist.h's PERSIST_ROWSTORE_HDR_LBA/PERSIST_ROWSTORE_ENT_LBA instead —
// see persist.h.
#define ROWSTORE_LBA_BASE  2000000ULL   // ROWSTORE_MAX_PAGES * 8 sectors reserved from here

// ─── Storage Isolation Roadmap Phase 3: real per-partition page sub-ranges ──
// Phases 1-2 (storage_quota.c) were accounting only -- a partition's pages
// were still scattered arbitrarily across the one shared ROWSTORE_MAX_PAGES
// pool. Phase 3 makes the isolation physically real: the page_id space is
// split into PARTITION_MAX equal, contiguous, fixed sub-ranges at compile
// time, and partition p's rowstore pages are drawn ONLY from
// [p * ROWSTORE_PAGES_PER_PARTITION, (p+1) * ROWSTORE_PAGES_PER_PARTITION) --
// never from any other partition's slice. This requires ROWSTORE_MAX_PAGES
// to divide evenly by PARTITION_MAX (262144 / 16 = 16384 exactly, 64 MiB per
// partition, today) -- if either constant ever changes without preserving
// that divisibility, the remainder pages silently become permanently
// unreachable by any partition (not a crash, just wasted tail space) rather
// than a hard build error, so whoever changes either constant should
// recheck this.
//
// This is a genuinely different, stricter ceiling than storage_quota.c's
// own configurable page_quota: a Phase 1 quota set ABOVE a partition's
// physical sub-range capacity is not an error, it just never binds -- the
// physical sub-range boundary is always the tighter of the two in that
// case. This is intentional, not a bug: one is a soft, admin-configurable
// accounting limit; the other is a hard, structural reservation. See
// docs/AeroSLS-Storage-Isolation-Roadmap-v0.1.md §8 for the full writeup.
#define ROWSTORE_PAGES_PER_PARTITION (ROWSTORE_MAX_PAGES / PARTITION_MAX)

// Per-partition bump-allocator cursor -- rowstore_partition_cursor[p] is the
// next free page_id within partition p's own sub-range (starts at
// p * ROWSTORE_PAGES_PER_PARTITION, set by rowstore_init(), and only ever
// increases, matching the single flat cursor's own "no reclaim in this
// first cut" posture, just now scoped per partition). NOT static, matching
// rowstore_next_free_page_id's own existing convention immediately below --
// persist.c's restore block writes it directly on restore.
extern uint32_t rowstore_partition_cursor[PARTITION_MAX];

// ─── Row values at the API boundary ─────────────────────────────────────────
// Everything is text in and text out, matching this codebase's existing
// convention (SLSRecordField.value is always a string too) — rowstore.c
// parses/formats according to each column's SLSFieldType internally.
struct RowValues {
    uint32_t count;
    // Phase 4 (SQL Feature-Parity Roadmap): bit c set => column c is a real
    // SQL NULL, and values[c]'s text content is meaningless (by convention
    // left as an empty string, never read by anything that checks this bit
    // first). ROWSTORE_MAX_COLUMNS=16 fits exactly in 16 bits. Zero-valued
    // by any existing `= {0}`/memset-to-zero construction, so every
    // pre-Phase-4 code path that never touches this field keeps its
    // original "nothing is NULL" behavior automatically.
    uint16_t null_mask;
    char     values[ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];
};

// A row's stable address: which page, which fixed-width slot within it.
struct RowId {
    uint32_t page_id;
    uint32_t slot_index;
};

// ─── A row-set table's schema-derived, fixed layout ─────────────────────────
// Computed once by rowstore_create_table(), from the table's existing
// SLSObjectSchema (object_schemas[obj_idx]) — no second schema concept.
struct RowTableLayout {
    uint32_t     column_count;
    SLSFieldType column_types[ROWSTORE_MAX_COLUMNS];
    char         column_names[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    uint32_t     column_offset[ROWSTORE_MAX_COLUMNS];  // byte offset within a row, after the tombstone byte
    uint32_t     row_width;                            // 1 (tombstone) + sum(column widths)
    uint32_t     rows_per_page;                         // (ROWSTORE_PAGE_SIZE - 4) / row_width
};

// ─── Per-table header — index-aligned with object_catalog[], same idiom as
// object_records[]/object_schemas[] already use. Small and fixed-size, so
// it persists via persist.c's ordinary array-snapshot pattern. ─────────────
struct RowTableHeader {
    uint64_t              object_id;
    uint8_t               active;   // this catalog slot is a row-set table
    struct RowTableLayout layout;
    uint32_t              row_count;         // active (non-tombstoned) rows
    uint32_t              page_count;        // pages currently allocated to this table
    uint32_t              first_page_id;     // ROWSTORE_INVALID_PAGE = none yet
    uint32_t              last_page_id;      // append target
    uint32_t              rows_in_last_page; // slots written (used or tombstoned) in last_page_id
};

extern struct RowTableHeader table_headers[ROWSTORE_MAX_TABLES];

// Global high-water mark across EVERY partition's own sub-range (Storage
// Isolation Roadmap Phase 3 turned this from "the" bump-allocator cursor
// into a plain upper bound: rowstore_alloc_page() now bumps the per-
// partition rowstore_partition_cursor[] above instead) — every page_id
// below this has been allocated by SOME partition at some point (this boot
// or a prior one) and may have real data on NVMe; every page_id at or above
// it has never been touched by anyone. Still exactly what rowstore_load_
// page() and the row_get/update/delete boundary checks need: a defensive
// "has this page_id ever been allocated at all" bound, which never needed
// partition attribution in the first place. NOT static: persist.c's restore
// block (Phase 16, "block 6") writes it directly on restore, matching every
// other subsystem's globals persist.c already pokes.
extern uint32_t rowstore_next_free_page_id;

// ─── Lifecycle ────────────────────────────────────────────────────────────
// Zeroes table_headers[]/the in-RAM page-pointer cache and resets the bump
// allocator. Called once at boot (kernel.c, alongside mqt_init()/
// agent_init() — before the later NVMe-gated persist_restore_all(), which
// may then overwrite this cold-start state with a real snapshot).
void rowstore_init(void);

// Enables row-set storage for an already-valloc'd, schema-set catalog
// object (sys_sls_schema_set must have registered at least one active
// field first — this does not invent a second schema mechanism). Computes
// and stores the table's RowTableLayout, sets uses_rowstore on its catalog
// entry, and persists both (table_headers[] via persist_rowstore_headers(),
// the flag via persist_catalog() — two separate arrays, two separate
// persist calls, matching how those two facts live in two separate structs
// today). Returns 0 on success; 1 if the object doesn't exist, has no
// schema, already uses row-set storage, or has more active schema fields
// than ROWSTORE_MAX_COLUMNS.
int rowstore_create_table(const char* table_name);

// ─── Phase B (gap remediation, docs/AeroSLS-Gap-Remediation-Roadmap-v0.1.md
// §Phase B): the live reachability path this function never had. Every
// caller of rowstore_create_table() before this was a host test -- there
// was no syscall, no shell command, and no HTTP route, meaning there was NO
// way, at runtime, to promote a valloc'd + schema'd object into a real
// row-set table. caller_uid travels inside the request struct, matching
// SYS_SLS_SQL_EXECUTE/SYS_SLS_VEC_CREATE's own established convention (do_
// syscall()'s opaque void* arg has no uid context of its own). This syscall
// does NOT valloc or schema_set for the caller -- sys_sls_valloc()
// (OBJ_TYPE_DB_TABLE) and sys_sls_schema_set() are already independently
// reachable (shell: "valloc"/"schema set"; HTTP: POST /api/valloc for the
// former, schema_set has no HTTP route yet either -- a smaller, separate
// gap, not addressed here) and this phase adds only the one missing final
// step, not a second way to do the first two.
#define SYS_SLS_ROWSTORE_CREATE_TABLE 225

struct SLSRowstoreCreateTableRequest {
    uint32_t caller_uid;   // currently unused by rowstore_create_table() itself (it takes no
                            // caller_uid -- table creation isn't gated by catalog_check_access(),
                            // matching valloc/schema_set's own ungated pre-creation posture), kept
                            // here anyway for the same reason every other Phase 22+ request struct
                            // carries one: consistency, and so a future permission gate on table
                            // creation doesn't need a struct layout change to add.
    char     table_name[OBJECT_NAME_LEN];
    int      status;       // rowstore_create_table()'s own return code (0 = success)
};

// Returns 0 on success, 1 on error (matching rowstore_create_table()'s own
// return contract) -- req->status is always filled in either way.
uint64_t sys_sls_rowstore_create_table(struct SLSRowstoreCreateTableRequest* req);

// Phase 5 (SQL Feature-Parity Roadmap, DDL): real DROP TABLE cleanup (not
// just sys_sls_vfree()'s bare catalog deactivation) -- see rowstore.c's own
// header comment on this function for the full scope writeup. Returns 0 on
// success. Non-zero: 1 = table not found / not a row-set table. 2 =
// permission denied (caller_uid needs PERM_WRITE).
int rowstore_drop_table(uint32_t caller_uid, const char* table_name);

// Phase 5 (SQL Feature-Parity Roadmap, DDL): live ALTER TABLE ADD COLUMN --
// a real storage-layout migration (row_width changes, every existing row
// is rewritten into fresh pages, any row_index on this table is rebuilt).
// See rowstore.c's own header comment on this function for the full scope
// writeup, including ROWSTORE_ADD_COLUMN_MAX_ROWS and the "old pages
// leaked, not reclaimed" limitation. Returns 0 on success. Non-zero: 1 =
// table not found/not a row-set table. 2 = permission denied. 3 = column
// name already exists. 4 = column/schema field capacity exhausted. 6 =
// page pool exhausted during migration. 7 = row count exceeds
// ROWSTORE_ADD_COLUMN_MAX_ROWS.
int rowstore_add_column(uint32_t caller_uid, const char* table_name,
                        const char* column_name, SLSFieldType column_type);

// Row CRUD. Every call resolves table_name to a catalog entry and gates on
// catalog_check_access() first (PERM_READ for get/scan, PERM_WRITE for
// insert/update/delete) — see the header comment above for why this is a
// deliberate strengthening over the legacy KV path, not a copy of it.
//
// Return codes, shared across all four:
//   0 = success
//   1 = table not found / not a row-set table
//   2 = permission denied
//   3 = row not found (bad/stale RowId, or already deleted)
//   4 = values->count doesn't match the table's column count
//   5 = a value failed to parse/validate against its column's type, or a
//       STRING value is too long (>= ROWSTORE_STRING_LEN bytes) — rejected,
//       never silently truncated
//   6 = page pool exhausted (insert only)
int rowstore_row_insert(uint32_t caller_uid, const char* table_name,
                        const struct RowValues* values, struct RowId* out_id);
int rowstore_row_get(uint32_t caller_uid, const char* table_name,
                     struct RowId id, struct RowValues* out);
int rowstore_row_update(uint32_t caller_uid, const char* table_name,
                        struct RowId id, const struct RowValues* values);
int rowstore_row_delete(uint32_t caller_uid, const char* table_name,
                        struct RowId id);

// Full-table scan in physical (page, then slot) order, invoking cb() for
// every active (non-tombstoned) row. The only "query" primitive this phase
// provides — WHERE/indexes/SQL are Phases 17-19. Returns the number of
// rows visited (0 if the table doesn't exist, isn't a row-set table, or
// access is denied).
typedef void (*RowScanCb)(struct RowId id, const struct RowValues* values, void* ctx);
uint32_t rowstore_table_scan(uint32_t caller_uid, const char* table_name,
                             RowScanCb cb, void* ctx);

#endif /* ROWSTORE_H */
