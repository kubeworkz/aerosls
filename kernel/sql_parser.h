/*
 * sql_parser.h — Phase 19 (relational layer): a hand-written lexer +
 * recursive-descent parser for AeroSLS's minimal SQL subset. See
 * docs/AeroSLS-RDBMS-Roadmap-v0.1.md §6 for the scoped grammar and the
 * "Findings addendum" for what's built here.
 *
 * Hand-rolled, not generated — matching this project's established style
 * (the SIMI assembler in Phase 1 is hand-rolled too). Zero dependency on
 * rowstore.h/row_index.h: this file only builds a `struct SqlStatement`
 * from text; it never looks up a table, evaluates anything, or touches the
 * kernel's actual data. That split is deliberate, not incidental — it's
 * what makes the parser real-execution-testable in total isolation, the
 * same "parser correctness is testable with zero kernel dependencies"
 * verification split the roadmap calls for. See sql_exec.h for the
 * planner/executor that actually runs a parsed statement.
 *
 * ─── Grammar (genuinely minimal, per the roadmap's own scope) ───────────────
 *   select_stmt := SELECT select_list FROM ident [AS ident] (join_clause)*
 *                  [WHERE predicate] [GROUP BY column_ref] [HAVING having_predicate]
 *                  [ORDER BY select_item [ASC|DESC]] [LIMIT number] [;]
 *   select_list := '*' | select_item (',' select_item)*
 *   select_item := agg_call | column_ref
 *   agg_call    := ident '(' ('*' | column_ref) ')'   -- ident must
 *                  case-insensitively be COUNT/SUM/AVG/MIN/MAX; only COUNT
 *                  may take '*'. These are NOT reserved keywords (matching
 *                  real SQL practice) -- disambiguated purely by the
 *                  following '(' token, so an ordinary column literally
 *                  named "count" still parses fine as a plain column_ref as
 *                  long as it isn't itself followed by '('.
 *   join_clause := (JOIN | LEFT [OUTER] JOIN) ident [AS ident]
 *                  ON column_ref '=' column_ref
 *                  -- SQL Feature-Parity Roadmap Phase 2: repeatable (up to
 *                  SQL_MAX_JOINS times, chaining left to right) and
 *                  aliasable; AS is optional, matching real SQL practice
 *                  (see the Phase 2 note below).
 *   having_predicate := same grammar as `predicate` below, except each
 *                  comparison's left-hand side is a select_item (agg_call or
 *                  column_ref), not a bare column_ref -- see the SQL
 *                  Feature-Parity Roadmap Phase 1 note below.
 *   insert_stmt := INSERT INTO ident '(' ident (',' ident)* ')'
 *                  VALUES '(' insert_value (',' insert_value)* ')'
 *                  (',' '(' insert_value (',' insert_value)* ')')* [;]
 *                  -- SQL Feature-Parity Roadmap Phase 6: VALUES may repeat
 *                  additional parenthesized tuples (multi-row INSERT), up
 *                  to SQL_INSERT_MAX_EXTRA_ROWS extra beyond the first (see
 *                  the Phase 6 note below); every tuple must independently
 *                  have exactly as many values as the column list has
 *                  columns (per-tuple arity is still checked, unchanged).
 *   insert_value := literal | NULL   -- SQL Feature-Parity Roadmap Phase 4:
 *                  a column's value may now be the literal NULL keyword,
 *                  not just a typed literal (see the Phase 4 note below).
 *                  SQL Feature-Parity Roadmap Phase 6: a column NOT named in
 *                  the column list is no longer a parse/exec error -- it is
 *                  filled with NULL for every row (partial-column INSERT;
 *                  see the Phase 6 note below).
 *   update_stmt := UPDATE ident SET ident '=' set_value (',' ident '=' set_value)*
 *                  [WHERE predicate] [;]
 *   set_value   := literal | arith_expr | NULL   -- SQL Feature-Parity
 *                  Roadmap Phase 3: a plain literal is still the byte-for-
 *                  byte pre-Phase-3 shape; arith_expr (below) lets a SET
 *                  value reference the row's OWN current column values
 *                  (e.g. `SET price = price * 1.1`) -- see the Phase 3 note
 *                  below. Phase 4 adds the bare NULL keyword as a third
 *                  option (`SET middle_name = NULL`).
 *   delete_stmt := DELETE FROM ident [WHERE predicate] [;]
 *   predicate   := and_expr (OR and_expr)*
 *   and_expr    := predicate_primary (AND predicate_primary)*
 *   predicate_primary := '(' predicate ')' | comparison   -- SQL
 *                  Feature-Parity Roadmap Phase 3: parenthesized grouping,
 *                  see the Phase 3 note below.
 *   comparison  := comparison_lhs compare_op literal
 *                | column_ref IN '(' literal (',' literal)* ')'
 *                | column_ref LIKE literal
 *                | column_ref IS [NOT] NULL   -- SQL Feature-Parity Roadmap
 *                  Phase 4: see the Phase 4 note below. Available on a
 *                  plain column_ref only (not comparison_lhs's arith_expr
 *                  form) -- `price + 1 IS NULL` isn't meaningful syntax
 *                  this grammar accepts.
 *   comparison_lhs := arith_expr   -- plain WHERE/HAVING comparisons only
 *                  (not HAVING's own select_item form below); a bare
 *                  column_ref with no arithmetic operator is arith_expr's
 *                  own single-operand case, byte-for-byte the pre-Phase-3
 *                  shape.
 *   arith_expr  := arith_operand [('+' | '-' | '*' | '/') arith_operand]
 *                  -- SQL Feature-Parity Roadmap Phase 3: AT MOST ONE
 *                  arithmetic operation, not a full precedence-climbing
 *                  expression grammar -- see the Phase 3 note below.
 *   arith_operand := ident | number   -- a column reference here is always
 *                  a single unqualified identifier, never "table.column"
 *                  (arithmetic is scoped to single-table WHERE/SET, not
 *                  JOIN chains -- see the Phase 3 note below).
 *   compare_op  := '=' | '!=' | '<>' | '<' | '>' | '<=' | '>='
 *   column_ref  := ident ['.' ident]      -- Phase 20: "table.column" when
 *                                            qualifying is needed (always
 *                                            required in the ON clause,
 *                                            since bare column names in a
 *                                            joined result are ambiguous by
 *                                            construction -- see sql_exec.h).
 *                                            Phase 2: the qualifier may be
 *                                            either the table's real name or
 *                                            its alias if one was given via
 *                                            AS -- exactly as written, never
 *                                            both at once (see the Phase 2
 *                                            note below).
 *   literal     := number | 'string' | TRUE | FALSE
 *
 * `predicate` now supports explicit parenthesized grouping (SQL
 * Feature-Parity Roadmap Phase 3, below) -- AND still binds tighter than OR
 * (standard SQL operator precedence) for free whenever no parens are
 * written, exactly as before; parens let a query override that natural
 * precedence explicitly. The predicate itself is still built directly as
 * a `struct Predicate` (predicate.h, Phase 18) via its own add_comparison/
 * add_and/add_or primitives (plus Phase 3's add_arith_comparison) — no
 * second expression-tree type invented.
 *
 * Phase 20's JOIN was originally narrower still: exactly two tables (`FROM
 * A JOIN B ON ...`, no chaining), INNER JOIN only, no table aliasing --
 * ON/SELECT-list/WHERE/ORDER BY column qualifiers had to be the tables' own
 * full names (`employees.id`, not `e.id`). SQL Feature-Parity Roadmap
 * Phase 2 (below) promoted three-or-more-table chaining, LEFT JOIN, and
 * table aliasing out of that original scope list. A `column_ref` is always
 * allowed to be qualified (`table.column`) even outside a JOIN, for grammar
 * uniformity, though a non-join query has no ambiguity to resolve and a
 * bare name is the normal case there.
 *
 * Explicitly out of scope, per the roadmap: RIGHT/FULL OUTER JOIN (Phase 2
 * added LEFT only), join reordering, hash/merge join, subqueries, views,
 * string functions, `NOT IN`, `ESCAPE` for LIKE. Arithmetic expressions,
 * LIKE, IN lists, and parenthesized WHERE grouping were promoted out of
 * this list by Phase 3 (below); `IS NULL`/`IS NOT NULL` and a real `NULL`
 * literal were promoted out of this list by Phase 4 (below).
 * INSERT requires an explicit column list covering every one of the
 * table's columns (partial-row/default-value INSERT is still not
 * supported — every column must be named — but Phase 4 lets any named
 * column's value be the literal NULL, matching rowstore_row_insert()'s
 * own existing all-columns-required contract while finally giving a real
 * way to populate a column with nothing).
 *
 * ─── SQL Feature-Parity Roadmap Phase 1: GROUP BY / HAVING / aggregates ────
 * (docs/AeroSLS-SQL-Feature-Parity-Roadmap-v0.1.md) GROUP BY is no longer
 * out of scope, promoted from a separate `aggregate`/`mqt` command surface
 * (kernel/aggregate.h) into real SQL grammar. That promotion turned out to
 * be grammar-and-executor work only, NOT a reuse of aggregate.c's own
 * math: aggregate_exec() operates entirely on the legacy object_records[]
 * KV model (field-name-suffix matching against a single flattened record
 * per object), which is structurally incompatible with rowstore.c's real
 * per-row storage that sql_exec.c operates on — the same
 * legacy-KV-vs-row-set wall row_constraint.h's own header comment names
 * for Phase 23's constraint engine. sql_exec.c's grouping/aggregation
 * logic is therefore a fresh implementation over real `RowValues` rows,
 * not a call into aggregate.c.
 *
 * Deliberately narrower than full SQL, matching this whole roadmap's
 * "smallest real version first" posture:
 *   - GROUP BY accepts exactly one column (no composite multi-column
 *     grouping yet — matching row_index.c's own single-column-index
 *     precedent).
 *   - GROUP BY combined with JOIN in the same statement is not supported
 *     (SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED) — a real, named scope cut, not
 *     an oversight.
 *   - A SELECT list under GROUP BY (or under a bare aggregate with no
 *     GROUP BY at all, e.g. `SELECT COUNT(*) FROM t`) may only name the
 *     GROUP BY column itself or an aggregate call — an ordinary
 *     non-aggregated, non-grouped column is a query error (matching
 *     standard SQL's own "column must appear in GROUP BY or be
 *     aggregated" rule), not silently taking its first-seen value.
 *   - COUNT(col) does not yet distinguish NULL from a real value (there is
 *     no true NULL representation until the roadmap's own Phase 4) — it
 *     currently behaves identically to COUNT(*) over the matched rows.
 *   - SUM/AVG require a UINT64 or FLOAT argument column (SQL_ERR_VALUE_
 *     INVALID otherwise); MIN/MAX work on any column type, using the same
 *     type-aware comparison ORDER BY already uses, so MIN/MAX over a
 *     STRING or BOOL column is real and correct, not just numeric.
 *   - No column aliasing (`AS`) — an aggregate select-item's output column
 *     name is always its canonical rendered text, e.g. "COUNT(*)",
 *     "SUM(amount)".
 *
 * HAVING reuses `struct Predicate`/`predicate_eval()` completely
 * unchanged, the same trick Phase 20's JOIN used for qualified names: a
 * HAVING comparison's left-hand side may be an aggregate call or a plain
 * column ref (see `select_item` above), rendered to the exact same
 * canonical text the SELECT list uses ("COUNT(*)" etc.) and evaluated
 * against a synthetic one-row-per-group `RowTableLayout` built at exec
 * time — so no predicate.h changes were needed at all. A HAVING clause
 * referencing a column/aggregate that isn't in the SELECT list is a query
 * error (SQL_ERR_COLUMN_NOT_FOUND), not silently-always-false.
 *
 * ─── SQL Feature-Parity Roadmap Phase 2: N-way JOIN + aliasing + LEFT JOIN ──
 * (docs/AeroSLS-SQL-Feature-Parity-Roadmap-v0.1.md) Generalizes Phase 20's
 * exactly-two-tables, no-alias, INNER-only JOIN into a real chain, reusing
 * every one of Phase 20's own ideas rather than replacing them — see
 * sql_exec.h's header comment for the executor-side generalization (the
 * "bake qualified names into a synthetic query-time layout" trick still
 * needs zero changes to predicate_eval()/find_column_index()/
 * compare_rows_by_column(), just a running layout that grows one table at a
 * time instead of being built once for exactly two tables).
 *
 *   - Up to SQL_MAX_JOINS chained JOIN clauses (SQL_MAX_JOINS + 1 tables
 *     total in one statement) — a real, named ceiling, not unlimited
 *     chaining, sized against ROWSTORE_MAX_COLUMNS=16's own natural
 *     pressure (a handful of narrow tables already saturates it).
 *   - AS is optional (`FROM employees e` and `FROM employees AS e` both
 *     parse identically) — matching real SQL practice. JOIN/LEFT/OUTER/AS
 *     are now reserved keywords (a table or column literally named one of
 *     these would break), the same kind of small, named regression risk
 *     Phase 20 already accepted for JOIN/ON.
 *   - Once a table has an alias, every qualifier referencing it throughout
 *     the statement (SELECT list, ON, WHERE, ORDER BY) must use that
 *     alias, not the real table name — never both at once. This is what
 *     makes a genuine self-join (`FROM employees e1 JOIN employees e2 ON
 *     e1.mgr_id = e2.id`) resolvable at all: without an alias, the same
 *     physical table appearing twice would produce colliding qualified
 *     column names in the combined layout with no way to disambiguate —
 *     not specially detected or rejected here (a real, un-guarded edge
 *     case, matching this whole roadmap's "smallest real version first"
 *     posture), just naturally avoided by always aliasing a self-join, the
 *     only sane way to write one anyway.
 *   - LEFT JOIN pads an unmatched right-side row with a documented
 *     per-type sentinel value (empty string / "0" / "false") rather than a
 *     real NULL, since real NULL doesn't exist yet (SQL Feature-Parity
 *     Roadmap Phase 4). Named explicitly here, not silently picked —
 *     revisit once Phase 4 lands. RIGHT/FULL OUTER JOIN are not
 *     implemented at all (see the out-of-scope list above).
 *   - GROUP BY/aggregates combined with ANY JOIN (not just a single one)
 *     remains SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED — Phase 1's scope cut,
 *     unchanged, just now covering the general N-way case too.
 *   - WHERE filtering still happens once, after the ENTIRE chain is
 *     combined, against the fully joined row — no per-step predicate
 *     pushdown, the same named non-goal Phase 20 already carried forward
 *     unchanged from a two-table chain to an N-table one.
 *
 * ─── SQL Feature-Parity Roadmap Phase 3: WHERE/SET expression richness ─────
 * (docs/AeroSLS-SQL-Feature-Parity-Roadmap-v0.1.md) Promotes parenthesized
 * grouping, `IN (...)`, `LIKE`, and single-operation arithmetic
 * (`+ - * /`) out of the out-of-scope list above. See predicate.h's own
 * Phase 3 header note for how each is evaluated -- this file only covers
 * the grammar/parsing side.
 *
 *   - Parenthesized grouping needed no new AST/predicate concept: a
 *     `predicate_primary` is either `'(' predicate ')'` (recursing back into
 *     the same `parse_predicate()`) or a plain `comparison` -- the ONLY
 *     grammar change grouping needed, since predicate.c's tree evaluator
 *     already handled arbitrarily-nested AND/OR.
 *   - `IN (a, b, c)` desugars at PARSE time into `(col = a) OR (col = b) OR
 *     (col = c)` -- no new predicate node kind. A long IN list can exhaust
 *     `PREDICATE_MAX_NODES` (32) exactly like a long chain of explicit
 *     ORs would -- a real, documented cap, not silently truncated (the same
 *     "WHERE/HAVING clause too complex" error every other node-pool
 *     exhaustion in this file already reports). `NOT IN` is not
 *     implemented.
 *   - `LIKE` is available on any plain (non-arithmetic) comparison LHS, in
 *     both WHERE and HAVING -- `'%'` (any sequence) and `'_'` (one
 *     character) wildcards, STRING columns only (fails closed, not a type
 *     coercion, against any other column type), no `ESCAPE` clause.
 *   - Arithmetic (`arith_expr`, see the grammar above) is available on a
 *     plain WHERE comparison's left-hand side only -- NOT HAVING's (an
 *     aggregate's rendered label isn't an arithmetic operand in this first
 *     cut) and NOT inside a JOIN's ON clause or a qualified `table.column`
 *     context (arithmetic operands are always a single unqualified
 *     identifier). At most ONE operation, not a full expression grammar --
 *     `price * 1.1 > 100` parses; `price * 1.1 + tax > 100` does not (a
 *     real, named ceiling, matching this whole roadmap's "smallest real
 *     version first" posture). The SAME arith_expr grammar also gives
 *     `UPDATE ... SET column = arith_expr` (e.g. `SET price = price *
 *     1.1`), evaluated per row against that row's OWN pre-UPDATE snapshot
 *     -- when a statement has
 *     multiple SET assignments, every arithmetic expression is computed
 *     from the row as it was BEFORE the statement, not chained sequentially
 *     (`SET a = a + 1, b = a` -- `b` sees `a`'s OLD value), matching
 *     standard SQL semantics, not implementation-order-dependent behavior.
 *   - A real lexer wrinkle, inherited rather than introduced by this phase:
 *     the pre-existing number-literal rule treats a `-` immediately
 *     followed by a digit as part of a negative-number literal regardless
 *     of context (there was no MINUS operator token before this phase to
 *     disambiguate against). This phase adds `+`/`-`/`/` as real operator
 *     tokens (`*` already existed), but ONLY when `-` is NOT immediately
 *     followed by a digit -- so subtracting a positive numeric literal
 *     needs a space after the minus sign (`price - 1` parses as
 *     subtraction; `price -1` and `price-1` both still lex as a single
 *     negative-number token, same as every literal negative number
 *     elsewhere in this grammar already did, and fail to parse as
 *     intended). Subtracting a COLUMN (`price - discount`) is unaffected,
 *     since a column reference never starts with a digit. Named here
 *     explicitly rather than silently traded away -- fixing it properly
 *     would mean moving negative-literal handling from the lexer into
 *     every `parse_literal()` call site, a larger change than this phase's
 *     own scope justifies.
 *   - `IS NULL`/`IS NOT NULL` is explicitly NOT built here, deferred
 *     whole to Phase 4 (real NULL) rather than building against a NULL
 *     representation that doesn't exist yet.
 *
 * ─── SQL Feature-Parity Roadmap Phase 4: real NULL ──────────────────────────
 * (docs/AeroSLS-SQL-Feature-Parity-Roadmap-v0.1.md) Builds on top of what
 * Phase 3 deliberately left out: a real `NULL` literal in `INSERT`/`UPDATE
 * ... SET`, and `IS NULL`/`IS NOT NULL` in `WHERE`/`HAVING`. See
 * `rowstore.h`'s own Phase 4 note for the storage-layer design (a real
 * per-column `null_mask`, not a magic-string convention) and `predicate.h`'s
 * for the evaluation-layer design.
 *
 *   - `IS NULL`/`IS NOT NULL` reuses the existing `PRED_NODE_COMPARISON`
 *     leaf shape unchanged (two new `PredicateCompareOp` values,
 *     `PRED_OP_IS_NULL`/`PRED_OP_IS_NOT_NULL`, on the SAME struct shape) --
 *     the same "reuse the leaf, add an op" pattern `LIKE` used in Phase 3.
 *     Parsed in `parse_comparison_tail()`, the shared WHERE/HAVING tail, so
 *     both a plain WHERE column and a HAVING aggregate label can use it
 *     (`HAVING COUNT(*) IS NULL` parses, though it's always false in
 *     practice since this codebase's aggregates never produce NULL).
 *     `=NULL`/`!=NULL` are deliberately NOT given special meaning here --
 *     only the dedicated `IS [NOT] NULL` operator resolves NULL, matching
 *     standard SQL's own distinction (`col = NULL` is legal SQL syntax in
 *     some dialects but is defined to always evaluate to unknown/false,
 *     never true, which this parser sidesteps entirely by not accepting a
 *     bare `NULL` as an ordinary comparison literal at all).
 *   - The `NULL` keyword IS accepted as an `INSERT ... VALUES` value and an
 *     `UPDATE ... SET` value (a new parallel `is_null[]`/`set_is_null[]`
 *     array on `SqlInsertStmt`/`SqlUpdateStmt`, additive alongside the
 *     existing `values[]`/`set_values[]` text arrays, matching every prior
 *     phase's "parallel array, zero-default, byte-compatible when unused"
 *     convention). `INSERT` still requires every column to be named in the
 *     column list -- `NULL` is a value you write explicitly, not an
 *     implicit default for an omitted column.
 *   - Arithmetic's `arith_expr` (Phase 3) does NOT gain NULL-operand
 *     support here -- `price * NULL` is not a parseable expression; `NULL`
 *     is only a value position (`INSERT`/`SET`) or tested via `IS [NOT]
 *     NULL`, never an arithmetic operand.
 *
 * ─── SQL Feature-Parity Roadmap Phase 6: multi-row + partial-column INSERT ──
 *   - Multi-row: `SqlInsertStmt` keeps its original `values[]`/`is_null[]`
 *     fields as row 0 (byte-for-byte the pre-Phase-6 shape) and adds
 *     `extra_values[][]`/`extra_is_null[][]` + `extra_row_count` for any
 *     additional `VALUES` tuples -- the same "parallel array, zero-default,
 *     byte-compatible when unused" convention as every prior phase
 *     (`extra_row_count == 0` for any ordinary single-tuple INSERT keeps
 *     every pre-Phase-6 caller/test reading `.values[i]`/`.is_null[i]`
 *     directly unaffected). `SQL_INSERT_MAX_EXTRA_ROWS` (7, so 8 rows
 *     total per statement) is a real, named ceiling -- chosen to keep
 *     `sizeof(struct SqlInsertStmt)` from exceeding `sizeof(struct
 *     SqlSelectStmt)`, which already sets `struct SqlStatement`'s union
 *     footprint; this is a modest ergonomic feature for a handful of rows
 *     per statement, not a bulk-loader primitive (`sql_execute()`'s
 *     512-byte `SQL_MAX_TEXT_LEN` input cap makes a much larger row count
 *     impractical to type as one statement anyway).
 *   - Partial-column: a column omitted from `INSERT INTO t (col, ...)`'s
 *     column list is no longer a parse or exec error. This needed NO new
 *     parser field -- `count` (the named-column count) simply may now be
 *     less than the table's full column count, and `sql_exec.c`'s
 *     `exec_insert()` fills any table column not present in the statement
 *     with a real NULL (Phase 4's `null_mask`) instead of rejecting the
 *     statement. Investigation confirmed the "every column must be named"
 *     contract lived entirely in `sql_exec.c` -- `rowstore_row_insert()`/
 *     `mvcc_row_insert()` always took a fully-populated `RowValues` and
 *     never themselves enforced which columns the caller supplied -- so
 *     this phase is purely a `sql_parser.c`/`sql_exec.c` change, confirming
 *     the roadmap draft's own open question.
 *   - Atomicity: a multi-row INSERT's rows are NOT given any new
 *     transaction plumbing. `sql_execute()` already wraps one whole
 *     statement in a single `mvcc_begin()`/`mvcc_commit()`/
 *     `mvcc_rollback()` (Phase 22); `exec_insert()` just loops over its
 *     rows against the SAME already-open `txn_id` and stops at the first
 *     failure, so a mid-statement failure rolls back every row the
 *     statement had already inserted for free, via the exact mechanism
 *     that already rolls back a single failed INSERT today.
 */
