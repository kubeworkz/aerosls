#include "authlist.h"
#include "kernel_io.h"
#include "../user/permissions.h"

// ─── String helpers (same small local-copy convention as group_profile.c's
// gp_* / auth.c's au_* / object_catalog.c's cat_*) ──────────────────────────
static int al_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void al_strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// ─── Globals ────────────────────────────────────────────────────────────────
struct SLSAuthListEntry authlist_table[AUTHLIST_MAX];
uint32_t                authlist_table_count = 0;

static int find_authlist_idx(const char* name) {
    for (int i = 0; i < AUTHLIST_MAX; i++) {
        if (authlist_table[i].active && al_streq(authlist_table[i].name, name)) return i;
    }
    return -1;
}

// ─── authlist_create ────────────────────────────────────────────────────────
int authlist_create(const char* name) {
    if (!name || !name[0]) return 0;
    if (find_authlist_idx(name) >= 0) {
        kernel_serial_printf("[AUTHLIST] ERROR: List '%s' already exists.\n", name);
        return 0;
    }
    for (int i = 0; i < AUTHLIST_MAX; i++) {
        if (!authlist_table[i].active) {
            struct SLSAuthListEntry* l = &authlist_table[i];
            al_strncpy(l->name, name, AUTHLIST_NAME_LEN);
            l->object_count        = 0;
            l->grantee_uid_count   = 0;
            l->grantee_group_count = 0;
            l->active              = 1;
            if ((uint32_t)(i + 1) > authlist_table_count) authlist_table_count = (uint32_t)(i + 1);
            kernel_serial_printf("[AUTHLIST] Created list '%s'.\n", name);
            return 1;
        }
    }
    kernel_serial_print("[AUTHLIST] ERROR: Authorization-list table full.\n");
    return 0;
}

// ─── authlist_grant_object ──────────────────────────────────────────────────
int authlist_grant_object(const char* list_name, const char* object_name, uint32_t perm_mask) {
    int idx = find_authlist_idx(list_name);
    if (idx < 0) {
        kernel_serial_printf("[AUTHLIST] ERROR: List '%s' not found.\n", list_name);
        return 0;
    }
    struct SLSAuthListEntry* l = &authlist_table[idx];

    // Update in place if this object is already attached.
    for (uint32_t i = 0; i < l->object_count; i++) {
        if (l->objects[i].active && al_streq(l->objects[i].object_name, object_name)) {
            l->objects[i].perm_mask = perm_mask;
            kernel_serial_printf("[AUTHLIST] '%s': updated grant on '%s' to 0x%02x.\n",
                                 list_name, object_name, perm_mask);
            return 1;
        }
    }
    if (l->object_count >= AUTHLIST_MAX_OBJECTS) {
        kernel_serial_printf("[AUTHLIST] ERROR: List '%s' object table full.\n", list_name);
        return 0;
    }
    struct SLSAuthListObjectGrant* g = &l->objects[l->object_count++];
    al_strncpy(g->object_name, object_name, OBJECT_NAME_LEN);
    g->perm_mask = perm_mask;
    g->active    = 1;
    kernel_serial_printf("[AUTHLIST] '%s': granted 0x%02x on '%s'.\n",
                         list_name, perm_mask, object_name);
    return 1;
}

// ─── authlist_grant_uid ─────────────────────────────────────────────────────
int authlist_grant_uid(const char* list_name, uint32_t uid) {
    int idx = find_authlist_idx(list_name);
    if (idx < 0) {
        kernel_serial_printf("[AUTHLIST] ERROR: List '%s' not found.\n", list_name);
        return 0;
    }
    struct SLSAuthListEntry* l = &authlist_table[idx];
    for (uint32_t i = 0; i < l->grantee_uid_count; i++) {
        if (l->grantee_uids[i] == uid) {
            kernel_serial_printf("[AUTHLIST] uid %u is already a grantee of '%s'.\n", uid, list_name);
            return 0;
        }
    }
    if (l->grantee_uid_count >= AUTHLIST_MAX_GRANTEE_UIDS) {
        kernel_serial_printf("[AUTHLIST] ERROR: List '%s' grantee-uid table full.\n", list_name);
        return 0;
    }
    l->grantee_uids[l->grantee_uid_count++] = uid;
    kernel_serial_printf("[AUTHLIST] uid %u added as grantee of '%s'.\n", uid, list_name);
    return 1;
}

