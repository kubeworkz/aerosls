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
