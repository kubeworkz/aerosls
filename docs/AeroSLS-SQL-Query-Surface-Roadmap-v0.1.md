# AeroSLS SQL Query-Surface Roadmap v0.1

## 0. Why this doc exists

The Database Gap Analysis's §2.4/§2.5 named the last big block of missing
SQL surface: views, CTEs, set operations, correlated subqueries (all
explicitly deferred by SQL Feature-Parity Phase 7's own "build a real
nested-execution model" framing), plus `GROUP BY` combined with any JOIN
and `RIGHT`/`FULL OUTER JOIN` (Phase 1/2 scope cuts). Phase 7's addendum
said this work was "worth scoping as its own follow-on doc once there's a
clearer read on how much is actually wanted" — this is that doc, grounded
in a fresh direct read of `sql_exec.c`/`sql_parser.h`/`cursor.h` as they
exist today (every architectural claim below carries a file:line from
that investigation, not from memory of older phases).

## 1. Current architecture — the three facts that shape everything below

**1.1 — JOIN and aggregation are mutually exclusive dispatch branches
that never compose.** `exec_select()` fans out at the top
(sql_exec.c:365-366): `has_aggregates` → `exec_select_group()`, else
`has_join` → `exec_select_join()`, else the plain path. The join path
materializes combined rows (plain `struct RowValues` under a synthetic
query-time `running` layout with `t.col` qualified names, :447-453) into
two static ping-pong buffers (`g_select_scratch`/`g_join_scratch_b`,
:336/:499). The aggregate path (:807-1018) buckets into
`g_agg_buckets[SQL_MAX_GROUPS=64]` — but keys its bucketing off the
*source table's* layout and reads rows straight from `mvcc_row_get()` on
`s->table_name` (:914-916). Nothing structural prevents feeding the
bucketing from an already-materialized row array + layout instead of a
raw table scan — that refactor IS Phase 1.

**1.2 — Nothing in the executor is re-entrant.** One statement = one
pass through `dispatch_stmt()` against single-instance static scratch:
`g_stmt_scratch` (the ~34KB parsed statement, :1824),
`g_subquery_stmt_scratch`/`g_subquery_result_text` (:1667/:1674), the
two select/join buffers, `g_agg_buckets`, `g_join_probe_pred` (:491).
Statics were chosen deliberately (the 34KB `SqlSelectStmt` — two
embedded 32-node Predicates — is too big for careless stacking in a
freestanding kernel). Set operations (two SELECTs per statement), views
and CTEs (a stored/inline SELECT executing inside an outer statement)
all need one level of nesting that today would clobber the outer
statement mid-flight. Phase 3 builds exactly that — depth-2, no more.

**1.3 — LEFT JOIN's "NULL" padding is not NULL.** `fill_join_sentinel()`
(:471-483) pads unmatched rows with type-based sentinels ("0"/"false"/"")
— written before Phase 4 gave this engine a real `null_mask`. A padded
row is indistinguishable from a real row of zeros, and `IS NULL` cannot
find it. Harmless-ish for LEFT JOIN's typical use; disqualifying for
`FULL OUTER` (whose whole point is finding the unmatched) — so real-NULL
padding is a prerequisite folded into Phase 2, not a cosmetic upgrade.

Also load-bearing: views have **zero** existing code (every `VIEW` hit
in the tree is a scope-cut comment), and a stored view definition can't
ride in an object record — `RECORD_VAL_LEN` caps each field value at 256
bytes, smaller than a realistic SELECT text. A view registry needs its
own storage shape (Phase 5 decides it).

## 2. Phase breakdown

### Phase 1 — GROUP BY / aggregates over JOIN — DONE

**Why first: highest demand-per-effort, and no dependency on
reentrancy.** Aggregate-over-join ("sum of order amounts per customer
name") is a bread-and-butter query shape; its absence
(`SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED`, sql_exec.c:808-812) is the most
painful single rejection in the engine.

**Findings addendum (as built).** Landed exactly on the refactor §1.1
predicted, in two moves. First, the join chain's materialization stage
(FROM scan + every nested-loop JOIN step, ~200 lines) was factored out
of `exec_select_join()` into `join_materialize()` — leaving combined
rows in `g_select_scratch[0..n)` under the qualified `running` layout,
with WHERE deliberately NOT applied inside (both callers apply it
afterward, preserving the established "WHERE once, at the very end"
rule); `exec_select_join()` is now that helper plus its original tail,
byte-for-byte in behavior (the full pre-existing join test suite passes
unchanged). Second, `exec_select_group()` gained a join-sourced mode:
`join_materialize()` → WHERE validated and applied against the combined
rows → the IDENTICAL bucketing/HAVING/format/ORDER BY code runs over
`g_select_scratch` rows instead of a fresh `mvcc_row_get()` feed — one
aggregate implementation, two row sources, not a second copy. In join
mode every name (GROUP BY, aggregate arguments, WHERE) resolves against
the combined `t.col`/`alias.col` namespace, with error messages saying
so. One subtlety worth naming: join mode reads source rows out of
`g_select_scratch` while the format stage later WRITES formatted group
rows into the same buffer — safe because bucketing fully completes (all
state in `g_agg_buckets[]`) before the first formatted row lands, and
commented at the site so a future reorder can't silently break it.
`SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED` is retired from the executor (the
enum value remains for ABI stability of error codes; nothing returns it).

**Verification.** 15 new checks in `sql_group_phase1_host_test.c`
scenario 10: COUNT(*) grouped by a joined-table column (real
join-then-group counts, not coincidental ones — the fixture regions are
deliberately asymmetric); SUM of a FROM-table column grouped by a
joined-table column; HAVING over the join-sourced aggregate; WHERE
filtering combined rows before bucketing; a three-table chain grouped by
the LAST table's column; and the two join-mode error paths (unqualified
aggregate argument, unknown qualified GROUP BY column). Two pre-existing
checks flipped polarity, named rather than quietly edited: the group
test's join-rejection check (now expects the join pipeline's own
TABLE_NOT_FOUND for an unknown table) and the N-way join test's
scenario 8 (now expects success). All 8 single-table aggregate
scenarios pass byte-identically. Full sweep 43/43; `sql_exec.c` clean
under both compile-check styles.