#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include <stdint.h>
#include "object_catalog.h"
#include "rowstore.h"
#include "predicate.h"

// ─── Limits ─────────────────────────────────────────────────────────────────
#define SQL_MAX_TEXT_LEN      512   // max input SQL statement length this parser accepts
#define SQL_ERR_MSG_LEN       128

typedef enum {
    SQL_STMT_SELECT = 0,
    SQL_STMT_INSERT,
    SQL_STMT_UPDATE,
    SQL_STMT_DELETE,
    // Phase 5 (SQL Feature-Parity Roadmap): DDL.
    SQL_STMT_CREATE_TABLE,
    SQL_STMT_ALTER_TABLE,
    SQL_STMT_DROP_TABLE,
    SQL_STMT_CREATE_INDEX,
    SQL_STMT_DROP_INDEX,
    SQL_STMT_INVALID,
} SqlStmtKind;

// Phase 1 (SQL Feature-Parity Roadmap): which aggregate function, if any, a
// SELECT-list item or HAVING comparison's left-hand side is. SQL_AGG_NONE
// means "plain column_ref" -- see sql_exec.c for the actual math.
typedef enum {
    SQL_AGG_NONE = 0,
    SQL_AGG_COUNT,
    SQL_AGG_SUM,
    SQL_AGG_AVG,
    SQL_AGG_MIN,
    SQL_AGG_MAX,
} SqlAggFunc;

