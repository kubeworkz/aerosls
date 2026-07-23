# AeroSLS VectorStore Gap Analysis v0.1

## 0. Why this doc exists

`AeroSLS-VectorStore-Interface-Roadmap-v0.1.md`'s six phases (deletion,
semantic search, HNSW rebuild/backfill, a dedicated frontend tab, schema
export/import, bulk data export/import) are all marked DONE. This doc is
the honest inventory of what's STILL outstanding across that roadmap and
its own follow-ons, refreshed against the code as it exists today (every
claim below checked by direct grep/read during this analysis, not
recalled from roadmap prose). Same exercise `AeroSLS-Database-Gap-
Analysis-v0.1.md` did for the database layer, scoped here to the
VectorStore subsystem (`kernel/vecstore.c`, `kernel/vec_index.c`,
`kernel/vec_join.c`, `net/ollama_client.c`, `SlsVectorStore.tsx`).

Findings are grouped by severity: §1 is functional gaps a real user would
hit; §2 is test-coverage limits that have accumulated across phases; §3
is a cross-roadmap gap this subsystem never got folded into; §4 was
originally four named, deliberate scope cuts -- re-investigated in a
follow-on pass, three turned out to be real, worth-fixing gaps after all
(now addressed) and one was re-affirmed as still a genuine, correct
design choice. Within each group, items are ordered by recommended
priority.

## 1. Functional gaps

### 1.1 Bulk vector DATA export/import has zero frontend wiring — CLOSED

**Closed.** `SlsVectorStore.tsx`'s Collections panel now has a per-row
"Data" export button (`handleExportData()`, `GET /api/vec/data/export/
<collection>` — kept per-row rather than a single header button, since
this route is scoped to one collection per call, unlike schema export's
"every readable collection at once") and a header-level "Import Data"
button (`handleImportDataFile()`, `POST /api/vec/data/import`, same
hidden-file-input + POST shape as the existing schema import button,
since a `VECTOR` line names its own target collection and one dump can
cover more than one). The existing explanatory paragraph was extended to
distinguish the two pairs and to name the buffer-size truncation
behavior honestly (`vec_data_export()`'s own real limit, see §1.4 below —
the export status message reports `vectors_written`/`vectors_total`
directly rather than hiding a partial batch). `npx tsc --noEmit -p .` and
`npm run lint` both clean; `npm run test` (52/52) unaffected, since this
is a UI-only change against an already-complete, already-host-tested
backend chain.

**Original finding (closed above), kept for the record.** Phase 6's own
text said plainly: "Not yet wired into the VectorStore tab as of this
phase... a separate, explicitly named follow-up." That follow-up never
happened until now. Confirmed directly: `SlsVectorStore.tsx` had
`handleExportSchema()`/`handleImportSchemaFile()` (Phase 5's definitions-
only export/import) but no reference anywhere to `/api/vec/data/export`
or `/api/vec/data/import` — grepping the whole file for `data/export`,
`data/import`, or `vec/data` returned nothing. The kernel/syscall/HTTP/
Terminal chain was already complete and host-tested (`vec_data_export_
import_host_test.c`, 42 checks), so this was pure UI wiring, not new
backend design.

### 1.2 `vecstore_create_collection()` has no permission gate — CLOSED

