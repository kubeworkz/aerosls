#include "tier_mgr.h"
#include "ipc.h"

struct TierStat tier_stats[TIER_MAX_TRACKED];
static uint32_t  tier_tick_counter  = 0;
volatile uint64_t tier_total_accesses   = 0;
volatile uint64_t tier_total_promotions = 0;

// ─── String comparison helper ─────────────────────────────────────────────────
static int tm_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ─── tier_mgr_init ────────────────────────────────────────────────────────────
void tier_mgr_init(void) {
    for (int i = 0; i < TIER_MAX_TRACKED; i++) {
        tier_stats[i].object_id    = 0;
        tier_stats[i].access_count = 0;
        tier_stats[i].idle_ticks   = 0;
        tier_stats[i].active       = 0;
    }
    kernel_serial_print(
        "[TIER] Storage Tier Manager initialized.\n"
        "[TIER] Thresholds: promote=8 accesses/tick  demote=30 idle ticks.\n");
}

// ─── tier_notify_access ───────────────────────────────────────────────────────
// Called on every page fault hit, record select, update, or insert so the tier
// manager can track which objects are hot and which have gone cold.
void tier_notify_access(uint64_t object_id) {
    __atomic_fetch_add(&tier_total_accesses, 1, __ATOMIC_RELAXED);
    // Find existing entry
    for (int i = 0; i < TIER_MAX_TRACKED; i++) {
        if (tier_stats[i].active && tier_stats[i].object_id == object_id) {
            __atomic_fetch_add(&tier_stats[i].access_count, 1, __ATOMIC_RELAXED);
            tier_stats[i].idle_ticks = 0;
            return;
        }
    }
    // First access — allocate a new stat slot
    for (int i = 0; i < TIER_MAX_TRACKED; i++) {
        if (!tier_stats[i].active) {
            tier_stats[i].object_id    = object_id;
            tier_stats[i].access_count = 1;
            tier_stats[i].idle_ticks   = 0;
            tier_stats[i].active       = 1;
            return;
        }
    }
    // Stat table full — silently continue (tier decisions will be skipped for
    // this object; it is not a fatal condition)
}

// ─── tier_mgr_tick ────────────────────────────────────────────────────────────
// Runs inside microkernel_service_poll() on the AP core.
// Evaluates the access pattern for every active catalog object and adjusts
// its storage tier automatically based on the configured thresholds.
void tier_mgr_tick(void) {
    tier_tick_counter++;

    // Evaluate every 10 calls to keep AP overhead low
    if (tier_tick_counter % 10 != 0) return;

    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;

        struct TierStat* ts = NULL;
        for (int j = 0; j < TIER_MAX_TRACKED; j++) {
            if (tier_stats[j].active &&
                tier_stats[j].object_id == e->object_id) {
                ts = &tier_stats[j];
                break;
            }
        }
        if (!ts) continue;  // never accessed — leave tier unchanged

        uint32_t accesses = ts->access_count;

        // ── Hot path: promote if above threshold and not already L1 ──────
        if (accesses > TIER_PROMOTE_THRESHOLD &&
            e->storage_tier > STORAGE_TIER_L1_CACHE) {

            e->storage_tier = (SLSStorageTier)(e->storage_tier - 1);
            __atomic_fetch_add(&tier_total_promotions, 1, __ATOMIC_RELAXED);
            kernel_serial_printf(
                "[TIER] AUTO-PROMOTE: '%-20s'  %s  (accesses=%u/tick)\n",
                e->name, tier_name(e->storage_tier), accesses);

            // Notify StorageTierMgr service via IPC (flush metadata to NVMe)
            struct IPCMessage m = {
                .src_port = 0,
                .dst_port = IPC_PORT_TIERMGR,
                .opcode   = TIER_OP_FLUSH,
                .payload  = { e->object_id, 0, 0, 0 }
            };
            ipc_post(IPC_PORT_TIERMGR, &m);
        }

        // ── Cold path: demote after prolonged idleness (except SYSTEM_METADATA)
        else if (accesses == 0 &&
                 ts->idle_ticks > TIER_DEMOTE_THRESHOLD &&
                 e->storage_tier < STORAGE_TIER_L3_SSD &&
                 e->type != OBJ_TYPE_SYSTEM_METADATA) {

            e->storage_tier = (SLSStorageTier)(e->storage_tier + 1);
            kernel_serial_printf(
                "[TIER] AUTO-DEMOTE:  '%-20s'  %s  (idle=%u ticks)\n",
                e->name, tier_name(e->storage_tier), ts->idle_ticks);
        }

        // ── Age counters ──────────────────────────────────────────────────
        if (accesses == 0) ts->idle_ticks++;
        else               ts->idle_ticks = 0;
        ts->access_count = 0;  // reset window for next evaluation
    }
}