// Phase 2 (SQL Feature-Parity Roadmap): which JOIN kind a join_clause uses.
// SQL_JOIN_INNER is the default (matches Phase 20's original INNER-only
// behavior byte-for-byte when no LEFT keyword is written); SQL_JOIN_LEFT
// pads an unmatched right-side row with a documented interim sentinel (see
// sql_exec.c's fill_join_sentinel()) since there's no real NULL yet.
typedef enum {
    SQL_JOIN_INNER = 0,
    SQL_JOIN_LEFT,
} SqlJoinType;

#define SQL_MAX_JOINS 3   // FROM + up to 3 JOINs = 4 tables total in one
                          // statement -- a real, named ceiling (see the
                          // Phase 2 header note above for why this size).

// One link in a JOIN chain -- table/alias plus the ON clause's two
// "qualifier.column" halves exactly as written (in whatever order the user
// wrote them); the executor resolves which one refers to the newly joined
// table vs. some table already earlier in the chain -- see sql_exec.h's
// header comment for why parsing doesn't try to.
struct SqlJoinClause {
    SqlJoinType type;
    char        table[OBJECT_NAME_LEN];
    char        alias[OBJECT_NAME_LEN];              // empty string if no alias was given
    char        on_left_qualifier[OBJECT_NAME_LEN];
    char        on_left_col[RECORD_KEY_LEN];
    char        on_right_qualifier[OBJECT_NAME_LEN];
    char        on_right_col[RECORD_KEY_LEN];
};

