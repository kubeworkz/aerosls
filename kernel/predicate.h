/*
 * predicate.h — Phase 18 (relational layer): predicate evaluation (WHERE
 * engine) for row-set tables (kernel/rowstore.h, Phase 16). See
 * docs/AeroSLS-RDBMS-Roadmap-v0.1.md §5 for the scoped design and the
 * "Findings addendum" for what's built here.
 *
 * Filtering before this phase was exactly one shape: cursor.c's
 * where_field/where_value, single-field string equality against the legacy
 * object_records[] path. This gives row-set tables a real WHERE clause:
 * comparison operators (=, !=, <, >, <=, >=), AND/OR combinators, and
 * type-aware comparison against each column's SLSFieldType — comparing
 * "100" and "9" as raw strings gives the wrong answer for a UINT64 column,
 * which this phase exists specifically to avoid.
 *
 * ─── Representation ──────────────────────────────────────────────────────
 * A predicate is a small, fixed-capacity, array-backed tree (PREDICATE_
 * MAX_NODES=32, matching legacy index_mgr.c's own INDEX_ENTRIES_MAX=32
 * sizing precedent) -- not a pointer-based AST with heap allocation, same
 * "no dynamic allocation, fixed arrays, index-addressed nodes" discipline
 * kernel/row_index.c (Phase 17) and every other kernel subsystem in this
 * project already follows. Two node kinds: PRED_NODE_COMPARISON (a leaf:
 * column name, operator, literal text) and PRED_NODE_AND/PRED_NODE_OR (two
 * child node indices). There is no parser yet (that's Phase 19) -- callers
 * build a Predicate directly via predicate_add_comparison()/_add_and()/
 * _add_or(), threading the returned indices together and setting
 * Predicate.root themselves, the same "caller assembles it from small
 * primitives" shape row_index.c's own build helpers use.
 *
 * Explicitly OUT OF SCOPE this phase (see the roadmap doc): arithmetic
 * expressions, string functions, subqueries, LIKE/pattern matching, IN
 * lists -- deliberately narrow, matching this whole roadmap's stated
 * non-goal of a full SQL expression language. An index-assisted scan path
 * (using a Phase 17 B-tree index when a predicate's column/operator match
 * one) is also deliberately deferred -- there is no query planner yet to
 * decide table-scan vs. index-scan; that's Phase 19's job, once it exists
 * to make the choice. This phase only proves the predicate itself is
 * correct and that a full-table scan can be filtered by one.
 *
 * ─── SQL Feature-Parity Roadmap Phase 3: WHERE/SET expression richness ─────
 * (docs/AeroSLS-SQL-Feature-Parity-Roadmap-v0.1.md) Promotes three of the
 * four items this file's own header comment above named out of scope:
 * parenthesized grouping, `IN (...)`, and `LIKE`. Arithmetic (`+ - * /`) is
 * also added, in `WHERE`/`HAVING` comparisons and `UPDATE ... SET`. `IS
 * NULL`/`IS NOT NULL` is deliberately NOT included here -- the roadmap
 * named it as depending on real `NULL` (Phase 4) or a merge with this
 * phase; this phase chose to stay narrower and defer it to Phase 4 outright
 * rather than build against a NULL representation that doesn't exist yet.
 *
 * Each addition reuses this file's EXISTING machinery rather than inventing
 * a parallel system, matching this whole roadmap's posture:
 *   - Parenthesized grouping needed ZERO changes here. `eval_node()` below
 *     already recurses through an arbitrarily-shaped AND/OR tree -- it was
 *     always the PARSER (sql_parser.c) that only ever built a flat
 *     `and_expr (OR and_expr)*` shape, never nested by explicit parens.
 *     Grouping is a pure parser-side change (parenthesized predicate
 *     recursively calls back into the same predicate-parsing entry point).
 *   - `IN (a, b, c)` needed ZERO changes here either -- it desugars at
 *     PARSE time into `(col = a) OR (col = b) OR (col = c)` using the
 *     already-existing `predicate_add_comparison()`/`predicate_add_or()`
 *     primitives, not a new node kind. `NOT IN` is not implemented.
 *   - `LIKE` reuses the existing `PRED_NODE_COMPARISON` leaf shape
 *     unchanged (a new `PRED_OP_LIKE` value for the existing `op` field is
 *     the only change to that struct) -- `%`/`_` wildcard matching against
 *     the row's STRING-typed column value; a `LIKE` against a non-STRING
 *     column fails closed (false), not a type coercion. No `ESCAPE` clause
 *     in this first cut, so a literal `%`/`_` in the pattern can't be
 *     matched -- named here, not silently dropped.
 *   - Arithmetic is the one genuinely new node kind (`PRED_NODE_ARITH_
 *     COMPARISON`) and the one genuinely new evaluation path
 *     (`predicate_eval_arith()`), since a computed numeric value has no
 *     existing primitive to reuse. Deliberately narrow: at most ONE
 *     arithmetic operation (`operand [+-*\/] operand`, not a full
 *     precedence-climbing expression grammar), each operand a plain
 *     unqualified column reference or a numeric literal (no `table.column`
 *     qualifiers -- arithmetic is scoped to single-table WHERE/SET, not
 *     JOIN chains). A UINT64/FLOAT column resolves to its numeric value; a
 *     STRING/BOOL column operand fails closed. Division by zero fails
 *     closed (the comparison is false), not `inf`/`NaN`. `predicate_eval_
 *     arith()` is exported specifically so `sql_exec.c`'s `UPDATE ... SET
 *     col = expr` support (e.g. `SET price = price * 1.1`) can reuse the
 *     exact same operand-resolution logic instead of a second copy.
 *
 * ─── SQL Feature-Parity Roadmap Phase 4: real NULL ──────────────────────────
 * `rowstore.h`'s `struct RowValues` gained a real `null_mask` this phase --
 * see that header's own Phase 4 note for the storage-layer design. This
 * file's job is evaluation: `PRED_OP_IS_NULL`/`PRED_OP_IS_NOT_NULL` are two
 * new comparison ops on the EXISTING `PRED_NODE_COMPARISON` leaf shape (no
 * new node kind -- `column_name` is set exactly like an ordinary comparison,
 * `literal` is unused/empty), matching this file's own established "reuse
 * the leaf shape, add an op" pattern from Phase 3's `LIKE`. Every OTHER
 * comparison kind (`compare_typed()`'s ordinary path, `LIKE`, arithmetic's
 * operand resolution) now checks `null_mask` first and fails closed (the
 * comparison is false) when the column is NULL -- collapsing SQL's real
 * three-valued (`TRUE`/`FALSE`/`UNKNOWN`) logic down to this codebase's
 * existing boolean WHERE evaluation, the same simplification this whole
 * roadmap has made consistently rather than introducing a third boolean
 * state throughout the evaluator for one phase's benefit.
 *
 * ─── SQL Feature-Parity Roadmap Phase 7: non-correlated subqueries ─────────
 * Scalar subqueries (`WHERE dept_id = (SELECT id FROM departments WHERE
 * name = 'Eng')`) and `IN (SELECT ...)`. This file stores the RAW SQL TEXT
 * of each subquery a Predicate references (`subqueries[]` below), not a
 * parsed/nested statement -- a parsed `struct SqlSelectStmt` is ~34KB
 * (`sql_parser.h`), so storing text (a few hundred bytes) and re-parsing it
 * once at resolve time matches this whole codebase's existing "text is the
 * interchange format, re-parse when needed" convention (`sql_execute()`
 * itself always re-parses from scratch on every call; there is no compiled-
 * query cache anywhere in this project) far more cheaply than embedding a
 * nested struct would. `PredicateNode` gained `uses_subquery`/
 * `subquery_index` (not a new node kind -- `kind` stays `PRED_NODE_
 * COMPARISON`, matching every prior phase's "reuse the leaf shape, add a
 * flag/op" pattern): a scalar-subquery node keeps its real, already-parsed
 * comparison `op` (EQ/NE/LT/...), just with `literal` unresolved until exec
 * time; an `IN (SELECT ...)` node uses the new marker op `PRED_OP_IN_
 * SUBQUERY` since (unlike a literal IN-list, which desugars into an OR-
 * chain entirely at PARSE time) the subquery's row values aren't known
 * until execution.
 *
 * This file (predicate.c/.h) does NOT execute subqueries itself -- doing so
 * would require calling back into `sql_exec.c`'s `exec_select()`/`mvcc.c`,
 * which predicate.c has never depended on (the dependency runs the other
 * way: sql_exec.c depends on predicate.h), the same layering this project
 * preserved when Phase 5's `mvcc_rebuild_versions_for_table()` fix was
 * deliberately placed in sql_exec.c rather than rowstore.c. Instead,
 * `sql_exec.c` gets a new resolve step (`resolve_predicate_subqueries()`)
 * that runs BEFORE the outer statement's own row scan begins: it executes
 * each referenced subquery exactly once via the ordinary `exec_select()`
 * path, then rewrites the Predicate in place -- filling a scalar node's
 * `literal` directly, or (for `PRED_OP_IN_SUBQUERY`) desugaring the result
 * rows into the SAME `(col = a) OR (col = b) OR ...` shape a literal IN-
 * list already builds, reusing `predicate_add_comparison()`/
 * `predicate_add_or()` unchanged. By the time `predicate_eval()` ever runs
 * for real row filtering, every node is an ordinary, already-resolved
 * comparison -- this file's own evaluation code needed almost no changes
 * (only a defensive fail-closed check for the case a node somehow reaches
 * evaluation still unresolved, and the new `PRED_OP_FALSE`/`PRED_OP_
 * IN_SUBQUERY` op handling below).
 *
 * **Explicitly out of scope, named rather than silently unsupported**:
 * CORRELATED subqueries (a subquery referencing the OUTER row's own
 * columns) are not detected or specially handled -- the subquery's WHERE
 * resolves purely against ITS OWN table's layout, exactly like any other
 * SELECT, so a query that LOOKS correlated either fails closed with
 * `SQL_ERR_COLUMN_NOT_FOUND` (the outer column doesn't exist in the
 * subquery's own table) or, if the subquery's table happens to have a
 * same-named column, silently resolves against THAT column instead --
 * genuinely different, real SQL semantics, not this feature's target.
 * Subqueries are resolved only inside a PLAIN (non-JOIN, non-aggregate)
 * SELECT/UPDATE/DELETE's WHERE clause (`sql_exec.c`'s `exec_select_join()`/
 * `exec_select_group()` and any HAVING clause never call the resolve step)
 * -- a subquery written there fails closed (evaluates false) rather than
 * being silently ignored or crashing, since `uses_subquery` staying set
 * unresolved is exactly the defensive case `predicate_eval()` now guards.
 * `CREATE VIEW`, `WITH` CTEs, and `UNION`/`INTERSECT`/`EXCEPT` remain
 * entirely unscoped, matching the roadmap doc's own Phase 7 framing.
 *
 * ─── Query-Surface Roadmap Phase 7: correlated subqueries ──────────────────
 * Promotes exactly the case the paragraph above named as out of scope: a
 * subquery whose own WHERE references the OUTER row's own column, written
 * as `OUTER.<column>` (e.g. `WHERE salary > (SELECT AVG(salary) FROM
 * employees e2 WHERE e2.dept = OUTER.dept)`). This is deliberately a
 * TEXTUAL substitution feature, not real scoped name resolution: `OUTER.
 * <column>` is never parsed as SQL grammar at all (no new token, no new
 * `column_ref` form) -- it is detected by a plain, case-insensitive,
 * token-boundary-checked substring scan over the subquery's own already-
 * captured raw text (`predicate_add_subquery()` below runs this scan once,
 * at ADD time, storing the result in the new `subquery_is_correlated[]`
 * array below, parallel to `subqueries[]`), and at EXEC time (kernel/
 * sql_exec.c) `OUTER.<column>` occurrences are textually replaced with
 * that column's ACTUAL VALUE from the current outer row (quoted per its
 * SLSFieldType, matching this codebase's `'` `''`-escaping convention)
 * before the resulting, now-ordinary text is re-parsed and executed via
 * the SAME `exec_subquery_column()` the non-correlated path already uses
 * -- one execution path serves both non-correlated (a plain literal
 * splice never occurs) and correlated (a literal splice happens first)
 * subqueries. This is the "honest v1" the roadmap doc itself specs:
 * `outer.col` spliced into the subquery text per row, executed once per
 * outer row (capped at a small, real, named budget --
 * `SQL_CORRELATED_MAX_OUTER_ROWS` in sql_exec.c -- rather than an
 * unbounded O(rows x subquery-cost) scan), not a general correlated-
 * subquery planner with any notion of indexing or rewrite-to-JOIN.
 *
 * A correlated subquery cannot be resolved ONCE before the outer scan the
 * way `resolve_predicate_subqueries()` resolves a non-correlated one --
 * it needs a DIFFERENT outer row's value on every evaluation. This file
 * stays exactly as ignorant of that as it was of subqueries generally
 * (see the paragraph above): `subquery_is_correlated[]` is pure metadata
 * sql_exec.c reads to decide HOW to resolve a marker node, not something
 * predicate.c ever acts on itself. `predicate_has_correlated_subquery()`
 * below is the one new accessor, letting sql_exec.c ask "does this WHERE
 * need the per-row path" without reaching into `Predicate`'s internals
 * directly (matching this file's existing thin-accessor convention).
 *
 * Scope, deliberately as narrow as the non-correlated feature was: v1 is
 * SELECT-only (kernel/sql_exec.c's `exec_update()`/`exec_delete()`
 * reject a correlated marker outright rather than attempting a per-row
 * mutation loop), and only a PLAIN SELECT (no JOIN, no aggregates, no
 * set operator, no CTE) -- combining any of those with a correlated
 * subquery is a real, named, LOUD rejection
 * (`SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED`), not the older, quieter
 * "unresolved marker fails closed to false" fallback the non-correlated
 * feature's own JOIN/GROUP BY gap still uses -- a NEW feature earns a
 * clear error instead of silently reusing that defensive fallback.
 */
