# AeroSLS Database Namespace & Access Roadmap v0.1

## 0. Why this doc exists

Scoped in direct response to a user question about the SQL engine: "Do we
have a `CREATE DATABASE` command and the ability to assign users to
databases with different access levels?" The honest answer, confirmed by
reading `sql_parser.c`'s `KEYWORDS[]` table and `sql_parser.h`'s
`SqlStmtKind` enum directly: no. Neither keyword exists anywhere in the
grammar, and there is no namespace/container concept above individual
tables anywhere in this codebase — every table, index, and other object
lives in one single flat array, `object_catalog[CATALOG_MAX_OBJECTS]`
(128 slots, `object_catalog.h:77-78`), distinguished only by a globally
unique name.

This doc scopes what it would take to add a real `CREATE DATABASE`/`DROP
DATABASE` DDL surface and database-scoped access grants on top of the
existing permission stack, without breaking anything already built across
the RDBMS, SQL Feature-Parity, or Navigator-Parity Gap roadmaps. Every
design decision below is a genuine fork with a recommended default, named
explicitly rather than picked silently — several of them are informed
directly by real bugs found earlier in this project (most notably the
DROP TABLE `mvcc_versions[]` ghost-row bug from the SQL Feature-Parity
Roadmap's Phase 8, which is the direct reason this doc rejects a
name-hash-derived database id in favor of a bump-allocated one — see
§1.2).

### Current state (confirmed directly, not assumed)

- **No namespace above tables.** `object_catalog[]` is one flat,
  name-keyed array. `SLSObjectType` (`object_catalog.h:8-23`) has no
  `DATABASE` entry — only `OBJ_TYPE_DB_TABLE`, `OBJ_TYPE_DB_INDEX`, and
  seven other unrelated object kinds.
- **Partition ≠ database.** `partition.h`/`partition.c` (LPAR groundwork)
  is the closest existing analog, but it's a resource-isolation boundary,
  not a logical grouping: `catalog_check_access()` hard-denies any uid
  outside an object's `partition_id` before even checking ownership
  (`object_catalog.c:145`). Every object defaults to `PARTITION_SYSTEM`
  (`= 0`) unless explicitly assigned. Reusing this mechanism for
  "database" would force every database to also become a resource/IPC
  isolation boundary — not what's being asked for, and named here so a
  future reader doesn't mistake partitions for a database substitute.
- **Permissions today are per-object, with two additive layers on top.**
  `catalog_check_access(uid, obj_name, needed_perm)`
  (`object_catalog.c:126-211`) checks, in exact order: kernel bypass →
  partition boundary → owner match → role grants
  (`catalog_role_grants()`, 4 fixed roles: `ROLE_SYSTEM_KERNEL`,
  `ROLE_DB_ADMIN`, `ROLE_APP_USER`, `ROLE_GUEST`) → group membership
  (`group_table[]`, additive OR) → authorization-list fallback
  (`authlist_check_access()`) → GUEST hard-deny → raw `perm_mask`
  fallback. Group profiles (`group_profile.h`) bind a role to up to 16
  member uids. Authorization lists (`authlist.h`) bind up to 8 explicit
  `{object_name, perm_mask}` grants to up to 16 grantee uids and 8
  grantee groups. Neither has any concept of "every table in X" — the
  object set is always an explicit, capped enumeration.
- **SQL DDL exists but is table/index-only.** `SqlStmtKind`
  (`sql_parser.h:376-388`) has `CREATE_TABLE`, `ALTER_TABLE`,
  `DROP_TABLE`, `CREATE_INDEX`, `DROP_INDEX` — no database-level
  statement kind. `KEYWORDS[]` (`sql_parser.c:81-111`) has no `DATABASE`
  token.

---

## §1. Design decisions & forks (recommended defaults, named explicitly)

### 1.1 — Namespace scope: logical grouping only, not true multi-schema namespacing

**Decision: a database is a permission/organizational container, not a
true namespace.** Table names remain globally unique across the whole
catalog exactly as today — `CREATE TABLE users IN DATABASE app` and
`CREATE TABLE users IN DATABASE reporting` would collide, the same as
`CREATE TABLE users` twice today does. The alternative (qualified names,
`app.users` vs. `reporting.users` coexisting) would require touching
every table-name lookup path across `sql_exec.c`, `rowstore.c`, `mvcc.c`,
`row_index.c`, and `object_catalog.c` itself — a much larger, invasive
rewrite of the SQL engine's core identity model, not a namespace feature
bolted on top of it. This is a real, deliberate scope cut, matching this
whole project's established "first cut, not the general case" posture —
named here so a future reader doesn't mistake the globally-unique-name
constraint for an oversight. Revisit only if same-name-across-databases
turns out to matter for a real workload, not speculatively.

### 1.2 — Database identity: a bump-allocated id, NOT a hash of the name