struct SqlSelectStmt {
    char     table_name[OBJECT_NAME_LEN];

    // ── Phase 20/Phase 2 (SQL Feature-Parity Roadmap): JOIN (zero-default --
    // has_join==0 keeps every non-JOIN query's behavior identical to before
    // either phase). table_alias is the FROM table's own optional alias;
    // joins[0..join_count) is the chain of JOIN clauses that follow it, in
    // the order written. Phase 2 generalized Phase 20's fixed "exactly one
    // JOIN, no alias" fields into this repeatable, aliasable form -- see
    // sql_exec.h's header comment for the executor-side generalization. ────
    uint8_t  has_join;                         // 1 iff join_count > 0
    char     table_alias[OBJECT_NAME_LEN];     // alias for the FROM table itself, empty if none
    uint32_t join_count;
    struct SqlJoinClause joins[SQL_MAX_JOINS];

    uint8_t  select_all;                                       // '*'
    char     columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];     // meaningful iff !select_all; may be
                                                                 // "table.column" qualified when has_join,
                                                                 // or -- Phase 1 -- a rendered aggregate
                                                                 // label ("COUNT(*)") when agg_fn[i] != SQL_AGG_NONE
    uint32_t column_count;

    // ── Phase 1 (SQL Feature-Parity Roadmap): GROUP BY / HAVING / aggregates.
    // agg_fn[i]/agg_arg[i] are parallel to columns[]/column_count above --
    // meaningful only when !select_all. agg_fn[i]==SQL_AGG_NONE means
    // columns[i] is a plain column_ref (byte-for-byte the pre-Phase-1
    // behavior); otherwise agg_arg[i] holds the function's argument column
    // name (or "*", COUNT only) and columns[i] holds the canonical rendered
    // label ("COUNT(*)", "SUM(amount)") used both as the output column name
    // and as what a HAVING clause must reference to mean this item. See the
    // header comment above for the full scope (single-column GROUP BY only,
    // no GROUP BY + JOIN, no aliasing). ─────────────────────────────────────
    uint8_t    has_aggregates;   // 1 if select_all==0 and any agg_fn[i] != SQL_AGG_NONE
    SqlAggFunc agg_fn[ROWSTORE_MAX_COLUMNS];
    char       agg_arg[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    uint8_t    has_group_by;
    char       group_by[RECORD_KEY_LEN];
    uint8_t    has_having;
    struct Predicate having;     // comparison column_names are rendered select_item labels (see above)

    uint8_t  has_where;
    struct Predicate where;                                     // comparison column_names may be qualified when has_join
    uint8_t  has_order_by;
    char     order_by[RECORD_KEY_LEN];                          // may be qualified when has_join
    uint8_t  order_desc;                                        // 0 = ASC (default), 1 = DESC
    uint8_t  has_limit;
    uint32_t limit;
};

