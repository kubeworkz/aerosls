# AeroSLS SQL Feature-Parity Roadmap v0.1

## 0. Why this doc exists

This doc scopes the gap between AeroSLS's SQL engine (`kernel/sql_parser.c`/
`sql_exec.c`, Phases 16-24 of the RDBMS roadmap) and SQLite's SQL *surface
area* — deliberately not its reliability/durability bar, which is a
separate, much larger kind of effort (fuzzing, crash-injection testing,
years of edge-case hardening) not addressed here. This doc is scoped to:
if you sat down and wrote real SQL against AeroSLS today, what would you
hit that SQLite wouldn't stop you on?

Every gap below is read directly from `sql_parser.h`'s own header comment
(which already documents its grammar and an explicit "out of scope" list —
this doc mostly promotes that list into a phased plan) plus
`row_constraint.h`, `rowstore.h`, and `object_catalog.h`, not assumed.

**What already exists, real and working:** a hand-rolled lexer +
recursive-descent parser for `SELECT`/`INSERT`/`UPDATE`/`DELETE`, a real
`WHERE` predicate engine with `AND`/`OR` and standard precedence, `ORDER
BY`/`LIMIT`, a two-table `INNER JOIN`, B-tree indexing, MVCC concurrency,
transactions, and `UNIQUE`/`NOT NULL`/`RANGE`/`REFERENCE` constraints with
real enforcement (`mvcc.c` calls into `row_constraint.c` on every write, not
opt-in). That's a genuine relational core, not a toy.

**The gaps, in the order this doc sequences them:**

1. `GROUP BY`/`HAVING`/aggregate functions aren't in the SQL grammar at
   all — `COUNT`/`SUM`/`AVG`/`MIN`/`MAX` only exist via a separate
   `aggregate`/`mqt` command surface outside `sql_parse()`. You cannot
   write `SELECT dept, COUNT(*) FROM employees GROUP BY dept` as SQL text
   today — the single highest-value gap, since aggregate queries are
   extremely common and this isn't a matter of missing polish, it's a
   missing grammar production.