**Closed.** `vecstore_create_collection()` (`kernel/vecstore.c`) gained a
`caller_uid` parameter and a `catalog_check_access(caller_uid, collection_
name, PERM_WRITE)` gate (return code 2 on denial), matching the level
`vecstore_insert()`/`vecstore_delete()` already use — placed right after
the catalog-object existence check, before the "already a vector
collection" check, so a caller without write access learns nothing about
whether the object is already promoted. Both call sites were updated to
thread a real `caller_uid` through: `sys_sls_vec_create()` (already had
`req->caller_uid` sitting unused in `SLSVecCreateRequest`) and
`vec_schema_import()`'s own `COLLECTION` line handler (`kernel/
vec_index.c`, already had `caller_uid` in scope as its own parameter).
The `vec_index.h` comment that named this exact gap (near `vec_schema_
import()`) is updated to say so. Seven test files called this function
directly with the old 2-argument signature; all seven were confirmed
safe to update with a plain `1` (checked each one's own `catalog_check_
access()` stub: all default-allow regardless of uid, and none of the
seven call sites falls inside a scenario's "force deny" window) and
updated via a targeted `sed` pass. `npx tsc`/frontend build untouched
(kernel-only change). Full 48-file host-test regression: 0 failures.
`gcc -fsyntax-only` clean in both host and freestanding (`-ffreestanding
-fno-pie -fno-pic`) styles for both modified files.

**Original finding (closed above), kept for the record.** Named honestly
in Phase 5's own "real differences" section and never revisited until
now. Confirmed directly (`kernel/vecstore.c:148-165`, pre-fix line
numbers): every other entry point in the file — insert, get, delete,
scan, the schema-export read gate — called `catalog_check_access()`;
collection creation called it nowhere. Any caller who could reach `vec
create` (or `vec_schema_import()`, which called this function exactly
as-is) could create a collection regardless of role.

### 1.3 `external_id` is never deduplicated — CLOSED

**Closed.** Added the opt-in `UNIQUE`-style constraint this doc's own
original finding named as the fix, matching the row-store's own `row_
constraint.c` precedent (`row_constraint_add_unique()`) rather than SQL's
inline `UNIQUE` column syntax, since this subsystem still has no schema-
definition statement of its own to attach a flag to at CREATE time.
`VecCollectionHeader` gained a `unique_external_id` byte (zero-initialized
— off by default for every collection, old and new alike), toggled only
via the new `vecstore_set_unique_external_id(caller_uid, collection_name,
enabled)` (gated on `catalog_check_access(..., PERM_WRITE)`, same level
§1.2 gated collection creation on). When on, `vecstore_insert()` full-scans
the collection's own active entries for a matching `external_id` before
writing and returns a new, distinct code (6) if found, rather than
silently duplicating — the same real per-write cost `row_constraint.c`'s
own `rc_unique_scan_cb()` already accepts for row-set `UNIQUE` columns, not
a new trade-off invented here. Turning uniqueness off never scans or
deletes existing rows — it only changes what happens on the next insert,
so duplicates created before opting in (or while opted out) are left
exactly as they are.

Reachable end to end: syscall `SYS_SLS_VEC_SET_UNIQUE` (268, next free
number after `SYS_SLS_DATABASE_REVOKE_GROUP`/267), dispatch wiring, kernel-
native shell (`vec collection unique <name> <on|off>`), HTTP (`POST /api/
vec/collections/unique`, body `{"name", "enabled": 1|0}` — an integer, not
a JSON boolean literal, since this codebase's `json_int()` has no boolean-
literal support and a silent `"true"` -> `0` misparse would have been a
wire-format bug baked in on day one), and the web Terminal (`shellCommands.
ts`, mirroring the native shell command 1:1). `vec_data_import()`'s own
per-line failure reporting gained a distinct message for return code 6, so
a caller who opts a collection into uniqueness before re-running the same
`vec data import` dump gets each duplicate line reported individually
rather than a generic failure.

Verified: `vecstore_host_test.c` gained a 14-check Scenario 14 (default-
off, toggle on/off, duplicate rejection, tombstoned-id reuse, access-denied
path, and "turning uniqueness off doesn't touch existing duplicates").
Full 48-file host-test regression: 0 failures. `gcc -fsyntax-only` clean
in both host and freestanding styles for every touched kernel file.
Frontend: `npx tsc --noEmit`, `npm run lint`, and `npm run test` (101/101,
up from 95) all clean for the new `vec collection unique` Terminal command.
No VectorStore tab UI toggle was added this pass — kept as an explicitly
named, small follow-up (matching this doc's own §1.1 "backend first,
frontend as a separate, explicitly named follow-up" precedent) rather than
silently expanding scope, since the Collections panel's own table has no
per-row settings surface today.

**Original finding (closed above), kept for the record.** Named as
inherited, pre-existing behavior in Phase 6 and confirmed still true at
the time: `vecstore_insert()`'s own header comment said "uniqueness, if
wanted, is the caller's responsibility." Re-running the same `vec data
import` twice (or calling `vec insert`/`vec embed-insert` twice with the
same `external_id` by mistake) silently duplicated the vector rather than
rejecting or overwriting — `entry_count` grew by 2, and a search could
return the same logical item twice under two different `VecId`s.

### 1.4 Bulk data export has no pagination — genuinely tight at real embedding dimensions — CLOSED

**Closed.** `vec_data_export()` gained a `skip_count` parameter and
`VecDataExportResult` a matching `entries_remaining` field: skip the first
`skip_count` active entries in the collection's own physical scan order
without writing them, fill the buffer from the next one, and report how
many entries the caller still hasn't seen (`vectors_total - skip_count -
vectors_written`, floored at 0). A caller walks an entire collection by
starting at `skip_count=0` and, after each call, advancing `skip_count` by
that call's own `vectors_written`, stopping once `entries_remaining`
reaches 0 — the request buffer's 8192-byte (or smaller) size no longer
caps how much of a collection is actually reachable, only how many calls
it takes. Named honestly rather than oversold: this costs an
`O(collection size)` full scan on every call (skipped entries are still
visited and counted, just not serialized), because `vecstore_collection_
scan()` has no random-access-by-ordinal primitive to skip ahead more
cheaply — correct, not the cheapest possible resumption mechanism, and a
real, deliberate trade-off rather than an oversight; worth revisiting only
if a real workload's collection sizes make repeated full scans a measured
problem.

Reachable end to end: the syscall struct (`SLSVecDataExportRequest`)
gained a `skip_count` input field (same `SYS_SLS_VEC_DATA_EXPORT`
number — this is a request-shape change, not a new syscall); the kernel-
native shell's `vec data export <collection> [skip]` takes an optional
trailing token; the HTTP route grew an optional `/skip/<N>` trailing path
segment (`GET /api/vec/data/export/<collection>[/skip/<N>]`) rather than a
query string, matching this file's own established "no query-string
parsing infrastructure exists here, path segments are the real precedent"
posture (`/api/tables/<name>/schema`'s own convention). The VectorStore
tab's per-collection "Data" export button (§1.1) now loops on this route
automatically, following `entries_remaining` until it reaches 0, stripping
the repeated header comment from every page after the first, and handing
the user ONE combined download — not a partial file with no indication
more existed. The web Terminal's own `vec data export` command was not
touched (it doesn't exist there yet — see §2's own separate, low-priority
finding on porting `vec schema/data export/import` to the web Terminal;
out of scope for this fix specifically).

Verified: `vec_data_export_import_host_test.c` gained checks for
`entries_remaining` on both the whole-collection and truncated cases, plus
a new pagination scenario that walks a deliberately small buffer across
enough calls to cover the whole collection, confirming every entry is
visited exactly once (no duplicates, no omissions) and `entries_remaining`
reaches 0 at the end. Full 48-file host-test regression: 0 failures.
`gcc -fsyntax-only` clean in both host and freestanding styles for every
touched kernel/net/user file. Frontend: `npx tsc --noEmit`, `npm run
lint`, and `npm run test` (101/101, unaffected — this was a UI-only change
to `SlsVectorStore.tsx`, not a Terminal command) all clean.

**Original finding (closed above), kept for the record.** Named explicitly
at design time and still true when this analysis was written: the export/
import request buffer was 8192 bytes (matching `SQL_SCHEMA_EXPORT_MAX_LEN`,
sized to stay safe as a kernel-stack local). At the 384-1024-float range
the roadmap itself surveys for real embedding models, a single vector's
line could approach or exceed the whole buffer, so one `vec data export`
call could capture only a handful of vectors — occasionally zero at the
higher end (1536+, e.g. some newer OpenAI/Ollama models) — per call, with
`result->truncated` at least reporting this honestly rather than hiding
it. There was no cursor/offset mechanism to resume a walk across multiple
calls for a large collection.

## 2. Test-coverage gaps that have accumulated across phases — CLOSED (for existing commands)

**Closed.** `shellCommands.test.ts` gained 43 new checks (52 → 95, all
passing) covering the 13 previously-untested `vec*` commands: `vec
create`, `vec list`, `vec search` (+ its `metric=`/`k=` kv parsing), `vec
search-text` (including the embed-failure path surfacing `ollama_status`
distinctly, not a generic error), `vec join`, `vec index create/list/
search` (+ its `ef` defaulting to `k` when omitted)/`search-text`/
`rebuild`, `vec delete` (+ a kernel-reported-failure path), `vec
collection drop`, and `vec index drop` — plus `isDestructive()` checks
confirming the three genuinely destructive commands are flagged and `vec
index rebuild`/`vec search` (repair and read-only, respectively) are not.
`npx tsc --noEmit -p .` clean; `npm run test` 95/95.

**A real, separate finding surfaced while doing this pass, not silently
folded in as if it were the same gap:** `vec schema export/import` and
`vec data export/import` — named in this doc's own original §2 text as
part of the untested set — turned out not to be untested commands at
all. Confirmed by grep across `shellCommands.ts`: those four were never
implemented as web-Terminal commands in the first place. They exist only
in the AeroSLS kernel's own native shell (`user/shell.c`, reachable
inside a real booted instance, not through the browser simulator) and,
since §1.1's fix, as direct buttons in the VectorStore tab's Collections
panel (which call the same HTTP routes directly, bypassing this router
entirely). Adding four brand-new commands was out of scope for a test-
coverage pass — recorded here as its own small, real gap rather than
quietly building it as an unrequested side effect of "add tests."

**Original finding (closed above for the 13 that existed), kept for the
record.** Each phase named its own frontend-test gap individually as
small and acceptable at the time ("the two new commands' own request/
response marshaling is verified by direct code review... not an executed
test"). This had compounded across five phases into a real, sizeable
blind spot: only `vec insert` and `vec embed-insert` had any test
coverage at all, out of what turned out to be 15 total `vec*` commands.

Separately, and named as a real, still-open limit back in Phase 1: no
executed test links the real `object_catalog.c` and the real
`vecstore.c` together to prove `sys_sls_vfree()` → `vecstore_notify_
object_freed()`'s call-order contract end-to-end. It's verified by direct
code review and by testing `vecstore_notify_object_freed()`'s own logic
in isolation, matching the real call order — a real gap if that call
order is ever refactored without noticing the implicit dependency.

## 3. Cross-roadmap gap: the VectorStore sits entirely outside the database namespace

**Closed** (narrower than originally feared — see below). A vector
collection is never a standalone struct: `vecstore_create_collection()`
always promotes a pre-existing `object_catalog[]` entry (found by name),
and every `vecstore.c` CRUD entry point (insert/get/delete/scan/create/
set_unique) already gates through `catalog_check_access(caller_uid,
collection_name, PERM_*)` — the exact same choke point every SQL table
uses. `catalog_check_access()` itself already runs
`database_check_access(uid, e->database_id, needed_perm)` against that
same shared entry's `database_id` field (Database Namespace & Access
Roadmap Phase 3, `object_catalog.c` line ~197). So the access-control
mechanism was never actually missing: once a vector collection's
underlying catalog object carries a `database_id`, it is already
database-scoped, with zero `vecstore.c` changes. This was proven directly
by extending `tests/database_grant_phase3_host_test.c` (new Scenarios
9-12): a `sys_sls_valloc()`-created object tagged with a real
`database_id`, and a `catalog_check_access()` check against it, both
running through the exact same code every vector collection's own
`catalog_check_access()` calls already use.

What was genuinely missing, found while tracing this end to end:

- **A live bug, not scoped to vectors at all**: `struct SLSVallocRequest`
  gained a `database_id` field when the Database Namespace Roadmap shipped,
  with a header comment saying it's "0 (NONE) if the caller doesn't set it
  — matching every pre-Phase-8 valloc call site's zero-initialized request
  struct" convention. That convention was violated: of the 11
  `SLSVallocRequest` construction sites across the codebase, only
  `sql_exec.c`'s own `CREATE TABLE` path (`se_memset(&vreq, 0, ...)`)
  actually zeroed it. The other 10 — `user/shell.c`'s `valloc` and
  `journal create` commands, `net/http.c`'s `POST /api/valloc`, `POST
  /api/program/create`, and `POST /api/journal` routes, and three call
  sites each in `kernel/agent.c` (agent/memory-table/workflow creation),
  one in `kernel/mqt.c`, and two in `kernel/stream.c` — left `database_id`
  as uninitialized stack garbage on every object they created. Fixed by
  explicitly zeroing it at all 10 sites.
- **No way to assign a database at creation time or after the fact**: the
  vector-collection-facing surface (`POST /api/valloc`, native shell
  `valloc`) had no way to set a nonzero `database_id`, and there was no
  generic equivalent of `ALTER TABLE ... SET DATABASE` (`sql_exec.c`'s
  `exec_alter_table()`, database-gap-analysis §2.3) for non-table catalog
  objects. Closed two ways: `POST /api/valloc` and native shell `valloc`
  both gained an optional trailing database name, resolved via
  `database_find_id()` the same way `IN DATABASE` does; and a new generic
  primitive, `catalog_set_database()` (`kernel/object_catalog.c`), mirrors
  `exec_alter_table()`'s direct `object_catalog[].database_id` write and
  `catalog_check_access()` PERM_WRITE gate but drops its `uses_rowstore`
  restriction, so it reaches any catalog object — including an
  already-promoted vector collection, which has no `ALTER` verb of its
  own. Reachable via `SYS_SLS_OBJECT_SET_DATABASE` (269), native shell
  `object set database <name> <database|none>`, `POST
  /api/objects/database`, and the web Terminal's `object set database`
  command.

**Original finding (closed above), kept for the record.** Not named in
either VectorStore roadmap phase, found during this analysis by checking
whether `struct VecCollectionHeader` carries any of the same namespacing
the Database Namespace & Access Roadmap (Phases 1-5, DONE) added to every
SQL table. It doesn't: confirmed by grep, `kernel/vecstore.h` has no
`database_id` field, no `database_grants[]` equivalent, nothing
resembling `USE DATABASE` scoping. Every relational table now belongs to
a database with its own grant list (`kernel/database.c`); every vector
collection is still global and flat, reachable and creatable by any
caller with catalog access, regardless of which "database" a related SQL
schema lives in. This is a real architectural asymmetry between the two
data-storage subsystems this kernel now has, not a bug — nothing is
broken — but a multi-tenant or multi-database deployment that expects
`vec_join.c`'s relational joins to respect the same per-database
isolation its SQL side of the join already enforces would find the
vector side of that join has no such boundary at all. (As it turns out,
the boundary was already there at the access-control layer — the gap was
the assignment surface and the valloc-time bug, not a missing
enforcement mechanism.)

## 4. Named, deliberate scope cuts — reasonable to leave alone

- **Collections have no metric of their own** (only an index fixes a
  metric) — **re-investigated, re-affirmed: still a real design choice,
  not a gap.** `vecstore_search()` (brute-force, exact) takes its metric
  as a per-call parameter precisely so one collection can be queried under
  L2 today and cosine tomorrow without any schema change; pinning a
  metric to the collection would only take away that flexibility, not add
  anything. An HNSW index (`vec_index_create()`) is a different story on
  purpose: the graph itself is metric-dependent — its neighbor edges are
  literally "closest under metric M" — so metric has to be fixed at
  index-creation time, and it correctly lives on `struct VecIndex`
  (`idx->metric`), not the collection. A collection with zero indexes
  exports with no metric information because none exists to lose, not
  because the exporter dropped it. No code change; this bullet's original
  claim holds up under a second look.
- ~~**No quoting in the schema/data export grammars**~~ — **Addressed.**
  Re-investigation found this reasoning was half right: the grammars
  genuinely don't need quoting *if* every name actually is a plain,
  space-free identifier — but that assumption was never enforced
  anywhere. `user/shell.c`'s `sh_token()` parser can't produce a name
  containing a space, but `POST /api/valloc`'s `json_str()` (`net/http.c`)
  copies any character verbatim out of a JSON string body, including
  spaces and control characters — so a collection or object created over
  HTTP with an embedded space would silently corrupt on the next
  `vec_schema_export()`/`vec_data_export()` round-trip (the space would be
  misread as an extra token boundary by `vse_next_token()`/
  `vs_next_token_bounded()` on import). Rather than add quoting/escaping
  to two separate grammars, the fix enforces the assumption itself at the
  two real name-creation choke points: `sys_sls_valloc()`
  (`kernel/object_catalog.c`, covers every object/collection name,
  including the HTTP path) and `vec_index_create()`
  (`kernel/vec_index.c`, covers index names, a separate namespace that
  never goes through `sys_sls_valloc()`). Both now reject a name that's
  empty or contains a space, tab, CR, LF, or other C0 control character,
  cleanly (return 0/1, matching each function's own existing bad-input
  convention) rather than silently accepting it. This strengthens, not
  reverses, the original reasoning: "no quoting needed because names are
  plain identifiers" is now an enforced invariant instead of an unchecked
  assumption. Covered by new host-test scenarios in
  `tests/database_grant_phase3_host_test.c` (Scenario 13) and
  `tests/vec_index_host_test.c` (Scenario 8b).
- ~~**Rebuild is a synchronous, opt-in scan**, never automatic on `vec
  index create`~~ — **Addressed.** Re-investigation concluded the
  original "acceptable at AeroSLS's current scale" framing undersold a
  real, surprising footgun: a caller creating an index over an
  already-populated collection got back something that silently,
  permanently excluded every pre-existing vector unless they separately
  knew to call `vec_index_rebuild()` — nothing about "creating an index"
  implies "except the data already there." `vec_index_create()`
  (`kernel/vec_index.c`) now backfills automatically (synchronously, not
  async — per this fix's explicit scope), reusing the same
  `vecstore_collection_scan()` + insert path `vec_index_rebuild()` already
  used, moved earlier in the file so both functions share it (this
  codebase's "one real choke point" convention). This stays at
  `vec_index_create()`'s own existing `PERM_READ` gate, unchanged:
  `vecstore_collection_scan()` itself only ever requires `PERM_READ`
  (confirmed directly), and every write from the backfill lands in the
  new index's own freshly allocated node slots, never in the collection —
  unlike `vec_index_rebuild()`'s `PERM_WRITE` gate, which exists because
  THAT function tombstones an *existing* index's prior contents, a real
  destructive action a brand-new, empty index never has. `vec_index_
  rebuild()` itself is unchanged and still the right tool for wiping and
  rebuilding an *existing* index (e.g. after suspected corruption, or a
  large batch of deletes). Covered by updated/new host-test scenarios in
  `tests/vec_index_host_test.c` (Scenarios 5 and 8, updated; Scenario 8b,
  new).
- ~~**No live-Ollama-instance test has ever been run**~~ — **Addressed.**
  `tests/ollama_client_live_host_test.c` now exists: it links the REAL,
  unmodified `net/ollama_client.c` (same as `ollama_client_host_test.c`
  does) but replaces that file's canned-bytes `tcp_connect()`/`tcp_send()`/
  `tcp_recv()`/`tcp_close()` stubs with real POSIX-socket implementations
  at that exact same API boundary — the only thing that changes is the
  transport underneath `ollama_embed()`, which stays completely unaware
  it's talking to real sockets instead of this kernel's own e1000/ARP
  stack. It's self-skipping, not a hard CI gate: a short non-blocking
  connect probe runs first, and if nothing answers at the configured
  endpoint (`OLLAMA_LIVE_HOST`/`PORT`, default `127.0.0.1:11434`) it
  prints `SKIP` and exits 0 — this development sandbox has no reachable
  Ollama instance, confirmed the same way the original mocked test's own
  header comment already documented, so `tests/run_all.sh`'s own
  regression sweep reports this file as a clean pass via that SKIP path
  today. `OLLAMA_LIVE_REQUIRE=1` promotes an unreachable endpoint to a
  hard failure for an environment that's expected to always have one.
  All four code paths were verified directly against a real local test
  HTTP server standing in for Ollama during this work: full success
  (`ALL CHECKS PASSED`, real embedding parsed off a real socket), a
  reachable-but-non-2xx response (treated as a pass for "wire format
  round-tripped against a genuine server," distinct from "model not
  pulled"), the default unreachable-SKIP path (exit 0), and the
  `OLLAMA_LIVE_REQUIRE=1` hard-fail path (exit 1). This closes the
  automation gap honestly, without requiring this specific sandboxed
  environment to have a live Ollama instance available — the
  infrastructure is real and repeatable; running it against a genuine
  Ollama server is a `git pull` + one command away for anyone who has one.

## 5. Suggested priority if any of this gets picked up

1. ~~**§1.1** (wire the Data export/import buttons into `SlsVectorStore.
   tsx`)~~ — **CLOSED.**
2. ~~**§2** (a consolidated Terminal command test pass)~~ — **CLOSED**
   for all 13 pre-existing untested commands; surfaced one new, small,
   separate finding (four commands that were never ported to the web
   Terminal at all) rather than closing it silently.
3. ~~**§1.2** (permission gate on collection creation)~~ — **CLOSED.**
4. ~~**§3** (database-namespace parity)~~ — **CLOSED.** Turned out to be a
   much smaller fix than feared, not a feature: access control was
   already free via the shared `catalog_check_access()` choke point; the
   real gaps were a live uninitialized-`database_id` bug (10 call sites)
   and a missing assignment surface, both closed.
5. ~~**§1.3** (opt-in `external_id` uniqueness)~~ — **CLOSED.**
6. ~~**§1.4** (pagination for bulk data export)~~ — **CLOSED.**
7. ~~**§4** (the 3 re-investigated scope cuts: live-Ollama test, name
   validation, auto-backfill)~~ — **CLOSED.** Live-Ollama test
   infrastructure built; name-validation gap and auto-backfill footgun
   both found to be real on re-investigation and fixed; the fourth item
   (collections having no metric of their own) was re-affirmed as a
   correct design choice, not a gap.
8. **New, small: port `vec schema export/import`/`vec data export/
   import` to the web Terminal** (found during §2's own pass) — low
   priority, since the same functionality is already reachable via the
   VectorStore tab's own buttons (§1.1) and the kernel-native shell; only
   worth doing for Terminal-surface completeness.
