/*
 * tenant.h — Multitenant Isolation Gap Analysis §5 item 1 / §7 item 2:
 * a single tenant identity that unifies `partition_id` and `database_id`.
 *
 * ─── The problem this closes ───────────────────────────────────────────
 * The gap analysis doc's §2 finding: provisioning a customer today is two
 * completely unrelated manual procedures — `partition_create()` (execution/
 * resource isolation, kernel/partition.c) and `database_create()` (SQL/
 * catalog namespace, kernel/database.c) — with no code anywhere that ties
 * a given partition_id to a given database_id. Confirmed by direct grep
 * before this file was written: neither partition.c nor database.c
 * references the other's identifier at all. Two operators could each
 * provision "customer X" and end up with a partition and a database that
 * share nothing but a naming convention a human remembered to follow.
 *
 * ─── Why a new table, not a field bolted onto struct SLSPartitionEntry
 * or struct SLSDatabaseEntry ─────────────────────────────────────────────
 * Same shape `partition_owner_table[]` (Multi-Node Phase 2) already
 * established for "which node owns this partition" instead of adding a
 * node_id field to SLSPartitionEntry: a separate table lets tenant
 * identity be added, queried, and (eventually) removed without touching
 * either subsystem's own struct layout or its own persistence format.
 * Neither partition.c nor database.c needs to know tenants exist for
 * their own logic to keep working exactly as before — the same
 * "everything already using partition_id/database_id directly doesn't
 * need to change" backward-compatibility posture partition.h's own top
 * comment establishes for PARTITION_SYSTEM/PARTITION_DEFAULT.
 *
 * ─── Why tenant_id is a third bump-allocated counter, never derived from
 * partition_id or database_id ────────────────────────────────────────────
 * Same reasoning database.h's own §1.2 write-up gives for why
 * database_id is never fnv1a(name): a tenant_id derived from either of
 * the other two ids would make `DROP` + recreate of just one side (e.g.
 * database_drop() without a corresponding partition_destroy(), which is
 * a real, valid, independent operation today and stays one) silently
 * reattach to a stale tenant record. `tenant_next_id` is its own
 * monotonically increasing counter, starting at 1, never reused.
 *
 * ─── What tenant_create() actually does, and what "atomic" means here ───
 * There is no cross-subsystem transaction primitive in this kernel — no
 * two-phase commit across partition.c/database.c/tenant.c. "Atomic" here
 * means what LPAR Phase 14's partition_destroy() and Multi-Node Phase 6's
 * partition_migrate() both already mean by it: an explicit, ordered
 * sequence of real calls into the already-existing, already-tested
 * subsystems, with an explicit rollback path if a later step fails, not a
 * new lower-level primitive. Order: partition_create() first (cheaper to
 * roll back — partition_destroy() already exists and is well-exercised),
 * then database_create(); if database_create() fails, the just-created
 * partition is torn down via partition_destroy() before tenant_create()
 * itself returns failure, so a caller never observes an orphaned
 * partition with no matching database. The reverse orphan (a database
 * with no matching partition) was already possible before this file
 * existed via direct database_create() calls, and stays possible — this
 * file only guarantees the invariant for identities it itself creates,
 * it does not retroactively police pre-existing state, matching every
 * other roadmap phase's "close the gap for what's provisioned going
 * forward" scope, not a data-migration guarantee for the past.
 *
 * ─── Why the caller is assigned into their own new partition ────────────
 * Mirrors object_catalog.c's own `sys_sls_valloc()` comment (referenced
 * directly from database.h): a freshly created boundary is useless to its
 * own creator if they're not inside it. `partition_assign_uid()` is
 * called with the new partition_id right after creation, except when
 * caller_uid is 0 (kernel) — uid 0 is permanently PARTITION_SYSTEM by
 * partition_assign_uid()'s own already-enforced invariant, so attempting
 * the assignment there would just fail; this file checks for that case
 * up front rather than relying on the failure to be silently ignored.
 *
 * ─── Why tenant_create() has no permission gate of its own ──────────────
 * Same posture database_create()/group_create()/authlist_create() already
 * have, for the same reason: a future Terminal/HTTP-level role gate
 * (e.g. requiring ROLE_SYSTEM_KERNEL or a new ROLE_TENANT_ADMIN) is the
 * right place to add that restriction if it turns out to matter, not
 * invented speculatively here.
 */
#ifndef TENANT_H
#define TENANT_H

#include <stdint.h>

#define TENANT_NAME_LEN 32
// Multitenant Isolation Gap Analysis §5 item 9 (capacity sizing): raised in
// lockstep with kernel/partition.h's PARTITION_MAX (16 -> 256). tenant_
// create() is this codebase's real go-forward provisioning path (§9 of
// that doc) and its step-1 partition_create() check would otherwise become
// unreachable dead capacity -- leaving TENANT_MAX at its old value while
// PARTITION_MAX climbed to 256 would have silently made TENANT_MAX (not
// PARTITION_MAX) the binding ceiling on real tenant count, defeating the
// entire point of the resize. Mirrors DATABASE_MAX below for the same
// "one tenant, one partition, one database" 1:1 relationship tenant_
// create() itself enforces.
#define TENANT_MAX      256

struct SLSTenantEntry {
    uint32_t tenant_id;      // bump-allocated, 1.. ; 0 reserved for NONE/not found — never derived from partition_id or database_id
    char     name[TENANT_NAME_LEN];
    uint32_t partition_id;   // the partition this tenant's execution/resource isolation lives in
    uint32_t database_id;    // the database this tenant's SQL/catalog namespace lives in
    uint32_t owner_uid;      // the uid that created this tenant (and was assigned into partition_id, unless it was uid 0)
    uint8_t  active;
};

