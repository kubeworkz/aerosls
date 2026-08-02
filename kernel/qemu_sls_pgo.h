/*
 * qemu_sls_pgo.h — Phase 3: persistent profile-guided optimization for QEMU-SLS.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 3.
 *
 * Flow:
 *   1. ap_kernel_main calls qemu_sls_pgo_scan_tick() every SCAN_INTERVAL ticks.
 *   2. scan_tick walks the TB table, enqueues any TB whose exec_count >= HOT_THRESHOLD.
 *   3. The TCG backend (outside this repo) calls qemu_sls_pgo_pop_hot() to get the
 *      next candidate, re-translates it with a higher optimization level, then calls
 *      qemu_sls_pgo_patch_jump() to redirect the old code and update the descriptor.
 *   4. exec_count is reset to 0 so the block can be profiled again in the next cycle.
 */
#ifndef QEMU_SLS_PGO_H
#define QEMU_SLS_PGO_H

#include <stdint.h>

/* exec_count threshold to classify a TB as hot. */
#define QEMU_PGO_HOT_THRESHOLD       1000U

/* How many kernel ticks between full TB-table scans (~10 s at 100 Hz). */
#define QEMU_PGO_SCAN_INTERVAL_TICKS 1000U

/* Ring-buffer capacity for hot TBs pending TCG re-optimization. */
#define QEMU_PGO_QUEUE_SIZE          64U

typedef struct {
    uint64_t guest_pc;
    uint32_t old_code_offset;
    uint32_t old_code_len;
    uint32_t exec_count;
    uint32_t _pad;
} QemuHotTB;

/* One-time init; call after qemu_sls_tcache_init(). */
void qemu_sls_pgo_init(void);

/*
 * Rate-limited background scan — add to ap_kernel_main's poll loop and the
 * uniprocessor fallback.  Enqueues hot TBs for TCG re-optimization.
 */
void qemu_sls_pgo_scan_tick(void);

/*
 * TCG backend: dequeue the next hot TB to re-optimize.
 * Fills *out and returns 1 when a candidate is available; returns 0 if empty.
 */
int  qemu_sls_pgo_pop_hot(QemuHotTB *out);

/*
 * TCG backend: after writing new optimized code at new_code_offset, call this
 * to patch the old code with a JMP to the new version, update the TB descriptor
 * (code_offset, code_len, exec_count = 0), and flush the instruction cache.
 */
void qemu_sls_pgo_patch_jump(uint64_t guest_pc,
                              uint32_t new_code_offset, uint32_t new_code_len);

#endif /* QEMU_SLS_PGO_H */