#ifndef PREDICATE_H
#define PREDICATE_H

#include <stdint.h>
#include "rowstore.h"

// ─── Limits ─────────────────────────────────────────────────────────────────
#define PREDICATE_MAX_NODES     32
#define PREDICATE_INVALID_NODE  0xFFFFFFFFu

typedef enum {
    PRED_OP_EQ = 0,
    PRED_OP_NE,
    PRED_OP_LT,
    PRED_OP_GT,
    PRED_OP_LE,
    PRED_OP_GE,
    PRED_OP_LIKE,   // Phase 3 (SQL Feature-Parity Roadmap): STRING columns only, '%'/'_' wildcards, no ESCAPE
    PRED_OP_IS_NULL,      // Phase 4 (SQL Feature-Parity Roadmap): column_name set, literal unused
    PRED_OP_IS_NOT_NULL,  // Phase 4 (SQL Feature-Parity Roadmap): column_name set, literal unused
    // Phase 7 (SQL Feature-Parity Roadmap): transient marker for `col IN
    // (SELECT ...)` -- always resolved away by sql_exec.c's
    // resolve_predicate_subqueries() into a real OR-chain (or PRED_OP_FALSE
    // for a zero-row result) before predicate_eval() runs for real; if it's
    // ever evaluated unresolved (a real bug, or a scope-cut path like a
    // JOIN's WHERE), it fails closed -- see eval_comparison().
    PRED_OP_IN_SUBQUERY,
    // Phase 7: an unconditionally-false comparison -- what an `IN (SELECT
    // ...)` node becomes when the subquery returns zero rows (no value can
    // ever equal "nothing"), matching standard SQL's own IN-with-empty-set
    // semantics. column_name/literal unused.
    PRED_OP_FALSE,
} PredicateCompareOp;

