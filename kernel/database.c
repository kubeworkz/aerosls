/*
 * database.c — AeroSLS Database Namespace & Access Roadmap, Phase 1
 * implementation. See database.h for the full design writeup.
 */
#include "database.h"
#include "object_catalog.h"
#include "kernel_io.h"

struct SLSDatabaseEntry databases[DATABASE_MAX];
uint32_t                database_next_id = 1;   // 0 reserved for NONE — see database.h's own comment

// ─── String helpers (no libc — db_* here, matching this codebase's
// established per-file convention: gp_* in group_profile.c, au_* in
// auth.c, cat_* in object_catalog.c, etc.). ─────────────────────────────
static int db_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void db_strncpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i + 1 < max && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static uint32_t db_strlen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

static int find_database_idx(const char* name) {
    for (int i = 0; i < DATABASE_MAX; i++) {
        if (databases[i].active && db_streq(databases[i].name, name)) return i;
    }
    return -1;
}

// ─── database_create ───────────────────────────────────────────────────
int database_create(uint32_t caller_uid, const char* name) {
    if (!name || !name[0]) {
        kernel_serial_print("[DATABASE] ERROR: database name required.\n");
        return 1;
    }
    if (db_strlen(name) >= DATABASE_NAME_LEN) {
        kernel_serial_print("[DATABASE] ERROR: database name too long.\n");
        return 1;
    }
    if (find_database_idx(name) >= 0) {
        kernel_serial_printf("[DATABASE] ERROR: database '%s' already exists.\n", name);
        return 1;
    }

    int free_slot = -1;
    for (int i = 0; i < DATABASE_MAX; i++) {
        if (!databases[i].active) { free_slot = i; break; }
    }
    if (free_slot < 0) {
        kernel_serial_print("[DATABASE] ERROR: database table full.\n");
        return 1;
    }

    struct SLSDatabaseEntry* d = &databases[free_slot];
    db_strncpy(d->name, name, DATABASE_NAME_LEN);
    d->owner_uid   = caller_uid;
    d->database_id = database_next_id++;   // bump-allocated — never fnv1a(name), see database.h
    d->active      = 1;

    kernel_serial_printf("[DATABASE] Created database '%s' (id=%u, owner_uid=%u).\n",
                         d->name, d->database_id, d->owner_uid);
    return 0;
}

// ─── database_drop ──────────────────────────────────────────────────────
int database_drop(uint32_t caller_uid, const char* name) {
    int idx = find_database_idx(name);
    if (idx < 0) {
        kernel_serial_printf("[DATABASE] ERROR: database '%s' not found.\n", name);
        return 1;
    }
    struct SLSDatabaseEntry* d = &databases[idx];

    // Minimal Phase 1 gate: owner or kernel only — see database.h's own
    // comment on why this doesn't (yet) go through the full role/group/
    // authlist chain catalog_check_access() runs for ordinary objects.
    if (d->owner_uid != caller_uid && catalog_get_role(caller_uid) != ROLE_SYSTEM_KERNEL) {
        kernel_serial_printf("[DATABASE] ERROR: uid %u is not permitted to drop database '%s'.\n",
                             caller_uid, name);
        return 2;
    }

    // No CASCADE in this phase (roadmap doc §1.6) — refuse rather than
    // orphan or silently detach any table still tagged with this
    // database's id.
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && object_catalog[i].database_id == d->database_id) {
            kernel_serial_printf(
                "[DATABASE] ERROR: database '%s' still has table '%s' assigned — "
                "drop or reassign it first (no CASCADE in this phase).\n",
                name, object_catalog[i].name);
            return 3;
        }
    }

    d->active = 0;
    kernel_serial_printf("[DATABASE] Dropped database '%s' (id=%u).\n", name, d->database_id);
    return 0;
}

// ─── database_list ──────────────────────────────────────────────────────
void database_list(void) {
    kernel_serial_printf(
        "\n[DATABASE] Database Directory\n"
        " %-8s %-32s %s\n"
        " %-8s %-32s %s\n",
        "ID", "Name", "Owner UID",
        "--------", "--------------------------------", "---------");

    int shown = 0;
    for (int i = 0; i < DATABASE_MAX; i++) {
        if (!databases[i].active) continue;
        kernel_serial_printf(" %-8u %-32s %u\n",
                             databases[i].database_id, databases[i].name, databases[i].owner_uid);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no databases defined)\n");
    kernel_serial_printf(" %d database(s).\n\n", shown);
}

