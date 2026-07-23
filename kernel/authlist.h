#ifndef AUTHLIST_H
#define AUTHLIST_H

#include <stdint.h>
#include "object_catalog.h"   // OBJECT_NAME_LEN
#include "group_profile.h"    // GROUP_NAME_LEN, group_contains_uid()

/*
 * authlist.h — Navigator-Parity Gap Roadmap Phase 3: authorization lists.
 *
 * IBM i pattern: a named list of {object, permission} pairs, granted once to
 * a set of users/groups, instead of repeating the same per-object grant for
 * every grantee (object_catalog.c's existing sys_sls_grant()/_revoke() only
 * ever operate on one object + one uid at a time). An authorization list is
 * the reusable "grant this whole bundle of access to these people" unit.
 *
 * Used by catalog_check_access() (object_catalog.c) as a fallback path,
 * after the existing owner/role/group checks fail and before falling back to
 * the object's own stored perm_mask -- see catalog_check_access()'s own
 * comment for the exact rule ordering. authlist_check_access() below is the
 * real function that path calls directly (not through do_syscall, same
 * "internal function + thin syscall wrapper" split this codebase already
 * uses for catalog_check_access() itself); SYS_SLS_AUTHLIST_CHECK exists
 * so an operator (via the Terminal/HTTP) can ask the same question the
 * kernel just asked internally, for introspection.
 *
 * Syscall surface is intentionally just three calls (CREATE/GRANT/CHECK),
 * per the roadmap's own scoping -- GRANT is a single, kind-tagged entry
 * point covering the three things one can attach to a list (an object grant,
 * a uid grantee, or a group grantee) rather than three separate syscalls,
 * since none of those individually need much of a request struct and three
 * near-empty structs would be more ceremony than the one tagged struct
 * below. This is a genuinely new multi-purpose-request pattern for this
 * codebase (nothing here directly precedents it), named explicitly rather
 * than presented as an established convention.
 */

#define AUTHLIST_NAME_LEN             32
#define AUTHLIST_MAX                  16
#define AUTHLIST_MAX_OBJECTS           8
#define AUTHLIST_MAX_GRANTEE_UIDS     16
#define AUTHLIST_MAX_GRANTEE_GROUPS    8

struct SLSAuthListObjectGrant {
    char     object_name[OBJECT_NAME_LEN];
    uint32_t perm_mask;
    uint8_t  active;
};

struct SLSAuthListEntry {
    char     name[AUTHLIST_NAME_LEN];
    struct SLSAuthListObjectGrant objects[AUTHLIST_MAX_OBJECTS];
    uint32_t object_count;
    uint32_t grantee_uids[AUTHLIST_MAX_GRANTEE_UIDS];
    uint32_t grantee_uid_count;
    char     grantee_groups[AUTHLIST_MAX_GRANTEE_GROUPS][GROUP_NAME_LEN];
    uint32_t grantee_group_count;
    uint8_t  active;
    // Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4: same
    // creation-time-stamped scope as group_profile.h's SLSGroupEntry --
    // see that struct's own comment. authlist_create()/authlist_grant_*()
    // now gate on tenant_caller_may_administer() and require every object/
    // uid/group attached to a list to belong to the same partition as the
    // list itself (see authlist.c for the full rationale).
    uint32_t partition_id;
};

extern struct SLSAuthListEntry authlist_table[AUTHLIST_MAX];
extern uint32_t                authlist_table_count;

// Creates a new empty authorization list, scoped to caller_uid's own
// partition. Returns 1 on success, 0 if the name is taken, the table is
// full, or caller_uid is not allowed to administer RBAC primitives for its
// own partition (tenant_caller_may_administer(), tenant.h) -- Multitenant
// Isolation Gap Analysis §5 item 5 / §7 item 4.
int authlist_create(uint32_t caller_uid, const char* name);

