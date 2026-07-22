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
#include "group_profile.h"   // GROUP_NAME_LEN, group_contains_uid()

#define DATABASE_NAME_LEN   32
#define DATABASE_MAX        32

// ─── Database Namespace & Access Roadmap Phase 3: grants ───────────────────
// See roadmap doc §1.4 for the full "why a new struct, not a mode flag on
// SLSAuthListEntry" reasoning. Sizes mirror authlist.h's own
// AUTHLIST_MAX/AUTHLIST_MAX_GRANTEE_UIDS/AUTHLIST_MAX_GRANTEE_GROUPS
// exactly -- same "how many distinct grant entries/grantees does a small
// simulated deployment plausibly need" judgment call, not a new sizing
// philosophy.
#define DATABASE_GRANT_MAX           64
#define DATABASE_GRANT_MAX_UIDS      16
#define DATABASE_GRANT_MAX_GROUPS     8

struct SLSDatabaseEntry {
    uint32_t database_id;   // bump-allocated, 1.. ; 0 reserved for NONE/unassigned — never fnv1a(name)
    char     name[DATABASE_NAME_LEN];
    uint32_t owner_uid;
    uint8_t  active;
};

// A grant is keyed by database_id (not name) -- once created, a grant
// stays valid even if nothing else references the name again, and (per
// §1.2's own bump-id reasoning) a DROP+recreate under the same name never
// silently reattaches an old grant to the new database, since the new
// database gets a genuinely different id. Grant-only in this phase (no
// revoke), matching authlist.c/group_profile.c's own current posture --
// named in the roadmap doc's §1.9 rather than silently omitted.
struct SLSDatabaseGrant {
    uint32_t database_id;
    uint32_t perm_mask;
    uint32_t grantee_uids[DATABASE_GRANT_MAX_UIDS];
    uint32_t grantee_uid_count;
    char     grantee_groups[DATABASE_GRANT_MAX_GROUPS][GROUP_NAME_LEN];
    uint32_t grantee_group_count;
    uint8_t  active;
};

extern struct SLSDatabaseEntry databases[DATABASE_MAX];
extern uint32_t                database_next_id;   // bump allocator cursor; starts at 1, never reused
extern struct SLSDatabaseGrant  database_grants[DATABASE_GRANT_MAX];
extern uint32_t                 database_grant_count;   // high-water mark, mirrors object_catalog_count's own bump style

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

// ─── Grants (Phase 3) ───────────────────────────────────────────────────────
//
// No caller_uid/permission gate on either grant function -- matches
// group_create()/authlist_grant_uid()/authlist_grant_group()'s own
// existing, already-named posture exactly (none of those three gate on a
// caller either): a future Terminal/HTTP-level role gate (e.g. requiring
// ROLE_DB_ADMIN) is the right place to add that restriction if it turns
// out to matter, not invented speculatively here (same reasoning
// database.h's own database_create() comment already gives).
//
// database_grant_uid()/database_grant_group() return codes:
//   0 = success
//   1 = db_name not found (not a real, active database)
//   2 = grantee-uid/grantee-group table full for this database's grant
//       entry (a fresh grant entry is created on first grant per database,
//       reused/updated in place on subsequent grants to the same database
//       -- mirrors authlist_grant_object()'s own "update in place if
//       already attached" convention)
int database_grant_uid(const char* db_name, uint32_t uid, uint32_t perm_mask);
int database_grant_group(const char* db_name, const char* group_name, uint32_t perm_mask);

// Returns 1 if uid is granted needed_perm on database_id via ANY active
// database grant -- either as a direct grantee, or as a member of one of
// the grant's grantee groups (via group_contains_uid()). Returns 0 if
// database_id is 0 (NONE/unassigned -- an untagged object has no database
// grant to check) or if no grant matches. This is the real function
// catalog_check_access() (object_catalog.c) calls directly, mirroring
// authlist_check_access()'s own "internal function object_catalog.c calls
// straight through" shape -- no syscall wrapper needed for this one since
// it's never called directly by an operator (Phase 4 adds a `database
// check` Terminal command as a thin wrapper around the same logic, same as
// SYS_SLS_AUTHLIST_CHECK does for authlist_check_access()).
int database_check_access(uint32_t uid, uint32_t database_id, uint32_t needed_perm);

