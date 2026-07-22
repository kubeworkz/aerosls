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
 * Explicitly out of scope, matching sql_parser.h: subqueries, views.
 * INSERT requires every column to be specified (no NULL/default support
 * yet, matching rowstore_row_insert()'s own existing contract). Column
 * projection in SELECT is metadata-only in this first cut: `SqlResult.
 * columns[]` reports what was asked for, but the cursor's materialized
 * rows always carry the FULL underlying row (or, for a JOIN, the full
 * combined row — see below) — genuine column-level narrowing at the
 * storage/serialization layer is deferred, a deliberate scope
 * simplification (see the Findings addendum). GROUP BY was promoted out of
 * this out-of-scope list by the SQL Feature-Parity Roadmap's Phase 1 (see
 * sql_parser.h's own header comment) -- `exec_select_group()` (sql_exec.c)
 * is a genuinely new grouping/aggregation executor over real RowValues
 * rows, NOT a call into kernel/aggregate.h's `aggregate_exec()` (that
 * module operates on the legacy object_records[] KV model exclusively and
 * is structurally incompatible with row-set tables -- confirmed by
 * reading it, not assumed).
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
 * any of them. WHERE filtering happens AFTER the join, against the
 * combined row — there is no predicate pushdown into either side's scan
 * before joining (a real, named non-goal: decomposing a predicate by
 * which table's columns it references is a query-optimizer concern, out
 * of scope per this roadmap's Phase 22 framing). A combined row is still
 * a plain `RowValues` (capped at `ROWSTORE_MAX_COLUMNS` = 16 total
 * columns across every joined table) rather than a new wider type —
 * SQL_ERR_JOIN_TOO_WIDE is returned up front if the running combined
 * column count would exceed that at any step.
 *
 * ─── SQL Feature-Parity Roadmap Phase 2: N-way JOIN + aliasing + LEFT JOIN ──
 * `exec_select_join()` was generalized from "exactly two tables, streamed
 * straight from a single nested-loop callback" into a real left-to-right
 * chain over up to `SQL_MAX_JOINS` JOIN clauses (sql_parser.h), reusing
 * every one of Phase 20's own ideas rather than replacing them:
 *   - the FROM table is materialized into a scratch buffer first (a plain
 *     `mvcc_table_scan()`, no WHERE pushdown, same as every other exec_*
 *     path's first step);
 *   - each JOIN clause then does exactly Phase 20's own per-row probe --
 *     for every row currently in the scratch buffer, build a single-
 *     comparison `Predicate` against the new table's join column and call
 *     the SAME `mvcc_find_matching_rows()` every other path uses (so
 *     "indexed nested-loop join when an index exists" still falls out for
 *     free per step, subject to the same Phase 22 MVCC-routing caveat
 *     already named for the two-table case) -- writing each match's
 *     combined row into a second scratch buffer;
 *   - the two scratch buffers ping-pong across steps (`g_select_scratch`/
 *     `g_join_scratch_b`, both static — see the "static scratch buffers,
 *     not stack locals" convention this file already established), with
 *     the running `struct RowTableLayout` growing by one table's worth of
 *     qualified columns per step, so the SAME "bake the right names into a
 *     synthetic layout" trick still needs zero changes to
 *     predicate_eval()/find_column_index()/compare_rows_by_column() for
 *     any chain length;
 *   - a column's qualifier throughout the WHOLE statement (ON/SELECT-list/
 *     WHERE/ORDER BY) is the table's alias if one was given via AS, else
 *     its real name — resolved by a plain `join_display_name()` helper,
 *     not new lookup machinery; an ON clause's two qualifiers are resolved
 *     by checking one side against the newly-joined table's own display
 *     name and the other against `find_column_index()` on the RUNNING
 *     layout built so far (which already contains every earlier table's
 *     qualified columns) — a direct generalization of Phase 20's original
 *     "check against exactly the two known table names" logic;
 *   - WHERE filtering still happens exactly once, after the ENTIRE chain
 *     is combined, against the final combined row — not once per step; no
 *     per-step predicate pushdown, the same named non-goal carried forward
 *     unchanged from a two-table chain to an N-table one.
 * LEFT JOIN reuses this same per-step probe: when a scratch-buffer row has
 * zero matches in the new table, the row is still emitted (not dropped),
 * with the new table's columns filled via `fill_join_sentinel()` — a
 * documented per-type placeholder (empty string / "0" / "false"), NOT a
 * real NULL, since there's no true NULL representation yet (SQL
 * Feature-Parity Roadmap Phase 4). Named explicitly here, not silently
 * picked — revisit once Phase 4 lands real NULL. RIGHT/FULL OUTER JOIN are
 * not implemented.
 *
 * --- Phase 22: every statement now runs inside a real MVCC transaction ---
 * Every SELECT/INSERT/UPDATE/DELETE/JOIN now executes against mvcc.c
 * (Phase 21) instead of calling rowstore.c/predicate_table_scan()
 * directly -- mvcc_row_insert/get/update/delete/mvcc_table_scan replace
 * the Phase 19/20 direct-to-rowstore.c calls one-for-one. sql_execute()
 * keeps its exact pre-Phase-22 signature and behaves as an autocommit
 * wrapper: it opens a fresh transaction (mvcc_begin()), runs the one
 * parsed statement against it, and commits on success or rolls back on any
 * error -- no caller of the existing API needs to change anything. For
 * genuine multi-statement transactions, sql_tx_begin()/sql_execute_tx()/
 * sql_tx_commit()/sql_tx_rollback() expose the transaction boundary
 * explicitly: the caller opens a transaction once, runs as many statements
 * against it as it likes via sql_execute_tx() (which does NOT commit or
 * roll back on its own), then closes it itself.
 *
 * A per-row write-write conflict (Phase 21's first-updater-wins rule) now
 * surfaces as SQL_ERR_WRITE_CONFLICT. For a multi-row UPDATE/DELETE, a
 * conflict on ANY matched row aborts the WHOLE statement (affected_rows
 * reported as 0) rather than applying a partial update and reporting a
 * partial count -- statement-level atomicity, matching this whole roadmap's
 * "fail cleanly, no partial effects" posture. Under autocommit, the
 * transaction is then rolled back, which correctly undoes every row this
 * one statement had already changed earlier in the same loop, not just the
 * row that conflicted.
 *
 * SqlResult.inserted_id changed type from Phase 16's struct RowId (a
 * physical address, which now changes on every UPDATE since Phase 21
 * never writes in place) to Phase 21's struct MvccRowId (a stable logical
 * identity) -- the correct thing for a caller to hold onto across
 * statements. No existing caller referenced this field (confirmed by
 * search before making the change), so this is not a breaking change in
 * practice, only in principle.
 *
 * --- A real, named scope cut: index-assisted planning doesn't survive
 * this phase ---
 * Phase 19's sql_find_matching_rows() (and Phase 20's join probe, which
 * reused it) preferred a Phase 17 B-tree index for a single top-level
 * equality/range comparison. That index stores PHYSICAL struct RowIds
 * tied to whatever rowstore_row_insert() call created them -- and under
 * Phase 21's MVCC, every UPDATE calls rowstore_row_insert() again for
 * the new version, so the index accumulates entries for EVERY version of
 * a row that ever existed, with no notion of which one is visible to a
 * given transaction's snapshot. Teaching the index to be MVCC-visibility-
 * aware (or adding a physical-id-to-current-version reverse lookup in
 * mvcc.c) is a real, separate design question this phase does not attempt
 * to answer under the pressure of also wiring up reachability and
 * transactions -- named here rather than silently dropped. The MVCC-routed
 * planner (mvcc_find_matching_rows(), sql_exec.c) always does a full,
 * snapshot-consistent mvcc_table_scan() plus a client-side
 * predicate_eval() recheck, for every WHERE clause shape, indexed column
 * or not. Correctness is unaffected -- every result is still checked
 * against the full predicate before being returned -- but the "indexed
 * nested-loop join for free" performance property Phase 20 specifically
 * built and verified no longer applies once a query runs through this
 * phase's transactional path. A natural candidate for a future phase (see
 * the roadmap's Phase 25 cost-based-optimization framing, or a small
 * dedicated follow-up).
 */