// ─── authlist_grant_group ───────────────────────────────────────────────────
int authlist_grant_group(const char* list_name, const char* group_name) {
    int idx = find_authlist_idx(list_name);
    if (idx < 0) {
        kernel_serial_printf("[AUTHLIST] ERROR: List '%s' not found.\n", list_name);
        return 0;
    }
    struct SLSAuthListEntry* l = &authlist_table[idx];
    for (uint32_t i = 0; i < l->grantee_group_count; i++) {
        if (al_streq(l->grantee_groups[i], group_name)) {
            kernel_serial_printf("[AUTHLIST] group '%s' is already a grantee of '%s'.\n",
                                 group_name, list_name);
            return 0;
        }
    }
    if (l->grantee_group_count >= AUTHLIST_MAX_GRANTEE_GROUPS) {
        kernel_serial_printf("[AUTHLIST] ERROR: List '%s' grantee-group table full.\n", list_name);
        return 0;
    }
    al_strncpy(l->grantee_groups[l->grantee_group_count++], group_name, GROUP_NAME_LEN);
    kernel_serial_printf("[AUTHLIST] group '%s' added as grantee of '%s'.\n", group_name, list_name);
    return 1;
}

// ─── authlist_check_access ───────────────────────────────────────────────────
int authlist_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    for (int i = 0; i < AUTHLIST_MAX; i++) {
        struct SLSAuthListEntry* l = &authlist_table[i];
        if (!l->active) continue;

        // Does this list grant needed_perm on obj_name at all?
        int object_matches = 0;
        for (uint32_t k = 0; k < l->object_count; k++) {
            if (l->objects[k].active && al_streq(l->objects[k].object_name, obj_name) &&
                (l->objects[k].perm_mask & needed_perm) == needed_perm) {
                object_matches = 1;
                break;
            }
        }
        if (!object_matches) continue;

        // Is uid a grantee of this list -- directly, or via a grantee group?
        for (uint32_t k = 0; k < l->grantee_uid_count; k++) {
            if (l->grantee_uids[k] == uid) return 1;
        }
        for (uint32_t k = 0; k < l->grantee_group_count; k++) {
            if (group_contains_uid(l->grantee_groups[k], uid)) return 1;
        }
    }
    return 0;
}

// ─── authlist_list ───────────────────────────────────────────────────────────
void authlist_list(void) {
    kernel_serial_print("\n[AUTHLIST] Authorization Lists\n");
    int shown = 0;
    for (int i = 0; i < AUTHLIST_MAX; i++) {
        struct SLSAuthListEntry* l = &authlist_table[i];
        if (!l->active) continue;
        kernel_serial_printf(" '%s': %u object grant(s), %u uid grantee(s), %u group grantee(s)\n",
                             l->name, l->object_count, l->grantee_uid_count, l->grantee_group_count);
        for (uint32_t k = 0; k < l->object_count; k++) {
            if (!l->objects[k].active) continue;
            kernel_serial_printf("    - %s: perm=0x%02x\n",
                                 l->objects[k].object_name, l->objects[k].perm_mask);
        }
        shown++;
    }
    if (!shown) kernel_serial_print(" (no authorization lists defined)\n");
    kernel_serial_printf(" %d list(s).\n\n", shown);
}

// ─── Syscall wrappers ────────────────────────────────────────────────────────
uint64_t sys_sls_authlist_create(struct SLSAuthListCreateRequest* req) {
    if (!req) return 1;
    return authlist_create(req->name) ? 0 : 1;
}

uint64_t sys_sls_authlist_grant(struct SLSAuthListGrantRequest* req) {
    if (!req) return 1;
    switch (req->kind) {
        case 0:  return authlist_grant_object(req->list_name, req->object_name, req->perm_mask) ? 0 : 1;
        case 1:  return authlist_grant_uid(req->list_name, req->grantee_uid) ? 0 : 1;
        case 2:  return authlist_grant_group(req->list_name, req->grantee_group) ? 0 : 1;
        default: return 1;
    }
}

uint64_t sys_sls_authlist_check(struct SLSAuthListCheckRequest* req) {
    if (!req) return 0;
    return authlist_check_access(req->uid, req->object_name, req->needed_perm) ? 1 : 0;
}