typedef enum {
    PRED_NODE_COMPARISON = 0,
    PRED_NODE_AND,
    PRED_NODE_OR,
    PRED_NODE_ARITH_COMPARISON,   // Phase 3 (SQL Feature-Parity Roadmap): see below
} PredicateNodeKind;

// Phase 3 (SQL Feature-Parity Roadmap): one operand of a small arithmetic
// expression -- either a plain unqualified column reference (resolved
// against the row/layout at eval time) or a numeric literal, exactly as
// written by the parser. text[] holds whichever text is appropriate;
// RECORD_KEY_LEN is plenty for either (column names are already capped
// there, and a numeric literal is always short).
struct PredArithOperand {
    uint8_t is_column;
    char    text[RECORD_KEY_LEN];
};

// Phase 3: which arithmetic operation combines two operands. PRED_ARITH_
// NONE means "just operand1, no operation" -- used by predicate_eval_arith()
// callers (like sql_exec.c's UPDATE ... SET) that want a single-operand
// value (a plain column copy or literal) through the same evaluation path.
typedef enum {
    PRED_ARITH_NONE = 0,
    PRED_ARITH_ADD,
    PRED_ARITH_SUB,
    PRED_ARITH_MUL,
    PRED_ARITH_DIV,
} PredArithOp;