#ifndef SQL_EXEC_H
#define SQL_EXEC_H

#include <stdint.h>
#include "sql_parser.h"
#include "rowstore.h"
#include "mvcc.h"

typedef enum {
    SQL_ERR_NONE = 0,
    SQL_ERR_PARSE,
    SQL_ERR_TABLE_NOT_FOUND,
    SQL_ERR_PERMISSION_DENIED,
    SQL_ERR_COLUMN_NOT_FOUND,
    SQL_ERR_COLUMN_COUNT_MISMATCH,
    SQL_ERR_VALUE_INVALID,
    SQL_ERR_ROW_NOT_FOUND,
    SQL_ERR_JOIN_INVALID,        // Phase 20/Phase 2: ON clause qualifiers don't resolve to the newly joined table + something already in the FROM/JOIN chain
    SQL_ERR_JOIN_TOO_WIDE,       // Phase 20/Phase 2: running combined column count would exceed ROWSTORE_MAX_COLUMNS
    SQL_ERR_WRITE_CONFLICT,      // Phase 22: another transaction already has a pending or committed supersession
    SQL_ERR_TXN_UNAVAILABLE,     // Phase 22: MVCC_MAX_TXNS concurrently active transactions already
    SQL_ERR_TXN_NOT_ACTIVE,      // Phase 22: sql_execute_tx()/sql_tx_commit()/sql_tx_rollback() given a bad/closed txn_id
    SQL_ERR_CONSTRAINT_VIOLATION, // Phase 23: row_constraint.c rejected the write/delete (UNIQUE, NOT NULL, RANGE, REFERENCE, or REFERENCED) -- see error_msg for which
    // Phase 1 (SQL Feature-Parity Roadmap):
    SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED, // GROUP BY/aggregate select list combined with JOIN in one statement -- a real, named scope cut, not an oversight (see sql_parser.h)
    SQL_ERR_GROUP_BY_COLUMN_INVALID,   // a SELECT-list column under GROUP BY/aggregates is neither the GROUP BY column nor an aggregate call
    // Phase 5 (SQL Feature-Parity Roadmap): DDL. A catch-all for the
    // structured, non-string error codes rowstore_create_table()/
    // rowstore_drop_table()/rowstore_add_column()/row_index_create()/
    // row_index_drop() already return (small ints, not their own enums) --
    // error_msg carries the specific reason, matching this whole file's
    // existing convention of mapping a lower-level int code into one
    // SqlErrorCode plus a descriptive message rather than growing a new
    // SqlErrorCode value per underlying function's every return code.
    SQL_ERR_DDL_FAILED,
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
    uint32_t          affected_rows;
    struct MvccRowId  inserted_id;   // INSERT only (Phase 22: logical id, not a physical RowId -- see header note)
};

