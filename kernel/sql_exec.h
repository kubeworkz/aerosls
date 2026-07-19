/*
 * sql_exec.h — Phase 19 (relational layer): the planner and executor that
 * runs a parsed `struct SqlStatement` (sql_parser.h) against real Phase 16
 * row-set tables. See docs/AeroSLS-RDBMS-Roadmap-v0.1.md §6 for scope and
 * the "Findings addendum" for what's built here.
 *
 * ─── Planner: "first applicable index wins," genuinely trivial ─────────────
 * For SELECT/UPDATE/DELETE's WHERE clause, `sql_find_matching_rows()`
 * (sql_exec.c, not in this header — internal to the executor) looks at
 * only the TOP-level predicate node: if it's a single comparison (not
 * wrapped in AND/OR) against a column with an existing Phase 17 index, and
 * the operator isn't `!=` (an index can't usefully narrow a not-equal
 * search), it uses `row_index_lookup()`/`row_index_range_scan()` to get
 * candidate row_ids instead of a full `predicate_table_scan()`. This is
 * candidate generation only, never the sole correctness gate: every
 * candidate is re-checked against the FULL predicate via `predicate_eval()`
 * before being accepted — the same "index scan narrows, a recheck filter
 * is the authority" shape real query engines use, and it sidesteps any
 * question of whether `row_index_range_scan()`'s inclusive-bounds
 * contract exactly matches a strict `<`/`>` operator's semantics, since
 * the recheck enforces the real semantics regardless of what the index
 * scan over- or under-shot. No cost model, no statistics — an AND/OR-
 * wrapped WHERE, or a WHERE on a column without an index, always falls
 * back to a full `predicate_table_scan()`.
 *
 * ─── Execution shape ─────────────────────────────────────────────────────
 * SELECT materializes its full result set (post-WHERE/ORDER BY/LIMIT) up
 * front, then opens a Phase 19 row-set cursor over it (cursor_open_rowset(),
 * cursor.h) — the caller fetches rows via cursor_fetch_rows(), the same
 * position/done pagination shape the legacy cursor path already used.
 * INSERT/UPDATE/DELETE execute directly against rowstore.c and report an
 * affected-row count.
 *
 * A first-cut ceiling applies to every statement kind that scans rows:
 * CURSOR_MAX_ROWSET_ROWS (cursor.h) candidate/result rows, matching this
 * whole roadmap's "documented cap, not silent" posture (BTREE_MAX_DUPES_
 * PER_KEY, ROWSTORE_MAX_PAGES). SqlResult.truncated tells the caller when
 * more rows matched than fit.
 *
 * Explicitly out of scope, matching sql_parser.h: subqueries, views, GROUP
 * BY beyond mqt.c. INSERT requires every column to be specified (no NULL/
 * default support yet, matching rowstore_row_insert()'s own existing
 * contract). Column projection in SELECT is metadata-only in this first
 * cut: `SqlResult.columns[]` reports what was asked for, but the cursor's
 * materialized rows always carry the FULL underlying row (or, for a JOIN,
 * the full combined row — see below) — genuine column-level narrowing at
 * the storage/serialization layer is deferred, a deliberate scope
 * simplification (see the Findings addendum).
 *
 * ─── Phase 20: two-table JOIN ─────────────────────────────────────────────
 * `exec_select_join()` (sql_exec.c) handles `FROM A JOIN B ON A.col=B.col`.
 * A synthetic `struct RowTableLayout` is built at execution time with
 * every column from both tables, renamed to "tablename.column" (no
 * aliasing — see sql_parser.h) — this is the one genuinely new idea this
 * phase adds: because the qualified name is baked directly into that
 * layout's `column_names[]`, every existing Phase 18/19 function that
 * takes a layout (`predicate_eval()`, `predicate_columns_valid()`,
 * `find_column_index()`, `compare_rows_by_column()`) works against a
 * joined result completely unchanged — no join-aware branch was added to
 * any of them. The join itself is a plain nested-loop: for each row of A
 * (table_name, the FROM table — always the outer loop, no cost-based
 * side selection), the matching row(s) of B are found by constructing a
 * single-comparison `Predicate` (B.join_col = this A-row's join value) and
 * calling the SAME `sql_find_matching_rows()` the single-table path uses
 * — which means "indexed nested-loop join when Phase 17 has an index on
 * B's join column" falls out for free from code that already existed,
 * not from new index-selection logic written for this phase. WHERE
 * filtering happens AFTER the join, against the combined row — there is
 * no predicate pushdown into either side's scan before joining (a real,
 * named non-goal: decomposing a predicate by which table's columns it
 * references is a query-optimizer concern, out of scope per this
 * roadmap's Phase 22 framing). A combined row is still a plain
 * `RowValues` (capped at `ROWSTORE_MAX_COLUMNS` = 16 total columns across
 * BOTH tables) rather than a new wider type — SQL_ERR_JOIN_TOO_WIDE is
 * returned up front if the two tables' combined column count would
 * exceed that.
 */
#ifndef SQL_EXEC_H
#define SQL_EXEC_H

#include <stdint.h>
#include "sql_parser.h"
#include "rowstore.h"

typedef enum {
    SQL_ERR_NONE = 0,
    SQL_ERR_PARSE,
    SQL_ERR_TABLE_NOT_FOUND,
    SQL_ERR_PERMISSION_DENIED,
    SQL_ERR_COLUMN_NOT_FOUND,
    SQL_ERR_COLUMN_COUNT_MISMATCH,
    SQL_ERR_VALUE_INVALID,
    SQL_ERR_ROW_NOT_FOUND,
    SQL_ERR_JOIN_INVALID,        // Phase 20: ON clause qualifiers don't resolve to the two joined tables
    SQL_ERR_JOIN_TOO_WIDE,       // Phase 20: combined column count would exceed ROWSTORE_MAX_COLUMNS
    SQL_ERR_INTERNAL,
} SqlErrorCode;

struct SqlResult {
    SqlStmtKind   kind;
    SqlErrorCode  error;             // SQL_ERR_NONE on success
    char          error_msg[SQL_ERR_MSG_LEN];

    // SELECT only:
    uint32_t cursor_id;                                       // fetch via cursor_fetch_rows()
    uint32_t row_count;                                        // rows actually materialized in the cursor
    uint8_t  truncated;                                        // 1 if more matches existed than CURSOR_MAX_ROWSET_ROWS
    char     columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];    // requested column list (metadata only -- see header note)
    uint32_t column_count;

    // INSERT/UPDATE/DELETE:
    uint32_t     affected_rows;
    struct RowId inserted_id;        // INSERT only
};

// Parses sql_text and executes it against real row-set tables (rowstore.c/
// row_index.c/predicate.c/cursor.c) as caller_uid. Returns 0 on success
// (out->error == SQL_ERR_NONE), 1 otherwise (out->error names why — a
// parse error, unknown table, permission denial, etc. — and out->error_msg
// is a short human-readable reason). Always fills *out, even on error.
int sql_execute(uint32_t caller_uid, const char* sql_text, struct SqlResult* out);

#endif /* SQL_EXEC_H */