// ─── One node — either a typed comparison leaf, an arithmetic-comparison
// leaf (Phase 3), or an AND/OR combinator. ──────────────────────────────────
struct PredicateNode {
    PredicateNodeKind kind;
    // meaningful when kind == PRED_NODE_COMPARISON or PRED_NODE_ARITH_COMPARISON
    // (both use `op` as the comparison operator and `literal` as the
    // right-hand-side text; only PRED_NODE_COMPARISON uses column_name --
    // PRED_NODE_ARITH_COMPARISON's left-hand side is arith_op1/arith_op/
    // arith_op2 below instead of a bare column name)
    char               column_name[RECORD_KEY_LEN];
    PredicateCompareOp op;
    char               literal[RECORD_VAL_LEN];   // text, matching RowValues' own
                                                    // "everything is text at the API
                                                    // boundary" convention
    // meaningful when kind == PRED_NODE_ARITH_COMPARISON (Phase 3): the
    // left-hand side is arith_op1 [arith_op arith_op2] instead of a bare
    // column_name -- see predicate_eval_arith() below.
    struct PredArithOperand arith_op1;
    PredArithOp              arith_op;
    struct PredArithOperand arith_op2;   // unused when arith_op == PRED_ARITH_NONE
    // meaningful when kind == PRED_NODE_AND / PRED_NODE_OR
    uint32_t left;
    uint32_t right;
    // Phase 7 (SQL Feature-Parity Roadmap): meaningful when kind ==
    // PRED_NODE_COMPARISON and this leaf's value comes from a subquery
    // instead of a literal typed directly in the SQL text. uses_subquery==0
    // (the zero default) keeps every pre-Phase-7 comparison node byte-for-
    // byte identical. subquery_index indexes this node's owning
    // Predicate.subqueries[] (below) -- see this header's own Phase 7 note.
    uint8_t  uses_subquery;
    uint32_t subquery_index;
};

