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

## Phase 4 — Real NULL + BLOB, dynamic-typing groundwork

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

---

## Phase 5 — DDL in SQL text (CREATE/ALTER/DROP TABLE, CREATE/DROP INDEX)

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

---

## Phase 6 — Multi-row INSERT + partial-column INSERT

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

---

## Phase 7 — Subqueries, views, CTEs, set operations

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