// Phase 6 (SQL Feature-Parity Roadmap): a real, named ceiling on additional
// VALUES tuples beyond row 0 -- see this header's own Phase 6 note above for
// the sizeof(SqlSelectStmt)-parity rationale.
#define SQL_INSERT_MAX_EXTRA_ROWS 7

struct SqlInsertStmt {
    char     table_name[OBJECT_NAME_LEN];
    char     columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    char     values[ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];   // meaningful iff !is_null[i]; row 0 -- see Phase 6 note
    uint32_t count;   // columns/values pair count -- the NAMED column count; Phase 6: may now be LESS than
                      // the table's full column count (partial-column INSERT fills the rest with NULL at exec time)

    // Phase 4 (SQL Feature-Parity Roadmap): parallel to values[] above, not
    // a replacement -- zero-default (is_null[i]==0 for every i) keeps every
    // pre-Phase-4 INSERT byte-for-byte identical, using values[i] exactly
    // as before. See rowstore.h/predicate.h for the null_mask machinery
    // this feeds.
    uint8_t  is_null[ROWSTORE_MAX_COLUMNS];

    // Phase 6 (SQL Feature-Parity Roadmap): additional VALUES tuples for a
    // multi-row INSERT (VALUES (...), (...), ...), purely additive --
    // extra_row_count==0 (the zero default) keeps every pre-Phase-6 INSERT
    // byte-for-byte identical, using only the row-0 fields above exactly as
    // before. Each extra_values[r]/extra_is_null[r] pair is shaped exactly
    // like values[]/is_null[] above and is keyed against the SAME columns[]
    // list (one column list per statement, shared by every tuple -- matching
    // standard SQL's own `INSERT ... (cols) VALUES (...), (...)` grammar).
    // See this header's own Phase 6 note above for the ceiling rationale.
    uint32_t extra_row_count;
    char     extra_values[SQL_INSERT_MAX_EXTRA_ROWS][ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];
    uint8_t  extra_is_null[SQL_INSERT_MAX_EXTRA_ROWS][ROWSTORE_MAX_COLUMNS];
};

