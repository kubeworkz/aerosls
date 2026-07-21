#ifndef TIER_MGR_H
#define TIER_MGR_H

#include <stdint.h>
#include "object_catalog.h"
#include "../drivers/io_prio.h"

// ─── Auto-Tier Policy Thresholds ─────────────────────────────────────────────
// A tick fires every 10 microkernel_service_poll() calls (from the AP loop).
#define TIER_PROMOTE_THRESHOLD   8    // accesses in one tick window → promote
#define TIER_DEMOTE_THRESHOLD   30    // consecutive idle ticks before demotion

// Must be >= CATALOG_MAX_OBJECTS
#define TIER_MAX_TRACKED        256

// ─── Cumulative event counters (never reset) ─────────────────────────────────
extern volatile uint64_t tier_total_accesses;   // every tier_notify_access() call
extern volatile uint64_t tier_total_promotions; // every auto or manual tier promote

// ─── Per-Object Access Statistics ────────────────────────────────────────────
struct TierStat {
    uint64_t object_id;
    uint32_t access_count;   // accesses recorded since the last tick evaluation
    uint32_t idle_ticks;     // consecutive tick evaluations with zero accesses
    uint8_t  active;
};

// ─── Tier → I/O Priority Mapping ─────────────────────────────────────────────
// Hot objects in SRAM/Cache get preemptive high-priority NVMe slots;
// cold SSD-resident objects use background low-priority lanes.
static inline enum IOPriority tier_to_io_priority(SLSStorageTier tier) {
    switch (tier) {
        case STORAGE_TIER_L1_CACHE: return PRIO_HIGH;
        case STORAGE_TIER_L2_DRAM:  return PRIO_MED;
        case STORAGE_TIER_L3_SSD:   return PRIO_LOW;
        default:                    return PRIO_MED;
    }
}

static inline const char* io_prio_name(enum IOPriority p) {
    switch (p) {
        case PRIO_HIGH: return "HIGH";
        case PRIO_MED:  return "MED";
        case PRIO_LOW:  return "LOW";
        default:        return "?";
    }
}

// ─── Syscall Numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_TIER_LIST    140
#define SYS_SLS_TIER_PROMOTE 141
#define SYS_SLS_TIER_DEMOTE  142

// ─── Public API ───────────────────────────────────────────────────────────────
extern struct TierStat tier_stats[TIER_MAX_TRACKED];

void     tier_mgr_init(void);
void     tier_notify_access(uint64_t object_id);   // call on every record access/fault
void     tier_mgr_tick(void);                       // periodic evaluation (from AP poll loop)

void     sys_sls_tier_list(void);
uint64_t sys_sls_tier_promote(const char* name);
uint64_t sys_sls_tier_demote(const char* name);

// ─── Navigator-Parity Gap Roadmap Phase 5b: per-tier byte accounting ─────────
// Previously nothing anywhere tracked bytes used per storage tier -- every
// existing tier stat (TierStat above, surfaced via GET /api/tiers) is
// access-count/idle-tick only. Real, already-there data makes this cheap:
// every active SLSObjectEntry already carries size_pages and storage_tier
// (object_catalog.h), so this just sums size_pages*4096 grouped by tier.
// Deliberately a pure function of object_catalog[]/object_catalog_count
// (no side effects, no dependency on tier_stats[]) so it's independently
// host-testable without needing the rest of the tier manager's state.
#define TIER_MGR_TIER_COUNT (STORAGE_TIER_L3_SSD + 1)   // 3: L1_CACHE, L2_DRAM, L3_SSD

// Fills bytes_per_tier[t]/count_per_tier[t] (both arrays of length
// TIER_MGR_TIER_COUNT, indexed by SLSStorageTier value) with the total bytes
// (size_pages * 4096) and object count of every active catalog object
// currently assigned to tier t. Both arrays are zeroed first, so callers
// don't need to pre-clear them.
void tier_capacity_totals(uint64_t bytes_per_tier[TIER_MGR_TIER_COUNT],
                          uint32_t count_per_tier[TIER_MGR_TIER_COUNT]);

// ─── Syscall ──────────────────────────────────────────────────────────────────
// Navigator-Parity Gap Roadmap Phase 5c: Terminal-facing twin of GET
// /api/disk (net/http.c's api_disk_json()) -- same underlying data
// (nvme_get_capacity_bytes() + tier_capacity_totals()), printed to the
// serial console. 253 is the next free number after this same phase's own
// SYS_SLS_NET_STATUS (252, net/net.h) -- confirmed via grep across every
// header defining SYS_SLS_* before picking this.
#define SYS_SLS_DISK_STATUS 253

void sys_sls_disk_status(void);

#endif /* TIER_MGR_H */