// Attaches (or updates, if already present) an {object_name, perm_mask}
// entry on the named list. Returns 1 on success, 0 if the list doesn't
// exist, caller_uid may not administer the list's partition, object_name
// doesn't exist or belongs to a different partition than the list
// (attaching a grant on another tenant's object is refused outright), or
// the object-entry table is full.
int authlist_grant_object(uint32_t caller_uid, const char* list_name, const char* object_name, uint32_t perm_mask);

// Adds uid as a direct grantee of the named list. Returns 1 on success, 0 if
// the list doesn't exist, caller_uid may not administer the list's
// partition, uid's own partition doesn't match the list's (cross-tenant
// grantees are refused outright), uid is already a grantee, or the grantee
// list is full.
int authlist_grant_uid(uint32_t caller_uid, const char* list_name, uint32_t uid);

// Adds group_name as a grantee of the named list -- every current and future
// member of that group inherits this list's grants. Returns 1 on success, 0
// if the list doesn't exist, caller_uid may not administer the list's
// partition, group_name doesn't exist or belongs to a different partition
// than the list (unlike the pre-Phase-3-scoping behavior, group_name MUST
// already exist as a real group now -- its partition has to be checked, so
// the "can be pre-configured before creation" allowance this function used
// to document no longer applies), or the grantee-group list is full.
int authlist_grant_group(uint32_t caller_uid, const char* list_name, const char* group_name);

// Returns 1 if uid is granted `needed_perm` on `obj_name` via ANY active
// authorization list -- either as a direct grantee, or as a member of one of
// the list's grantee groups (via group_contains_uid(), group_profile.h).
// Returns 0 if no list grants it. This is the real function
// catalog_check_access() calls directly; SYS_SLS_AUTHLIST_CHECK below is a
// thin wrapper around the exact same logic for external introspection.
int authlist_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm);

// Prints the authorization-list table to the serial port, mirrors
// sys_sls_auth_list()'s own style.
void authlist_list(void);

// ─── Syscalls ─────────────────────────────────────────────────────────────
// 240-242, after group_profile.h's 237-239. 244 added slightly after the
// initial three (241/242/243 already claimed by GRANT/CHECK/security_audit.h's
// AUDIT_LIST) once it became clear the Terminal needed a real way to list
// authorization lists too, mirroring SYS_SLS_GROUP_LIST's own existence next
// to GROUP_CREATE/ADD_MEMBER -- named here rather than silently renumbering
// anything already assigned.
#define SYS_SLS_AUTHLIST_CREATE 240
#define SYS_SLS_AUTHLIST_GRANT  241
#define SYS_SLS_AUTHLIST_CHECK  242
#define SYS_SLS_AUTHLIST_LIST   244

struct SLSAuthListCreateRequest {
    char     name[AUTHLIST_NAME_LEN];
    uint32_t caller_uid;   // Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4
};

// kind: 0 = attach object (object_name + perm_mask used), 1 = add grantee
// uid (grantee_uid used), 2 = add grantee group (grantee_group used).
struct SLSAuthListGrantRequest {
    char     list_name[AUTHLIST_NAME_LEN];
    uint8_t  kind;
    char     object_name[OBJECT_NAME_LEN];
    uint32_t perm_mask;
    uint32_t grantee_uid;
    char     grantee_group[GROUP_NAME_LEN];
    uint32_t caller_uid;   // Multitenant Isolation Gap Analysis §5 item 5 / §7 item 4
};

struct SLSAuthListCheckRequest {
    uint32_t uid;
    char     object_name[OBJECT_NAME_LEN];
    uint32_t needed_perm;
};

uint64_t sys_sls_authlist_create(struct SLSAuthListCreateRequest* req);
uint64_t sys_sls_authlist_grant(struct SLSAuthListGrantRequest* req);
uint64_t sys_sls_authlist_check(struct SLSAuthListCheckRequest* req);

#endif /* AUTHLIST_H */