struct SqlUpdateStmt {
    char     table_name[OBJECT_NAME_LEN];
    char     set_columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    char     set_values[ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];   // meaningful iff !set_is_arith[i] (byte-for-byte pre-Phase-3 shape)
    uint32_t set_count;

    // ── Phase 3 (SQL Feature-Parity Roadmap): SET column = arith_expr
    // (e.g. `SET price = price * 1.1`), evaluated per row at UPDATE time
    // against that row's OWN pre-UPDATE values. Purely additive, parallel
    // to set_columns[]/set_values[] above (not a replacement) -- zero-
    // default (set_is_arith[i]==0 for every i) keeps every pre-Phase-3
    // UPDATE statement byte-for-byte identical, using set_values[i]
    // exactly as before. See predicate.h for the shared PredArithOperand/
    // PredArithOp/predicate_eval_arith() machinery this reuses. ──────────
    uint8_t                 set_is_arith[ROWSTORE_MAX_COLUMNS];
    struct PredArithOperand set_arith_op1[ROWSTORE_MAX_COLUMNS];
    PredArithOp              set_arith_op[ROWSTORE_MAX_COLUMNS];
    struct PredArithOperand set_arith_op2[ROWSTORE_MAX_COLUMNS];   // unused when set_arith_op[i] == PRED_ARITH_NONE