2. `JOIN` is capped at exactly two tables, `INNER` only, no table aliases
   (`sql_parser.h`'s own words: "explicitly out of scope: three-or-more-
   table joins, OUTER JOIN... table aliasing"). `employees.id`, not `e.id`.
3. The `WHERE`/`SET` expression language is minimal: no parenthesized
   grouping beyond natural `AND`-before-`OR` precedence, no `IN (...)`, no
   `LIKE`, no `IS NULL`/`IS NOT NULL`, no arithmetic. `comparison :=
   column_ref compare_op literal` is the entire expression grammar today.
4. There's no `NULL` as a real, representable value — `struct RowValues`
   (`rowstore.h`) is fixed-width strings with no null bitmap or sentinel
   type at all. `NOT NULL` constraint checking exists, but what it's
   actually checking against isn't a true tri-state column value the way
   SQLite has. No `BLOB` type either — the four column types are fixed at
   `STRING`/`UINT64`/`FLOAT`/`BOOL` (`object_catalog.h`), not SQLite's
   dynamic typing with type affinity.
5. There's no DDL in SQL text, at all. `row_constraint.h`'s own comment
   names this directly: "`sql_parser.c`'s grammar has no DDL at all
   today — tables are created via the pre-existing `sys_sls_schema_set` +
   `rowstore_create_table()` path." `CREATE TABLE`, `ALTER TABLE`, `DROP
   TABLE`, `CREATE INDEX`, and constraint registration are all
   direct-API-only, never `sql_parse()`-reachable.
6. `INSERT` requires a full, explicit column list covering every column in
   one row per statement — no multi-row `VALUES (...), (...), ...`, no
   partial-column insert with defaults.
7. No subqueries (scalar or `IN (SELECT ...)`), no views, no CTEs (`WITH`),
   no `UNION`/`INTERSECT`/`EXCEPT`, no window functions, no triggers — all
   explicitly named out of scope in `sql_parser.h`'s own comment and never
   revisited since.

---

## Phase 1 — GROUP BY / HAVING / aggregate functions in core SQL grammar — DONE

**Goal:** make `SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING
COUNT(*) > 2` real SQL text, not a separate `aggregate`/`mqt` command.

**What already exists and is reusable:** the `aggregate`/`mqt` command
surface (`sql_exec.c` or wherever it currently lives — confirm exact file
during implementation) already has working `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`
math and `group_by`/`having` filtering logic; this phase's job is mostly
grammar (recognize `GROUP BY`/`HAVING` and aggregate-function-call syntax
in `sql_parse()`, extend `struct SqlSelectStmt`) and wiring the parsed form
into the planner/executor path that already knows how to do the
computation, not reinventing the aggregation math.

**What needs to be built:** `GROUP BY column_ref (',' column_ref)*` and
`HAVING predicate` productions in `select_stmt`; recognizing
`COUNT(*)`/`COUNT(col)`/`SUM(col)`/`AVG(col)`/`MIN(col)`/`MAX(col)` as
valid `select_list` entries (a new AST node distinct from a plain
`column_ref`, since it carries a function kind + optional argument);
`sql_exec.c` executor support for grouping rows by key and evaluating
`HAVING` against each group's aggregate result before emitting it.

**Verification:** host test covering grouped aggregates over multiple
groups, `HAVING` filtering some groups out, `COUNT(*)` vs `COUNT(col)`
(the latter should exclude any not-really-NULL-yet-but-empty sentinel
values consistently — flag any inconsistency found here rather than
silently picking a convention); compile-check; regression sweep against
the existing `aggregate`/`mqt` host tests to confirm they still pass
unchanged (this phase adds a grammar path to the same math, it shouldn't
change the existing one).

### Scope correction made during implementation

The original draft assumed the existing `aggregate`/`mqt` command surface
(`kernel/aggregate.h`/`.c`, reachable via `POST /api/aggregate`) could be
wired into as-is, with this phase mostly being grammar work over shared
math. Reading `aggregate.c` directly disproved that: it operates
exclusively on the legacy `object_records[]` KV model via field-name-suffix
matching, and is structurally incompatible with the row-set model
(`rowstore.h`/`RowValues`) that `sql_exec.c` actually executes queries
against. This is the same "structural wall" `row_constraint.h`'s own header
comment names for why `row_constraint.c` had to be built as a new parallel
file next to legacy `constraint.c`, rather than extended — an established
precedent in this codebase (`row_index.c` next to `index_mgr.c`, `mvcc.c`
with no legacy analog). `exec_select_group()` in `sql_exec.c` is therefore
a fresh implementation over the row-set model, not a call into
`aggregate_exec()`. The existing `aggregate`/`mqt` KV-model command surface
is untouched and still works exactly as before (confirmed via the
unmodified `sql_exec_host_test`/`sql_join_host_test`/`sql_tx_host_test`/
`sql_parser_host_test` all still passing unchanged).

Grammar and executor design followed the same "bake canonical names into a
synthetic result layout" pattern Phase 20's JOIN established (`table.column`
qualified names baked into a query-time-only `RowTableLayout`): here,
rendered aggregate-call text like `COUNT(*)`/`SUM(amount)` is baked into
both the grouped result's synthetic column names and (for `HAVING`) the
predicate's comparison `column_name`, so `predicate_eval()`,
`compare_rows_by_column()`, and `cursor_open_rowset()` all "just work"
unchanged — zero changes needed to `predicate.h`. `MIN`/`MAX` reuse the
existing type-aware comparison logic via a new `compare_typed()` helper
extracted from `compare_rows_by_column()`, so they work correctly on
non-numeric columns (verified with a `STRING` column) and not just numbers.
`COUNT`/`SUM`/`AVG`/`MIN`/`MAX` were deliberately kept as ordinary
identifiers, not reserved keywords — disambiguated purely by lookahead for
an immediately-following `(` — matching real SQL practice and verified by
a dedicated regression test confirming a column literally named `count`
still parses as a plain column reference.

One real gap was found by the host test, not by inspection: `ORDER BY`'s
grammar had not been extended to accept aggregate-call syntax even though
`HAVING`'s had, so `ORDER BY COUNT(*) DESC` failed to parse. Fixed by
routing `ORDER BY`'s parsing through the same `parse_select_item()`
machinery `HAVING` already used, discarding the function/arg parts it
doesn't need.

Single-column `GROUP BY` only (no multi-column grouping); `GROUP BY`
combined with `JOIN` is an explicit, cleanly-rejected error
(`SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED`) rather than attempted, since row-set
JOIN's synthetic combined layout and GROUP BY's synthetic grouped layout
were not designed to compose — left for a future phase if ever needed. No
result-column aliasing (`COUNT(*) AS total`) — out of scope here, matches
Phase 2's JOIN aliasing being its own separate phase.

**Verification — actual results:** `tests/sql_group_phase1_host_test.c`
(new, 34 checks) covers grouped `COUNT`/`SUM`/`AVG`/`MIN`/`MAX` together,
`MIN`/`MAX` on a `STRING` column, `HAVING` with both `COUNT` and `AVG`,
bare aggregates with and without `GROUP BY` (including the zero-match case
correctly still returning one row per SQL semantics, not an empty result),
`ORDER BY`+`LIMIT` on a grouped/aggregate result, six distinct error paths
(non-grouped/non-aggregated column, `SUM()` on a `STRING`, unknown `GROUP
BY` column, `HAVING` referencing an aggregate not in the `SELECT` list,
unknown `ORDER BY` column, `GROUP BY`+`JOIN`), parser-level checks
(`SUM(*)` correctly rejected, `HAVING` without an aggregate correctly
rejected at parse time), and the `count`-as-column-name regression. Full
regression sweep (`tests/run_all.sh`): **28/28 host test files passing, 0
failed** — includes the new file plus all four other `sql_*_host_test.c`
files that link the modified `sql_parser.c`/`sql_exec.c` (`sql_exec_host_test`,
`sql_join_host_test`, `sql_parser_host_test`, `sql_tx_host_test`), all
unchanged. `net/http.c` compile-checked clean against the updated
`sql_exec.h`/`sql_parser.h` (pre-existing, unrelated `-Wimplicit-function-
declaration` warnings only — no errors, nothing touching the SQL/aggregate
change surface).

---

## Phase 2 — N-way JOIN + table aliasing (+ OUTER JOIN) — DONE

**Goal:** `SELECT e.name, d.name FROM employees e JOIN departments d ON
e.dept_id = d.id JOIN salaries s ON s.emp_id = e.id` — chained joins with
aliases, plus `LEFT JOIN` at minimum.

**What already exists and is reusable:** Phase 20's two-table `INNER
JOIN` already solved the hard part — qualified `table.column` resolution
across two row sources and a real `ON` predicate. This phase generalizes
that from "exactly two tables" to "a chain of `join_clause`s," and adds an
alias table so `e.id` resolves to `employees.id` without new resolution
logic, just an alias→real-name lookup ahead of the existing qualifier
resolution.

**What needs to be built:** `join_clause := (JOIN | LEFT JOIN) ident
[AS ident] ON predicate (join_clause)*` (repeatable, not fixed at one);
an alias map in `struct SqlSelectStmt`; executor changes to build a
multi-way join result incrementally (chain nested-loop joins left to
right — matching this codebase's consistent "smallest real version first"
posture rather than a real join-order optimizer, which is Phase 6 in the
reliability-adjacent sense but explicitly not attempted here); `LEFT
JOIN` NULL-padding for unmatched left-side rows, which depends on Phase 4
(real `NULL`) landing first or a documented interim sentinel.

**Verification:** host test with a 3-table chain confirming correct
row multiplication and column resolution; `LEFT JOIN` test confirming
unmatched rows appear once with the right-side columns empty/NULL rather
than being silently dropped; compile-check; regression sweep against the
existing Phase 20 two-table-join host test (should be a strict subset of
this phase's new behavior, not a behavior change).

### Scope correction made during implementation

The original draft framed this as mostly a grammar/alias-map addition on
top of Phase 20's existing two-table probe. Reading `exec_select_join()`
directly showed that its actual join logic (`join_outer_cb()`, a single
`mvcc_table_scan()` callback that both scanned table A and probed table B
inline) was written specifically for exactly two tables and couldn't be
parameterized to a third without restructuring — so this phase replaced it
with a real left-to-right chain: the FROM table is materialized into a
scratch buffer first, then each JOIN clause probes the new table once per
row currently in that buffer, writing matches into a second scratch buffer
that ping-pongs with the first across steps (`g_select_scratch`/
`g_join_scratch_b`, both static, matching this file's existing "static
scratch buffers, not stack locals" convention). The running combined
`RowTableLayout` grows by one table's worth of qualified columns per step,
so the same "bake the right names into a synthetic layout" trick Phase 20
used for exactly two tables needed zero changes to work for any chain
length — `predicate_eval()`/`find_column_index()`/`compare_rows_by_column()`
are still completely join-chain-unaware.

`SqlSelectStmt`'s old fixed `has_join`/`join_table`/`join_left_*`/
`join_right_*` fields (sized for exactly one JOIN) were replaced with
`join_count`/`joins[SQL_MAX_JOINS]` (a `struct SqlJoinClause` array) plus a
`table_alias` field for the FROM table itself — a real struct-layout
change, not additive, since the old fields had no room for a chain. Every
consumer of those old field names (`sql_parser.c`, `sql_exec.c`) was
updated together; nothing outside those two files touched them (confirmed
by search before making the change), so this wasn't a breaking change to
any external caller in practice.

A real regression surfaced during verification, not by inspection: the
first version of the generalized ON-clause resolution validated the
"outer" side (the side referencing some table already earlier in the
chain) with a single `find_column_index()` call against the running
combined layout's flat qualified namespace. That collapses two genuinely
different error conditions into one — "this qualifier doesn't name any
table in the chain" (`SQL_ERR_JOIN_INVALID`) and "this qualifier is fine
but that table has no such column" (`SQL_ERR_COLUMN_NOT_FOUND`) — because
a nonexistent qualified name and a qualified name with a typo'd column
both just fail to appear in the running layout's name list. The existing
Phase 20 regression test (`sql_join_host_test.c`, kept unchanged as the
backward-compatibility check) caught this immediately: its scenario 5
"`ON clause column missing from its table fails cleanly
(SQL_ERR_COLUMN_NOT_FOUND)`" check failed after the rewrite, reporting
`SQL_ERR_JOIN_INVALID` instead. Fixed by tracking a small `chain[]` array
of (display name → real per-table layout) pairs alongside the running
combined layout, so the outer side can be validated in the same two
separate steps Phase 20 originally used for its fixed pair: first "does
this qualifier name a table in the chain at all," then "does that
specific table's own real layout have this column" — restoring the
original error distinction for chains of any length, not just two tables.

Design decisions made explicit, matching this whole roadmap's "flag the
limitation, don't silently pick a convention" posture:
  - `SQL_MAX_JOINS = 3` (FROM + up to 3 JOINs = 4 tables total in one
    statement) — a real, named ceiling, sized against
    `ROWSTORE_MAX_COLUMNS = 16`'s own natural pressure rather than picked
    arbitrarily.
  - `AS` is optional, matching real SQL practice — a bare identifier
    immediately after a table name is unambiguously an alias, since no
    other production can start there (confirmed by enumerating every
    token that can legally follow a FROM/JOIN table name — all keywords,
    never a bare identifier).
  - `LEFT JOIN` pads an unmatched row with a documented per-type sentinel
    (`""`/`"0"`/`"false"`), not a real `NULL` — Phase 4 hasn't landed yet,
    so this is the "documented interim sentinel" option the original
    draft named as an alternative to blocking on Phase 4. Verified
    directly in the new host test (a `LEFT JOIN` row's unmatched side
    checked for the exact sentinel values, not just "doesn't crash").
  - Once a table has an alias, only that alias may qualify it — never both
    the alias and the real name in the same statement. This is what makes
    a genuine self-join (`employees e1 JOIN employees e2 ON e1.mgr_id =
    e2.id`) resolvable at all, and is exercised directly in the new host
    test as its own scenario (a manager-lookup self-join), not just
    asserted in prose.
  - `RIGHT`/`FULL OUTER` JOIN remain out of scope, as originally planned.
    `GROUP BY`/aggregates combined with any JOIN (not just a single one)
    remains `SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED` — Phase 1's scope cut,
    unchanged, now confirmed to still apply to the N-way case via a
    dedicated regression check.

**Verification — actual results:** `tests/sql_join2_phase2_host_test.c`
(new, 30 checks) covers a 3-table aliased INNER JOIN chain; `LEFT JOIN`
sentinel-padding for an unmatched row; a self-join via aliasing (manager
lookup); `WHERE` applied once against the fully combined 3-table row;
`ORDER BY DESC` + `LIMIT` on a joined chain result; the original Phase 20
unaliased two-table syntax still parsing and executing unchanged; six
error paths (unresolved ON qualifier, unknown joined table, combined
column count exceeding `ROWSTORE_MAX_COLUMNS`, exceeding `SQL_MAX_JOINS`,
`GROUP BY` + JOIN, permission denial through a chain); and parser-level
checks (`LEFT OUTER JOIN` with `OUTER` as a no-op, implicit vs. explicit
`AS` aliasing, multi-join `join_count`/alias capture). Full regression
sweep (`tests/run_all.sh`): **29/29 host test files passing, 0 failed** —
includes the new file, the original `sql_join_host_test.c` (24 checks,
now correctly re-passing after the outer-side error-distinction fix
above), and every other `sql_*_host_test.c`/`persist_rdbms_vecstore_host_test`
file that links the modified `sql_parser.c`/`sql_exec.c`, all unchanged.
`net/http.c` compile-checked clean against the updated `sql_exec.h`/
`sql_parser.h` (same pre-existing, unrelated `-Wimplicit-function-
declaration` warnings as Phase 1 — no errors, nothing touching the JOIN
change surface).

---

## Phase 3 — WHERE/SET expression richness — DONE

**Goal:** parenthesized grouping, `IN (...)`, `LIKE`, `IS NULL`/`IS NOT
NULL`, and basic arithmetic (`+ - * /`) in `WHERE`/`SET`/`HAVING`.

**What already exists and is reusable:** `predicate.h`'s `struct
Predicate` with its `add_comparison`/`add_and`/`add_or` primitives
(Phase 18) is the right extension point — parenthesized grouping is a
parser-side tree-shape change (build a real predicate tree instead of the
current flat `and_expr (OR and_expr)*` shape), not a new predicate
runtime; `IN`/`LIKE`/`IS NULL` are new comparison *kinds* added to the
same `struct Predicate` machinery, not a parallel system.

**What needs to be built:** real parenthesized `predicate` grouping
(currently absent per `sql_parser.h`'s own comment: "no explicit
parenthesized grouping in this first cut"); `IN (literal, literal, ...)`
and (once Phase 7's subqueries exist) `IN (SELECT ...)`; `LIKE` with `%`/`_`
wildcards; `IS NULL`/`IS NOT NULL` (depends on Phase 4 landing real NULL
first, or this phase and Phase 4 merge); a small arithmetic expression
grammar for numeric columns in `WHERE`/`SET` (`price * 1.1 > 100`).

**Verification:** host test per new predicate kind (grouping precedence,
`IN` membership, `LIKE` wildcard matching including edge cases like empty
pattern and literal `%`/`_` via escaping if scoped in, `IS NULL`
including the NULL-representation decision from Phase 4); compile-check;
regression sweep confirming existing flat `AND`/`OR` queries parse
identically (this phase changes the predicate tree's *shape* capability,
not its meaning for statements that don't use the new grouping).

### Scope correction made during implementation

Three of the four planned features needed **zero runtime changes** to
`predicate.c` — only `sql_parser.c` grammar work — confirming the original
draft's instinct that these are parser-side additions, not new predicate
machinery: parenthesized grouping reuses `eval_node()`'s existing recursion
through an arbitrarily-nested `AND`/`OR` tree (it was always the parser
that only ever built a flat shape); `IN (a, b, c)` desugars at parse time
into `(col = a) OR (col = b) OR (col = c)` via the already-existing
`predicate_add_comparison()`/`predicate_add_or()` primitives, no new node
kind; `LIKE` is a new `PRED_OP_LIKE` value on the existing
`PredicateCompareOp` enum, not a new node kind either (`%`/`_` wildcards
only, `STRING` columns only — fails closed, no coercion, for non-`STRING`
columns; no `ESCAPE` clause). Arithmetic (`+ - * /`) is the one genuinely
new piece of machinery: a new `PRED_NODE_ARITH_COMPARISON` node kind,
deliberately scoped to **at most one operation** per comparison (not a
full precedence-climbing expression grammar) — `operand [+-*/] operand
compare_op literal` — where each operand is a single unqualified
identifier or numeric literal. Division by zero and non-numeric column
operands (`STRING`/`BOOL`) both fail closed (comparison evaluates false,
not a crash or `inf`/`NaN`). Arithmetic is available in plain `WHERE`
comparisons and `UPDATE ... SET` values; it is explicitly NOT available on
`HAVING`'s aggregate-label comparison form, and NOT inside `ON` clauses.

`IS NULL`/`IS NOT NULL` was **deliberately deferred whole to Phase 4**,
not built here and not merged — the original draft named "depends on
Phase 4 landing real NULL first, or this phase and Phase 4 merge" as the
two options; this phase chose to stay narrower rather than build against
a `NULL` representation that doesn't exist yet. `NOT IN` and `LIKE`'s
`ESCAPE` clause are likewise named out of scope, not oversights (both
verified via a dedicated parser-level test confirming they fail to parse
cleanly rather than silently misbehaving).

A real, if narrow, **lexer ambiguity** was found and deliberately left
narrow rather than fixed more invasively: the pre-existing number-literal
rule (`-` immediately followed by a digit → a single negative-number
token) is checked before the new `+`/`-`/`/` operator tokens, unconditionally.
This means `price - 1` (space on both sides of `-`) parses correctly as
subtraction, but `price -1` or `price-1` (no space between `-` and the
digit) still lex as a single negative-number token and fail to parse as
subtraction. Fixing this properly would mean moving negative-literal
parsing out of the lexer and into every `parse_literal()` call site — many
call sites, meaningfully higher regression risk for a rarely-hit edge
case — so this phase named the limitation explicitly instead of guessing.
Subtracting a column (`price - discount`) is unaffected, since columns
never start with a digit.

A real **regression was caught during verification**, not by inspection:
the one-operand lookahead `parse_comparison()` needs to disambiguate a
plain column reference from the start of an arithmetic expression
(`parse_arith_operand()`) was written to accept only a single unqualified
`TOK_IDENT` — silently breaking every qualified `table.column`/`alias.column`
reference on a plain `WHERE` comparison's left-hand side, since
`e.name != 'alice'` would parse `e` as the operand and leave `.name !=
'alice'` unconsumed. This didn't show up in this phase's own new host test
(which only exercises single-table queries), but the full regression sweep
caught it immediately: `sql_join2_phase2_host_test.c` scenario 4 and three
scenarios in `sql_join_host_test.c` (the Phase 20 regression test) started
failing — every one of them a `WHERE` clause with a qualified column name
on a joined query. Fixed by teaching `parse_arith_operand()` to also
consume an optional `.ident` qualifier suffix (byte-identical to
`parse_column_ref()`'s own dot-handling), then having `parse_comparison()`
check for a `.` in the parsed operand text and — when present — skip the
arithmetic-operator lookahead entirely and fall straight through to the
byte-compatible `parse_comparison_tail()` path, restoring pre-Phase-3
behavior for every qualified `WHERE` comparison exactly. (A qualified
column was never meant to be a valid arithmetic operand per this phase's
own scope — the fix only restores the *non*-arithmetic path for qualified
names, it doesn't extend arithmetic to JOINs.)

Design decisions made explicit, matching this whole roadmap's "flag the
limitation, don't silently pick a convention" posture:
  - `UPDATE ... SET` arithmetic reuses the exact same `PredArithOperand`/
    `PredArithOp`/`predicate_eval_arith()` machinery as `WHERE`, via new
    additive parallel arrays on `SqlUpdateStmt` (`set_is_arith[]`/
    `set_arith_op1[]`/`set_arith_op[]`/`set_arith_op2[]`) added alongside
    the untouched pre-existing `set_columns[]`/`set_values[]`/`set_count`
    — specifically so `tests/sql_parser_host_test.c`'s direct reads of
    those original fields keep working unchanged.
  - When an `UPDATE` has multiple `SET` assignments, every arithmetic
    expression is computed from the row **as it was before the
    statement** — a pristine pre-`UPDATE` snapshot — not chained in
    left-to-right order. `SET a = a + 1, b = a` sees `b` get `a`'s OLD
    value, matching standard SQL semantics rather than implementation-
    order-dependent behavior. Implemented by computing every `SET`
    value into a scratch array first (all reads against the untouched
    fetched row), then applying the whole scratch array to the row only
    after every value is computed.
  - An arithmetic `SET` failing to evaluate (non-numeric operand,
    division by zero) aborts the whole statement with
    `SQL_ERR_VALUE_INVALID` and zero affected rows — matching this
    function's existing write-conflict/constraint-violation "fail
    cleanly, no partial effects" posture, not a partial update.
  - `try_index_assisted_eq()` needed no changes at all: its existing guard
    (`n->kind != PRED_NODE_COMPARISON || n->op != PRED_OP_EQ`) already
    correctly declines the index fast-path for both new cases
    (`PRED_NODE_ARITH_COMPARISON` is the wrong kind, `PRED_OP_LIKE` is the
    wrong op), falling back to a full scan safely — confirmed by reading
    the guard directly, not just assumed.

**Verification — actual results:** `tests/sql_expr_phase3_host_test.c`
(new, 34 checks) covers parenthesized grouping actually changing
precedence versus the same clause ungrouped (not just parsing); `IN`
membership including zero-match and single-value cases; `LIKE` with `%`
and `_` wildcards, a non-matching pattern, and LIKE against a non-`STRING`
column failing closed; arithmetic `WHERE` comparisons (`price * 2 > 40`,
`price + 5 = 15`) and division-by-zero failing closed; arithmetic `SET`
(`price = price * 1.1`), a multi-assignment `UPDATE` mixing one arithmetic
and one literal `SET`, and the pre-`UPDATE`-snapshot old-value semantic;
an arithmetic `SET` referencing a non-numeric column failing closed with
`SQL_ERR_VALUE_INVALID`; parser-level checks confirming `NOT IN`, `IS
NULL`, and multi-operation arithmetic all correctly fail to parse (named
out-of-scope items, not silent misbehavior); and permission denial
propagating cleanly through the new `LIKE`/arithmetic predicate kinds.
Full regression sweep (`tests/run_all.sh`): **30/30 host test files
passing, 0 failed** — includes the new file and, after the qualified-
column regression fix above, every JOIN test (`sql_join_host_test.c`,
`sql_join2_phase2_host_test.c`) and every other `sql_*_host_test.c` file
unchanged. `net/http.c` compile-checked clean against the updated
`sql_exec.h`/`sql_parser.h`/`predicate.h` (same pre-existing, unrelated
`-Wimplicit-function-declaration` warnings as Phases 1 and 2 — no errors).

---

## Phase 4 — Real NULL + BLOB, dynamic-typing groundwork — DONE

**Goal:** `NULL` as a genuine third state distinct from `""`/`0`, and a
`BLOB` column type — closing the type-system gap named in §0.4.

**What already exists and is reusable:** `RowConstraintKind
ROW_CONSTRAINT_NOT_NULL` already exists and is enforced
(`row_constraint_check_write()`) — this phase needs to give it something
real to check against instead of (presumably) an empty-string convention;
confirm exactly what today's `NOT_NULL` check compares against during
implementation before assuming it needs to change, since it may already
be checking a convention this phase can just formalize rather than
replace.

**What needs to be built:** a null bitmap or sentinel byte added to
`struct RowValues` (`rowstore.h`) — a real per-column "is this NULL"
flag, not an encoding trick inside the existing fixed-width string slot;
`SLSFieldType` (`object_catalog.h`) gains `FIELD_TYPE_BLOB`; parser/
executor support for the `NULL` literal in `INSERT`/`UPDATE` and `IS
NULL`/`IS NOT NULL` predicates (Phase 3); `field_type_name()` and every
serializer touching `RowValues` (JSON routes in `net/http.c`, the
Terminal's `sql`/`select` output formatting) updated to round-trip NULL
correctly instead of rendering it as an empty string indistinguishable
from a real empty string value.

**Verification:** host test confirming NULL survives an INSERT → SELECT
round-trip distinctly from an empty string / zero; `NOT NULL` constraint
correctly rejects a real NULL insert (and, importantly, confirms it did
*not* already silently work via the empty-string convention pre-phase —
name explicitly whichever is true); compile-check across every touched
serializer; regression sweep for `row_constraint_journal_host_test` and
`rowstore_host_test` (both touch `RowValues` shape directly and are the
most likely to break silently from a struct layout change).

### Scope correction made during implementation

Investigation confirmed the "presumably an empty-string convention"
hedge in the original draft was exactly right: pre-Phase-4, `NOT_NULL`/
`UNIQUE`/`RANGE`/`REFERENCE` all checked `strlen(val) == 0` against the
fixed-width string slot — there was no real NULL anywhere in the
codebase, just an overloaded empty string standing in for it. This phase
replaced that convention with a genuine third state rather than
formalizing it: `struct RowValues` (`rowstore.h`) gained a `uint16_t
null_mask` bitmap (bit *c* set ⇒ column *c* is really NULL), which is a
**one-way, on-disk row-format change** (row width +2 bytes — byte 0 is
still the tombstone, bytes 1–2 are now the null mask, column data starts
at byte 3 instead of byte 1). This has no migration path and none was
built, matching the exact precedent set by Phase 2's JOIN struct-layout
change and every prior Vector Store roadmap phase that changed an
on-disk shape: acceptable because this is a research/dev codebase with
no real deployed data, and named explicitly here rather than silently
shipped.

Because the old `strlen(val) == 0` check conflated "empty string" and
"NULL," fixing it to read the real `null_mask` bit is a genuine,
intentional **behavior correction**, not just a refactor: `UNIQUE`
previously allowed two rows with an empty-string value in a UNIQUE
column to silently coexist (both looked like "NULL" and were exempted);
now a real empty string is a normal, comparable value and only an actual
NULL is exempt. `RANGE` previously would have tried to numeric-parse an
empty string and failed the range check; now a real NULL vacuously
passes RANGE (matching standard SQL constraint semantics), while an
empty string is checked as a real value. `REFERENCE` got the same
correction on both the write-side FK check and the delete-side
referenced-row scan. This was caught concretely, not just reasoned
about: `row_constraint_journal_host_test.c`'s own scenario 3 asserted
that an empty-string tag violates `NOT NULL` — true only under the old
convention — and needed rewriting to use a real `NULL` literal, with a
new companion scenario 3b added confirming an empty string is *not* a
`NOT NULL` violation post-fix.

SQL's tri-valued logic (`TRUE`/`FALSE`/`UNKNOWN`) was deliberately
**not** implemented in full. Instead, NULL operands collapse to this
codebase's existing "fail closed" pattern already used throughout
`predicate.c` for unresolvable columns and literals: NULL in an ordinary
comparison, `LIKE`, or arithmetic operand always makes the containing
comparison evaluate false. The one deliberate exception is `IS NULL`/
`IS NOT NULL`, which inspect the `null_mask` bit directly rather than
failing closed — matching standard SQL's own special-casing of that one
operator pair. `= NULL` was deliberately **not** given special meaning
(only the dedicated `IS [NOT] NULL` operator resolves NULL), verified by
a parser-level test confirming `WHERE col = NULL` fails to parse
cleanly, sidestepping the ambiguity standard SQL itself warns against.

`IS NULL`/`IS NOT NULL` reused Phase 3's "reuse the leaf shape, add an
op" precedent exactly: `PRED_OP_IS_NULL`/`PRED_OP_IS_NOT_NULL` are new
values on the existing `PRED_NODE_COMPARISON` leaf (no new node kind).
`NULL` as an `INSERT`/`UPDATE` value follows Phase 3's additive-
parallel-array convention: `SqlInsertStmt.is_null[]` parallel to
`values[]`, `SqlUpdateStmt.set_is_null[]` parallel to `set_values[]`/
`set_is_arith[]` — every pre-Phase-4 statement's original fields are
untouched, so `tests/sql_parser_host_test.c`'s direct field reads keep
working unchanged.

`BLOB` scope was kept deliberately narrow: `FIELD_TYPE_BLOB` is stored
and compared exactly like `STRING` (same 64-byte inline slot, same
text-in/text-out API boundary). There is no true large-object/TOAST
overflow mechanism and no base64/binary-safe encoding at the SQL text
layer — it exists purely so a schema can name its intent ("this is
binary data") distinctly from `STRING`, not as a new storage class.

**Explicitly out of scope, named rather than silently dropped:**
LEFT JOIN's sentinel-padding (`""`/`"0"`/`"false"`, from Phase 2) was
**not** retrofitted to use real NULL — revisiting already-tested JOIN
code was judged higher regression risk than benefit for this phase.
Aggregate functions (`SUM`/`AVG`/`COUNT`, Phase 1) do **not** skip NULL
columns the standard-SQL way. The frontend (`slsos-sim`) has no NULL-
aware rendering — only the JSON API round-trip guarantee (`null` in
JSON, via `net/http.c`'s per-column serializer) is this phase's actual
wire-format deliverable.

**Verification — actual results:** `tests/sql_null_phase4_host_test.c`
(new, 37 checks) covers a real NULL INSERT → SELECT round-trip distinct
from empty string/zero; `NOT NULL` rejecting a real NULL while an empty
STRING passes (the corrected behavior, not the old convention); `UNIQUE`
no longer treating two empty strings as both-NULL-exempt; `RANGE`
vacuously passing for NULL; `REFERENCE` exempting NULL on both insert
and delete paths; `IS NULL`/`IS NOT NULL` in `WHERE`; NULL failing closed
in `=`, arithmetic, and `LIKE`; `UPDATE ... SET col = NULL` and back to a
real value; a `BLOB` column round-trip; and parser-level checks (`=
NULL` fails to parse, `is_null[]`/`set_is_null[]` set correctly).

Regression sweep surfaced three real, expected fallout failures from the
row-format and constraint-semantics changes — all investigated and
fixed, not silently patched over: `rowstore_host_test.c` hardcoded
`row_width == 74`/`rows_per_page == 55` and specific row-index page-
boundary assertions computed from the pre-Phase-4 layout; updated to the
new `row_width == 76`/`rows_per_page == 53` and the corresponding
boundary rows. `row_constraint_journal_host_test.c`'s scenario 3 (see
above) was corrected to test real NULL instead of empty-string, plus a
new scenario 3b; this fix's own test-data change then exposed an
**unrelated authoring bug** — the replacement row used `id=30`, which
collided with a *different*, pre-existing scenario 12 that also inserts
`id=30` inside a rollback test, so scenario 12's post-rollback SELECT
found the still-present row from scenario 3b's insert instead of a
truly-absent row. Fixed by moving scenario 3b to an unused id; confirmed
this was a test-authoring collision, not a kernel bug, by re-running
clean afterward. `persist_rdbms_vecstore_host_test.c` built `struct
RowValues` values directly on the stack (bypassing the parser/executor
entirely) and relied on the same old empty-string-as-NULL convention for
one assertion, plus left `null_mask` uninitialized (reading stack
garbage) in three other hand-built `RowValues` — a real latent bug this
phase's struct change exposed rather than caused, since a zero-init
convention happened to mask it before `null_mask` existed. Fixed by
explicitly initializing `null_mask` in every hand-built `RowValues` in
that file and updating the NOT NULL assertion to set the real null bit
instead of relying on an empty string. A fourth failure,
`sql_expr_phase3_host_test.c` scenario 7c, was Phase 3's own regression
test asserting `IS NULL` "fails to parse cleanly (named out-of-scope
item)" — correct when Phase 3 shipped, false now that Phase 4 promoted
`IS NULL` out of scope-cut status; updated to assert it parses cleanly.

Full regression sweep (`tests/run_all.sh`): **31/31 host test files
passing, 0 failed** — includes the new file and all four fixes above.
`net/http.c` compile-checked clean against the updated `rowstore.h`/
`predicate.h`/`sql_parser.h`/`sql_exec.h` (only the same pre-existing,
unrelated implicit-function-declaration warnings seen in every prior
phase — no errors).

---

## Phase 5 — DDL in SQL text (CREATE/ALTER/DROP TABLE, CREATE/DROP INDEX) — DONE

**Goal:** `CREATE TABLE employees (id UINT64, name STRING)`, `ALTER TABLE
employees ADD COLUMN dept STRING`, `DROP TABLE employees`, `CREATE INDEX
idx1 ON employees (dept)` as real SQL text.

**What already exists and is reusable:** every piece of machinery this
phase needs already exists and works, per `row_constraint.h`'s own
comment — `sys_sls_schema_set`/`rowstore_create_table()` for table
creation, `row_index_create()` for indexes, `row_constraint_add_*()` for
constraints. This phase is entirely new *grammar* that calls the same
existing functions the direct-API/HTTP path already calls, not new
storage-engine work — matching the exact gap that comment names as
"a future 'make DDL live' phase."

**What needs to be built:** `create_table_stmt`, `alter_table_stmt`
(`ADD COLUMN` only for v1 — `DROP COLUMN`/`RENAME` are real storage-layout
changes to `RowTableLayout` and can be scoped separately if `ADD COLUMN`
alone proves insufficient), `drop_table_stmt`, `create_index_stmt`,
`drop_index_stmt` productions in `sql_parser.c`; `sql_exec.c` dispatch for
each straight into the existing functions named above; column-type
keywords (`STRING`/`UINT64`/`FLOAT`/`BOOL`, plus `BLOB` if Phase 4
landed first) recognized in `CREATE TABLE`'s column-definition list, and
inline `UNIQUE`/`NOT NULL`/`REFERENCES table(col)` constraint syntax
translated into the corresponding `row_constraint_add_*()` calls at
`CREATE TABLE` time.

**Verification:** host test creating a table via SQL text and confirming
it's indistinguishable from one created via the direct API (same
`RowTableLayout`, same queryable via `SELECT`); `ALTER TABLE ADD COLUMN`
test confirming existing rows get a sensible default/NULL for the new
column, not corruption; `DROP TABLE`/`DROP INDEX` test confirming clean
teardown with no dangling `row_constraints[]`/`row_index` entries left
pointing at a freed `table_object_id`; compile-check; full regression
sweep (this phase touches the shared DDL functions every other phase's
tests also call, so regressions here would be broad and worth catching
early).

### Scope correction made during implementation

The original draft underestimated `ALTER TABLE ADD COLUMN` as pure
grammar wiring onto `sys_sls_schema_set()`. Investigation found
`sys_sls_schema_set()` has a **post-promotion freeze guard**: it rejects
any schema change once `object_catalog[idx].uses_rowstore` is true (return
code 4) — a deliberate boundary from Phase 24, not an oversight. Once a
table is promoted to row-set storage its `row_width`/`rows_per_page` are
computed once at `rowstore_create_table()`/`compute_layout()` time and
baked into every page's fixed-width slot layout, so adding a column is
architecturally incompatible with any in-place widening. `ADD COLUMN`
therefore required a genuine new kernel function, `rowstore_add_column()`:
collects every existing row via `rowstore_table_scan()`, writes the new
field directly into `object_schemas[]` (bypassing the freeze guard
deliberately, since this *is* the sanctioned mutation path now),
recomputes the layout, and rewrites all rows into freshly allocated pages
at the new width — existing rows get a real NULL in the new column, not
garbage or corruption. Matching this whole subsystem's pre-existing "no
reclaim in first cut" posture (`rowstore_alloc_page()` never frees pages;
`row_indexes[]`/`row_constraints[]` are bump-allocated with no
compaction), the old, narrower pages are simply abandoned after migration
— a deliberate, named scope cut, not a silent leak.

`DROP TABLE` had the same kind of gap: `sys_sls_vfree()` alone was
confirmed via direct read to have zero awareness of row-set-specific
state. A new `rowstore_drop_table()` was built to do the real cleanup —
deactivating the matching `row_indexes[]`, `row_constraints[]`, and
`row_journal_attachments[]` entries and `table_headers[]`/`uses_rowstore`
before calling `sys_sls_vfree()` — so `DROP TABLE` doesn't leave dangling
index/constraint/journal entries pointing at a freed `table_object_id`.
A companion `row_index_drop()` was added to `row_index.c` (deactivates a
named index's `row_indexes[]` slot; same no-reclaim posture as everything
else in that subsystem) to support both `DROP INDEX` and the index-rebuild
step below.

Any index defined on a table is rebuilt (not patched) after `ADD COLUMN`'s
page migration, reusing the exact drop-then-recreate pattern `persist.c`'s
own boot-restore path already established for `row_index_create()`.
Testing surfaced a real, previously-latent bug the same migration exposed
in a second subsystem: `mvcc.c` caches `logical_id → physical_id` in its
own `mvcc_versions[]` array at INSERT/UPDATE time, and `mvcc_row_get()`
resolves purely through that cache — a raw rowstore-level page migration
like `ADD COLUMN`'s is invisible to it unless told to rebuild. Caught
concretely: a test asserting a migrated row's new column read back as
NULL kept reading stale data from the old, abandoned physical page instead
(misinterpreted under the new, wider layout). Fixed with a new
`mvcc_rebuild_versions_for_table()` (mirrors `mvcc_bootstrap_from_rowstore()`'s
own boot-restore pattern), called from `sql_exec.c`'s `exec_alter_table()`
on success — deliberately **not** called from inside `rowstore_add_column()`
itself, to preserve this codebase's existing layering discipline
(`rowstore.c` has never depended on `mvcc.h`; the dependency runs the
other way) and avoid silently breaking every pre-Phase-5 host test that
links `rowstore.c` without `mvcc.c`.

`CREATE TABLE`'s three-step chain (`sys_sls_valloc()` →
`sys_sls_schema_set()` looped per column → `rowstore_create_table()`,
plus inline constraint registration for `NOT NULL`/`UNIQUE`/`REFERENCES`)
is **not transactional**: if a later step fails, earlier steps are not
rolled back. This deliberately matches the pre-existing HTTP route's own
established `api_schema_set_post()` behavior ("stop at first failure,
don't unwind") rather than introducing new atomicity this codebase doesn't
have anywhere else yet.

**Verification — actual results:** `tests/sql_ddl_phase5_host_test.c`
(new, 36 checks) covers `CREATE TABLE` with inline constraints and
confirms they actually enforce; `CREATE INDEX` finding rows; `ALTER TABLE
ADD COLUMN` migration correctness (existing rows get a real NULL in the
new column, the table's index is rebuilt and still works); `DROP INDEX`
genuinely stopping the index from being used; `DROP TABLE` leaving no
dangling `row_constraints[]`/index entries; real permission-denial
enforcement via genuine ownership/role checks (this test links the real
`object_catalog.c`, unlike most prior DDL-adjacent tests which stub
`catalog_check_access()`); and parser-level edge cases.

Regression sweep initially broke 15 of the other 31 host test files —
entirely link errors, not logic bugs — because `sql_exec.c`'s
`exec_create_table()` and `rowstore.c`'s new `rowstore_drop_table()`/
`rowstore_add_column()` now unconditionally reference `sys_sls_valloc`/
`sys_sls_schema_set`/`sys_sls_vfree` (only defined in `object_catalog.c`)
and, for `rowstore.c` specifically, `row_indexes[]`/`row_constraints[]`/
`row_journal_attachments[]`/`row_index_create()`/`row_index_drop()`/the
`persist_row_*()` functions. Every host test linking `sql_exec.c` and/or
`rowstore.c` without those real files needed new stubs. Fixed per this
codebase's established convention (stub, don't link, to keep each test's
dependency graph as narrow as its own header comment commits to):
`legacy_rowstore_boundary_host_test.c` (links the real `object_catalog.c`,
so `sys_sls_valloc/schema_set/vfree` already resolved, but not
`row_index.c`/`row_constraint.c`/`row_journal.c`) gained stub globals for
`row_indexes[]`/`row_constraints[]`/`row_journal_attachments[]` and stub
`row_index_create()`/`row_index_drop()`/`persist_row_index_defs()`/
`persist_row_constraints()`/`persist_row_journal()`. Thirteen other files
gained three new failure-code no-op stubs (`sys_sls_valloc()`/
`sys_sls_schema_set()`/`sys_sls_vfree()`, or a subset — `mvcc_host_test`/
`predicate_host_test`/`rowstore_host_test` only needed `sys_sls_vfree()`
plus a stub `row_index_drop()`; `row_index_host_test`/
`persist_rdbms_vecstore_host_test` only needed `sys_sls_vfree()`, since
both already link the real `row_index.c`). None of these older tests
exercise SQL-text `CREATE`/`DROP TABLE` at runtime, so no-op stubs are
correct, not a gap — real coverage of those paths lives entirely in the
new `sql_ddl_phase5_host_test.c`.

Full regression sweep (`tests/run_all.sh`): **32/32 host test files
passing, 0 failed** — includes the new file and all 14 stub fixes above.
`net/http.c` compile-checked clean against the updated `rowstore.h`/
`sql_parser.h`/`sql_exec.h`/`mvcc.h`/`row_index.h` (only the same
pre-existing, unrelated implicit-function-declaration warnings seen in
every prior phase — no errors).

---

## Phase 6 — Multi-row INSERT + partial-column INSERT — DONE

**Goal:** `INSERT INTO t (a, b) VALUES (1, 'x'), (2, 'y'), (3, 'z')` and
`INSERT INTO t (a) VALUES (1)` (other columns get NULL/default, not a
parse error).

**What already exists and is reusable:** `rowstore_row_insert()`
(`rowstore.h`) already takes one `struct RowValues` per call — this phase
is a parser/executor loop over multiple parsed value-tuples calling the
existing single-row insert path once per row (a transaction-scoped loop,
matching how `mvcc.c` already wraps individual writes), not a new bulk-
insert primitive.

**What needs to be built:** `VALUES '(' literal-list ')' (',' '('
literal-list ')')*` grammar; partial-column INSERT filling any column not
named in the statement with NULL (Phase 4) rather than requiring
`rowstore_row_insert()`'s current all-columns-required contract to be
satisfied by the caller — worth confirming during implementation whether
that contract lives in `rowstore.c` itself or only in `sql_exec.c`'s
caller-side assembly of the `RowValues`, since that determines whether
this phase touches `rowstore.c` at all or stays entirely in the SQL layer.

**Verification:** host test for multi-row insert (confirm all rows land,
in order, in one statement); partial-column insert test confirming
omitted columns are NULL, not zero-filled/corrupted; a mixed-transaction
test (multi-row INSERT inside an explicit transaction, then rollback)
confirming MVCC treats all rows from one statement as atomic together;
compile-check; regression sweep.

### Scope correction made during implementation

Investigation confirmed the draft's own open question cleanly: the "every
column must be named" contract lived entirely in `sql_exec.c`'s
`exec_insert()` (`if (s->count != layout->column_count) ...`), never in
`rowstore_row_insert()`/`mvcc_row_insert()` — both always accepted
whatever `RowValues` the caller handed them, with no enforcement of which
columns the SQL text actually named. Phase 6 therefore stayed entirely in
the SQL layer (`sql_parser.c`/`sql_parser.h`/`sql_exec.c`/`sql_exec.h`),
touching neither `rowstore.c` nor `mvcc.c` — exactly as the roadmap draft
predicted this outcome would look like.

`SqlInsertStmt` kept its original `values[]`/`is_null[]` fields as row 0,
byte-for-byte identical to every pre-Phase-6 caller, and added
`extra_values[]`/`extra_is_null[]`/`extra_row_count` for additional
`VALUES` tuples — the same "parallel array, zero-default, byte-compatible
when unused" convention every prior phase in this roadmap has used. A
real, named ceiling (`SQL_INSERT_MAX_EXTRA_ROWS = 7`, so 8 rows total per
statement) was chosen to keep `sizeof(struct SqlInsertStmt)` from
exceeding `sizeof(struct SqlSelectStmt)`, which already sets `struct
SqlStatement`'s union footprint — this is scoped as a modest ergonomic
feature for a handful of rows per statement, not a bulk-loader primitive,
consistent with the 512-byte `SQL_MAX_TEXT_LEN` input cap already making a
much larger row count impractical to type as one statement.

Partial-column INSERT needed no new parser field at all: `count` (the
named-column count) simply may now be less than the table's full column
count, and `exec_insert()` fills any table column absent from the
statement with a real NULL (Phase 4's `null_mask`) rather than rejecting
it. The executor was restructured around a new `build_insert_row_values()`
helper (shared by every row in a multi-row statement, since all tuples use
the same column list) that resolves each named column's index once and
marks every *unnamed* column NULL — explicit-NULL and omitted-column both
resolve to the same real null bit, deliberately indistinguishable at the
storage layer. Naming *more* columns than the table has remains a real,
distinct error (`SQL_ERR_COLUMN_COUNT_MISMATCH`) — Phase 6 only relaxed
"fewer," never "more."

Multi-row atomicity needed **no new transaction plumbing**: `sql_execute()`
already wraps one whole statement in a single `mvcc_begin()`/
`mvcc_commit()`/`mvcc_rollback()` (Phase 22). `exec_insert()` simply loops
its rows against the same already-open `txn_id` and stops at the first
failure; the existing autocommit wrapper then rolls back every row the
statement had already inserted, for free, via the exact mechanism that
already rolled back a single failed INSERT before this phase. This was
verified concretely, not just reasoned about: a constraint violation on
the *second* tuple of a 3-row batch was confirmed to roll back the *first*
tuple's already-inserted row too, and the same held for an explicit
`sql_execute_tx()`/`sql_tx_rollback()` transaction wrapping a multi-row
INSERT.

`SqlResult.inserted_id` keeps its pre-Phase-6 single-`MvccRowId` shape
rather than growing an array — for a multi-row INSERT it now reports the
*last* row's id, matching common SQL client convention (e.g.
`LAST_INSERT_ID`). `affected_rows` reports the count of rows actually
inserted before any failure. Callers that need every inserted row's id
back are expected to `SELECT` them afterward, matching how this codebase
has consistently avoided growing wire-format arrays for what would
otherwise be a rare need.

**Verification — actual results:** `tests/sql_insert_phase6_host_test.c`
(new, 49 checks) covers a 3-row INSERT landing every row in order;
partial-column INSERT with omitted columns confirmed as real NULL (not
zero-filled); multi-row + partial-column + explicit-NULL combined in one
statement; a mid-batch `UNIQUE` violation rolling back the *entire*
autocommit statement including already-inserted earlier rows (confirmed
by re-querying for genuine absence, not just a reported error); the same
atomicity under an explicit `sql_execute_tx()` transaction, both on
rollback and on commit; parser-level checks (`extra_row_count` correctness,
a later tuple's wrong arity still failing to parse, naming more columns
than the table has still rejected as `SQL_ERR_COLUMN_COUNT_MISMATCH`
distinct from a same-count-but-wrong-name `SQL_ERR_COLUMN_NOT_FOUND`, and
an ordinary single-row INSERT still parsing with `extra_row_count == 0`);
and permission denial failing a multi-row INSERT cleanly with no partial
row left behind.

Regression sweep surfaced two real, expected fallout failures from the
deliberate "partial-column INSERT is no longer an error" behavior change
— both pre-existing tests that had asserted the OLD "missing a column is
rejected" behavior as a real check, now stale by design, not a bug: in
`sql_exec_host_test.c`, scenario 2's `INSERT INTO employees (id, name)
VALUES (99, 'x')` (omitting `active`) used to correctly fail and now
correctly succeeds; the check was repointed at the still-real "naming more
columns than the table has" error instead of being deleted, preserving
the original `SQL_ERR_COLUMN_COUNT_MISMATCH` assertion under its accurate
new condition. The same fix was applied to `sql_tx_host_test.c`'s
scenario 2. Both fixes were verified to not just silence the failure but
restore a real, still-meaningful check — the old row-count-mismatch
assertion is preserved, just triggered by the opposite (still-erroring)
condition, and downstream row-count assertions elsewhere in both files
were confirmed unaffected since the fix avoids ever actually inserting the
row (the too-many-columns case fails before any row is written).

Full regression sweep (`tests/run_all.sh`): **33/33 host test files
passing, 0 failed** — includes the new file and both fixes above.
`net/http.c` compile-checked clean against the updated `sql_parser.h`/
`sql_exec.h` (only the same pre-existing, unrelated implicit-function-
declaration warnings seen in every prior phase — no errors); its SQL
route needed no changes at all, since it already passes raw SQL text
through to `sql_execute()` and serializes `SqlResult` generically
(`affected_rows` already existed as a JSON field).

---

## Phase 7 — Subqueries, views, CTEs, set operations — DONE

**Goal:** the remaining, most SQLite-like-but-hardest gaps: scalar and
`IN (SELECT ...)` subqueries, `CREATE VIEW`, `WITH` CTEs, and
`UNION`/`INTERSECT`/`EXCEPT`.

**Sequenced last, deliberately:** every one of these either depends on
infrastructure from earlier phases (views need DDL from Phase 5;
`IN (SELECT ...)` needs Phase 3's `IN` grammar already in place) or is a
substantially bigger executor change than anything above — a subquery
means the executor can recursively invoke itself and materialize an
intermediate result set, which today's `sql_exec.c` has never needed to
do (every existing query, including the two/N-table join, resolves
directly against real stored tables, never against another query's
output). This is the point where "extend the existing planner" stops
being accurate framing and "build a real nested-execution model" starts
being the honest one — worth scoping as its own follow-on doc once
Phases 1-6 are in and there's a clearer read on how much of this is
actually wanted, rather than committing to detailed sub-phases here for
work that's still fairly speculative.

**What's known now:** scalar subqueries (`WHERE dept_id = (SELECT id FROM
departments WHERE name = 'Eng')`) are the smallest real slice — a
subquery that must return exactly one row/column, executed once,
substituted as a literal into the outer predicate; `IN (SELECT ...)` is
the next smallest, since it reuses Phase 3's `IN` grammar with a subquery
in place of a literal list; correlated subqueries, views, CTEs, and set
operations are all bigger asks than a first cut needs to include and are
left unscoped until the smaller slice ships and proves out the
recursive-execution approach.

**Verification (once scoped in detail):** whatever this phase becomes
should follow the same bar as every phase above — host test, compile-
check, full regression sweep — but the detailed test plan isn't written
here since the implementation approach itself isn't committed yet.

### Scope correction made during implementation

The "smallest real slice" framing above held exactly as written: this
phase shipped non-correlated scalar and `IN (SELECT ...)` subqueries in a
plain (non-JOIN, non-aggregate) SELECT/UPDATE/DELETE's WHERE clause.
`CREATE VIEW`, `WITH` CTEs, `UNION`/`INTERSECT`/`EXCEPT`, and correlated
subqueries remain entirely unscoped — named, not silently dropped — and
are the natural seed for a follow-on doc if wanted.

**The "recursive execution model" concern above was real but smaller than
feared.** The actual blocker wasn't recursion in general — it was that
`sql_exec.c` already had exactly two non-reentrant static scratch globals
(`g_stmt_scratch` holding the outer statement's own parsed fields for the
whole rest of `dispatch_stmt()`'s call, and `g_select_scratch` holding
whatever SELECT is currently materializing its result rows) that a naive
"just call `sql_execute()` again for the subquery" implementation would
have silently clobbered mid-flight. The fix was two dedicated, separate
static scratch buffers — one per translation unit, at the exact point
each is needed:

- `kernel/sql_parser.c`: `g_subquery_skip_scratch` (a `struct
  SqlStatement`), used ONLY at parse time to validate an embedded
  subquery's shape (must parse as `SQL_STMT_SELECT`, `!has_join`,
  `!has_aggregates`, exactly one selected column) via a throwaway
  `sql_parse()` call on the captured raw text. A `g_subquery_validating`
  re-entrancy guard rejects a subquery containing another nested subquery
  with a clean "nested subqueries are not supported" parse error, rather
  than letting the inner validation's own `sql_parse()` call recursively
  clobber the outer validation's still-in-progress result in the same
  static buffer — this is what actually enforces "one level of subquery
  only," not a grammar restriction.
- `kernel/sql_exec.c`: `g_subquery_stmt_scratch` (a second, independent
  `struct SqlStatement`), used ONLY at exec time by `exec_subquery_column()`
  to re-parse a subquery's raw text right before running it — kept
  entirely separate from `g_stmt_scratch` so resolving a subquery never
  overwrites the outer statement's own in-progress WHERE clause (the
  exact corruption a naive shared-scratch design would have caused, and
  the reason `resolve_predicate_subqueries()` cannot simply call
  `sql_execute()`/`sql_execute_tx()` recursively).

**Subqueries are raw TEXT, not parsed structs, in the predicate pool.**
`predicate.h` gained `Predicate.subqueries[PREDICATE_MAX_SUBQUERIES=3][256]`
— each slot holds the subquery's original SQL text, re-parsed once at
resolve time — rather than embedding a full parsed `SqlSelectStmt` (~34KB)
per reference, which would have made `PREDICATE_MAX_NODES=32`-sized
predicate pools far too expensive. This is a direct continuation of the
codebase's existing "text is the interchange format, re-parse when
needed" convention (there has never been a compiled-query cache anywhere
in this codebase; `sql_execute()` always re-parses from scratch on every
call) rather than a new pattern invented for this phase.

**`IN (SELECT ...)` desugars into the same OR-chain a literal `IN`-list
already builds, at RESOLVE time instead of parse time.** The parser emits
a transient marker (`PRED_OP_IN_SUBQUERY`, `uses_subquery=1`,
`subquery_index` pointing into `Predicate.subqueries[]`) and defers
building the chain until `resolve_predicate_subqueries()` runs, since the
number of OR branches depends on how many rows the subquery actually
returns — unknowable at parse time. Resolution reuses
`predicate_add_comparison()`/`predicate_add_or()` unchanged, then copies
the freshly-built chain's ROOT NODE CONTENT back into the marker node's
own slot index (`pred->nodes[i] = pred->nodes[acc]`) so every existing
parent AND/OR node's `left`/`right` reference to index `i` stays valid
with no new "reparent" primitive needed — the newly-allocated `acc` slot
is simply abandoned, this codebase's established bump-allocated-pool
convention (`row_indexes[]`, `row_constraints[]`, rowstore pages all work
the same way: no reclaim in the first cut). A zero-row `IN (SELECT ...)`
resolves to a new `PRED_OP_FALSE` op (standard SQL: `IN` against an empty
set is always false) rather than an empty OR-chain, which would have had
no valid root node index to install.

**A scalar subquery returning zero rows resolves to `PRED_OP_FALSE`, not
an error or a NULL-comparison special case.** Standard SQL treats
comparing against an empty scalar subquery result as UNKNOWN, which
`WHERE` treats as false — the same simplification this codebase's NULL
handling already makes elsewhere (Phase 4). A scalar subquery returning
MORE than one row IS a genuine error (`SQL_ERR_VALUE_INVALID`, an
existing error code — no new `SqlErrorCode` value was needed anywhere in
this phase), aborting the whole statement before any row is scanned,
matching this file's existing "statement-level atomicity" posture.

**`exec_subquery_column()` deliberately bypasses `exec_select()` and the
cursor layer entirely**, calling `mvcc_find_matching_rows()`/
`mvcc_row_get()` directly — the same two calls `exec_select()` itself
uses internally. A subquery's result is consumed immediately and
completely by `resolve_predicate_subqueries()`, so materializing it as a
real cursor would burn one of only `CURSOR_MAX==8` simultaneously-open
slots for no reason, and risk exactly the "silent pool exhaustion" bug
Phase 6 already hit once (`cursor_open_rowset()` returning 0 as both a
legitimate id and its own failure sentinel).

**Resolution is wired into `dispatch_stmt()`, not into `exec_select()`/
`exec_update()`/`exec_delete()` themselves**, gated on
`has_where && !has_join && !has_aggregates` for SELECT (UPDATE/DELETE have
no JOIN/aggregate concept to gate on, just `has_where`) — this is what
makes "`exec_select_join()`/`exec_select_group()`/HAVING never resolve
subqueries" true by construction rather than by convention: those
functions are never even called with an unresolved marker still needing
resolution attempted, because `dispatch_stmt()` only calls
`resolve_predicate_subqueries()` on the plain paths before dispatching.
A subquery marker written inside a JOIN's WHERE or a HAVING clause still
PARSES fine (the same `parse_comparison_tail()` handles both positions
regardless of statement shape) but fails closed at eval time via
`eval_comparison()`'s defensive `uses_subquery`/`PRED_OP_IN_SUBQUERY`
check — verified directly in the new host test (scenario 9: a subquery
inside a JOIN's WHERE runs cleanly and matches zero rows, not a crash and
not "matches everything").

**Host test:** `tests/sql_subquery_phase7_host_test.c`, 34 checks across
11 scenarios, linked against the real `sql_exec.c`/`sql_parser.c`/
`predicate.c`/`row_index.c`/`rowstore.c`/`persist.c`/`cursor.c`/`mvcc.c`/
`row_constraint.c`/`row_journal.c`. Covers: scalar subquery in a plain
SELECT's WHERE; `IN (SELECT ...)` in a plain SELECT's WHERE; scalar
subquery in UPDATE's WHERE (verified via re-SELECT that only the intended
row changed); `IN (SELECT ...)` in DELETE's WHERE (verified via re-SELECT
that only the intended row is gone); a zero-row `IN (SELECT ...)`
resolving to no matches, not an error; a zero-row scalar subquery
resolving to no matches, not an error; a >1-row scalar subquery rejected
with `SQL_ERR_VALUE_INVALID`; four unsupported subquery shapes
(multi-column, aggregate, JOIN, nested) all rejected at PARSE time before
any execution is attempted; a subquery inside a JOIN's WHERE failing
closed instead of crashing or matching everything; a subquery AND'd with
an ordinary literal condition (proving the marker-node content-copy trick
doesn't corrupt the parent AND node's `left`/`right` references); and
permission denial on a subquery's own table resolving to zero rows
cleanly rather than a crash or a false grant (the same fail-safe posture
`sql_join_host_test.c`'s own scenario 7 already established for JOINs).

**Full regression sweep: 34/34 host test files, 0 failed** (up from 33 —
the new file). `net/http.c` compile-checked clean against the updated
`predicate.h`/`sql_parser.h`/`sql_exec.h` (only the same pre-existing,
unrelated warnings seen in every prior phase — misleading-indentation and
one implicit `strcmp` declaration, none touching this phase's changes);
its SQL route needed no changes at all, since it already passes raw SQL
text through to `sql_execute()` and serializes `SqlResult` generically.

This closes out the SQL Feature-Parity Roadmap — Phases 1 through 7 are
all DONE.

---

## Suggested sequencing

1. **Phase 1 (GROUP BY/aggregates)** first — highest real-world value,
   almost entirely reuses existing aggregate math, smallest true
   executor change of any phase here.
2. **Phase 2 (N-way JOIN/aliasing)** next — independent of Phase 1,
   directly extends Phase 20's existing two-table join rather than
   replacing it.
3. **Phase 3 (expression richness)** — benefits from being before Phase 4
   (IS NULL needs somewhere to point) but its IN/LIKE/grouping/arithmetic
   pieces are independently useful even before NULL lands.
4. **Phase 4 (real NULL/BLOB)** — sequenced here because Phases 2's OUTER
   JOIN and Phase 3's IS NULL both want it, and because it's a storage-
   layout change worth landing before Phase 6's partial-column INSERT
   needs something real to fill omitted columns with.
5. **Phase 5 (DDL in SQL text)** — independent of Phases 1-4, could run
   in parallel with any of them; sequenced fifth mostly because it's
   pure grammar-over-existing-functions and lower risk than the phases
   above, a good "breather" phase.
6. **Phase 6 (multi-row/partial INSERT)** — depends on Phase 4 for
   partial-column defaults to mean anything real.
7. **Phase 7 (subqueries/views/CTEs/set ops)** last, both because it's
   the biggest single jump in executor complexity and because it
   benefits from every phase above already being in place (DDL for
   views, IN for IN-subqueries, and a settled NULL representation for
   correlated/outer-join-adjacent subquery results).
