# AeroSLS Database Gap Analysis v0.1

## 0. Why this doc exists

Every database-layer roadmap this project has run is now formally closed:
the RDBMS Roadmap (Phases 16-24, including the Phase 24 legacy-path fork,
resolved as Option B), the SQL Feature-Parity Roadmap (Phases 1-8), the
Database Namespace & Access Roadmap (Phases 1-5), both VectorStore
roadmaps, and the standalone cascading phase (FK `ON DELETE
CASCADE`/`SET NULL` + `DROP DATABASE ... CASCADE`) that closed the two
longest-standing named non-goals. This doc is the honest inventory of
what is STILL open across all of them — the same exercise
`AeroSLS-Gap-Analysis-v0.1.md` did for the whole system, scoped here to
the database layer only, and refreshed against the code as it exists
today (every claim below was checked by direct grep/read during this
analysis, not recalled from roadmap prose).

Findings are grouped by severity: §1 is the one genuine correctness bug
found; §2 is functional gaps a real user would hit; §3 is capacity/
lifecycle limits that degrade or fail under sustained use; §4 is named,
deliberate scope cuts that remain reasonable to leave alone until a real
workload asks. Within each group, items are ordered by recommended
priority.

## 1. A real correctness bug: `databases[]`/`database_grants[]` are not persisted, and the id allocator resets — CLOSED