**Scope.** Refactor `exec_select_group()`'s scan-and-bucket stage to
consume (rows[], layout, count) instead of scanning `s->table_name`
directly — the plain path passes the real table layout and a
`mvcc_row_get()` feed exactly as today; the new combined path first runs
the existing join pipeline to materialize combined rows under the
`running` qualified layout, then hands that array to the same bucketing.
GROUP BY / aggregate-argument / HAVING columns resolve against the
qualified `t.col` names in the combined layout. The 256-row
materialization cap and `SQL_MAX_GROUPS`=64 apply unchanged (the join
already has both). WHERE applies before bucketing (already the join
path's order).

**Verification.** Host test: two-table join with SUM/COUNT per group,
HAVING over the aggregate, a three-table chain, GROUP BY on a qualified
column from the *joined* (not FROM) table, and the existing
single-table aggregate scenarios re-run unchanged. Extend
`sql_group_phase1_host_test.c` (it owns the aggregate scenarios and the
UNSUPPORTED regression check, which flips polarity).

### Phase 2 — Real-NULL join padding + RIGHT / FULL OUTER JOIN — DONE

**Findings addendum (as built).** `fill_join_sentinel()` was replaced
outright by `fill_join_null_pad()` (`sql_exec.c`): instead of a
per-type sentinel string, it sets `values[c][0] = '\0'` and the real
`null_mask` bit for each padded column — the exact Phase 4 NULL
representation every other nullable column already uses, so `IS NULL`
finds padded columns for free. A new `copy_row_segment()` helper
replaced the old bare `se_strcpy` loops at every row-combining site
(matched rows, LEFT-padded rows, and the new anti-pass rows): it copies
both a source segment's text *and* its `null_mask` bits into the
combined row at the shifted bit position, closing a gap the original
sentinel design never had to worry about — a genuinely-NULL column on a
*matched* row (not just a padded one) now survives into the combined
result as NULL instead of silently reading back as non-NULL empty text.

`RIGHT JOIN` and `FULL OUTER JOIN` were **not** implemented as a
literal operand swap — `cur` (the accumulated outer side) is a
synthetic in-memory row array once a step is past the first JOIN, not a
real table, so it can't be probed via `mvcc_find_matching_rows()` the
way `jc->table` can. Instead, `join_materialize()`'s per-step loop
tracks a `g_join_matched_ids[]` set of every `jc->table` row id matched
by any outer row during the ordinary probe pass, then (for RIGHT/FULL
only) runs one extra `mvcc_table_scan()` anti-pass over `jc->table`
afterward: any row whose id never appeared in the matched set gets
emitted with the accumulated side NULL-padded via
`fill_join_null_pad(&combined, 0, running.column_count)`. Per-type
behavior: INNER keeps only matches; LEFT and FULL both pad an
unmatched *outer* row (`take_m == 0` branch); RIGHT deliberately does
**not** pad the unmatched-outer branch — its right-side orphans come
from the anti-pass instead, so an outer row with no match is simply
dropped there, same as INNER. FULL gets both behaviors (LEFT's
unmatched-outer padding plus the anti-pass), which is the whole "LEFT
pass + right-anti-pass" design the scope called for, achieved without
ever needing to query the synthetic `cur` array as if it were a table.

One correctness fix folded in along the way, named because it wasn't
explicitly scoped: a NULL join key. The probe step used to build an
equality predicate against `cur[r].values[outer_col]` unconditionally,
even when that column was itself NULL (empty placeholder text) —
harmless in practice against a UINT64 key column but a real semantic
bug in general (standard SQL: NULL never equals anything, not even
another NULL). The probe is now skipped entirely — `take_m` stays 0 —
whenever `cur[r].null_mask` has the join column's bit set, so a NULL
join key always behaves like a genuine non-match rather than
accidentally depending on what text a NULL column's placeholder happens
to hold.

**Verification.** `sql_join2_phase2_host_test.c` grew from 24 to 51
checks. Scenario 2 flipped (sentinel `"0"`/`""` expectation → real-NULL
empty-text expectation) and gained 2b: `IS NULL`/`IS NOT NULL` correctly
isolate LEFT-padded vs. genuinely-matched rows. New scenario 11: RIGHT
JOIN keeps an unmatched department (Marketing) with the employee side
padded, while dropping the unmatched employee (dave's dangling FK) —
proving the asymmetry is real, not accidental. New scenario 12: FULL
OUTER finds both orphans at once (dave AND Marketing) in one query, and
`WHERE ... IS NULL OR ... IS NULL` isolates exactly those two rows.
New scenario 13: an employee with a genuinely NULL `dept_id` (not a
dangling FK) still produces exactly one correctly-padded row, proving
the NULL-join-key skip doesn't crash or double-match. New scenario 14:
parser coverage for bare and `OUTER`-qualified `RIGHT`/`FULL JOIN`.
Full regression sweep: 43/43, `sql_exec.c` and `sql_parser.c` both
clean under the `-I`-flagged and zero-flag (`-ffreestanding`, matching
`X86_CFLAGS`) compile-check styles.

**Original scope (as planned).**
- Replace `fill_join_sentinel()`'s sentinels with real `null_mask` bits
  (Phase 4 semantics). This is a behavior change to existing LEFT JOIN
  output — padded columns become IS-NULL-matchable and stop masquerading
  as zeros — the correction, named plainly, is the point.
- `RIGHT JOIN` = operand-swapped LEFT (normalize at exec time, not by
  duplicating the padding logic).
- `FULL OUTER JOIN` = the LEFT pass plus an appended anti-pass over the
  right table (rows with no left match, left side NULL-padded). Both
  passes share the existing probe machinery; the 256-row cap and
  3-join chain cap apply unchanged.

**Verification (as planned).** Extend `sql_join2_phase2_host_test.c`:
LEFT-padded rows now match `IS NULL` (the flipped expectation is the
test), RIGHT equivalence to the mirrored LEFT, FULL OUTER finding both
sides' orphans.

### Phase 3 — Depth-2 executor reentrancy (the enabler, no user-visible feature) — DONE

**Findings addendum (as built).** Landed exactly as scoped, via macro
indirection rather than threading a bank index through every call site.
`g_stmt_scratch`, `g_select_scratch`, `g_join_scratch_b`, `g_agg_buckets`,
and `g_join_probe_pred` each became a `_bank[SQL_EXEC_MAX_DEPTH]` array
sitting behind an object-like macro of the buffer's OLD name (e.g.
`#define g_select_scratch (g_select_scratch_bank[g_exec_depth])`) — every
existing reference throughout the file, dozens of them across
`exec_select`/`join_materialize`/`exec_select_group`, kept compiling
completely unchanged, since `g_select_scratch[i]` textually becomes
`g_select_scratch_bank[g_exec_depth][i]`, still a valid expression. A
grep for local variables/parameters shadowing any of the five names
confirmed this was safe before writing it. `SQL_EXEC_MAX_DEPTH` is 2:
depth 0 is the top-level statement, depth 1 is one nested statement
inside it. `sql_exec_depth_enter()`/`sql_exec_depth_leave()` (both
static, internal) are the pair a future nested caller must use
symmetrically; entering a third level returns 0 and sets the new
`SQL_ERR_NESTING_TOO_DEEP` (`sql_exec.h`) rather than wrapping around and
reusing bank[1] while it's still live.

`g_join_matched_ids` (Query-Surface Phase 2's RIGHT/FULL anti-pass set)
was deliberately left unbanked alongside the subquery scratch the
original scope already named — it's entirely populated and consumed
within one synchronous join step, never live across a recursive
dispatch call, so banking it would add BSS cost for a hazard that
doesn't exist. `dispatch_stmt()` itself needed zero changes: it already
took its statement via a pointer parameter rather than touching
`g_stmt_scratch` directly, so depth management lives entirely at the
call boundary (whoever recurses into `sql_execute_tx()`/`dispatch_stmt()`
calls `sql_exec_depth_enter()` first), not inside the dispatcher.

Since Phase 3 has no real production nested-call site yet (that's Phase
4's UNION), the internal-only test needed its own entry point to drive
real nesting through these statics: `sql_exec_test_phase3_nesting()` and
`sql_exec_test_phase3_depth_exceeded()`, both non-static but declared in
sql_exec.h under an explicit "TEST-ONLY, not part of the public SQL
surface" header — no shell/HTTP/syscall path reaches either. The nesting
test parses an outer statement into bank[0], snapshots its kind and FROM
table name, runs a wholly unrelated statement to full completion at
depth 1, returns to depth 0, verifies the snapshot survived byte-for-byte,
and only then dispatches the outer statement for real — so the test
checks actual correct query output, not just "didn't crash."

**Verification.** New `sql_exec_depth_phase3_host_test.c`, 14 checks.
Scenario 1: a JOIN+GROUP BY outer statement (exercising
`g_select_scratch`/`g_join_scratch_b`/`g_join_probe_pred`/`g_agg_buckets`
together) survives an unrelated plain SELECT running to completion at
depth 1 in between parse and dispatch — verified via the outer's actual
fetched rows (`COUNT(*)` per group), not just its row count. Scenario 2:
roles swapped, proving the banking isn't order-dependent. Scenario 3:
a third nesting level is rejected with `SQL_ERR_NESTING_TOO_DEEP`.
Scenario 4: an ordinary top-level query still works right after the
depth-exceeded probe, proving no leaked depth state. Scenario 5: plain
single-level dispatch (the overwhelming majority of all existing
queries) is unaffected — same JOIN+GROUP BY query, no nesting touched,
byte-identical result. Full regression sweep: 44/44 (43 prior + this new
file); `sql_exec.c` and `sql_exec.h` both clean under the `-I`-flagged
and zero-flag (`-ffreestanding`, matching `X86_CFLAGS`) compile-check
styles. Zero observable behavior change to any existing single-level
query, confirmed by the full suite staying green with no other test
file touched.

**Original scope (as planned).** A single explicit nesting depth counter
and depth-indexed banks (size 2) for exactly the scratch a nested SELECT
touches: `g_stmt_scratch`, the select/join ping-pong buffers,
`g_agg_buckets`, `g_join_probe_pred`. ~2× BSS cost of those statics
(~100KB-class, cheap against the existing budget — `RowJournalEntry`
alone spends 130KB); depth >2 fails loud with a distinct error, never
recurses silently. Phase 7's subquery scratch is deliberately NOT banked
here — subqueries stay pre-resolved at depth 0 (their existing design),
and a nested branch containing its own subquery is rejected in v1 rather
than speculatively supported. This phase ships with an internal-only
test (a nested `dispatch_stmt` call proving the outer statement
survives) and changes zero observable behavior.

### Phase 4 — UNION / UNION ALL / INTERSECT / EXCEPT — depends on Phase 3 — DONE

**Findings addendum (as built).** Landed exactly as scoped, with one
grammar refinement discovered during implementation. `SqlSelectStmt`
gained `has_set_op`/`set_op` (`SqlSetOpKind`, `sql_parser.h`) and a
`set_op_rhs_text[SQL_SETOP_RHS_TEXT_LEN]` buffer (`SQL_SETOP_RHS_TEXT_LEN`
is literally `SQL_MAX_TEXT_LEN`, not a fresh magic number, since the
right branch's text is always a suffix of the whole input and can never
exceed it). `UNION`/`INTERSECT`/`EXCEPT`/`ALL` joined the keyword table
with the project's standard reserved-word-shadowing note.

The refinement: the capture point sits in `parse_select_body()` between
HAVING and ORDER BY, so a trailing `ORDER BY`/`LIMIT` falls straight
through to the SAME unmodified code that already parses those two
clauses — landing on the OUTER statement's own fields by construction,
never the right branch's, with zero new parsing logic for "which
statement does this ORDER BY belong to." The raw-text capture itself
mirrors `try_parse_embedded_subquery()`'s paren-depth-aware token skip,
but stops at a top-level (depth 0) `;`/EOF/`ORDER`/`LIMIT` instead of a
matching `)` — and rejects a second top-level set-op keyword outright
("chained set operators are not supported (one per statement in v1)"),
which is also what makes chaining structurally impossible to reach at
exec time: any input that would trigger `exec_select_set_op()`
recursing into its own right branch gets rejected at the OUTER
statement's own parse time first, before exec ever runs. Eager
validation reuses the exact `g_subquery_skip_scratch`/
`g_subquery_validating`-style throwaway-parse-plus-reentrancy-flag
pattern (`g_setop_skip_scratch`/`g_setop_validating`).

Exec side: `exec_select_set_op()` runs the left branch through the
ordinary `exec_select()` dispatch (so a JOIN or aggregate query works as
the left branch for free, proven by scenario 9), fetches its rows via
`cursor_fetch_rows()`, then calls `sql_exec_depth_enter()` and runs the
right branch as a genuinely ordinary NESTED `sql_execute_tx()` call at
depth 1 — this is Phase 3's banking paying off exactly as that phase's
own comments predicted: the left branch's `g_select_scratch`/
`g_join_scratch_b`/`g_agg_buckets`/`g_join_probe_pred` at depth 0 are
untouched by whatever the right branch's own JOIN/aggregate/WHERE logic
does at depth 1, with zero new plumbing beyond calling
`sql_exec_depth_enter()`/`_leave()` around the nested call. Three new
scratch buffers (`g_setop_left_scratch`/`_right_scratch`/
`_merge_scratch`) are deliberately NOT depth-banked, using the same
"provably never live across a nested call" reasoning already used to
justify not banking the subquery scratch and Query-Surface Phase 2's
`g_join_matched_ids` — a set-op statement can never be reached from
within its own right branch's execution (see the chaining-rejection
point above), so no two `exec_select_set_op()` frames can ever be
simultaneously mid-flight.

A real, named v1 simplification not explicitly called out in the
original scope: ORDER BY over a set operator's merged result compares
values as plain text regardless of either branch's declared column
type (via `se_strcmp()`, not `compare_rows_by_column()` and its
`RowTableLayout`/type machinery) — a merged column's "true" type has no
single answer once JOINs/aggregates on either side are in play, so v1
doesn't invent one. Row equality for UNION/INTERSECT/EXCEPT dedup
treats two NULLs at the same position as equal (set-membership
semantics, explicitly distinguished in the code from WHERE's own
NULL-never-equals-anything predicate semantics).

**Verification.** New `sql_setop_phase4_host_test.c`, 30 checks, fixture
built around one deliberately-overlapping row (`carol`/`eng`, identical
in both `employees_ny` and `employees_sf`) so every operator's dedup/
membership behavior is independently checkable. UNION dedups it (5
rows, not 6); UNION ALL keeps both copies (6 rows); INTERSECT returns
exactly the shared row; EXCEPT returns the left side minus the overlap
(2 rows). ORDER BY + LIMIT over a UNION's merged result sorts correctly
and truncates correctly. Column-count mismatch rejected with
`SQL_ERR_COLUMN_COUNT_MISMATCH`. A chained `A UNION B UNION C` is a
parse error, not silent misinterpretation. Parser-level checks confirm
all four operators set the right `SqlSetOpKind`, and that a trailing
`ORDER BY DESC LIMIT n` lands on the outer statement's own fields. A
JOIN as the set-op's own left branch executes correctly. A
both-branches-near-cap case (150 + 150 = 300 rows via `UNION ALL`)
truncates honestly at 256 with `truncated=1` set, never silently
dropped or overflowed. Full regression sweep: 45/45 (44 prior + this
new file); `sql_exec.c`, `sql_exec.h`, and `sql_parser.c`/`.h` all clean
under the `-I`-flagged and zero-flag (`-ffreestanding`, matching
`X86_CFLAGS`) compile-check styles.

**Original scope (as planned).** Parser: a trailing set-op token after a
complete SELECT captures the *raw text span* of the right branch (the
same capture-and-reparse-at-exec convention Phase 7 established for
subqueries — deliberately NOT a linked list of embedded `SqlSelectStmt`,
which at ~34KB each is exactly the struct-chaining trap the
investigation flagged). One set-op per statement in v1 (no
`A UNION B UNION C` chains). Exec: run the left branch to a
materialized rowset, run the right branch at depth 1, then merge:
UNION ALL concatenates; UNION dedups (O(n²) text compare, honest at a
256-row cap); INTERSECT/EXCEPT are membership filters. Column-count
mismatch between branches is a loud error; column NAMES follow the left
branch (standard SQL posture). ORDER BY/LIMIT apply to the merged
result only.

**Verification (as planned).** New host test: all four operators, dedup
vs ALL, column-count mismatch rejection, ORDER BY over the merged set,
and a both-branches-near-cap truncation case.

### Phase 5 — CREATE VIEW / DROP VIEW — depends on Phase 3 — DONE

**Findings addendum (as built).** Landed exactly as scoped, with one
storage refinement discovered during implementation: `sql_text[1024]`
in the original sketch became `VIEW_SQL_TEXT_LEN` = `SQL_MAX_TEXT_LEN`
(512, same reasoning as `SQL_SETOP_RHS_TEXT_LEN` — the captured tail
is always a suffix of the whole input's own cap and can never exceed
it, so a separate 1024 magic number was never needed). New
`kernel/view.h`/`view.c` — `struct SLSViewDef { name[OBJECT_NAME_LEN];
sql_text[VIEW_SQL_TEXT_LEN]; owner_uid; active; }`, `VIEW_MAX` 16 —
mirrors `database.c`'s shape exactly, including its own
`find_table_name_collision()` guard (CREATE VIEW cannot shadow an
existing rowstore table; the reverse — CREATE TABLE later shadowing an
existing view — is a named, harmless gap since table resolution always
wins). Persistence is the plain Gap-1 pattern: `PERSIST_MAGIC_VIEW`,
`PERSIST_VIEW_HDR_LBA`/`PERSIST_VIEW_ENT_LBA`, a `persist_views()`
writer, and a restore block (13th and last) in `persist_restore_all()`.

Parser: `TOK_KW_VIEW` joined the keyword table; `parse_create_view_body()`
reuses the same capture-and-reparse-at-exec convention Phase 4 and
Phase 7 established — a paren-depth-aware token skip captures the raw
`AS <select...>` tail verbatim, then a throwaway `sql_parse()` call
behind a `g_view_validating` reentrancy guard eagerly rejects anything
that isn't a real `SELECT` at CREATE time. `parse_drop_view_body()` is
a one-liner by comparison.

Exec: `exec_select()`'s table-resolution fallback tries
`view_find_index()` after `find_table_catalog_index()` comes up empty,
and hands off to `exec_select_view()`, which enters Phase 3's depth
bank, runs the view's own stored SELECT as a genuinely nested
`sql_execute_tx()` call, leaves the bank, then composes the outer
WHERE/ORDER BY/LIMIT over the materialized result — reusing
`setop_row_collect_cb` from the Phase 4 set-op code above it rather
than writing a second row-collection helper. Two mechanisms fall out
for free rather than needing bespoke detection code: a view's inner
error (e.g. `SQL_ERR_TABLE_NOT_FOUND` when the underlying table was
dropped after the view was created) propagates as-is instead of being
wrapped, avoiding the "denial looks like absence" bug class named
repeatedly elsewhere in this project; and views-of-views fail loud with
the existing `SQL_ERR_NESTING_TOO_DEEP` — a view-of-a-view needs 2
levels of nesting, which exceeds Phase 3's 2-deep depth budget, so no
dedicated view-of-view check was needed. Views in JOINs
(`SQL_ERR_VIEW_IN_JOIN_UNSUPPORTED`) and DML through views
(`SQL_ERR_DML_THROUGH_VIEW_UNSUPPORTED`) are each a small, explicit
check at the existing `tidx < 0` fallback points in `join_materialize()`
/ `exec_insert()`/`exec_update()`/`exec_delete()`, ahead of the generic
`SQL_ERR_TABLE_NOT_FOUND`. Permission landed exactly as scoped:
invoker's-rights (the view's stored SELECT runs under the CALLER's
uid, not the owner's).

One real, named v1 simplification not explicitly called out in the
original scope, same bug class as Phase 4's own ORDER BY note: since
`struct SqlResult` reports column NAMES only (no types), the outer
WHERE/ORDER BY validation against a view's result builds a synthetic
all-`FIELD_TYPE_STRING` `RowTableLayout` — so an outer `ORDER BY` over
a numeric column of a view sorts lexicographically, not numerically.
Confirmed honestly in the test (scenario 2c) rather than worked around.
Also confirmed directly: this codebase's column projection is
metadata-only everywhere (`out->columns[]` reports what was asked for,
but materialized rows always carry the full underlying row) — true for
views exactly like every other query path, and worth naming since it
is easy to miss when reading `exec_select_view()` for the first time.

**Verification.** New `sql_view_phase5_host_test.c`, 52 checks: create/
query/drop round trip; outer WHERE/ORDER BY/LIMIT composed over a view
(including the documented lexicographic-ORDER-BY simplification above);
a view whose underlying table is dropped after creation failing loud
at query time with the real error, not a silent empty result; every v1
rejection (view in JOIN as FROM source and as JOIN side, view-of-view,
INSERT/UPDATE/DELETE through a view); CREATE VIEW validation (table-
name collision, duplicate view name, non-SELECT or empty AS-body parse
errors); DROP VIEW not-found and permission-denied (non-owner/non-
kernel rejected, owner can still drop under a non-kernel role); and a
real persistence round trip through the fake-NVMe harness (view text
and owner_uid survive byte-for-byte, and the restored view is genuinely
queryable afterward, not just present as inert bytes).

That last check surfaced a real finding, worth recording since it cost
real debugging time: the test's first attempt returned every matching
row doubled after the restore. Instrumentation proved the duplication
was not view-specific — a plain, view-free `SELECT * FROM employees
WHERE dept = 'eng'` issued right after `persist_restore_all()` doubled
up the same way. Root cause: `mvcc_bootstrap_from_rowstore()`'s own
header comment already says it plainly — "NOT idempotent — calling
this twice would create duplicate versions of the same physical rows;
`persist_restore_all()` is the only correct call site." The real boot
sequence (`kernel.c`) calls `mvcc_init()` before `persist_restore_all()`
runs; the test originally simulated "reboot" by wiping only `views[]`,
leaving the process's already-live `mvcc_versions[]` entries (from the
fixture rows inserted earlier via `sql_execute()`) in place, so
`persist_restore_all()`'s internal `mvcc_bootstrap_from_rowstore()`
call registered a second, redundant version for every already-live row
on top of the ones still there. Not a kernel bug — a missing-reset gap
in the test's own "reboot" simulation, fixed by calling `mvcc_init()`
immediately before `persist_restore_all()`, matching the real boot
ordering. Full regression sweep: 46/46 (45 prior + this new file);
`view.h`/`view.c`, `persist.h`/`.c`, `sql_parser.h`/`.c`, and
`sql_exec.h`/`.c` all clean under both the `-I`-flagged and zero-flag
(`-ffreestanding`, matching `X86_CFLAGS`) compile-check styles. 22
pre-existing host tests needed `kernel/view.c` added to their own
"Build and run" link lines (the new `sql_exec.c`/`persist.c`
dependency wasn't previously linked anywhere); 6 of those also needed
a `catalog_get_role()` stub added, since they don't otherwise link
`database.c`.

**Original scope (as planned).** Storage decision (recommended): a
dedicated view registry, not object-record reuse —
`RECORD_VAL_LEN`=256 can't hold a real SELECT text (investigation §7).
`struct SLSViewDef { name[OBJECT_NAME_LEN]; sql_text[1024]; owner_uid;
active; }`, `VIEW_MAX` 16, its own `PERSIST_MAGIC_VIEW` region (the
Gap-1 pattern, mechanical to add).

`CREATE VIEW v AS <select...>` stores the tail text verbatim (validated
by a parse-only check at create time); `DROP VIEW v` removes it. Query
side, v1 is deliberately narrow: `SELECT ... FROM v [WHERE ...] [ORDER
BY ...] [LIMIT n]` where `v` resolves to a view executes the stored
text at depth 1, then applies the outer projection/WHERE/ORDER BY/LIMIT
over the materialized result. Views inside JOINs, views of views, and
INSERT/UPDATE/DELETE through views are all rejected loud in v1 — each
is a real follow-on, not an oversight. Permission: executing a view
runs the stored text under the CALLER's uid (invoker's-rights, the
simpler and safer first cut; definer's-rights named as the alternative
not taken).

New host test: create/query/drop round trip, outer WHERE-over-view,
view text that no longer parses (underlying table dropped) failing
loud at query time, persistence round trip via the fake-NVMe harness,
and each v1 rejection case.

### Phase 6 — WITH (non-recursive, single CTE) — depends on Phases 3 & 5 — DONE

**Findings addendum (as built).** Landed exactly as scoped, with the two
open questions the original scope left for implementation time both
resolved and verified. `struct SqlSelectStmt` gained `has_cte`/
`cte_name[OBJECT_NAME_LEN]`/`cte_text[SQL_CTE_TEXT_LEN]`
(`SQL_CTE_TEXT_LEN` is `SQL_MAX_TEXT_LEN`, the same non-fresh-magic-
number reasoning as `SQL_SETOP_RHS_TEXT_LEN`/`SQL_VIEW_TEXT_LEN` — the
captured body is always a substring of the whole input). `TOK_KW_WITH`
joined the keyword table with the project's standard reserved-word-
shadowing note.

Parser: `parse_with_select()` (`p->cur == TOK_KW_WITH` on entry) consumes
`WITH <name> AS (`, then does a paren-depth-aware capture starting at
depth 1 (the opening paren already consumed) down to the matching close
— the same capture style `parse_create_view_body()` uses, but stopping
at a balanced `)` instead of a top-level `;`/EOF, since a CTE body is
always parenthesized while a view's AS-tail runs to the statement's own
end. Eager validation reuses the exact throwaway-parse-plus-reentrancy-
flag pattern (`g_cte_skip_scratch`/`g_cte_validating`) as CREATE VIEW and
the set operator's own right branch. The real subtlety: `parse_select_
body()` (which parses the OUTER select) `sq_memset()`s its target struct
at its own top, so `parse_with_select()` keeps the captured name/text in
LOCAL buffers until after that call returns, then copies them onto the
now-populated struct's `has_cte`/`cte_name`/`cte_text` fields — get the
ordering backwards and the memset silently erases the capture. A CTE
body containing another `WITH` (nested or self-referential) is refused
at PARSE time by the SAME `g_cte_validating` guard, for free — no
dedicated recursion check needed there.

Exec: `exec_select()` checks `has_cte` FIRST, ahead of set-op/aggregate/
JOIN dispatch AND ahead of `find_table_catalog_index()`/
`view_find_index()` — the OPPOSITE precedence from Phase 5's views,
which only ever win as a fallback once a real table lookup has already
failed. This single check is what makes "a CTE shadows a same-named
real TABLE" (standard SQL scoping) work, and — a real, deliberate
improvement over the view precedent, not just a mirror of it — it also
catches JOIN/aggregates/a set operator combined with a CTE-matching FROM
table_name all in ONE place, before any of those three would otherwise
dispatch away. Views have no single choke point for this and so still
report a plain `SQL_ERR_TABLE_NOT_FOUND` for e.g. "view GROUP BY" (a
named gap in Phase 5's own code comment); CTEs get one consistently-
worded rejection (`SQL_ERR_CTE_SCOPE_UNSUPPORTED`) instead. The SECOND
call site for that same error code is `join_materialize()`, for a CTE
referenced on the JOIN side of a query whose own FROM source is
something else (`FROM other JOIN cte_name ON ...`) — the mirrored
FROM-source check the view precedent needed at this same call site
turned out to be unreachable dead code for CTEs, since `exec_select()`'s
own top-of-function check already rejects that combination earlier;
the code comment at the JOIN-side check explains why no mirror was
added. `exec_select_cte()` itself is structurally identical to
`exec_select_view()` (same synthetic all-`FIELD_TYPE_STRING`
`RowTableLayout` simplification for the outer WHERE/ORDER BY, same
real-error propagation instead of a wrapped generic one) — reusing that
function's own header comment's reasoning rather than repeating it, with
one difference: there is no registry lookup, since `cte_text` is already
sitting on the statement struct being executed.

Both of the roadmap's own open questions resolved and verified directly:
a CTE over a view fails loud with the SAME `SQL_ERR_NESTING_TOO_DEEP`
depth guard views-of-views uses (the CTE body's nested execution runs at
depth 1; if THAT body's own FROM names a view, resolving it needs depth
2, one past the 2-deep budget) — not a bespoke check, exactly the "views
of views" precedent reused rather than re-invented. And non-recursion is
enforced structurally, not by a dedicated runtime check: a CTE body that
names its OWN `cte_name` re-parses via an entirely fresh, independent
nested `sql_execute_tx()` call at exec time that has no memory of being
"inside" a CTE named `x`, so its own `FROM x` resolves via the ordinary
table/view lookup, finds neither (a CTE is never registered anywhere
global), and fails loud with a plain `SQL_ERR_TABLE_NOT_FOUND` rather
than looping or crashing.

**Verification.** New `sql_cte_phase6_host_test.c`, 33 checks, reusing
`sql_view_phase5_host_test.c`'s fixture/stub scaffolding (still linking
`view.c`/`persist.c`, since one scenario exercises a CTE whose own body
queries a view): the basic `WITH ... SELECT` round trip; outer WHERE/
ORDER BY/LIMIT composed over a no-inner-WHERE CTE plus the same loud-
error-on-unknown-column checks views got; a CTE named `employees`
shadowing the real `employees` table (the CTE's own body legitimately
queries the real table under the hood, the outer query sees only the
CTE's narrower result, and a later ordinary query with no WITH prefix
still sees the real table's full data — the shadow is scoped to its own
statement only); all four v1 combined-scope rejections (JOIN as FROM
source, JOIN on the JOIN side, aggregates, a set operator), all
`SQL_ERR_CTE_SCOPE_UNSUPPORTED`; a CTE over a view failing loud with
`SQL_ERR_NESTING_TOO_DEEP`; a self-referencing CTE body failing loud as
a plain unknown table, not a hang; a nested `WITH` inside a CTE body
rejected at parse time; and parser-level checks confirming `has_cte`/
`cte_name`/`cte_text` capture, empty-body/non-SELECT-body/missing-
trailing-SELECT parse errors, and that an ordinary WITH-less SELECT
still parses with `has_cte == 0` (zero-default, byte-for-byte pre-
Phase-6 behavior). No debugging needed this round — every check passed
on the first build, unlike Phase 5's own persistence-round-trip
surprise. Full regression sweep: 47/47 (46 prior + this new file); no
pre-existing host test needed any link-line changes (unlike Phase 5's
22-file sweep), since CTEs added no new linked source file — everything
lives in the already-linked `sql_parser.c`/`sql_exec.c`.
`sql_parser.h`/`.c` and `sql_exec.h`/`.c` all clean under both the
`-I`-flagged and zero-flag (`-ffreestanding`, matching `X86_CFLAGS`)
compile-check styles.

**Original scope (as planned).** `WITH name AS (<select...>) SELECT ...
FROM name ...` is exactly a statement-scoped view: the parser captures
the CTE body's raw text span; the executor materializes it at depth 1,
then the outer SELECT runs with `name` resolving to the materialized
rowset through the same resolve-hook Phase 5 adds for views (one
mechanism, two sources). One CTE per statement, non-recursive, not
referencable from JOINs in v1 — same narrowness posture as Phase 5's
query side, widened only when something needs it. `WITH RECURSIVE` is a
non-goal for this whole roadmap, named here once.

Extend Phase 5's view test (shared machinery): CTE shadowing a real
table name (CTE wins, standard scoping), CTE over a view (depth budget
respected or rejected loud — decide at implementation against the real
depth accounting).

### Phase 7 — Correlated subqueries — last, and honestly may stay deferred

**Why last.** Phase 7 (parity roadmap) pre-resolves subqueries ONCE
before the outer scan against fixed text — correlation breaks every
part of that: the subquery text needs per-outer-row binding
substitution, re-execution N times (a real O(rows × subquery-cost)
multiplier with no indexes to soften it), and reentrant subquery
scratch (deliberately excluded from Phase 3's banks). The honest v1, if
wanted: textual binding substitution (`outer.col` spliced into the
subquery text per row) executing at depth 1 per outer row, capped at a
small outer-row budget (e.g. first 64 rows, loud truncation) to keep
the quadratic cost named and bounded. **Recommendation: leave this
phase unstarted until a real query needs it** — Phases 4-6 cover most
practical uses (a correlated EXISTS is often re-expressible as a JOIN,
which Phase 1 makes aggregatable).

## 3. Suggested sequencing

Phases 1 and 2 first, in either order — both are self-contained executor
work with no reentrancy dependency, and each closes a named Gap-Analysis
item outright. **Phases 1-6 are now DONE.** Phase 7 (correlated
subqueries) deliberately unscheduled — the roadmap's own recommendation
is to leave it unstarted until a real query needs it, since Phases 4-6
already cover most practical uses.

## 4. Non-goals for this whole roadmap

`WITH RECURSIVE`; multiple set-ops per statement; views in JOINs /
writable views / views-of-views (each a named v1 rejection with an
obvious follow-on path); query planning/optimization of any kind beyond
the existing index probe (nested-loop + 256-row materialization stays
the honest execution model); subquery nesting beyond depth 2; and
lifting the 256-row cursor cap (a capacity item tracked in the Gap
Analysis §3.3, orthogonal to grammar surface).