// ─── A predicate: a small fixed pool of nodes plus a root index. An empty
// predicate (root == PREDICATE_INVALID_NODE, the state predicate_init()
// leaves it in) evaluates to true for every row -- the standard "no WHERE
// clause matches everything" SQL semantic, and the useful default for
// predicate_table_scan() below. ───────────────────────────────────────────
// Phase 7 (SQL Feature-Parity Roadmap): a real, small, named ceiling on how
// many distinct subqueries one WHERE clause may reference -- non-correlated
// scalar/IN subqueries are a modest ergonomic feature here, not a general
// nested-query engine (see this header's own Phase 7 note above), and each
// slot costs PREDICATE_SUBQUERY_TEXT_LEN bytes of raw text regardless of
// whether it's used, so this stays deliberately small. Sized independently
// of sql_parser.h's SQL_MAX_TEXT_LEN (predicate.h must not depend on
// sql_parser.h -- that dependency already runs the other way) but intended
// to comfortably fit any realistic single-table, no-join, no-aggregate
// SELECT text.
#define PREDICATE_MAX_SUBQUERIES     3
#define PREDICATE_SUBQUERY_TEXT_LEN  256

struct Predicate {
    struct PredicateNode nodes[PREDICATE_MAX_NODES];
    uint32_t             node_count;
    uint32_t             root;
    // Phase 7: raw SQL text of each subquery this predicate's nodes
    // reference by subquery_index -- see this header's own Phase 7 note.
    char     subqueries[PREDICATE_MAX_SUBQUERIES][PREDICATE_SUBQUERY_TEXT_LEN];
    uint32_t subquery_count;
    // Query-Surface Roadmap Phase 7: parallel to subqueries[] above --
    // subquery_is_correlated[i]!=0 iff subqueries[i]'s raw text contains an
    // OUTER.<column> reference, set once by predicate_add_subquery() at ADD
    // time (see this header's own Phase 7 addendum above). Zero-default
    // (every pre-this-phase Predicate never had a correlated subquery, so
    // this array is simply never inspected by old code paths).
    uint8_t  subquery_is_correlated[PREDICATE_MAX_SUBQUERIES];
};

void predicate_init(struct Predicate* p);

// Appends a comparison leaf; returns its node index, or PREDICATE_INVALID_
// NODE if the node pool (PREDICATE_MAX_NODES) is exhausted. Does not touch
// p->root -- the caller wires nodes together and sets p->root explicitly.
uint32_t predicate_add_comparison(struct Predicate* p, const char* column_name,
                                  PredicateCompareOp op, const char* literal);

// Appends an AND/OR combinator over two already-added node indices; returns
// its node index, or PREDICATE_INVALID_NODE if exhausted or either child
// index is out of range.
uint32_t predicate_add_and(struct Predicate* p, uint32_t left, uint32_t right);
uint32_t predicate_add_or(struct Predicate* p, uint32_t left, uint32_t right);