**Decision: `database_id` is a small bump-allocated counter (1, 2, 3, ...
— 0 reserved for "unassigned"), never derived from `fnv1a(name)`.** This
is directly informed by a real bug this project already found and fixed:
`object_catalog.c`'s `object_id = fnv1a(name)` is deterministic per name,
so `DROP TABLE foo; CREATE TABLE foo (...)` recreates the identical
`object_id` — and `mvcc.c`'s own version cache wasn't cleared on drop,
causing old rows to resurrect as ghost rows on the new table's first scan
(SQL Feature-Parity Roadmap Phase 8's own fix,
`mvcc_notify_table_dropped()`). A hash-derived `database_id` would set up
the exact same failure class one level up: `DROP DATABASE app; CREATE
DATABASE app;` would silently reattach to every stale reference any
subsystem forgot to clear. A bump-allocated id makes that class of bug
structurally impossible instead of requiring a matching cleanup fix later
— recreating a database under a previously-used name gets a genuinely
fresh, empty id, and any table still tagged with the *old* id is an
explicit, visible orphan (a real, named consequence — see §1.6) rather
than a silent resurrection.

### 1.3 — Where the tag lives: a new `database_id` field, mirroring `partition_id` exactly

**Decision: add `uint32_t database_id;` to `struct SLSObjectEntry`
(`object_catalog.h:80-99`) and to `struct SLSVallocRequest`
(`object_catalog.h:171-181`), defaulting to `0` (NONE/unassigned) exactly
like `partition_id`'s own established pattern.** Every one of the ~100+
existing tables across every prior phase's host tests gets `database_id =
0` for free via struct zero-init — zero migration, zero behavior change
for any table that never opts in, matching `partition_id`'s own original
rollout exactly. A new small dedicated subsystem, `kernel/database.h/.c`
(mirroring `group_profile.h`/`authlist.h`'s own "small array + name-keyed
lifecycle" shape, not `partition.h`'s heavier isolation-boundary one),
owns the `struct SLSDatabaseEntry { uint32_t database_id; char
name[DATABASE_NAME_LEN]; uint32_t owner_uid; uint8_t active; }` array and
its create/drop/list lifecycle.

### 1.4 — Grant mechanism: a new dedicated grant array, reusing `authlist`'s grantee-list *shape*, not its struct

**Decision: a new `struct SLSDatabaseGrant` array in `kernel/database.h/.c`
— not a "database mode" bolted onto `struct SLSAuthListEntry`.**
`SLSAuthListEntry`'s `objects[AUTHLIST_MAX_OBJECTS]` array is
purpose-built for an explicit, capped, enumerated object list; grafting a
second, entirely different grant scope ("every object with this
database_id") onto the same struct would need a mode flag and would waste
or repurpose that array's meaning depending on the flag — a mixed-purpose
struct, not a clean one. Instead, `SLSDatabaseGrant` reuses the *shape*
of authlist's grantee lists (a fixed uid array + a fixed grantee-group-name
array) as a fresh, small copy — matching this codebase's own
repeatedly-stated convention (e.g. `predicate.c`'s own separate
`pe_parse_f64`, `vec_index.c`'s own `vse_next_token` vs. `sql_exec.c`'s
splitter) that small per-file helper-scale structures get copied, not
retrofitted onto an existing struct with a mode flag, while only
correctness-critical *shared math* (e.g. `vs_topk_insert`) gets actually
exported and reused as one copy. Sketch:

```c
#define DATABASE_NAME_LEN            32
#define DATABASE_MAX                 32
#define DATABASE_GRANT_MAX           64
#define DATABASE_GRANT_MAX_UIDS      16
#define DATABASE_GRANT_MAX_GROUPS     8

struct SLSDatabaseEntry {
    uint32_t database_id;   // bump-allocated, 0 = NONE (see §1.2) — never fnv1a(name)
    char     name[DATABASE_NAME_LEN];
    uint32_t owner_uid;
    uint8_t  active;
};

struct SLSDatabaseGrant {
    uint32_t database_id;
    uint32_t perm_mask;
    uint32_t grantee_uids[DATABASE_GRANT_MAX_UIDS];
    uint32_t grantee_uid_count;
    char     grantee_groups[DATABASE_GRANT_MAX_GROUPS][GROUP_NAME_LEN];
    uint32_t grantee_group_count;
    uint8_t  active;
};

int  database_create(uint32_t caller_uid, const char* name);   // 0=success, bump-allocates database_id
int  database_drop(uint32_t caller_uid, const char* name);     // 0=success, refuses if any table still tagged (§1.6)
void database_list(void);                                       // mirrors group_list()/authlist_list()'s
                                                                  // kernel_serial_printf dump — no SQL involvement,
                                                                  // matching those two subsystems' own precedent
                                                                  // that plain listing never needs SQL grammar
