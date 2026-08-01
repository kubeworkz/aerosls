/*
 * checkpoint_delta.c — incremental checkpoint dirty tracking
 * (Core Backup Strategies, Step 4).
 *
 * Freestanding: no libc.
 */
#include "checkpoint_delta.h"

/* One bit per region in a 32-bit mask */
static volatile uint32_t ckpt_dirty_bits = 0xFFFFu;  /* start all-dirty */
static uint32_t ckpt_incr_counter = 0;

void ckpt_mark_dirty(uint32_t region) {
    if (region < CKPT_NUM_REGIONS)
        __atomic_fetch_or(&ckpt_dirty_bits, (1u << region), __ATOMIC_RELAXED);
}

int ckpt_is_dirty(uint32_t region) {
    if (region >= CKPT_NUM_REGIONS) return 0;
    return (__atomic_load_n(&ckpt_dirty_bits, __ATOMIC_RELAXED) >> region) & 1;
}

uint32_t ckpt_dirty_mask(void) {
    return __atomic_load_n(&ckpt_dirty_bits, __ATOMIC_RELAXED);
}

void ckpt_clear_all(void) {
    __atomic_store_n(&ckpt_dirty_bits, 0, __ATOMIC_RELAXED);
    ckpt_incr_counter++;
}

void ckpt_mark_all_dirty(void) {
    __atomic_store_n(&ckpt_dirty_bits, (1u << CKPT_NUM_REGIONS) - 1, __ATOMIC_RELAXED);
}

uint32_t ckpt_incremental_count(void) { return ckpt_incr_counter; }

void ckpt_reset_incremental_count(void) { ckpt_incr_counter = 0; }

int ckpt_should_force_full(void) {
    return ckpt_incr_counter >= CKPT_FULL_INTERVAL;
}