extern struct SLSTenantEntry tenants[TENANT_MAX];
extern uint32_t               tenant_next_id;   // bump allocator cursor; starts at 1, never reused

// ─── Lifecycle ────────────────────────────────────────────────────────────
//
// tenant_create() return codes:
//   0 = success (*out_tenant_id set to the new tenant's id)
//   1 = bad/empty/too-long name, duplicate tenant name, or the tenant
//       table is full — nothing was created in either subsystem
//   2 = partition_create() failed (partition table full, PARTITION_MAX
//       reached) — nothing was created in either subsystem
//   3 = database_create() failed (name collision at the database layer,
//       or the database table is full) — the partition created in this
//       same call was rolled back via partition_destroy() before
//       returning, so no orphan partition is left behind
//
// out_tenant_id may be NULL if the caller only cares about success/failure.
int tenant_create(uint32_t caller_uid, const char* name, uint32_t* out_tenant_id);

void tenant_list(void);

// Resolves a name to its tenant_id, or 0 if not found/inactive. 0 doubles
// as both "unassigned" and "not found" by construction, matching
// database_find_id()'s own convention exactly.
uint32_t tenant_find_id(const char* name);

// Reverse lookups: given a partition_id/database_id, which tenant (if
// any) owns it. Returns 0 if no active tenant record references that id
// — including for ids that predate this file (e.g. a partition created
// directly via partition_create() before tenant_create() ever ran on
// this system), which is the honest, correct answer: no tenant record
// claims it, whether or not a human intends it to belong to one.
uint32_t tenant_find_by_partition(uint32_t partition_id);
uint32_t tenant_find_by_database(uint32_t database_id);

// ─── Tenant-scoped RBAC administration gate (Multitenant Isolation Gap
// Analysis §5 item 5 / §7 item 4) ───────────────────────────────────────────
//
// group_profile.c's group_create()/group_add_member() and authlist.c's
// authlist_create()/authlist_grant_*() had NO permission gate at all before
// this — any uid, with zero identity checking, could call them via
// do_syscall() and create a group/authlist naming ANY role, any member uid,
// any object. That's not a partition-boundary leak in the access-check sense
// (catalog_check_access()'s own e->partition_id != partition_get_for_uid(uid)
// hard boundary at the top of that function still applies to every actual
// object access a group/authlist grant enables), but it is exactly the "a
// customer needs to administer their own users without needing global
// authority" gap the doc names: there was no way for a tenant's own owner to
// manage groups/authlists scoped to just their tenant, and conversely
// nothing stopped one tenant's caller from naming another tenant's uids as
// members/grantees of a group/authlist that (while ultimately harmless
// against object access, per the boundary above) still leaks tenant
// membership information and clutters a shared, unscoped global namespace.
//
// This lives in tenant.c, not duplicated once each in group_profile.c and
// authlist.c, because "who administers this tenant" is tenant.c's own
// concept — it already owns tenants[].owner_uid, the answer to exactly this
// question. Returns 1 if caller_uid may administer (create/manage groups,
// authorization lists, and other tenant-scoped RBAC primitives for)
// partition_id:
//   - caller_uid's role (object_catalog.c's catalog_get_role()) is
//     ROLE_SYSTEM_KERNEL — mirrors catalog_check_access()'s own
//     kernel-always-passes rule; matches every other admin gate in this
//     codebase.
//   - partition_id == PARTITION_SYSTEM: no single tenant owns the system
//     partition by definition (it's the default for uid 0 and every uid
//     never assigned to a tenant) — only a global ROLE_DB_ADMIN caller may
//     administer it, the same system-wide admin authority that already
//     existed before tenants did.
//   - otherwise: caller_uid must be the exact owner_uid stamped on the
//     tenant record whose partition_id matches (tenant_find_by_partition()
//     + a linear scan of tenants[], mirroring tenant_find_by_partition()'s
//     own O(n) posture — TENANT_MAX is 32, this isn't a hot path). A
//     partition with no tenant record at all (e.g. one created directly via
//     partition_create() before tenant_create() ever ran) has no self-
//     service owner and is denied — the honest answer, not a silent
//     fallback to some other authority.
int tenant_caller_may_administer(uint32_t caller_uid, uint32_t partition_id);

// ─── Syscalls ─────────────────────────────────────────────────────────────
// 270-271, immediately following object_catalog.h's SYS_SLS_OBJECT_SET_
// DATABASE = 269 -- confirmed via a fresh grep across every kernel/*.h
// SYS_SLS_* define before picking these, matching this codebase's own
// "reconfirm the next free number at implementation time" convention.
#define SYS_SLS_TENANT_CREATE 270
#define SYS_SLS_TENANT_LIST   271

struct SLSTenantCreateRequest {
    char     name[TENANT_NAME_LEN];
    uint32_t caller_uid;
};

// Thin wrapper, same shape as sys_sls_database_create() etc. Returns the
// new tenant_id (>=1) on success, 0 on any failure — the syscall boundary
// collapses tenant_create()'s richer 0/1/2/3 rc to the coarser "0 =
// failure" any caller reaching this via do_syscall() gets, matching every
// other SYS_SLS_* wrapper's own convention; a caller needing the richer
// rc can call tenant_create() directly (kernel-internal callers only).
uint64_t sys_sls_tenant_create(struct SLSTenantCreateRequest* req);

#endif /* TENANT_H */
