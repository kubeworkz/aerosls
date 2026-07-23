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

**2.1 — No `REVOKE` for database grants (§1.9, deliberate but now the
sharpest auth gap).** Grants are strictly additive with no removal API at
any layer — kernel, syscall, HTTP, Terminal, or SQL. Worse, `perm_mask`
is one shared field per database (every grant call overwrites it for ALL
grantees), so the only way to narrow one uid's access is to... not exist.
A `database_revoke_uid/_group()` pair is mechanical to add; the shared-
perm-mask design (a named simplification) is the deeper limit.

**2.2 — No SQL `GRANT`/`REVOKE` (§1.5).** Grants are Terminal/HTTP-only.
Fine as a decision, but combined with 2.1 the full grant lifecycle is
unmanageable from SQL entirely.

**2.3 — No `ALTER TABLE ... SET DATABASE` (§1.7).** A table's database
assignment is fixed at creation. §1.6's own text names the consequence:
"empty a database" means dropping tables outright (or now, CASCADE).
The roadmap itself calls this "purely not built yet, would be easy to
add: extend `ALTER TABLE`'s existing grammar with one more clause."
Cheapest high-value item in this list.

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

**2.7 — `RANGE` constraints have no SQL syntax.** Registerable only via
the direct API; `sql_schema_export()` emits a `-- note:` comment rather
than a reconstructable statement, so a schema round-trip silently loses
them (named in the export's own output, but still a loss). A `CHECK
(col BETWEEN x AND y)`-shaped grammar clause would close both the DDL
gap and the round-trip gap at once.

**2.8 — FK actions are delete-side only.** `ON UPDATE
CASCADE`/`SET NULL` don't exist (the parser explicitly rejects `ON
UPDATE`, tested). Updating a parent's referenced key while children
point at it is currently allowed and silently orphans them — the write-
side REFERENCE check validates the *child's* new value on child writes,
but nothing re-validates children when the *parent's* key changes. That
asymmetry is worth naming as the follow-on to the cascading phase.

## 3. Capacity & lifecycle limits

**3.1 — Constraint pool slots leak on `DROP TABLE`.** `rowstore_drop_
table()` deactivates a dropped table's `row_constraints[]` entries, but
`rc_add()` is a pure bump allocator (`row_constraints[row_constraint_
count++]`) that never scans for deactivated slots — unlike
`row_index_create()`, which does reuse free slots (confirmed by direct
read of both). Repeated create-with-constraints/drop cycles exhaust
`ROW_CONSTRAINT_MAX` (64) permanently until reboot. One-line-class fix:
give `rc_add()` the same find-free-slot-first scan its sibling has.

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
2. **§3.1 constraint slot reuse** — one-line-class fix for a permanent
   resource leak.
3. **§2.1 `REVOKE`** (kernel + Terminal/HTTP), with §2.2 SQL grammar
   optionally riding along.
4. **§2.3 `ALTER TABLE ... SET DATABASE`** — cheapest functional win.
5. **§2.8 ON UPDATE FK asymmetry** — at minimum a RESTRICT-style check
   on parent-key updates, closing the silent-orphan hole; full ON UPDATE
   actions only if wanted.
6. **§2.7 RANGE/CHECK SQL syntax** — closes a schema round-trip loss.
7. **§2.4/2.5 query-surface items** (views/CTEs/set ops; GROUP BY over
   JOIN) — the big-ticket work, each deserving its own scoping pass, in
   whichever order a real workload demands.

Items in §3.3/§3.4 are watch-items, not action items, until something in
1-7 or real usage forces them.
