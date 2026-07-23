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

### Phase 4 — UNION / UNION ALL / INTERSECT / EXCEPT — depends on Phase 3

**Scope.** Parser: a trailing set-op token after a complete SELECT
captures the *raw text span* of the right branch (the same
capture-and-reparse-at-exec convention Phase 7 established for
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

**Verification.** New host test: all four operators, dedup vs ALL,
column-count mismatch rejection, ORDER BY over the merged set, and a
both-branches-near-cap truncation case.

### Phase 5 — CREATE VIEW / DROP VIEW — depends on Phase 3

**Storage decision (recommended): a dedicated view registry**, not
object-record reuse — `RECORD_VAL_LEN`=256 can't hold a real SELECT
text (investigation §7). `struct SLSViewDef { name[OBJECT_NAME_LEN];
sql_text[1024]; owner_uid; active; }`, `VIEW_MAX` 16, its own
`PERSIST_MAGIC_VIEW` region (the Gap-1 pattern, mechanical to add).

**Scope.** `CREATE VIEW v AS <select...>` stores the tail text verbatim
(validated by a parse-only check at create time); `DROP VIEW v` removes
it. Query side, v1 is deliberately narrow: `SELECT ... FROM v [WHERE
...] [ORDER BY ...] [LIMIT n]` where `v` resolves to a view executes the
stored text at depth 1, then applies the outer projection/WHERE/ORDER
BY/LIMIT over the materialized result. Views inside JOINs, views of
views, and INSERT/UPDATE/DELETE through views are all rejected loud in
v1 — each is a real follow-on, not an oversight. Permission: executing
a view runs the stored text under the CALLER's uid (invoker's-rights,
the simpler and safer first cut; definer's-rights named as the
alternative not taken).

**Verification.** New host test: create/query/drop round trip, outer
WHERE-over-view, view text that no longer parses (underlying table
dropped) failing loud at query time, persistence round trip via the
fake-NVMe harness, and each v1 rejection case.

### Phase 6 — WITH (non-recursive, single CTE) — depends on Phases 3 & 5

**Scope.** `WITH name AS (<select...>) SELECT ... FROM name ...` is
exactly a statement-scoped view: the parser captures the CTE body's raw
text span; the executor materializes it at depth 1, then the outer
SELECT runs with `name` resolving to the materialized rowset through
the same resolve-hook Phase 5 adds for views (one mechanism, two
sources). One CTE per statement, non-recursive, not referencable from
JOINs in v1 — same narrowness posture as Phase 5's query side, widened
only when something needs it. `WITH RECURSIVE` is a non-goal for this
whole roadmap, named here once.

**Verification.** Extend Phase 5's view test (shared machinery): CTE
shadowing a real table name (CTE wins, standard scoping), CTE over a
view (depth budget respected or rejected loud — decide at
implementation against the real depth accounting).

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
item outright. **All three (1, 2, 3) are now DONE.** Phases 4 and 5 follow in
either order (both depend only on 3); Phase 6 after 5 (shares its
resolve hook). Phase 7 deliberately unscheduled — revisit after 4-6 land
and real usage shows what's still missing.

## 4. Non-goals for this whole roadmap

`WITH RECURSIVE`; multiple set-ops per statement; views in JOINs /
writable views / views-of-views (each a named v1 rejection with an
obvious follow-on path); query planning/optimization of any kind beyond
the existing index probe (nested-loop + 256-row materialization stays
the honest execution model); subquery nesting beyond depth 2; and
lifting the 256-row cursor cap (a capacity item tracked in the Gap
Analysis §3.3, orthogonal to grammar surface).