    // ── Phase 4 (SQL Feature-Parity Roadmap): SET column = NULL. Another
    // additive parallel array, same convention as set_is_arith[] above --
    // zero-default keeps every pre-Phase-4 UPDATE byte-for-byte identical.
    // A SET value is exactly one of: a plain literal (set_values[i]), an
    // arithmetic expression (set_is_arith[i]), or NULL (set_is_null[i]) --
    // mutually exclusive by construction in parse_set_value(). ───────────
    uint8_t  set_is_null[ROWSTORE_MAX_COLUMNS];

    uint8_t  has_where;
    struct Predicate where;
};

struct SqlDeleteStmt {
    char    table_name[OBJECT_NAME_LEN];
    uint8_t has_where;
    struct Predicate where;
};

// ── Phase 5 (SQL Feature-Parity Roadmap): DDL. ──────────────────────────────
// One column definition inside a CREATE TABLE's column list. Inline
// constraint syntax (NOT NULL / UNIQUE / REFERENCES table(col)) is parsed
// here and translated into row_constraint_add_*() calls at CREATE TABLE
// exec time -- not a separate ALTER TABLE ADD CONSTRAINT statement (out of
// scope; matches row_constraint.h's own "registration is a direct API
// call" precedent, just reached from CREATE TABLE grammar instead of a
// host test now). At most one of has_reference/is_unique/not_null combined
// per column in this first cut -- a column CAN be both UNIQUE and NOT NULL
// and REFERENCES all at once (they're independent flags, not mutually
// exclusive), this comment just clarifies none of them are themselves
// combinable with a second instance of themselves (e.g. two REFERENCES).
struct SqlColumnDef {
    char         name[RECORD_KEY_LEN];
    SLSFieldType type;
    uint8_t      not_null;
    uint8_t      is_unique;
    uint8_t      has_reference;
    char         ref_table[OBJECT_NAME_LEN];
    char         ref_column[RECORD_KEY_LEN];
};

