/*
 * row_constraint.h — Phase 23 (relational layer): UNIQUE / NOT NULL /
 * RANGE / REFERENCE (foreign key) enforcement for row-set tables. See
 * docs/AeroSLS-RDBMS-Roadmap-v0.1.md §10 for scope and the "Findings
 * addendum" for what's built here.
 *
 * ─── Why this is a new, parallel subsystem, not an extension of
 * constraint.c ────────────────────────────────────────────────────────────
 * A direct audit (grep, not assumption) found `kernel/constraint.c` — the
 * legacy KV path's UNIQUE/NOT_NULL/RANGE/REFERENCE machinery — has zero
 * reference to `rowstore.h`/`uses_rowstore`/`row_index.h` anywhere. It
 * operates exclusively on `object_records[]`'s one-record-per-object
 * model, the same structural wall Phase 16 built `rowstore.c` to get
 * around in the first place. Retrofitting `constraint.c` to also
 * understand row-set tables would mean teaching one file two incompatible
 * storage models; building a new, parallel file instead matches every
 * precedent this roadmap has already set (`row_index.c` next to
 * `index_mgr.c`, `mvcc.c` with no legacy analog at all).
 *
 * ─── Where enforcement lives: mvcc.c, not sql_exec.c ────────────────────────
 * Constraint checks are called automatically from `mvcc_row_insert()`/
 * `_update()`/`_delete()` (kernel/mvcc.c), NOT from `sql_exec.c`. This
 * matters: `mvcc.c` is the one real choke point every row mutation now
 * passes through (Phase 22 confirmed this — `rowstore.c` itself has no
 * gate of its own beyond `catalog_check_access()`), so putting the check
 * there means it can never be bypassed by a future caller that talks to
 * `mvcc.c` directly instead of going through SQL — the same "automatic
 * maintenance, not an opt-in step" reasoning Phase 17 used to justify
 * `rowstore.c` calling `row_index_notify_insert/update/delete()`
 * automatically rather than requiring every caller to remember to.
 *
 * ─── Avoiding a circular header dependency ──────────────────────────────
 * `mvcc.c` needs to call into this file (constraint checks), and this
 * file's own implementation needs to call `mvcc_table_scan()` (to check
 * UNIQUE/REFERENCE against the set of rows actually visible to the
 * calling transaction's snapshot — a raw, unversioned `rowstore_table_
 * scan()` would incorrectly compare a candidate value against a row's own
 * stale, already-superseded historical versions too). This IS a two-way
 * *call* dependency between mvcc.c and row_constraint.c, but not a
 * circular *header* dependency: this header uses only primitive types and
 * `struct RowValues` (rowstore.h) in its own declarations, never anything
 * from mvcc.h — only row_constraint.c's implementation file includes
 * mvcc.h. Two .c files calling into each other's public .h API is
 * ordinary and safe in C as long as neither header requires a type the
 * other declares, which is confirmed true here.
 *
 * ─── Registration: a direct API call, not SQL DDL ───────────────────────
 * Matching Phase 17's own `row_index_create()` precedent exactly:
 * constraints are registered by calling `row_constraint_add_*()`
 * directly, not through a SQL `CREATE TABLE ... UNIQUE` or `ALTER TABLE
 * ADD CONSTRAINT` statement — `sql_parser.c`'s grammar has no DDL at all
 * today (tables are created via the pre-existing `sys_sls_schema_set` +
 * `rowstore_create_table()` path), so extending it would be new grammar
 * work out of this phase's scope. Worth naming plainly: this repeats the
 * exact same "not syscall/DDL-reachable yet" gap Phase 22 found and fixed
 * for the SQL engine itself, except here for index AND constraint
 * creation both — a real, accumulating gap, not a new inconsistency this
 * phase introduces (row_index_create() was never made reachable either).
 * A future "make DDL live" phase is the natural place to close it for
 * both at once.
 *
 * ─── First-cut scope, matching this roadmap's own explicit non-goals ────
 * No CHECK constraints beyond RANGE's plain min/max shape (matching
 * legacy constraint.c's own RANGE precedent). No cascading FOREIGN KEY
 * actions (ON DELETE CASCADE/SET NULL) — a REFERENCE constraint's DELETE-
 * side check (row_constraint_check_delete()) is plain RESTRICT: block the
 * delete if any other table's REFERENCE constraint still points at this
 * row, full stop, never delete-then-fix-up. No composite (multi-column)
 * constraints, matching Phase 17's own single-column-index precedent.
 */
#ifndef ROW_CONSTRAINT_H
#define ROW_CONSTRAINT_H