// Phase 7 (SQL Feature-Parity Roadmap): appends raw subquery text to p's
// subqueries[] pool; returns its index, or PREDICATE_INVALID_NODE if
// PREDICATE_MAX_SUBQUERIES is exhausted or text is too long for
// PREDICATE_SUBQUERY_TEXT_LEN. Called by the parser once per embedded
// subquery it encounters.
uint32_t predicate_add_subquery(struct Predicate* p, const char* raw_text);

// Phase 7: marks an already-added PRED_NODE_COMPARISON leaf (node_idx, as
// returned by predicate_add_comparison()) as needing subquery resolution --
// sets uses_subquery=1/subquery_index=subq_idx on that node. Does nothing
// (silently) if node_idx is out of range or not a comparison leaf, since
// every call site already controls exactly which node it just added.
void predicate_mark_subquery(struct Predicate* p, uint32_t node_idx, uint32_t subq_idx);

// Query-Surface Roadmap Phase 7: 1 iff `p` contains at least one
// PRED_NODE_COMPARISON leaf with uses_subquery set whose owning subquery is
// correlated (subquery_is_correlated[subquery_index] != 0), else 0. Lets
// sql_exec.c decide whether a WHERE clause needs the per-outer-row
// resolution path without reaching into Predicate's internals directly.
int predicate_has_correlated_subquery(const struct Predicate* p);

// Phase 3 (SQL Feature-Parity Roadmap): appends an arithmetic-comparison
// leaf (`op1 [arith_op op2] cmp_op literal`) -- see the header comment
// above. Returns its node index, or PREDICATE_INVALID_NODE if the node
// pool is exhausted, same convention as predicate_add_comparison().
uint32_t predicate_add_arith_comparison(struct Predicate* p, struct PredArithOperand op1,
                                        PredArithOp arith_op, struct PredArithOperand op2,
                                        PredicateCompareOp cmp_op, const char* literal);

// Phase 3: resolves and computes a small arithmetic expression (op1
// [arith_op op2]) against one row -- shared by this file's own
// PRED_NODE_ARITH_COMPARISON evaluation (below) and sql_exec.c's
// UPDATE ... SET column = expr support, so both use exactly the same
// operand-resolution logic rather than two copies. Returns 0 on success
// (fills *out), 1 if a column operand doesn't resolve against layout, its
// column type isn't numeric (UINT64/FLOAT), its stored text fails to
// parse, a literal operand's text fails to parse, or (arith_op ==
// PRED_ARITH_DIV) op2 evaluates to exactly zero -- fail-closed in every
// case, the caller decides what that means (a false comparison, or a
// query error for UPDATE ... SET).
int predicate_eval_arith(const struct RowTableLayout* layout, const struct RowValues* row,
                         struct PredArithOperand op1, PredArithOp arith_op, struct PredArithOperand op2,
                         double* out);

// ─── Evaluation ─────────────────────────────────────────────────────────────
// Evaluates p against one row, given its table's layout (for column name ->
// index/SLSFieldType resolution). A NULL predicate, or one whose root is
// PREDICATE_INVALID_NODE, evaluates to true (matches every row). A
// comparison against a column name not present in layout, or a literal
// that fails to parse against that column's type, evaluates to FALSE for
// that comparison (fail closed -- "denial looks like absence," the same
// posture this project has used since Phase 7's capability checks) rather
// than crashing or silently matching. Pure function: no I/O, no globals.
int predicate_eval(const struct Predicate* p, const struct RowTableLayout* layout,
                   const struct RowValues* row);

// ─── Table-scan-with-predicate ──────────────────────────────────────────────
// Thin wrapper over rowstore_table_scan() (Phase 16): scans table_name,
// invoking cb() only for rows that pass pred (or every row, if pred is
// NULL/empty). Reuses rowstore_table_scan()'s own catalog_check_access()
// gate unmodified -- no new permission-check code path, same "one choke
// point" precedent access control has followed in this project since
// Phase 7. Returns the number of MATCHING rows (cb invocations), not the
// number scanned -- 0 if the table doesn't exist, isn't a row-set table,
// or access is denied, same as rowstore_table_scan()'s own return
// convention in those cases.
uint32_t predicate_table_scan(uint32_t caller_uid, const char* table_name,
                              const struct Predicate* pred, RowScanCb cb, void* ctx);

#endif /* PREDICATE_H */