int  database_grant_uid(const char* db_name, uint32_t uid, uint32_t perm_mask);
int  database_grant_group(const char* db_name, const char* group_name, uint32_t perm_mask);
int  database_check_access(uint32_t uid, uint32_t database_id, uint32_t needed_perm);
```

`catalog_check_access()` (`object_catalog.c:126-211`) gets one new
additive check, `database_check_access(uid, e->database_id, needed_perm)`,
inserted **alongside the group/authlist block, before the GUEST hard-deny**
— the exact same placement reasoning that block's own comment already
gives (`object_catalog.c:174-178` area): a uid with no individual role
assignment defaults to `ROLE_GUEST` via `catalog_get_role()`'s own
documented default, so any additive grant source (group, authlist, and
now database) must run *before* that hard-deny or it's silently
unreachable for exactly the uids this feature is meant to serve. This is
a soft, additive grant (like authlist), not a second hard boundary (like
partition) — a database grant can only ever add access, never restrict
what an individual role/owner check already allowed.

### 1.5 — SQL surface: DDL only, no SQL `GRANT` statement

**Decision: `CREATE DATABASE`/`DROP DATABASE` join the existing DDL
grammar; database *grants* stay Terminal/HTTP-level, matching how
`grant`/`revoke` already work today.** This codebase has no SQL `GRANT`
statement at all — `grant <uid> <object> <perm>`/`revoke <uid> <object>
<perm>` are Terminal commands (`user/shell.c`), not `sql_parse()`-reachable
text, and group/authlist management is exclusively Terminal+syscall, never
SQL. Inventing a SQL `GRANT ... ON DATABASE ... TO ...` statement would be
new grammar territory well beyond "add a namespace layer," and would be
the only SQL-level permission statement in the whole engine — a
significant, unjustified inconsistency. New Terminal commands instead,
mirroring the existing `group`/`authlist` command families exactly:

```
database create <name>
database drop <name>
database list
database grant uid <name> <uid> <perm>
database grant group <name> <group> <perm>
```

New SQL grammar, minimal and DDL-only:

```
CREATE DATABASE <name>
DROP DATABASE <name>
CREATE TABLE <name> ( ... ) [IN DATABASE <db_name>]
```

`IN DATABASE` reuses the existing `TOK_KW_IN` token (already used for
`IN (...)` predicate lists — one keyword, two grammar positions,
consistent with how `sql_parser.c` already reuses tokens contextually)
followed by a new `TOK_KW_DATABASE` and a `TOK_IDENT`. The natural
insertion point is `parse_create_table_body()`
(`sql_parser.c:1223-1246`), right after the column list's closing
`TOK_RPAREN` is consumed and before `finish_statement(p)` is called — an
optional trailing clause, absent for every existing `CREATE TABLE`
statement in every prior phase's host tests, so this is purely additive
to the grammar. `struct SqlCreateTableStmt` (`sql_parser.h:572-576`)
gains two fields: `char database_name[OBJECT_NAME_LEN]; uint8_t
has_database;` — the struct's overall size should be checked against
`SqlSelectStmt`'s own footprint (the current union size-setter,
`sql_parser.h`'s own comment near the `SqlStatement` union) during
implementation, not assumed here.

No `LIST DATABASES` SQL statement — plain listing is Terminal/HTTP-only
(`database list`), matching `vec schema export`/`group list`/`authlist
list`'s own established precedent that enumeration never needs SQL
grammar.

### 1.6 — `DROP DATABASE` semantics: refuse if non-empty, no `CASCADE` — CLOSED (cascading phase)

**Original v1 decision: `database_drop()` returns a distinct "database
still has tables assigned" error rather than silently orphaning or
cascading.** No `CASCADE` keyword in v1 — a real, named simplification
versus a full SQL engine's `DROP DATABASE ... CASCADE`, deferred rather
than built speculatively before a real workload asks for it (this
project's repeatedly-stated posture, most recently named in
`vec_index.h`'s own "started ahead of its own gate" callout, applied here
in the opposite direction: don't build the cascade path until something
needs it). An operator must explicitly reassign or drop every table
tagged with a database's id first.

**Cascading-phase addendum (as built): `DROP DATABASE <name> CASCADE` now
exists.** The plain `DROP DATABASE` keeps the refuse-if-non-empty
behavior above byte-for-byte (the parser's `cascade` flag zero-defaults),
so nothing pre-cascading changed. The implementation deliberately lives
in `sql_exec.c`'s `exec_drop_database()`, NOT inside `database_drop()`
itself: `database_drop()`'s rc==3 ("still has tables") can only be
returned after its own permission gate has already passed (rc==2 fires
first), so the executor's drop-the-children-then-retry loop reuses that
gate and the emptiness check completely unchanged — `kernel/database.c`
needed zero edits for this feature. Each child table is dropped through
the same factored-out `drop_one_table_by_name()` path a direct `DROP
TABLE` takes (including the mvcc ghost-row fix), which also enforces the
caller's own per-table permission: a database owner lacking rights on
some table inside it gets a partial-failure error with already-dropped
siblings staying dropped — DDL is not transactional anywhere in this
engine (CREATE TABLE's own multi-step sequence has the same property),
named honestly rather than papered over. Verified end-to-end in
`tests/sql_database_phase2_host_test.c` (scenarios 6-7): refuse-then-
cascade-succeeds, both tables genuinely dropped (not detached), dropped
names reusable, plus parser coverage for the new grammar. The related FK
`ON DELETE CASCADE`/`SET NULL` actions landed in the same cascading
phase — see the RDBMS roadmap's Phase 23 addendum for that half.

There is deliberately no `ALTER TABLE ... SET DATABASE` in v1 either
(§1.7) — meaning in practice, "empty a database" without CASCADE still
means dropping its tables outright, not reassigning them elsewhere. Named
as real, current friction, not hidden.

### 1.7 — No `ALTER TABLE ... SET DATABASE`

A table's database assignment is fixed at `CREATE TABLE ... IN DATABASE`
time only, in this first cut. Reassignment after creation is out of
scope — a real, explicit gap for future work, not a limitation implied
by anything structural (unlike §1.1's namespace-scope cut, this one is
purely "not built yet, would be easy to add: extend `ALTER TABLE`'s
existing grammar with one more clause").

### 1.8 — Cross-database `JOIN` is not restricted (a non-goal, not a gap)

A query may freely `JOIN` tables from two different databases, exactly
like every real SQL engine (Postgres cross-schema joins, SQLite `ATTACH`)
already allows. This is named explicitly so a future reader doesn't
assume database boundaries were ever meant to restrict query reach —
they're a permission/organization boundary, not a query-isolation one
(that's what partitions are for, per §0's own distinction).

### 1.9 — Grant-only, matching `authlist`/`group`'s own current posture

`database_grant_uid()`/`database_grant_group()` are additive only —
matching `authlist_grant_uid()`/`authlist_grant_group()`/
`group_add_member()`'s own current signatures (none of the three
confirmed to have a revoke counterpart as of this writing). If a future
audit finds `authlist`/`group` already have revoke support this doc
missed, `database` should grow the matching revoke function at the same
time, not before — kept consistent with its two closest precedents rather
than inventing a capability neither of them has yet.

---

## §2. Phase breakdown

### Phase 1 — `kernel/database.h`/`.c`: lifecycle + catalog tagging — DONE

**Scope, as built:** `struct SLSDatabaseEntry databases[DATABASE_MAX]`
(32 slots) + `database_create()`/`database_drop()`/`database_list()`/
`database_find_id()` (§1.3, §1.6), matching the design sketch above
exactly except that `database_find_id()` was added beyond the original
sketch — needed for Phase 2's `IN DATABASE <name>` resolution and for
this phase's own host test, and cheap enough (a plain name→id lookup) to
build now rather than defer. `database_id` added to `SLSObjectEntry`
(`object_catalog.h`, right after `uses_rowstore`) and `SLSVallocRequest`
(right after `partition_id`), defaulting to 0 for every one of this
project's ~100+ existing tables via struct zero-init, mirroring
`partition_id`'s original rollout exactly. `sys_sls_valloc()`
(`object_catalog.c`) stamps `e->database_id = req->database_id` as a
plain, unconditional copy — deliberately NOT `partition_id`'s
owner-resolution dance, since a database tag is additive-only (§1.3, and
see database.h's own header comment for the full reasoning). No grants
yet (Phase 3), no SQL yet (Phase 2) — pure plumbing, exactly as scoped.

**A real, deliberate deviation from the original design sketch:**
`database_create()`/`database_drop()` return `0` for success (matching
`vecstore_create_collection()`'s convention), not `1` for success the
way `group_create()`/`group_add_member()` (`group_profile.c`) actually do
— confirmed by reading that file directly while grounding this phase's
design. The codebase has no single universal success-code convention
across these two "small named-array subsystem" families; this phase
keeps the convention already committed to in this doc's own §1.4 sketch
comments (`// 0=success`) rather than silently switching to match
`group_create()`'s inverse one partway through implementation.