#include <stdint.h>
#include "rowstore.h"

// ─── Limits ─────────────────────────────────────────────────────────────────
#define ROW_CONSTRAINT_MAX 64   // total constraints across every row-set table, bump-allocated, no reclaim

typedef enum {
    ROW_CONSTRAINT_UNIQUE = 0,
    ROW_CONSTRAINT_NOT_NULL,
    ROW_CONSTRAINT_RANGE,
    ROW_CONSTRAINT_REFERENCE,
} RowConstraintKind;

typedef enum {
    ROW_CONSTRAINT_OK = 0,
    ROW_CONSTRAINT_ERR_TABLE_NOT_FOUND,
    ROW_CONSTRAINT_ERR_COLUMN_NOT_FOUND,
    ROW_CONSTRAINT_ERR_POOL_FULL,
    ROW_CONSTRAINT_ERR_REF_TABLE_NOT_FOUND,
    ROW_CONSTRAINT_ERR_REF_COLUMN_NOT_FOUND,
    ROW_CONSTRAINT_ERR_TYPE_MISMATCH,        // REFERENCE: column and ref_column have different SLSFieldTypes
    ROW_CONSTRAINT_ERR_RANGE_INVALID,        // RANGE: min/max don't parse against the column's type
    ROW_CONSTRAINT_VIOLATION_UNIQUE,
    ROW_CONSTRAINT_VIOLATION_NOT_NULL,
    ROW_CONSTRAINT_VIOLATION_RANGE,
    ROW_CONSTRAINT_VIOLATION_REFERENCE,      // INSERT/UPDATE: FK value doesn't match any row in the referenced table
    ROW_CONSTRAINT_VIOLATION_REFERENCED,     // DELETE: this row is still referenced by another table (RESTRICT)
} RowConstraintResult;

struct RowConstraintDef {
    uint8_t           active;
    RowConstraintKind kind;
    uint64_t          table_object_id;
    char              table_name[OBJECT_NAME_LEN];
    uint32_t          column_index;
    char              literal_min[RECORD_VAL_LEN];   // RANGE only
    char              literal_max[RECORD_VAL_LEN];   // RANGE only
    uint64_t          ref_table_object_id;           // REFERENCE only
    char              ref_table_name[OBJECT_NAME_LEN]; // REFERENCE only
    uint32_t          ref_column_index;              // REFERENCE only
};

extern struct RowConstraintDef row_constraints[ROW_CONSTRAINT_MAX];
extern uint32_t                row_constraint_count;

void row_constraint_init(void);

// Registration -- direct API calls (see header comment for why, not SQL
// DDL). Each resolves table_name/column_name against the table's real
// RowTableLayout (table_headers[], rowstore.h) at call time.
RowConstraintResult row_constraint_add_unique(const char* table_name, const char* column_name);
RowConstraintResult row_constraint_add_not_null(const char* table_name, const char* column_name);
RowConstraintResult row_constraint_add_range(const char* table_name, const char* column_name,
                                             const char* min_literal, const char* max_literal);
RowConstraintResult row_constraint_add_reference(const char* table_name, const char* column_name,
                                                 const char* ref_table_name, const char* ref_column_name);

// Called automatically by mvcc_row_insert()/mvcc_row_update() (mvcc.c)
// before any physical write happens. Checks every active constraint
// attached to table_object_id against `values`, a candidate row about to
// be inserted or updated, under txn_id's own snapshot (so UNIQUE/
// REFERENCE checks see the transaction's own uncommitted writes too --
// "read your own writes" applies to constraint checking exactly like it
// does to every other mvcc.c operation). exclude_logical_id lets an
// UPDATE's UNIQUE check skip comparing the row against its own prior
// value (0 -- an id no real row ever has, see mvcc.h -- for INSERT, which
// has no existing row to exclude).
RowConstraintResult row_constraint_check_write(uint64_t txn_id, uint32_t caller_uid,
                                               uint64_t table_object_id,
                                               const struct RowValues* values,
                                               uint64_t exclude_logical_id);

// Called automatically by mvcc_row_delete() (mvcc.c) before a delete is
// applied. Short-circuits to ROW_CONSTRAINT_OK immediately, without
// fetching the row's values at all, if no REFERENCE constraint anywhere
// targets table_object_id -- the common case, kept cheap.
RowConstraintResult row_constraint_check_delete(uint64_t txn_id, uint32_t caller_uid,
                                                const char* table_name, uint64_t table_object_id,
                                                struct RowId physical_id);

#endif /* ROW_CONSTRAINT_H */