struct SqlCreateTableStmt {
    char     table_name[OBJECT_NAME_LEN];
    struct SqlColumnDef columns[ROWSTORE_MAX_COLUMNS];
    uint32_t column_count;
};

// ALTER TABLE ... ADD COLUMN only for v1 -- see rowstore.c's rowstore_add_
// column() header comment for why DROP COLUMN/RENAME are real, separate,
// bigger storage-layout undertakings scoped out of this phase.
struct SqlAlterTableStmt {
    char         table_name[OBJECT_NAME_LEN];
    char         column_name[RECORD_KEY_LEN];
    SLSFieldType column_type;
};

struct SqlDropTableStmt {
    char table_name[OBJECT_NAME_LEN];
};

struct SqlCreateIndexStmt {
    char index_name[OBJECT_NAME_LEN];
    char table_name[OBJECT_NAME_LEN];
    char column_name[RECORD_KEY_LEN];
};

struct SqlDropIndexStmt {
    char index_name[OBJECT_NAME_LEN];
};

struct SqlStatement {
    SqlStmtKind kind;
    union {
        struct SqlSelectStmt      select;
        struct SqlInsertStmt      insert;
        struct SqlUpdateStmt      update;
        struct SqlDeleteStmt      del;
        struct SqlCreateTableStmt create_table;
        struct SqlAlterTableStmt  alter_table;
        struct SqlDropTableStmt   drop_table;
        struct SqlCreateIndexStmt create_index;
        struct SqlDropIndexStmt   drop_index;
    } u;
};

// Parses one SQL statement from text (case-insensitive keywords; a single
// trailing ';' is optional and ignored). Returns 0 on success, filling
// *out. Returns 1 on a syntax/scope error, filling err[0..err_max) with a
// short human-readable message (e.g. "expected FROM", "unknown statement
// keyword", "too many columns") and leaving *out in an undefined state.
// Pure function: no globals, no table lookups, no I/O -- text in, a typed
// statement struct out, nothing else.
int sql_parse(const char* text, struct SqlStatement* out, char* err, uint32_t err_max);

#endif /* SQL_PARSER_H */
