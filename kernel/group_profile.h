#ifndef GROUP_PROFILE_H
#define GROUP_PROFILE_H

#include <stdint.h>
#include "object_catalog.h"   // SLSRole

/*
 * group_profile.h — Navigator-Parity Gap Roadmap Phase 3: group profiles.
 *
 * IBM i's "group profile" concept: a named collection of user profiles that
 * all inherit a shared set of authorities, so an operator grants access once
 * to the group instead of once per member. AeroSLS's role model
 * (object_catalog.h's role_table[]/SLSRole) only ever supported exactly one
 * role per uid -- there was no notion of a uid belonging to more than one
 * authority-granting entity at once.
 *
 * Investigation note (worth naming, since it looked at first like this might
 * already half-exist): user/permissions.h's `struct ExpandedMatrixEntry` has
 * had a `gid` field since long before this phase, and `struct ShellSession`
 * (user/shell.c) has carried a `.gid` since the same era. Neither is wired to
 * anything real -- `ExpandedMatrixEntry` is never instantiated or looked up
 * anywhere in the actual catalog code (docs/SLS-OS.md's own design draft is
 * the only place it's used), and ShellSession's `gid` is explicitly commented
 * as "no per-request gid to seed from; cosmetic only" at its one real call
 * site (net/http.c). So this phase is building group membership for real for
 * the first time, not wiring up a dormant field -- correcting what looked
 * like it might be a shortcut.
 *
 * Design: additive, not a replacement for the existing 4-role model. A group
 * carries its own SLSRole (the authority its members inherit while acting
 * under it) plus a fixed list of member uids. catalog_check_access()
 * (object_catalog.c) checks the caller's own individual role first, exactly
 * as before; if that denies access, it now also checks every group the
 * caller is an active member of, using the exact same role-grants-access
 * logic (see catalog_role_grants() below) as if the caller held that group's
 * role directly. The caller's own role and every group's role are each
 * evaluated independently and OR'd together -- a group can only ever grant
 * additional access, never take away what the caller's own role already has.
 *
 * Sizing: GROUP_TABLE_MAX matches role_table[ROLE_TABLE_MAX]'s own 64-entry
 * convention (object_catalog.h) -- same "how many distinct authority
 * entries does a small simulated deployment plausibly need" judgment call,
 * not a new sizing philosophy. GROUP_MAX_MEMBERS is deliberately much
 * smaller (16) -- a group modeling a real admin team, not a mass role.
 */

#define GROUP_NAME_LEN     32
#define GROUP_TABLE_MAX    64
#define GROUP_MAX_MEMBERS  16

struct SLSGroupEntry {
    char     name[GROUP_NAME_LEN];
    SLSRole  group_role;               // authority members inherit while covered by this group
    uint32_t member_uids[GROUP_MAX_MEMBERS];
    uint32_t member_count;
    uint8_t  active;
    // Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4: which
    // partition this group is scoped to, stamped at group_create() time
    // from the creating caller's own partition (partition_get_for_uid())
    // and never reassigned afterward -- the same "owner/scope fixed at
    // creation" posture stream.c's owner_uid/partition_id and tenant.c's
    // tenants[] already established. group_create()/group_add_member() now
    // gate on tenant.c's tenant_caller_may_administer(caller_uid,
    // partition_id) (see tenant.h), and group_add_member() additionally
    // requires the target uid's own partition to match -- see
    // group_profile.c for the full rationale.
    uint32_t partition_id;
};

extern struct SLSGroupEntry group_table[GROUP_TABLE_MAX];
extern uint32_t             group_table_count;   // high-water mark, mirrors object_catalog_count's own bump style

// Reusable role-grants-access logic, factored out of catalog_check_access()
// (object_catalog.c) so both the caller's own individual role AND every
// group role they inherit can be evaluated through the exact same rules --
// not a second, hand-duplicated copy of the DB_ADMIN/APP_USER/GUEST checks.
// Deliberately does NOT include the owner-uid check or the perm_mask
// fallback (object_catalog.c's catalog_check_access() still owns those --
// they're not role-specific) or the kernel-always-passes / partition-
// boundary checks (also caller-specific, not role-specific). Returns 1 if
// `role` alone would grant `needed_perm` on `e`, 0 otherwise.
int catalog_role_grants(SLSRole role, struct SLSObjectEntry* e, uint32_t needed_perm);

// Creates a new group with the given name and role, scoped to caller_uid's
// own partition. Returns 1 on success, 0 if the name is already taken, the
// table is full (bump-allocated, no reclaim -- same fixed-size-registry
// posture as role_table[]/auth_tokens[] elsewhere in this codebase; a
// destroy/reclaim path is a real future improvement, not attempted in this
// first pass), or caller_uid is not allowed to administer RBAC primitives
// for its own partition (tenant_caller_may_administer(), tenant.h) --
// Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4.
int group_create(uint32_t caller_uid, const char* name, SLSRole role);

// Adds uid to the named group's member list. Returns 1 on success, 0 if the
// group doesn't exist, caller_uid is not allowed to administer that group's
// partition, uid's own partition doesn't match the group's partition
// (cross-tenant membership is rejected outright, not silently scoped), uid
// is already a member, or the member list is full.
int group_add_member(uint32_t caller_uid, const char* name, uint32_t uid);

// Returns 1 if uid is an active member of the named (active) group, 0
// otherwise (including if the group doesn't exist). No permission gate --
// this is the read-only check catalog_check_access() itself calls on every
// access decision, not an administrative action.
int group_contains_uid(const char* name, uint32_t uid);

// Returns 1 and sets *out_partition_id if the named (active) group exists,
// 0 otherwise. Used by authlist.c to validate that a grantee group belongs
// to the same partition as the authorization list granting to it.
int group_get_partition_id(const char* name, uint32_t* out_partition_id);

// Prints the group table to the serial port, mirrors sys_sls_auth_list()'s
// own style.
void group_list(void);

// ─── Syscalls ─────────────────────────────────────────────────────────────
// 237-239 are the next free numbers after Phase 2's own additions topped
// out at 236 (SYS_SLS_VEC_INDEX_REBUILD, vec_index.h) -- confirmed via grep
// across every header defining SYS_SLS_* before picking these.
#define SYS_SLS_GROUP_CREATE      237
#define SYS_SLS_GROUP_ADD_MEMBER  238
#define SYS_SLS_GROUP_LIST        239

struct SLSGroupCreateRequest {
    char     name[GROUP_NAME_LEN];
    SLSRole  role;
    uint32_t caller_uid;   // Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4
};

struct SLSGroupAddMemberRequest {
    char     name[GROUP_NAME_LEN];
    uint32_t uid;
    uint32_t caller_uid;   // Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4
};

uint64_t sys_sls_group_create(struct SLSGroupCreateRequest* req);
uint64_t sys_sls_group_add_member(struct SLSGroupAddMemberRequest* req);

#endif /* GROUP_PROFILE_H */