// ─── database_find_id ───────────────────────────────────────────────────
uint32_t database_find_id(const char* name) {
    int idx = find_database_idx(name);
    return idx < 0 ? 0 : databases[idx].database_id;
}

// ─── Phase 3: grants ────────────────────────────────────────────────────
struct SLSDatabaseGrant database_grants[DATABASE_GRANT_MAX];
uint32_t                database_grant_count = 0;

// Finds this database's existing grant entry, if any -- one entry per
// database_id (not per grant call), matching authlist_grant_object()'s own
// "update in place if already attached" convention rather than authlist's
// separate per-object-per-list entries (a database has exactly one grant
// scope, unlike an authlist's many object grants).
static int find_grant_idx(uint32_t database_id) {
    for (int i = 0; i < DATABASE_GRANT_MAX; i++) {
        if (database_grants[i].active && database_grants[i].database_id == database_id) return i;
    }
    return -1;
}

// Finds (or creates, on first grant for this database_id) the grant entry
// to add a grantee to. Returns NULL if the grant table is genuinely full
// and no entry for this database_id already exists.
static struct SLSDatabaseGrant* find_or_create_grant(uint32_t database_id) {
    int idx = find_grant_idx(database_id);
    if (idx >= 0) return &database_grants[idx];

    for (int i = 0; i < DATABASE_GRANT_MAX; i++) {
        if (!database_grants[i].active) {
            struct SLSDatabaseGrant* g = &database_grants[i];
            g->database_id         = database_id;
            g->perm_mask            = 0;
            g->grantee_uid_count    = 0;
            g->grantee_group_count  = 0;
            g->active               = 1;
            if ((uint32_t)(i + 1) > database_grant_count) database_grant_count = (uint32_t)(i + 1);
            return g;
        }
    }
    return 0;
}

int database_grant_uid(const char* db_name, uint32_t uid, uint32_t perm_mask) {
    int db_idx = find_database_idx(db_name);
    if (db_idx < 0) {
        kernel_serial_printf("[DATABASE] ERROR: database '%s' not found.\n", db_name);
        return 1;
    }
    uint32_t database_id = databases[db_idx].database_id;

    struct SLSDatabaseGrant* g = find_or_create_grant(database_id);
    if (!g) {
        kernel_serial_print("[DATABASE] ERROR: database grant table full.\n");
        return 2;
    }

    // perm_mask is ONE shared field on the whole grant entry (see
    // database.h's own struct SLSDatabaseGrant comment) -- every call sets
    // it, whether uid is a brand-new grantee or already one, exactly
    // mirroring how every grantee of an authlist shares that list's own
    // set of object grants uniformly. This is a real, named simplification
    // vs. per-grantee permission levels, not an oversight.
    g->perm_mask = perm_mask;

    for (uint32_t i = 0; i < g->grantee_uid_count; i++) {
        if (g->grantee_uids[i] == uid) {
            kernel_serial_printf("[DATABASE] '%s': updated grant for uid %u to 0x%02x.\n",
                                 db_name, uid, perm_mask);
            return 0;
        }
    }
    if (g->grantee_uid_count >= DATABASE_GRANT_MAX_UIDS) {
        kernel_serial_printf("[DATABASE] ERROR: '%s' grantee-uid table full.\n", db_name);
        return 2;
    }
    g->grantee_uids[g->grantee_uid_count++] = uid;
    kernel_serial_printf("[DATABASE] '%s': granted 0x%02x to uid %u.\n", db_name, perm_mask, uid);
    return 0;
}