// ─── sys_sls_tier_list ────────────────────────────────────────────────────────
void sys_sls_tier_list(void) {
    // Gap Remediation Phase C fix: kernel_serial_print() takes exactly one
    // argument (kernel_io.h) -- this was passing a format string plus 5
    // extra args to it, the same bug found and fixed in object_catalog.c's
    // sys_sls_obj_list() (see that file's own comment). kernel_serial_printf()
    // is the variadic one.
    kernel_serial_printf(
        "\n[TIER] Storage Tier Map\n"
        " %-22s  %-10s  %-8s  %-10s  %s\n"
        " %-22s  %-10s  %-8s  %-10s  %s\n",
        "Object", "Tier", "Accesses", "Idle Ticks", "I/O Priority",
        "----------------------", "----------",
        "--------", "----------", "------------");

    uint32_t shown = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;

        uint32_t accesses   = 0;
        uint32_t idle_ticks = 0;
        for (int j = 0; j < TIER_MAX_TRACKED; j++) {
            if (tier_stats[j].active &&
                tier_stats[j].object_id == e->object_id) {
                accesses   = tier_stats[j].access_count;
                idle_ticks = tier_stats[j].idle_ticks;
                break;
            }
        }

        const char* prio = io_prio_name(tier_to_io_priority(e->storage_tier));
        kernel_serial_printf(
            " %-22s  %-10s  %-8u  %-10u  %s\n",
            e->name, tier_name(e->storage_tier), accesses, idle_ticks, prio);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no objects allocated)\n");
    kernel_serial_print("\n");
}

// ─── sys_sls_tier_promote ─────────────────────────────────────────────────────
uint64_t sys_sls_tier_promote(const char* name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !tm_streq(e->name, name)) continue;

        if (e->storage_tier == STORAGE_TIER_L1_CACHE) {
            kernel_serial_printf(
                "[TIER] '%s' is already at L1_CACHE (top tier).\n", name);
            return 1;
        }
        SLSStorageTier old = e->storage_tier;
        e->storage_tier = (SLSStorageTier)(e->storage_tier - 1);
        kernel_serial_printf(
            "[TIER] PROMOTE: '%s'  %s  ->  %s\n",
            name, tier_name(old), tier_name(e->storage_tier));

        // Reset idle counter so this doesn't get immediately demoted again
        for (int j = 0; j < TIER_MAX_TRACKED; j++) {
            if (tier_stats[j].active &&
                tier_stats[j].object_id == e->object_id) {
                tier_stats[j].idle_ticks   = 0;
                tier_stats[j].access_count = 0;
                break;
            }
        }
        return 0;
    }
    kernel_serial_printf("[TIER] tier promote: Object '%s' not found.\n", name);
    return 1;
}

// ─── sys_sls_tier_demote ──────────────────────────────────────────────────────
uint64_t sys_sls_tier_demote(const char* name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !tm_streq(e->name, name)) continue;

        if (e->storage_tier == STORAGE_TIER_L3_SSD) {
            kernel_serial_printf(
                "[TIER] '%s' is already at L3_SSD (bottom tier).\n", name);
            return 1;
        }
        SLSStorageTier old = e->storage_tier;
        e->storage_tier = (SLSStorageTier)(e->storage_tier + 1);
        kernel_serial_printf(
            "[TIER] DEMOTE: '%s'  %s  ->  %s\n",
            name, tier_name(old), tier_name(e->storage_tier));
        return 0;
    }
    kernel_serial_printf("[TIER] tier demote: Object '%s' not found.\n", name);
    return 1;
}
