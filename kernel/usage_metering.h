#ifndef USAGE_METERING_H
#define USAGE_METERING_H

#include <stdint.h>
#include "partition.h"   // PARTITION_MAX

/*
 * usage_metering.h -- Multitenant Isolation Gap Analysis §5 item 6 / §7
 * item 6: per-partition usage metering.
 *
 * ─── The problem this closes ───────────────────────────────────────────
 * The gap analysis doc's §3 table found this bluntly: "Does not exist.
 * Repo-wide search for billing/metering/usage-report/cost-tracking
 * infrastructure returns nothing relevant -- the only 'quota' concept
 * anywhere is the frame-count quota from LPAR Phase 13, which is an
 * admission-control limit, not a metered, exportable usage record."
 * Confirmed again directly before writing this file: no metering/billing
 * code exists anywhere in the repo. This file is the first one.
 *
 * ─── What gets metered, and why these two things specifically ──────────
 * Two real, already-tracked-somewhere resources, deliberately NOT a
 * ground-up instrumentation of every subsystem:
 *
 *   1. Cumulative HTTP requests per partition. net/http_rate_limit.c
 *      already resolves partition_id per authenticated request and
 *      counts it against a FIXED WINDOW that resets every
 *      HTTP_PARTITION_RATE_WINDOW_TICKS -- perfect for admission control,
 *      useless for billing (a window resets its counter to 0, so there is
 *      no running total anywhere). usage_metering_record_request() is a
 *      second, independent, NEVER-reset counter recording the same event.
 *
 *   2. Frame-ticks (a RAM-equivalent of "instance-hours"): every tick,
 *      each active partition's CURRENT frame usage
 *      (frame_pool.h's partition_get_frame_usage(), LPAR Phase 13) is
 *      multiplied by the ticks elapsed since the last sample and added to
 *      a running total -- the same "quantity × time held" shape a real
 *      cloud billing system uses for GB-hours. This makes a genuine,
 *      honest usage record out of a gauge (current usage) that Phase 13
 *      already tracks but never accumulated over time.
 *
 * Deliberately NOT metered in this first pass: CPU time (no per-partition
 * scheduling-tick counter exists anywhere yet -- LPAR Phase 12 only
 * proved starvation-prevention fairness, not usage accounting) and disk
 * bytes (no per-partition storage counter exists yet either -- see
 * AeroSLS-Storage-Isolation-Roadmap-v0.1.md, itself deliberately deferred
 * as its own separate, larger body of work). Metering only ever reports
 * what a real counter already, honestly exists to report; it does not
 * invent new instrumentation across subsystems this phase doesn't touch.
 * A future phase that adds CPU-tick or disk-byte accounting elsewhere
 * should extend usage_table[] with the new field the same way, not create
 * a second, parallel metering mechanism.
 *
 * ─── Why cumulative, never reset (in this phase) ────────────────────────
 * A real billing period boundary (reset counters at the start of each
 * invoice cycle, or better, timestamp-keyed historical records so a past
 * period's numbers survive the next period's reset) is a real design
 * question deliberately NOT answered here -- this phase's whole point is
 * proving the counters themselves are real and accurate first. Every
 * counter here is lifetime-cumulative-since-boot, the honest, simplest
 * thing to build and verify; period-boundary semantics are named as
 * explicit future work, not guessed at.
 *
 * ─── Why a flat PARTITION_MAX-sized table, not a bolted-on field ────────
 * Same "separate table keyed by partition_id, not a field added to
 * struct SLSPartitionEntry" shape partition_owner_table[]
 * (Multi-Node Phase 2) and frame_pool.c's own partition_frame_usage[]/
 * partition_frame_quota[] arrays already established -- partition.c's own
 * struct doesn't need to know metering exists for its own logic to keep
 * working exactly as before.
 */

#define USAGE_METER_TICK_GATE 100   // sample every 100 calls to usage_metering_tick(),
                                    // the same "keep AP overhead low" reasoning
                                    // tier_mgr_tick()'s own "every 10 calls" gate uses,
                                    // just a coarser interval since frame usage changes
                                    // far less often than tier hot/cold access patterns

struct SLSUsageEntry {
    uint64_t http_requests_total;   // never reset -- see header comment
    uint64_t frame_ticks_total;     // sum of (frame_usage * elapsed_ticks) since boot
    uint64_t last_sample_tick;      // kernel_tick_counter value at the last accumulation
    uint8_t  initialized;           // 0 until the first tick has been observed for this
                                     // partition -- the first observation seeds
                                     // last_sample_tick without accumulating, so a
                                     // partition created long after boot doesn't get
                                     // charged a bogus frame-tick jump for time it didn't
                                     // exist (mirrors the same "first observation seeds,
                                     // doesn't charge" caution database_id/persist.c
                                     // magic-number-gated restores already use)
};

extern struct SLSUsageEntry usage_table[PARTITION_MAX];   // indexed directly by partition_id

// Records one HTTP request against uid's own partition (resolved via
// partition_get_for_uid(), the same choke point http_partition_rate_check()
// itself already uses). Never fails, never denies -- this is accounting,
// not admission control; http_partition_rate_check() remains the sole
// admission-control decision point, called separately and first.
void usage_metering_record_request(uint32_t uid);

// Periodic sampler, called from the AP service-poll loop right alongside
// tier_mgr_tick() (see kernel/microkernel.c). Internally gated to run its
// real accumulation only every USAGE_METER_TICK_GATE calls -- cheap to
// call every pass, matching tier_mgr_tick()'s own established convention.
void usage_metering_tick(void);

// Read accessors -- never fail, return 0 for an inactive/out-of-range
// partition_id, the same "never fails, 0 is always a safe default"
// posture partition_get_for_uid()/partition_get_frame_usage() already
// established.
uint64_t usage_metering_get_requests(uint32_t partition_id);
uint64_t usage_metering_get_frame_ticks(uint32_t partition_id);

// Debug/introspection listing, mirrors sys_sls_partition_list()'s style --
// prints every active partition's cumulative counters plus its CURRENT
// frame usage (a live gauge, not itself part of the cumulative record) to
// the serial port.
void usage_metering_list(void);

// ─── Syscalls ─────────────────────────────────────────────────────────────
// 272, immediately following tenant.h's SYS_SLS_TENANT_LIST = 271 --
// confirmed via a fresh grep across every kernel/*.h SYS_SLS_* define
// before picking this, matching this codebase's own "reconfirm the next
// free number at implementation time" convention.
#define SYS_SLS_USAGE_REPORT 272

void sys_sls_usage_report(void);

#endif /* USAGE_METERING_H */