int database_grant_group(const char* db_name, const char* group_name, uint32_t perm_mask) {
    int db_idx = find_database_idx(db_name);
    if (db_idx < 0) {
        kernel_serial_printf("[DATABASE] ERROR: database '%s' not found.\n", db_name);
        return 1;
    }
    uint32_t database_id = databases[db_idx].database_id;

    struct SLSDatabaseGrant* g = find_or_create_grant(database_id);
    if (!g) {
        kernel_serial_print("[DATABASE] ERROR: database grant table full.\n");
        return 2;
    }

    // perm_mask is ONE shared field on the whole grant entry -- see
    // database_grant_uid()'s own comment above for the full reasoning.
    g->perm_mask = perm_mask;

    for (uint32_t i = 0; i < g->grantee_group_count; i++) {
        if (db_streq(g->grantee_groups[i], group_name)) {
            kernel_serial_printf("[DATABASE] '%s': updated grant for group '%s' to 0x%02x.\n",
                                 db_name, group_name, perm_mask);
            return 0;
        }
    }
    if (g->grantee_group_count >= DATABASE_GRANT_MAX_GROUPS) {
        kernel_serial_printf("[DATABASE] ERROR: '%s' grantee-group table full.\n", db_name);
        return 2;
    }
    db_strncpy(g->grantee_groups[g->grantee_group_count++], group_name, GROUP_NAME_LEN);
    g->perm_mask |= perm_mask;
    kernel_serial_printf("[DATABASE] '%s': granted 0x%02x to group '%s'.\n", db_name, perm_mask, group_name);
    return 0;
}

// ─── database_check_access ──────────────────────────────────────────────
// Called from catalog_check_access() (object_catalog.c), alongside the
// group/authlist block, before the GUEST hard-deny -- see roadmap doc
// §1.4 for the exact placement reasoning (a uid with no individual role
// defaults to ROLE_GUEST, so any additive grant source must run before
// that hard-deny or it's silently unreachable for exactly the uids this
// feature is meant to serve).
int database_check_access(uint32_t uid, uint32_t database_id, uint32_t needed_perm) {
    if (database_id == 0) return 0;   // untagged object — nothing to check

    int idx = find_grant_idx(database_id);
    if (idx < 0) return 0;
    struct SLSDatabaseGrant* g = &database_grants[idx];

    if ((g->perm_mask & needed_perm) != needed_perm) return 0;

    for (uint32_t i = 0; i < g->grantee_uid_count; i++) {
        if (g->grantee_uids[i] == uid) return 1;
    }
    for (uint32_t i = 0; i < g->grantee_group_count; i++) {
        if (group_contains_uid(g->grantee_groups[i], uid)) return 1;
    }
    return 0;
}

// ─── Syscall wrappers (Phase 4) ─────────────────────────────────────────
// Same "null-check req, forward fields, collapse to 0/1" shape as
// sys_sls_group_create()/sys_sls_authlist_create() etc. (group_profile.c/
// authlist.c) -- no new convention introduced here.
uint64_t sys_sls_database_create(struct SLSDatabaseCreateRequest* req) {
    if (!req) return 1;
    return database_create(req->caller_uid, req->name) == 0 ? 0 : 1;
}

uint64_t sys_sls_database_drop(struct SLSDatabaseDropRequest* req) {
    if (!req) return 1;
    return database_drop(req->caller_uid, req->name) == 0 ? 0 : 1;
}

uint64_t sys_sls_database_grant_uid(struct SLSDatabaseGrantUidRequest* req) {
    if (!req) return 1;
    return database_grant_uid(req->db_name, req->uid, req->perm_mask) == 0 ? 0 : 1;
}

uint64_t sys_sls_database_grant_group(struct SLSDatabaseGrantGroupRequest* req) {
    if (!req) return 1;
    return database_grant_group(req->db_name, req->group_name, req->perm_mask) == 0 ? 0 : 1;
}

// Resolves db_name -> database_id via database_find_id() (0 if unknown --
// database_check_access() already treats database_id 0 as "nothing to
// check", so an unknown name cleanly reports "not granted" rather than a
// separate error path, matching this request struct's own header comment
// in database.h).
uint64_t sys_sls_database_check(struct SLSDatabaseCheckRequest* req) {
    if (!req) return 0;
    uint32_t database_id = database_find_id(req->db_name);
    return database_check_access(req->uid, database_id, req->needed_perm) ? 1 : 0;
}