**Also confirmed directly (not assumed) before writing this phase:**
neither `group_profile.c` nor `authlist.c` has an explicit `_init()`
function or any `kernel.c` boot-time wiring at all — both rely purely on
C's own static zero-initialization, unlike `vecstore_init()`/
`vec_index_init()`/`partition_init()`, which kernel.c does call
explicitly. `database.c` follows the lighter, no-init convention:
`database_next_id` starts at 1 via its own global initializer
(`uint32_t database_next_id = 1;`), not a runtime reset call, and there
is no `database_init()` function and no new `kernel.c` boot wiring.
`kernel/database.c` was added to the Makefile's `X86_C_SRC` list
(alongside `group_profile.c`/`authlist.c`, its closest precedents) but
deliberately NOT to `RV_C_SRC` — neither of those two files is built for
the RISC-V target either, an existing, pre-this-phase parity gap this
doc doesn't attempt to close.

**Verification:** `tests/database_host_test.c`, 29 checks, linking the
REAL `kernel/database.c` (not a reimplementation), using `vec_index_
host_test.c`'s own lighter scaffold (host-declare `object_catalog[]`/
`object_catalog_count` directly, stub `catalog_get_role()` behind a
controllable global defaulting to `ROLE_APP_USER`) rather than `sql_
schema_export_import_host_test.c`'s heavier "link the real object_
catalog.c" one — justified because `database.c`'s only dependency on
`object_catalog.c` is the plain array plus one access-check function,
never the full valloc/persist/`fnv1a` machinery that heavier scaffold
exists to exercise. Covers: basic create + `database_find_id()`;
duplicate-name rejection (and confirms a *failed* create never burns a
`database_id`); empty-name and over-length-name rejection; `DROP
DATABASE` refusing a non-empty database and leaving it genuinely intact
(not partially dropped) on refusal; the permission gate on drop (non-
owner/non-kernel denied; the actual owner can drop their own database
even under `ROLE_APP_USER`; a non-owner *with* `ROLE_SYSTEM_KERNEL` can
still drop it); dropping a nonexistent database failing cleanly; the
headline §1.2 behavior — recreate a database under a previously-used
name, confirm the new `database_id` genuinely differs from the old one,
and confirm a table still carrying the *old* id is a visible, detectable
orphan (not silently reattached to the new database); the `DATABASE_MAX`
table-full boundary; and `database_find_id()` returning 0 both for a
name that was never created and for one that was genuinely dropped.

