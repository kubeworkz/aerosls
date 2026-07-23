#include "tier_mgr.h"
#include "ipc.h"
#include "../drivers/nvme_admin.h"   // Navigator-Parity Gap Roadmap Phase 5c -- nvme_get_capacity_bytes() for sys_sls_disk_status()
#include "storage_quota.h"           // Storage Isolation Roadmap Phase 2 -- per-partition byte usage/quota in sys_sls_disk_status()

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

// ─── tier_capacity_totals ─────────────────────────────────────────────────────
// Navigator-Parity Gap Roadmap Phase 5b — see tier_mgr.h's own comment for
// why this is a new, pure, independently-testable function rather than
// folded into sys_sls_tier_list()'s existing per-object printer.
void tier_capacity_totals(uint64_t bytes_per_tier[TIER_MGR_TIER_COUNT],
                          uint32_t count_per_tier[TIER_MGR_TIER_COUNT]) {
    for (int t = 0; t < TIER_MGR_TIER_COUNT; t++) {
        bytes_per_tier[t] = 0;
        count_per_tier[t] = 0;
    }
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;
        int t = (int)e->storage_tier;
        if (t < 0 || t >= TIER_MGR_TIER_COUNT) continue;   // defensive; every real tier value fits
        bytes_per_tier[t] += (uint64_t)e->size_pages * 4096ULL;
        count_per_tier[t] += 1;
    }
}

// ─── sys_sls_disk_status ──────────────────────────────────────────────────────
// Navigator-Parity Gap Roadmap Phase 5c. Extended by Storage Isolation
// Roadmap Phase 2 with a per-partition byte-level breakdown, appended after
// the existing system-wide tier table -- exactly the "expose Phase 1's
// counters via the existing sys_sls_disk_status()/tier-totals reporting
// path, broken out per-partition" ask (roadmap doc §4 item 2). The
// underlying counters (storage_quota.c) already exist and are already the
// single source of truth for on-disk page usage as of Phase 1; this just
// converts pages -> bytes (x 4096) and prints them here too, rather than
// introducing a second, parallel accounting mechanism.
void sys_sls_disk_status(void) {
    static const char* tier_keys[TIER_MGR_TIER_COUNT] = { "L1_CACHE", "L2_DRAM", "L3_SSD" };
    uint64_t bytes_per_tier[TIER_MGR_TIER_COUNT];
    uint32_t count_per_tier[TIER_MGR_TIER_COUNT];
    tier_capacity_totals(bytes_per_tier, count_per_tier);

    kernel_serial_printf(
        "\n[DISK] Storage Status\n"
        " NVMe Capacity: %llu bytes (~%llu MB)\n",
        (unsigned long long)nvme_get_capacity_bytes(),
        (unsigned long long)(nvme_get_capacity_bytes() / (1024 * 1024)));
    kernel_serial_printf(" %-10s  %-16s  %s\n", "Tier", "Bytes Used", "Object Count");
    for (int t = 0; t < TIER_MGR_TIER_COUNT; t++) {
        kernel_serial_printf(" %-10s  %-16llu  %u\n",
                             tier_keys[t], (unsigned long long)bytes_per_tier[t], count_per_tier[t]);
    }

    kernel_serial_print("\n Per-Partition Storage Usage (rowstore+vecstore, bytes):\n");
    kernel_serial_printf(" %-10s  %-16s  %s\n", "Partition", "Bytes Used", "Bytes Quota");
    int shown = 0;
    for (uint32_t p = 0; p < PARTITION_MAX; p++) {
        uint64_t page_usage = storage_get_page_usage(p);
        uint64_t page_quota = storage_get_page_quota(p);
        if (page_usage == 0 && page_quota == 0) continue;   // same skip rule as sys_sls_partition_storage_quota_list()
        uint64_t bytes_used  = page_usage * 4096ULL;
        if (page_quota == 0) {
            kernel_serial_printf(" %-10u  %-16llu  unlimited\n", p, (unsigned long long)bytes_used);
        } else {
            kernel_serial_printf(" %-10u  %-16llu  %llu\n", p, (unsigned long long)bytes_used,
                                 (unsigned long long)(page_quota * 4096ULL));
        }
        shown++;
    }
    kernel_serial_printf(" %d partition(s) with nonzero disk usage or a configured quota.\n", shown);
    kernel_serial_print("\n");
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
