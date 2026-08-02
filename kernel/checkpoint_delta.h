#ifndef CHECKPOINT_DELTA_H
#define CHECKPOINT_DELTA_H

#include <stdint.h>

/*
 * checkpoint_delta.h — incremental checkpoint dirty tracking
 * (Core Backup Strategies, Step 4).
 *
 * Tracks which persist regions have been mutated since the last checkpoint.
 * On each checkpoint_trigger(), only dirty regions are actually written.
 * After a configurable number of incremental checkpoints, a full checkpoint
 * is forced (compaction).
 *
 * Mutation sites call ckpt_mark_dirty(CKPT_REGION_*) after modifying data.
 * This is safe-by-default: a missed mark causes an unnecessary full write
 * (the shadow-compare in persist.c still skips unchanged frames), not a
 * missed write. The dirty tracking is an OPTIMIZATION that avoids even
 * calling persist_*() for regions with no changes, not a correctness gate.
 */

// ─── Region IDs (one per persist_*() function) ────────────────────────────────
#define CKPT_REGION_CATALOG       0
#define CKPT_REGION_RECORDS       1
#define CKPT_REGION_SCHEMAS       2
#define CKPT_REGION_PROGRAMS      3
#define CKPT_REGION_PARTITIONS    4
#define CKPT_REGION_ROWSTORE      5
#define CKPT_REGION_ROW_CONSTR    6
#define CKPT_REGION_ROW_INDEX     7
#define CKPT_REGION_VECSTORE      8
#define CKPT_REGION_VEC_INDEX     9
#define CKPT_REGION_ROW_JOURNAL   10
#define CKPT_REGION_DATABASES     11
#define CKPT_REGION_VIEWS         12
#define CKPT_REGION_TENANTS       13
#define CKPT_REGION_SERVICES      14
#define CKPT_REGION_WORKLOADS     15
#define CKPT_REGION_TCACHE        16
#define CKPT_NUM_REGIONS          17

// ─── Compaction interval: force a full checkpoint every N incremental ones ────
#define CKPT_FULL_INTERVAL        8

// ─── Public API ───────────────────────────────────────────────────────────────

/* Mark a region as dirty. Called from mutation sites. */
void ckpt_mark_dirty(uint32_t region);

/* Query whether a region is dirty since last checkpoint. */
int  ckpt_is_dirty(uint32_t region);

/* Get the full dirty mask (one bit per region). */
uint32_t ckpt_dirty_mask(void);

/* Clear all dirty flags. Called after a successful checkpoint. */
void ckpt_clear_all(void);

/* Mark all regions dirty (forces a full checkpoint next time). */
void ckpt_mark_all_dirty(void);

/* How many incremental checkpoints since the last full one. */
uint32_t ckpt_incremental_count(void);

/* Reset incremental counter (called after a full checkpoint). */
void ckpt_reset_incremental_count(void);

/* Should the next checkpoint be full? (counter >= CKPT_FULL_INTERVAL or first) */
int ckpt_should_force_full(void);

#endif /* CHECKPOINT_DELTA_H */
