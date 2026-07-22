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

### 1.6 — `DROP DATABASE` semantics: refuse if non-empty, no `CASCADE`

**Decision: `database_drop()` returns a distinct "database still has
tables assigned" error rather than silently orphaning or cascading.** No
`CASCADE` keyword in v1 — a real, named simplification versus a full SQL
engine's `DROP DATABASE ... CASCADE`, deferred rather than built
speculatively before a real workload asks for it (this project's
repeatedly-stated posture, most recently named in `vec_index.h`'s own
"started ahead of its own gate" callout, applied here in the opposite
direction: don't build the cascade path until something needs it). An
operator must explicitly reassign or drop every table tagged with a
database's id first. There is deliberately no `ALTER TABLE ... SET
DATABASE` in v1 either (§1.7) — meaning in practice, "empty a database"
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

### Phase 2 — SQL grammar: `CREATE DATABASE`/`DROP DATABASE`/`IN DATABASE`

**Scope:** new `TOK_KW_DATABASE` keyword, `SQL_STMT_CREATE_DATABASE`/
`SQL_STMT_DROP_DATABASE` statement kinds, and the optional `IN DATABASE
<name>` clause on `CREATE TABLE` (§1.5). Executor wiring in `sql_exec.c`
calls straight into Phase 1's `database_create()`/`database_drop()`, and
stamps `database_id` onto the newly created table's catalog entry when
`IN DATABASE` is present — the exact integration point inside whichever
function currently creates/derives that catalog entry for `CREATE TABLE`
needs to be confirmed by investigation at implementation time (this doc
scopes the design, not the exact call site), matching this whole
project's own "investigate current implementation first" convention at
the start of every phase.

**Verification:** parser host test (tokens/grammar for all three new
productions, including rejecting `IN DATABASE` on a nonexistent
database name — resolved at exec time, not parse time, matching how
`REFERENCES` constraint validation already works: the parser accepts the
syntax, the executor validates the reference) + exec host test (real
`CREATE DATABASE`/`CREATE TABLE ... IN DATABASE`/`DROP DATABASE`
round trip against the real `database.c` and `object_catalog.c`).

### Phase 3 — Database-scoped grants + `catalog_check_access()` wiring

**Scope:** `struct SLSDatabaseGrant` + `database_grant_uid()`/
`database_grant_group()`/`database_check_access()` (§1.4). Wire
`database_check_access()` into `catalog_check_access()` at the position
named in §1.4 (alongside the group/authlist block, before the GUEST
hard-deny).

**Verification:** host test proving the additive-OR contract directly:
a uid with no individual role (defaults to GUEST) gains access to a table
purely via a database grant; a uid outside the grant gets denied; a
database grant never overrides an explicit owner/role denial path that
doesn't exist (there is none — grants are purely additive, matching
group/authlist's own already-tested behavior) confirmed by re-running
the existing `security_phase3_host_test.c` unmodified to prove this
addition doesn't regress any existing group/authlist/role scenario.

### Phase 4 — Syscalls + HTTP + Terminal reachability

**Scope:** new syscalls for create/drop/list/grant-uid/grant-group
(next free numbers to be reconfirmed via a fresh grep at implementation
time — 260+ as of this doc's own writing, immediately following this
session's `SYS_SLS_VEC_DATA_IMPORT = 259`). HTTP routes mirroring the
`group`/`authlist` JSON route shapes already established
(`net/http.c`). Terminal commands per §1.5's exact list, plus `print_help()`
entries under a new "Database Namespace & Access" heading.

**Verification:** full regression sweep (`bash tests/run_all.sh`),
confirming every syscall number is genuinely unused and every new host
test file passes alongside all pre-existing ones — the same bar every
phase this session has already met.

### Phase 5 — Frontend (deferred until Phase 1-4 are live, per this
session's own established "backend first" posture)

**Scope (tentative, not yet designed in detail):** a small "Databases"
panel in `slsos-sim` — likely inside `SlsDbEngine.tsx` alongside the SQL
Console/Tables Browser, listing databases, their tables, and their
grants, with create/drop/grant forms mirroring the Collections panel
pattern already used in `SlsVectorStore.tsx`. Exact placement and scope
to be revisited once Phases 1-4 are real and the shape of what's worth
surfacing is clearer.

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