// Parses sql_text and executes it against real row-set tables, now wrapped
// in its own real MVCC transaction (Phase 22: begin, run the one
// statement, commit on success / roll back on any error -- an autocommit
// convenience wrapper over sql_execute_tx()). Returns 0 on success
// (out->error == SQL_ERR_NONE), 1 otherwise (out->error names why -- a
// parse error, unknown table, permission denial, write conflict, etc. --
// and out->error_msg is a short human-readable reason). Always fills
// *out, even on error.
int sql_execute(uint32_t caller_uid, const char* sql_text, struct SqlResult* out);

// --- Phase 22: explicit multi-statement transactions -----------------------
// Thin wrappers over mvcc_begin()/mvcc_commit()/mvcc_rollback() (mvcc.h),
// exposed here so a SQL-facing caller doesn't need to know about mvcc.h at
// all. sql_tx_begin() returns 0 on failure (MVCC_MAX_TXNS concurrently
// active already), matching mvcc_begin()'s own "0 = invalid" convention.
uint64_t sql_tx_begin(void);
int      sql_tx_commit(uint64_t txn_id);
int      sql_tx_rollback(uint64_t txn_id, uint32_t caller_uid);

// Runs ONE statement against an ALREADY-OPEN txn_id (from sql_tx_begin()).
// Unlike sql_execute(), this does NOT commit or roll back txn_id itself --
// the caller controls the transaction boundary and must call
// sql_tx_commit()/sql_tx_rollback() explicitly once it's done issuing
// statements. A write-write conflict here does not close the transaction
// either (matching mvcc_row_update()/_delete()'s own contract) -- the
// caller decides whether to retry the statement or abandon the whole
// transaction via sql_tx_rollback().
int sql_execute_tx(uint64_t txn_id, uint32_t caller_uid, const char* sql_text, struct SqlResult* out);

// --- Phase 22: syscall surface ----------------------------------------------
// SYS_SLS_SQL_EXECUTE (220) -- the first live, dispatch-reachable entry
// point into the SQL engine (see the Findings addendum for why this
// mattered: sql_execute() was previously called nowhere outside its own
// host tests). Autocommit only, matching sql_execute()'s own contract --
// explicit multi-statement transactions via sql_tx_*()/sql_execute_tx()
// are not (yet) exposed as their own syscalls, a deliberate first-cut
// scope cut, not an oversight (see the roadmap's own scope notes).
#define SYS_SLS_SQL_EXECUTE 220

struct SLSSqlRequest {
    uint32_t         caller_uid;
    char             sql_text[SQL_MAX_TEXT_LEN];
    struct SqlResult result;   // filled in by the call
};

// Returns 0 on success, 1 on error (matching sql_execute()'s own return
// contract) -- req->result is always fully populated either way.
uint64_t sys_sls_sql_execute(struct SLSSqlRequest* req);

#endif /* SQL_EXEC_H */
