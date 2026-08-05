/*
 * qemu_sls_pgo.c — Phase 3 persistent PGO for QEMU-SLS.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 3.
 */

#include "qemu_sls_pgo.h"
#include "qemu_sls_tcache.h"
#include "kernel_io.h"
#include "timer.h"
#include <stddef.h>
#include <stdint.h>

/* ─── hot-block queue (lock-free SPSC ring) ─────────────────────────────── */

static QemuHotTB hot_queue[QEMU_PGO_QUEUE_SIZE];
/* head/tail use __atomic ops matching the discipline in smp.c. */
static volatile uint32_t hot_queue_head;
static volatile uint32_t hot_queue_tail;

static void pgo_enqueue(uint64_t guest_pc, uint32_t code_offset,
                         uint32_t code_len, uint32_t exec_count) {
    uint32_t tail = __atomic_load_n(&hot_queue_tail, __ATOMIC_RELAXED);
    uint32_t next = (tail + 1) & (QEMU_PGO_QUEUE_SIZE - 1);
    if (next == __atomic_load_n(&hot_queue_head, __ATOMIC_ACQUIRE))
        return;  /* queue full — drop; next scan will re-detect */
    QemuHotTB *slot = &hot_queue[tail];
    slot->guest_pc       = guest_pc;
    slot->old_code_offset = code_offset;
    slot->old_code_len    = code_len;
    slot->exec_count      = exec_count;
    slot->_pad            = 0;
    __atomic_store_n(&hot_queue_tail, next, __ATOMIC_RELEASE);
}

/* ─── scan callback ─────────────────────────────────────────────────────── */

static void scan_cb(uint64_t guest_pc, uint32_t code_offset,
                    uint32_t code_len, uint32_t exec_count) {
    pgo_enqueue(guest_pc, code_offset, code_len, exec_count);
}

/* ─── qemu_sls_pgo_init ─────────────────────────────────────────────────── */

void qemu_sls_pgo_init(void) {
    hot_queue_head = 0;
    hot_queue_tail = 0;
    kernel_serial_printf(
        "[QEMU-SLS PGO] hot threshold=%u, scan interval=%u ticks\n",
        QEMU_PGO_HOT_THRESHOLD, QEMU_PGO_SCAN_INTERVAL_TICKS);
}

/* ─── qemu_sls_pgo_scan_tick ────────────────────────────────────────────── */

void qemu_sls_pgo_scan_tick(void) {
    static uint64_t next_scan = 0;
    if (kernel_tick_counter < next_scan) return;
    next_scan = kernel_tick_counter + QEMU_PGO_SCAN_INTERVAL_TICKS;
    qemu_sls_tcache_foreach_hot(QEMU_PGO_HOT_THRESHOLD, scan_cb);
}

/* ─── qemu_sls_pgo_pop_hot ──────────────────────────────────────────────── */

int qemu_sls_pgo_pop_hot(QemuHotTB *out) {
    uint32_t head = __atomic_load_n(&hot_queue_head, __ATOMIC_ACQUIRE);
    if (head == __atomic_load_n(&hot_queue_tail, __ATOMIC_RELAXED)) return 0;
    *out = hot_queue[head];
    __atomic_store_n(&hot_queue_head,
                     (head + 1) & (QEMU_PGO_QUEUE_SIZE - 1),
                     __ATOMIC_RELEASE);
    return 1;
}

/* ─── qemu_sls_pgo_patch_jump ──────────────────────────────────────────── */

void qemu_sls_pgo_patch_jump(uint64_t guest_pc,
                              uint32_t new_code_offset, uint32_t new_code_len) {
    /* Resolve the old code location before updating the descriptor. */
    uint32_t dummy_len;
    uint8_t *old_code = (uint8_t *)qemu_sls_tcache_lookup(guest_pc, &dummy_len, 0);
    if (!old_code) return;

    uint8_t *new_code = qemu_sls_codebuf + new_code_offset;

    /* Write JMP rel32 — 5 bytes: E9 + signed 32-bit displacement. */
    int32_t rel32 = (int32_t)(new_code - (old_code + 5));
    old_code[0] = 0xE9;
    old_code[1] = (uint8_t)((uint32_t)rel32);
    old_code[2] = (uint8_t)((uint32_t)rel32 >>  8);
    old_code[3] = (uint8_t)((uint32_t)rel32 >> 16);
    old_code[4] = (uint8_t)((uint32_t)rel32 >> 24);

    /* Flush the patched cache line before any core executes the new path. */
    __asm__ volatile("clflush (%0)" :: "r"(old_code) : "memory");
    __asm__ volatile("mfence"       :::                "memory");

    /* Update descriptor: exec_count reset so the block can be re-profiled. */
    qemu_sls_tcache_update_tb(guest_pc, new_code_offset, new_code_len);

    kernel_serial_printf(
        "[QEMU-SLS PGO] patched 0x%016lx old+%u → new+%u (%u bytes)\n",
        guest_pc, (unsigned)(old_code - qemu_sls_codebuf),
        new_code_offset, new_code_len);
}