// ─── Syscalls (Phase 4) ─────────────────────────────────────────────────────
// 260-265, immediately following Phase 8 follow-on's SYS_SLS_VEC_DATA_IMPORT
// = 259 (kernel/vecstore.h) -- confirmed via a fresh grep across every
// kernel/*.h SYS_SLS_* define before picking these, matching this whole
// codebase's own "reconfirm the next free number at implementation time,
// don't trust an older doc's guess" convention.
//
// Six numbers, not five -- database_check_access()'s own header comment
// above already promised a `database check` Terminal command "same as
// SYS_SLS_AUTHLIST_CHECK does for authlist_check_access()", closing that
// loop here rather than leaving the promise and the roadmap doc's own
// original 5-item §1.5 sketch in silent disagreement (see this phase's
// roadmap doc write-up for the explicit note).
#define SYS_SLS_DATABASE_CREATE     260
#define SYS_SLS_DATABASE_DROP       261
#define SYS_SLS_DATABASE_LIST       262
#define SYS_SLS_DATABASE_GRANT_UID  263
#define SYS_SLS_DATABASE_GRANT_GROUP 264
#define SYS_SLS_DATABASE_CHECK      265

struct SLSDatabaseCreateRequest {
    char     name[DATABASE_NAME_LEN];
    uint32_t caller_uid;
};

struct SLSDatabaseDropRequest {
    char     name[DATABASE_NAME_LEN];
    uint32_t caller_uid;
};

struct SLSDatabaseGrantUidRequest {
    char     db_name[DATABASE_NAME_LEN];
    uint32_t uid;
    uint32_t perm_mask;
};

struct SLSDatabaseGrantGroupRequest {
    char     db_name[DATABASE_NAME_LEN];
    char     group_name[GROUP_NAME_LEN];
    uint32_t perm_mask;
};

// Takes a database NAME (not a raw database_id) -- an operator asking
// "would uid X get perm Y on database Z" thinks in names, exactly like
// SYS_SLS_AUTHLIST_CHECK's own object_name-based request. Resolves the
// name to a database_id via database_find_id() inside sys_sls_database_
// check() itself (0/not-found resolves to database_id 0, which database_
// check_access() already treats as "nothing to check" -- so an unknown
// name cleanly reports "not granted" rather than a separate error path).
struct SLSDatabaseCheckRequest {
    uint32_t uid;
    char     db_name[DATABASE_NAME_LEN];
    uint32_t needed_perm;
};

// Thin wrappers, same shape as sys_sls_group_create()/sys_sls_authlist_
// create() etc. -- 0=success/granted, 1=failure, matching this codebase's
// existing convention that a syscall's return doesn't need a richer error
// code than the internal function it forwards to already provides (the
// internal function's own richer rc, e.g. database_drop()'s 1/2/3, is
// still available to any caller reaching these C functions directly; the
// syscall boundary only ever needs a coarser 0/1 the way every other
// SYS_SLS_* wrapper in this codebase already collapses to).
uint64_t sys_sls_database_create(struct SLSDatabaseCreateRequest* req);
uint64_t sys_sls_database_drop(struct SLSDatabaseDropRequest* req);
uint64_t sys_sls_database_grant_uid(struct SLSDatabaseGrantUidRequest* req);
uint64_t sys_sls_database_grant_group(struct SLSDatabaseGrantGroupRequest* req);
uint64_t sys_sls_database_check(struct SLSDatabaseCheckRequest* req);

#endif /* DATABASE_H */
