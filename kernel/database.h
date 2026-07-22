/*
 * database.h — AeroSLS Database Namespace & Access Roadmap, Phase 1:
 * lifecycle + catalog tagging. See docs/AeroSLS-Database-Namespace-
 * Roadmap-v0.1.md for the full design writeup (§1-§2); this header
 * implements exactly what that doc's Phase 1 section scopes and nothing
 * more — no grants yet (Phase 3), no SQL grammar yet (Phase 2).
 *
 * ─── What a "database" is here: a permission/organizational tag, not a
 * true namespace (roadmap doc §1.1) ────────────────────────────────────
 * Table names remain globally unique across the whole `object_catalog[]`
 * exactly as before this phase — a database groups existing tables under
 * one name for listing/grant purposes, it does not let two tables share
 * a name across two different databases. That would require touching
 * every table-name lookup path across `sql_exec.c`/`rowstore.c`/
 * `mvcc.c`/`row_index.c` — a much larger rewrite of the SQL engine's core
 * identity model than a namespace layer bolted on top of it. A real,
 * deliberate scope cut, not an oversight.
 *
 * ─── Why `database_id` is a bump-allocated counter, never fnv1a(name)
 * (roadmap doc §1.2) ─────────────────────────────────────────────────────
 * Directly informed by a real bug this project already found and fixed:
 * `object_catalog.c`'s `object_id = fnv1a(name)` is deterministic per
 * name, so `DROP TABLE foo; CREATE TABLE foo (...)` recreated the
 * identical `object_id`, and `mvcc.c`'s version cache — never cleared on
 * drop — resurrected old rows as ghost rows on the new table's first scan
 * (SQL Feature-Parity Roadmap Phase 8's `mvcc_notify_table_dropped()`
 * fix). A hash-derived `database_id` would set up the exact same failure
 * class one level up: `DROP DATABASE app; CREATE DATABASE app;` would
 * silently reattach to every stale `database_id` reference any subsystem
 * forgot to clear. `database_next_id` below is a monotonically
 * increasing counter, never reused even after a drop — recreating a
 * database under a previously-used name gets a genuinely fresh, empty
 * id, and any table still tagged with the *old* id becomes a visible,
 * detectable orphan rather than a silent resurrection.
 *
 * ─── Why `database_create()` has no permission gate of its own ──────────
 * Matches `group_create()` (group_profile.c) and `authlist_create()`
 * (authlist.c) — neither takes a `caller_uid` at all, let alone gates on
 * one — and `vecstore_create_collection()`'s own already-named gap
 * (VectorStore Interface Roadmap Phase 5: "no permission gate of its
 * own"). `database_create()` DOES take `caller_uid` (unlike those two)
 * so it has an `owner_uid` to stamp and to check in `database_drop()`
 * below, but it does not itself refuse any caller — a future Terminal/
 * HTTP-level role gate (e.g. requiring `ROLE_DB_ADMIN`) is the right
 * place to add that restriction if it turns out to matter, matching this
 * whole codebase's "decide the real design question when a real caller
 * needs it" posture, not invented speculatively here.
 *
 * ─── Why `database_drop()` DOES gate (owner or ROLE_SYSTEM_KERNEL) ───────
 * A deliberately minimal gate for Phase 1, since the full additive-grant
 * story (`database_check_access()`, Phase 3) doesn't exist yet. Mirrors
 * `catalog_check_access()`'s own two most basic, unconditional checks
 * (kernel bypass, owner match) directly rather than inventing a separate
 * rule — not the full role/group/authlist chain, just the two checks
 * that don't depend on anything Phase 3 will add.
 *
 * ─── Why a database is a soft, additive tag, never a hard boundary like
 * `partition_id` ─────────────────────────────────────────────────────────
 * `SLSObjectEntry.database_id`/`SLSVallocRequest.database_id` mirror
 * `partition_id`'s exact field shape, but NOT its defaulting logic.
 * `partition_id` defaults to the *owner's own* partition (not a
 * hardcoded 0) specifically because `catalog_check_access()`'s partition
 * check is a hard boundary that runs before the owner check — a naive
 * 0-default would lock an owner out of their own freshly created object
 * if their own partition isn't 0 (object_catalog.c's own comment on
 * `sys_sls_valloc()` explains this in full). A database grant (Phase 3)
 * will only ever ADD access, never restrict it — so `database_id` needs
 * no such resolution: a direct, unconditional copy of whatever the
 * caller supplied (0/NONE if they didn't set it) is correct and
 * sufficient, and this phase adds no new access restriction of any kind.
 *
 * ─── No `database_init()` — matches group_profile.c/authlist.c's own
 * "zero-init only, no kernel.c boot wiring" convention exactly (confirmed
 * by grep before writing this: neither is called from kernel.c's boot
 * sequence, unlike `vecstore_init()`/`vec_index_init()`/
 * `partition_init()`, which are). `database_next_id` starts at 1 via its
 * own global initializer below, not a runtime reset call — the same
 * reason those two Phase-3-cluster files never needed one either: their
 * state has no "pool cursor" needing a runtime reset distinct from C's
 * own static zero-init, and neither does this one now that
 * `database_next_id`'s initial value (1, not 0) is expressed directly at
 * its declaration.
 */
#ifndef DATABASE_H
#define DATABASE_H

#include <stdint.h>

#define DATABASE_NAME_LEN   32
#define DATABASE_MAX        32

struct SLSDatabaseEntry {
    uint32_t database_id;   // bump-allocated, 1.. ; 0 reserved for NONE/unassigned — never fnv1a(name)
    char     name[DATABASE_NAME_LEN];
    uint32_t owner_uid;
    uint8_t  active;
};

extern struct SLSDatabaseEntry databases[DATABASE_MAX];
extern uint32_t                database_next_id;   // bump allocator cursor; starts at 1, never reused

// ─── Lifecycle ────────────────────────────────────────────────────────────
//
// database_create() return codes:
//   0 = success
//   1 = bad/empty/too-long name, duplicate name, or the database table is full
//
// database_drop() return codes:
//   0 = success
//   1 = not found
//   2 = permission denied (caller is neither the owner nor ROLE_SYSTEM_KERNEL)
//   3 = database still has one or more active tables tagged with its
//       database_id — no CASCADE in this phase (roadmap doc §1.6);
//       reassign or drop those tables first
int  database_create(uint32_t caller_uid, const char* name);
int  database_drop(uint32_t caller_uid, const char* name);
void database_list(void);

// Resolves a name to its database_id, or 0 if not found/inactive. 0
// doubles as both "unassigned" (on a table's database_id field) and "not
// found" (from this lookup) by construction — database_id 0 is never
// assigned to a real database, since database_next_id starts at 1.
uint32_t database_find_id(const char* name);

#endif /* DATABASE_H */