**Closed (Gap 1 remediation, as built).** `persist_databases()` now
snapshots `databases[]` + `database_grants[]` + `database_next_id` under
`PERSIST_MAGIC_DATABASE` (LBA 6688-6752, frame counts from real
`sizeof()`: 44B×32 + 340B×64), called after every `database_create()`/
`database_drop()`/`database_grant_uid()`/`database_grant_group()`.
`database_next_id` rides in the header's third uint32 (the same
header-carried-scalar trick `persist_row_constraints()` uses for its
count) — restoring it is the load-bearing half, closing the
silent-reattachment reset described below. `database_grant_count` is
deliberately NOT persisted: it's a high-water mark recomputed from the
restored array's active flags, the same derived-not-stored treatment
`row_journal_attachment_count` gets. Restore is `persist_restore_all()`
block 12, with the established magic + size-mismatch cold-start fallback
(cold start lands exactly on pre-Gap-1 behavior, logged rather than
silent). Verified by 5 new round-trip checks in
`tests/persist_rdbms_vecstore_host_test.c` — real `persist_databases()`
→ wipe-to-compile-time-defaults "reboot" → real restore, asserting the
definition, the grant (in its original, deliberately non-zero slot), the
recomputed high-water grant count, `next_id == 8` post-restore, and a
stomped-magic corruption case cold-starting cleanly. The link-time
ripple (persist.c ↔ database.c now reference each other's symbols) hit
10 pre-existing host tests, fixed with the established dummy-globals /
no-op-stub patterns; full sweep 43/43. The original finding is kept
below for the record.

**`kernel/persist.c` has zero reference to `databases[]`,
`database_grants[]`, or `database_next_id` — confirmed by grep.** Row
tables, their schemas, constraints, index definitions, and each object's
`database_id` tag ARE all persisted and restored. The database
*definitions* those tags point at are not. After a reboot:

- Every table's `database_id` tag survives (object catalog persistence),
  but `databases[]` is empty — every tagged table is an orphan pointing
  at a database that no longer exists. `DROP DATABASE`'s emptiness check
  scans by id, so the vanished database can't even be dropped-and-
  recreated cleanly around its own stale tags.
- All database grants are gone. Fail-closed (`database_check_access()`
  returns "not granted" for an unknown id, and database grants are
  purely additive), so this loses access rather than leaking it — but
  operators must re-grant everything after every boot.
- **The sharp edge: `database_next_id` resets to 1.** §1.2 of the
  Namespace roadmap chose a monotonic, never-reused id allocator
  *specifically* to prevent stale-tag reattachment (the ghost-row
  failure class, one level up). That guarantee only holds within one
  boot: the first `CREATE DATABASE` after a restart gets id 1, and any
  table persisted with a `database_id` of 1 from a *previous* boot's
  entirely different database silently attaches to the new one —
  including inheriting whatever grants the new database is given. The
  exact silent-reattachment failure §1.2's design exists to prevent,
  reintroduced through the persistence hole.

**Recommended fix (small, follows existing machinery exactly):** a
`persist_databases()` writing `databases[]` + `database_grants[]` +
`database_next_id` under a new `PERSIST_MAGIC_DATABASE`, called from
`database_create()`/`database_drop()`/both grant functions, restored in
`persist_restore_all()` with the established size-mismatch cold-start
fallback. Until then, the practical mitigation is treating databases as
per-boot scratch state — which nothing in the docs currently warns
about. (`group_profile.c`/`authlist.c` share the same non-persistence,
noted here since `database.h` explicitly models its conventions on them;
that half is security-layer scope, not this doc's.)

## 2. Functional gaps a real user hits

**2.1 — No `REVOKE` for database grants — CLOSED (Gap 3 remediation, as
built).** `database_revoke_uid()`/`database_revoke_group()` now exist at
every layer the grant surface exists at: kernel functions (order-
preserving in-place compaction; distinct rc=2 "nothing to revoke" rather
than collapsing into success), syscalls 266-267 + dispatch, and
`database revoke uid/group` Terminal commands — grants never had an HTTP
mutation route, so revoke deliberately doesn't either (matching
surfaces, not exceeding them). Removing the LAST grantee of either kind
deactivates the whole grant entry, so a later re-grant starts from a
clean slate instead of silently resurrecting a stale shared `perm_mask`
(tested explicitly). Revokes persist via Gap 1's `persist_databases()`.
Verified with 12 new checks in `database_grant_phase3_host_test.c`
(scenarios 7-8): real revoke through the real `catalog_check_access()`
chain, scoped removal (co-grantees unaffected), double-revoke and
unknown-name/group error paths, last-grantee entry deactivation, and
fresh-grant-after-full-revocation. Found and fixed in passing: both
pre-existing `database grant uid/group` Terminal commands had prefix-
length off-by-ones (+20/+22 vs real strlen 19/21) silently eating the
database name's first character — the exact hand-counted-offset bug
class `user/shell.c`'s own comments already warned about once. The
shared one-`perm_mask`-per-database design (§1.4) remains unchanged and
remains the deeper limit: there is still no per-grantee permission
narrowing, only whole-grantee removal — a revoke removes you outright.
The original finding is kept below in §5's ordering for the record.

**2.2 — No SQL `GRANT`/`REVOKE` (§1.5).** Grants are Terminal/HTTP-only.
Fine as a decision, but combined with 2.1 the full grant lifecycle is
unmanageable from SQL entirely.

**2.3 — No `ALTER TABLE ... SET DATABASE` — CLOSED (Gap 4 remediation,
as built).** `ALTER TABLE t SET DATABASE <name>` reassigns a row-set
table's database tag; `ALTER TABLE t SET DATABASE NULL` untags it
(database_id back to 0/NONE — reusing the existing NULL keyword,
semantically "no database," rather than inventing a NONE keyword that
would shadow another identifier). `SqlAlterTableStmt` gained an
`alter_kind` discriminator (zero-default = ADD COLUMN, so every
pre-existing statement parses byte-identically). The executor branch is
a pure catalog-metadata retag — no storage touched, no rows moved —
gated through `catalog_check_access()` (PERM_WRITE) and persisted via
`persist_catalog()` (the tag lives on the catalog entry, so it's the
catalog snapshot's job, not Gap 1's database one). Access follows the
tag live in both directions, proven by test: a `dst` grantee gains
access the moment a table moves in, and loses it the moment the table
is untagged. §1.6/§1.7's "empty a database means dropping its tables
outright" friction is closed with a real reassignment path — verified
end-to-end (refuse-drop → reassign → drop succeeds) in
`sql_database_phase2_host_test.c` scenario 8 (13 new checks, including
unknown-database/unknown-table error paths and full parser coverage of
both forms plus the ADD COLUMN zero-default).

**2.4 — Views, CTEs, set operations, correlated subqueries (SQL parity
Phase 7's own honest cut).** Phase 7 shipped non-correlated scalar and
`IN (SELECT ...)` subqueries only; `CREATE VIEW`, `WITH`,
`UNION`/`INTERSECT`/`EXCEPT`, and correlated subqueries were explicitly
deferred as "a real nested-execution model," worth its own scoping doc.
Still the largest single block of missing SQL surface.

**2.5 — `RIGHT`/`FULL OUTER JOIN`, and `GROUP BY` combined with any
JOIN.** The latter (`SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED`) is the more
painful in practice — aggregate-over-join is a bread-and-butter query
shape. Named in Phases 1/2, still open.

**2.6 — `ALTER TABLE` is ADD COLUMN only.** No `DROP COLUMN`, no
`RENAME`, no `ADD CONSTRAINT`/`DROP CONSTRAINT` (see 3.2). `rowstore.c`'s
own comment names DROP COLUMN/RENAME as "real, separate, bigger
storage-layout undertakings."

**2.7 — `RANGE` constraints have no SQL syntax — CLOSED (Gap 6
remediation, as built).** `CHECK (col BETWEEN lo AND hi)` now exists as
an inline column clause in CREATE TABLE (new CHECK/BETWEEN reserved
keywords; AND reused). Only the same-column BETWEEN shape is supported —
the inner column name must match the column being defined, validated at
parse time and rejected loud rather than silently misapplied, matching
`row_constraint.c`'s own single-column RANGE model; general CHECK
expressions remain Phase 23's original non-goal. The executor registers
it via the existing `row_constraint_add_range()`, so registration-time
bound validation (unparseable bounds vs. the column's type) surfaces at
CREATE time, not first insert (tested). `sql_schema_export()` now emits
the clause inline as reconstructable SQL — the `-- note:` lossy-export
path and its `out_has_range` plumbing are deleted, with STRING bounds
re-quoted and numeric bounds bare. Verified both directions: DDL →
enforcement (in-range passes, out-of-range INSERT and UPDATE rejected)
in `sql_ddl_phase5_host_test.c` (scenarios 8h-8m parser, 9 executor),
and the full round-trip loss closed in
`sql_schema_export_import_host_test.c`'s rewritten scenario 2 (create
via DDL → export contains the CHECK clause, no note → drop → import →
the re-imported table still rejects out-of-range inserts).

**2.8 — FK actions are delete-side only — SILENT-ORPHAN HALF CLOSED
(Gap 5 remediation, as built).** The dangerous half of this asymmetry is
fixed: `row_constraint_check_parent_update()` runs from
`mvcc_row_update()` alongside the existing outbound-FK check, and blocks
(RESTRICT, `VIOLATION_REFERENCED`) any update that rewrites a referenced
parent's key column while children still point at the old value. The
check is precise, not blanket: updates to non-key columns of a
referenced parent pass with no child scan, an unchanged key passes, a
NULL old key passes (nothing can reference NULL), and a key rewrite on a
row with no actual children succeeds — all tested (9 new checks,
scenario 19 of `row_constraint_journal_host_test.c`), including the
before/after pair (blocked while referenced, succeeds after the child is
deleted). RESTRICT applies to EVERY constraint regardless of its
`on_delete_action` — a CASCADE-on-delete parent's key rewrite is still
blocked, not cascaded (tested), because `ON UPDATE CASCADE`/`SET NULL`
actions remain deliberately unbuilt (the parser still rejects `ON
UPDATE` outright): `on_delete_action` governs deletes only, and full
update-side actions stay a named deferral per this section's original
"only if wanted" framing — no longer a correctness hole, now purely a
convenience feature waiting for a real workload to ask.

## 3. Capacity & lifecycle limits

**3.1 — Constraint pool slots leak on `DROP TABLE` — CLOSED (Gap 2
remediation, as built).** `rc_add()` now scans `row_constraints[]` for
the first inactive slot before allocating, exactly the find-free-slot-
first shape `row_index_create()` always had; `row_constraint_count`
stays a high-water mark (every iteration loop and `persist_row_
constraints()` bound on it), growing only when a genuinely fresh slot
past it is taken. Verified in `row_constraint_journal_host_test.c`
scenario 18: deactivate a slot in place (byte-for-byte what `rowstore_
drop_table()`'s cleanup loop does), register a new constraint, assert it
landed in the reused slot with the count unchanged, and confirm the
slot-reused constraint genuinely enforces through a real SQL INSERT
rejection. Original finding, for the record: `rc_add()` was a pure bump
allocator (`row_constraints[row_constraint_count++]`) that never scanned
for deactivated slots, so repeated create-with-constraints/drop cycles
exhausted `ROW_CONSTRAINT_MAX` (64) permanently until reboot.

**3.2 — No constraint removal API at all.** Constraints die only with
their table. No `ALTER TABLE DROP CONSTRAINT`, no direct-API removal. A
mistyped RANGE or an obsolete UNIQUE means recreating the table.

**3.3 — Fixed pools, no reclaim, single boot-lifetime budgets:**
`MVCC_MAX_VERSIONS` 4096 shared across all tables (reclaim only on
rollback — every committed update/delete permanently consumes versions);
`ROW_JOURNAL_MAX_ENTRIES` 16 (a deliberately tiny ring, ~30x-larger
entries than legacy journal.c's); `CURSOR_MAX` 8 (the cascading phase's
own test work showed how easily un-closed SELECT cursors exhaust it —
callers must close diligently, nothing reaps leaked cursors);
`DATABASE_MAX` 32 / `DATABASE_GRANT_MAX` 64; `ROWSTORE_MAX_COLUMNS` 16.
The MVCC version pool is the one most likely to bite first under real
sustained writes.

**3.4 — DDL is not transactional.** A mid-sequence `CREATE TABLE`
failure leaves partial state (valloc'd but unpromoted, or missing later
constraints); `DROP DATABASE CASCADE`'s partial-failure mode leaves
earlier-dropped siblings dropped. All named honestly at each site, none
rolled back. Acceptable posture, but it accumulates as more multi-step
DDL (CASCADE being the newest) is built on top.

**3.5 — Cascade caps.** `ROW_CASCADE_MAX_ROWS` 64 children per
constraint per delete, `ROW_CASCADE_MAX_DEPTH` 8. Overflow fails the
delete cleanly (tested) — but a parent with 65+ children simply cannot
be deleted via CASCADE at all; the operator must pre-delete children in
batches. Fine for now; worth remembering when 3.3's pools grow.

## 4. Named scope cuts that remain reasonable

Deliberate decisions, re-affirmed rather than re-litigated here:
triggers (never scoped anywhere); composite multi-column constraints and
indexes; dynamic typing beyond Phase 4's groundwork; `LIKE ... ESCAPE`;
aggregate result aliasing (`COUNT(*) AS total`); cross-database JOIN
freely allowed (§1.8, a non-goal); databases as logical grouping rather
than true namespacing — no qualified `db.table` names (§1.1); legacy KV
path frozen per Phase 24's Option B with the `uses_rowstore` guard;
`LIST DATABASES` staying Terminal/HTTP-only; no per-grantee permission
levels within one database grant (the shared `perm_mask`, §1.4 — though
see 2.1 for where it chafes).

## 5. Suggested priority order

1. **§1 database persistence** — ~~the only silent-corruption-class item~~
   **DONE** (see §1's closure addendum).
2. **§3.1 constraint slot reuse** — **DONE** (see §3.1's closure
   addendum).
3. **§2.1 `REVOKE`** — **DONE** (see §2.1's closure addendum). §2.2's
   SQL `GRANT`/`REVOKE` grammar did NOT ride along — grants stay
   Terminal-only per §1.5's standing decision, now consistently for both
   directions.
4. **§2.3 `ALTER TABLE ... SET DATABASE`** — **DONE** (see §2.3's
   closure addendum).
5. **§2.8 ON UPDATE FK asymmetry** — **DONE** (the RESTRICT minimum; see
   §2.8's closure addendum — full ON UPDATE actions remain a named
   deferral, now convenience-only rather than a correctness hole).
6. **§2.7 RANGE/CHECK SQL syntax** — **DONE** (see §2.7's closure
   addendum).
7. **§2.4/2.5 query-surface items** (views/CTEs/set ops; GROUP BY over
   JOIN) — **SCOPED**: see
   `AeroSLS-SQL-Query-Surface-Roadmap-v0.1.md` (7 phases: GROUP BY over
   JOIN; real-NULL padding + RIGHT/FULL OUTER; depth-2 reentrancy
   groundwork; set operations; views; CTEs; correlated subqueries
   deliberately unscheduled).

Items in §3.3/§3.4 are watch-items, not action items, until something in
1-7 or real usage forces them.