No bug was found in the feature itself while writing this test — the
first full run passed clean (unlike, e.g., the SQL roadmap's own DROP
TABLE mvcc bug or the VectorStore roadmap's "ON images" test bug), worth
recording honestly rather than manufacturing a "gap found" narrative
where none occurred. This is arguably a direct dividend of §1.2's own
design decision: the bump-allocated-id choice was made specifically to
prevent the exact bug class a hash-derived id would have reintroduced,
so its own host test scenario (7) existing and passing is closer to
"confirming a known fix holds" than "catching something new."

**Full regression sweep: 38/38 host test files, 0 failed** (up from 37 —
the new file). `kernel/database.c` and `kernel/object_catalog.c` (with
its new `database_id` field and `sys_sls_valloc()` wiring) both compile-
checked clean.

### Phase 2 — SQL grammar: `CREATE DATABASE`/`DROP DATABASE`/`IN DATABASE` — DONE

**Scope as built:** new `TOK_KW_DATABASE` keyword (`sql_parser.c`'s
`KEYWORDS[]`), `SQL_STMT_CREATE_DATABASE`/`SQL_STMT_DROP_DATABASE`
statement kinds, `struct SqlCreateDatabaseStmt`/`struct SqlDropDatabaseStmt`
(just a `database_name[OBJECT_NAME_LEN]` each — no `ALTER DATABASE`, no
`CASCADE` field, per §1.6/§1.7), and the optional `IN DATABASE <name>`
clause on `CREATE TABLE` (`has_database`/`database_name` added to `struct
SqlCreateTableStmt`, zero-defaulted so every pre-Phase-2 `CREATE TABLE`
statement is byte-for-byte unaffected). `IN DATABASE` reuses the existing
`TOK_KW_IN` token exactly as §1.5 scoped — one keyword, two grammar
positions (the pre-existing `IN (...)` predicate-list grammar, and this
new trailing clause after a `CREATE TABLE` column list).

**The exact integration point** (left open by this doc's original
sketch, now confirmed): `exec_create_table()` in `sql_exec.c` resolves
`s->database_name` to a real `database_id` via `database_find_id()`
*before* calling `sys_sls_valloc()` — if `has_database` is set and the
name doesn't resolve, the whole statement fails cleanly with
`SQL_ERR_DDL_FAILED` and nothing is valloc'd (no partial catalog entry
left behind, matching this codebase's existing "fail before the first
side effect" DDL convention). If it resolves, the id is copied straight
into `struct SLSVallocRequest.database_id`, which Phase 1's
`sys_sls_valloc()` already copies unconditionally onto the new
`SLSObjectEntry` (no owner-resolution defaulting, per §1.3). This mirrors
`REFERENCES` constraint validation's own posture exactly: the parser
accepts the syntax unconditionally, the executor validates the reference
at exec time.

`exec_create_database()`/`exec_drop_database()` are thin wrappers —
straight passthroughs to Phase 1's `database_create()`/`database_drop()`,
mapping their return codes onto `SQL_ERR_NONE`/`SQL_ERR_PERMISSION_DENIED`
(rc 2, from `database_drop()`)/`SQL_ERR_DDL_FAILED` (everything else,
including rc 3's "still has tables tagged with it" refusal). No SQL
`GRANT` exists anywhere in this codebase, so these two statements are the
entire SQL-reachable surface for databases in this phase, exactly as
§1.5 scoped — database grants stay Terminal/HTTP-only (Phase 3, not yet
built).

**Verification:** `tests/sql_database_phase2_host_test.c` (new, 24
checks, all passed clean on the first run) — links the REAL, unmodified
`kernel/database.c` (Phase 1), `kernel/object_catalog.c`,
`kernel/sql_parser.c`, and `kernel/sql_exec.c` (the same real-catalog
scaffold `sql_ddl_phase5_host_test.c` established), not a
reimplementation of any of them. Scenarios: `CREATE DATABASE`/`DROP
DATABASE` via SQL text including duplicate-name and unknown-name
failure; `CREATE TABLE ... IN DATABASE` tagging the *real* catalog
entry's `database_id` to match the *real* database's `database_id`
(reading `object_catalog[]` directly, not just checking `sql_execute()`'s
return code); a plain `CREATE TABLE` with no `IN DATABASE` leaving
`database_id` at 0/NONE (purely additive, never a forced default); `IN
DATABASE` naming a nonexistent database failing at exec time with
nothing partially created, while the parser itself still parses the
statement cleanly (confirming the resolution genuinely happens in
`sql_exec.c`, not `sql_parser.c`); `DROP DATABASE` refusing over SQL
while a table is still tagged with it, then succeeding once that table
is dropped (proving the SQL surface reaches the exact same real
`database_drop()` refusal Phase 1's own host test already exercised
directly); and parser-level checks for every malformed variant (`CREATE
DATABASE`/`DROP DATABASE` with no name, `IN DATABASE` with no name, `IN`
not followed by `DATABASE`).

**Regression:** adding `kernel/database.c` to `sql_exec.c`'s dependency
graph broke the link step of every other host test that links
`sql_exec.c` without also linking `kernel/database.c` (13 files) — fixed
by adding `kernel/database.c` to each one's own "Build and run:" link
line, and (for the 11 that don't already link the real
`kernel/object_catalog.c`) a minimal `catalog_get_role()` linkability
stub, since `database_drop()`'s permission gate calls it unconditionally
even though none of those tests' own scenarios exercise `CREATE`/`DROP
DATABASE`. Full sweep after the fix: **39/39 host test files passed, 0
failed** (up from 38 — the new Phase 2 file itself, plus the 13
link-fixed files, all present and passing).

### Phase 3 — Database-scoped grants + `catalog_check_access()` wiring — DONE

**Scope as built:** `struct SLSDatabaseGrant` (`database.h`) exactly as
sketched in §1.4 — `database_id` + one shared `perm_mask` + a fixed
grantee-uid array + a fixed grantee-group-name array, sized identically to
`authlist.h`'s own `AUTHLIST_MAX`/`AUTHLIST_MAX_GRANTEE_UIDS`/
`AUTHLIST_MAX_GRANTEE_GROUPS`. `database_grant_uid()`/`database_grant_group()`
take no `caller_uid`/permission gate of their own — matches `group_create()`/
`authlist_grant_uid()`/`authlist_grant_group()`'s own already-named
"no gate, a future Terminal/HTTP role gate is the right place if it
matters" posture exactly, not a new decision. Both return `0`=success
(matching `database_create()`/`database_drop()`'s own Phase 1 convention,
not `authlist`'s `1`=success), `1`=database name not found, `2`=grantee
table full. A fresh `SLSDatabaseGrant` entry is created on a database's
first grant and reused/updated in place on every later grant to that same
database (`find_or_create_grant()`), one entry per `database_id` — never
per grant call.

**A real, named design property** (not a bug): `perm_mask` is ONE shared
field per database grant entry, applying uniformly to every grantee (uid
or group) of that grant — mirroring how every grantee of an `authlist`
already shares that list's own set of object grants uniformly. A later
`database_grant_uid()`/`database_grant_group()` call on the same database
overwrites `perm_mask` for every existing grantee too, not just the newly
named one. This is a real simplification versus per-grantee permission
levels (the roadmap's own §1.4 sketch only ever specified one `perm_mask`
field on the struct), named explicitly here rather than silently
discovered later.

`database_check_access(uid, database_id, needed_perm)` returns 0
immediately for `database_id == 0` (untagged — nothing to check), then
checks the one grant entry for that `database_id`: `perm_mask` must cover
`needed_perm`, and `uid` must be a direct grantee or a member (via
`group_contains_uid()`) of one of the grantee groups.

**`catalog_check_access()` wiring** (`object_catalog.c`): one new call,
`database_check_access(uid, e->database_id, needed_perm)`, inserted
immediately after the existing `authlist_check_access()` fallback and
before the `ROLE_GUEST` hard-deny — the exact position §1.4 named, for the
identical reason the group/authlist checks already there give in their own
comments (a uid with no individual role defaults to `ROLE_GUEST`, so any
additive grant source must run before that hard-deny or it's silently
unreachable for exactly the uids it's meant to serve).

**Verification:** `tests/database_grant_phase3_host_test.c` (new, 18
checks, all passed clean on the first run) — links the REAL, unmodified
`kernel/object_catalog.c`, `kernel/database.c`, `kernel/group_profile.c`,
`kernel/authlist.c`, `kernel/security_audit.c` (the same scaffold
`security_phase3_host_test.c` already established), not a reimplementation
of any of them. Proves the headline additive-OR contract directly: a
grant made against a *database* (before a table even existed) is honored
the moment a table is tagged with that `database_id`; a bare-GUEST uid
(no `role_table[]` entry, no group, no authlist) gains access purely via
the database grant; an unrelated uid outside the grant stays denied; a
grantee *group* works identically to a grantee uid (two levels of
indirection: uid → group → database grant → object, mirroring authlist's
own grantee-group mechanism); granting a nonexistent database name fails
cleanly; and a denial still reaches the real security audit log unchanged.
`security_phase3_host_test.c` itself was re-run completely unmodified
(only its own "Build and run:" link line gained `kernel/database.c`) and
still passes all 30 of its original checks, confirming the new database
grant path is genuinely additive and didn't regress role/group/authlist.

**Regression:** wiring `database_check_access()` into `object_catalog.c`
broke two more host test files that link the real `object_catalog.c`
without also linking `kernel/database.c`
(`legacy_rowstore_boundary_host_test.c`, fixed by adding
`kernel/database.c` to its link line, which already links
`group_profile.c`/`authlist.c` so no additional stub was needed), and
adding `group_contains_uid()` as a genuine (non-stubbed) call inside
`database.c` broke link for every one of Phase 2's 11 lighter-scaffold
host tests plus `database_host_test.c` itself (12 files total) — fixed by
adding a minimal `group_contains_uid()` linkability stub to each,
alongside the `catalog_get_role()` stub Phase 2 already added there, since
none of those tests' own scenarios exercise database grants either. Full
sweep after all fixes: **40/40 host test files passed, 0 failed** (up
from 39 — the new Phase 3 file itself, plus the two additionally
link-fixed files, all present and passing).

### Phase 4 — Syscalls + HTTP + Terminal reachability — DONE

**Scope as built:** six syscalls, not five — `SYS_SLS_DATABASE_CREATE`
(260) / `_DROP` (261) / `_LIST` (262) / `_GRANT_UID` (263) /
`_GRANT_GROUP` (264) / `_CHECK` (265), reconfirmed via a fresh grep across
every `kernel/*.h` `SYS_SLS_*` define at implementation time (259, `SYS_
SLS_VEC_DATA_IMPORT`, was still the genuine max — the doc's own earlier
guess held). The 6th number closes a real discrepancy this doc's own §1.5
left open: `database.h`'s own Phase 3 header comment on `database_check_
access()` had already promised a `database check` Terminal command "same
as `SYS_SLS_AUTHLIST_CHECK`", but §1.5's own 5-item sketch never listed
one. Rather than silently picking a side, this phase closes the loop by
adding it — `authlist` has both the syscall and the Terminal command, so
the precedent clearly supports it, and the C function
(`database_check_access()`) already existed with nothing left to build
beyond the syscall/Terminal wrapper. Request structs and `sys_sls_
database_*()` wrappers live in `database.h`/`database.c` (not a separate
file), matching `group_profile.h`/`authlist.h`'s own "syscall numbers next
to the structs, in the subsystem header" convention. All five
create/drop/grant-uid/grant-group/check wrappers collapse their internal
function's richer return code to a plain 0/1 (`_LIST` returns void, prints
via `database_list()` directly) — the same coarsening every existing
`SYS_SLS_GROUP_*`/`SYS_SLS_AUTHLIST_*` wrapper already does, not a new
convention. `sys_sls_database_check()` resolves its request's `db_name` to
a `database_id` via `database_find_id()` itself (0/unknown resolves
cleanly to "not granted", matching `database_check_access()`'s own
already-tested 0-database_id behavior, not a separate error path).

Dispatch wiring: six new `case` labels in `kernel/syscall_dispatch.c`,
immediately after `SYS_SLS_VEC_DATA_IMPORT`'s own case, mirroring the
existing `SYS_SLS_GROUP_*`/`SYS_SLS_AUTHLIST_*` block's exact shape
(a plain cast-and-call for five of them, a direct `database_list(); return
0;` for the list case, matching `SYS_SLS_GROUP_LIST`'s own pattern).

HTTP: investigation before implementation turned up that `group`/
`authlist` do NOT actually have dedicated JSON POST routes for their own
create/grant/check operations today — only a read-only `GET /api/security/
groups` and `GET /api/security/authlists`, both added in the Navigator-
Parity Gap Roadmap. Mutating group/authlist operations are reachable only
through the generic `/api/shell/exec` text passthrough; a separate,
already-named "Shell-Command JSON-Promotion Roadmap" promoted *other*
command families (grant/revoke, chmod, partitions, webapp, workflow,
journal, tier, object) to dedicated JSON routes, but never group/authlist.
So "HTTP routes mirroring the group/authlist JSON route shapes already
established" is honored precisely as scoped: one new read-only route,
`GET /api/security/databases` (`api_security_databases_json()`,
`net/http.c`), listing every active database's name/`database_id`/
`owner_uid` plus (if a grant entry exists for that `database_id`) its
grantee-uid-count/grantee-group-count/`perm_mask` summary — mirroring
`api_security_groups_json()`/`api_security_authlists_json()`'s exact
shape. `database create`/`drop`/`grant uid`/`grant group`/`check` remain
`/api/shell/exec`-only, at full parity with `group`/`authlist` today, not
a gap introduced by this phase. Dedicated POST routes for these would be
*going beyond* current group/authlist parity, not mirroring it — noted
here rather than built speculatively.

Terminal: `database create <name>` / `drop <name>` / `list` / `grant uid
<name> <uid> <perm>` / `grant group <name> <group> <perm>` / `check <name>
<uid> <perm>` added to `user/shell.c`'s command dispatcher, in the exact
`sh_starts()`/`sh_eq()` prefix-match style every existing command uses,
immediately after the `authlist list` block. `create`/`drop` thread
`current_session_uid` as `caller_uid` (the same uid source every other
uid-taking command in this file already uses — confirmed by grep before
writing this, not `sess->uid` directly, which only the top of the
dispatcher itself touches). A new "`-- Database Namespace & Access (Phase
4) --`" heading with all six command lines was added to `print_help()`,
positioned after the "Authorization Lists" heading and before "Security
Audit Log", matching every other heading's banner style.

**Verification:** `bash tests/run_all.sh` full regression sweep — 40/40
host test files still pass unchanged (none of them link `syscall_
dispatch.c`/`net/http.c`/`user/shell.c`, so this phase's changes create no
new ripple into the host-test suite at all). Each of the five touched/new
files (`kernel/database.h`, `kernel/database.c`, `kernel/syscall_
dispatch.c`, `net/http.c`, `user/shell.c`) was individually compile-
checked with `gcc -fsyntax-only` (the established substitute for a full
`x86_64-elf-gcc` cross-build, which isn't available in this environment) —
no new errors, only pre-existing style warnings already present before
this phase's edits (missing-include implicit-declaration warnings that
predate this work, confirmed by checking they reference lines this phase
never touched).

### Phase 5 — Frontend — DONE

**Design as built:** a 7th sub-tab, "Databases", added to `SlsDbEngine.
tsx`'s existing `DB_TABS` array/sub-tab bar (same pattern SQL Console/
Schema Explorer/etc. already use — no new top-level sidebar entry, no
`App.tsx` changes needed). This confirms the roadmap's own placement
guess from the tentative scope note above; what didn't survive contact
with the real Phase 4 surface was the "mirror the Collections panel
pattern" plan for the mutation forms — see below.

**Investigation finding that reshaped the plan:** `SlsVectorStore.tsx`'s
CollectionsPanel (the named pattern to mirror) turned out to hit
dedicated JSON routes (`GET/POST/DELETE /api/vec/collections`), which
databases don't have — Phase 4 deliberately scoped database's HTTP
surface to exactly one `GET /api/security/databases` read route, leaving
create/drop/grant/check reachable only via `POST /api/shell/exec`
(genuine parity with group/authlist today, per Phase 4's own writeup). A
second finding sharpened this further: `SlsSecurityDashboard.tsx` (the
obvious place to look for prior art on a shell/exec-only mutation form,
since group/authlist have the identical constraint) turned out to have
*no* mutation UI at all — it's a read-only audit-log table. The
Shell-Command JSON-Promotion Roadmap had already **deleted** this
project's old `POST /api/shell/exec` fallback plumbing entirely once
every command family it covered got a dedicated JSON route — and
group/authlist/database were never in that promoted list. So
`DatabasesPanel` is the first UI component in this codebase to call
`POST /api/shell/exec` directly; there was no existing wrapper to reuse.

**What was built** (`slsos-sim/src/components/SlsDbEngine.tsx`):
- `shellExec(command)` — a small fresh helper: JSON body `{ command }`,
  response `{ ok: "true"|"false", output: <raw kernel-printed text> }`.
  `ok` reflects whether `sls_shell_execute()` recognized the command, not
  whether the operation itself succeeded, so `output` is surfaced to the
  user verbatim (flash message / `<pre>` block) rather than re-parsed —
  there's no structured success/failure field to parse.
- `DatabasesPanel`: a list table (`GET /api/security/databases` —
  name/database_id/owner_uid/grantee_uid_count/grantee_group_count/
  grant_perm_mask) styled identically to CollectionsPanel's own table
  (same header/row Tailwind classes), with a `ConfirmDropButton` — a
  local copy of `SlsVectorStore.tsx`'s two-step arm/confirm delete
  pattern, since `SlsDbEngine.tsx` had no destructive-action control of
  its own to reuse before this phase.
- Create Database form (`database create <name>`) and Grant Access form
  (`database grant uid|group <db> <uid|group> <perm>`, uid/group toggled
  via radio buttons) styled to match CollectionsPanel's Create Collection
  card.
- A `DbPermPicker` — three checkboxes (Read/Write/Execute) that build the
  same bitmask `user/permissions.h` defines (`PERM_READ=1, PERM_WRITE=2,
  PERM_EXECUTE=4`), reused for both the grant form and a Check Access
  panel (`database check <db> <uid> <perm>`) that echoes the kernel's
  check-result text — this last panel goes beyond the roadmap's original
  "list + create/drop/grant forms" scope, added because Phase 4 already
  built the `database check` Terminal command and a bare grant form gives
  no way to verify a grant actually took effect without switching to the
  Terminal tab.

**Verification:** `npx tsc --noEmit` — zero errors. No host-test or
kernel-side changes in this phase (frontend-only), so no regression
sweep was needed; the 40/40 kernel host-test suite is unaffected. A live
browser click-through pass was not performed in this environment (no
running `slsos-sim` dev server / backend instance available in this
sandbox to click against) — noted honestly as a scope limitation, same
as `x86_64-elf-gcc`'s unavailability was named explicitly in Phase 4.

This closes the Database Namespace & Access Roadmap — all five phases
are now done.

---

## Suggested sequencing

Phases 1-4 are a strict dependency chain (each phase's host test links
the previous phase's real, unmodified code, matching this whole
project's established verification convention) and should be built in
order. Phase 5 is independent of exact timing and can slip behind other
work without blocking anything else. No phase here blocks or is blocked
by any currently-open work in the SQL Feature-Parity or VectorStore
Interface roadmaps — this is a new, orthogonal capability layered on top
of the existing object catalog, not a modification of either engine's
existing behavior for any object that doesn't opt in.
