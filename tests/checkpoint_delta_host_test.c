/*
 * checkpoint_delta_host_test.c — verification for kernel/checkpoint_delta.c
 * (Core Backup Strategies, Step 4: incremental checkpointing).
 *
 * Tests: dirty marking, masking, clearing, compaction interval, and
 * integration with checkpoint_trigger()'s region-skipping logic.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/checkpoint_delta_host_test \
 *       tests/checkpoint_delta_host_test.c kernel/checkpoint_delta.c
 *   /tmp/checkpoint_delta_host_test
 */
#include "kernel/checkpoint_delta.h"
#include <stdio.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
/* Passes print too, and at column 0. tests/run_all.sh counts checks with
 * grep -c '^ok:', so a file that counted passes silently reported "0 checks"
 * while running 30 of them -- indistinguishable in the suite output from a
 * test that had quietly stopped asserting anything. */
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Scenario 1: initial state is all-dirty ───────────────────────── */
static void test_initial_dirty(void) {
    printf("--- Scenario 1: initial state ---\n");
    /* checkpoint_delta.c initialises with all bits set */
    uint32_t mask = ckpt_dirty_mask();
    CHECK(mask == 0xFFFF, "all 16 regions start dirty");
    CHECK(ckpt_is_dirty(CKPT_REGION_CATALOG), "catalog is dirty");
    CHECK(ckpt_is_dirty(CKPT_REGION_WORKLOADS), "workloads is dirty");
}

/* ─── Scenario 2: clear and mark individual regions ────────────────── */
static void test_mark_clear(void) {
    printf("--- Scenario 2: mark and clear ---\n");
    ckpt_clear_all();
    CHECK(ckpt_dirty_mask() == 0, "after clear_all, mask is 0");
    CHECK(!ckpt_is_dirty(CKPT_REGION_RECORDS), "records is clean");

    ckpt_mark_dirty(CKPT_REGION_RECORDS);
    CHECK(ckpt_is_dirty(CKPT_REGION_RECORDS), "records is dirty after mark");
    CHECK(!ckpt_is_dirty(CKPT_REGION_CATALOG), "catalog still clean");
    CHECK(ckpt_dirty_mask() == (1u << CKPT_REGION_RECORDS), "only records bit set");

    ckpt_mark_dirty(CKPT_REGION_ROWSTORE);
    CHECK(ckpt_dirty_mask() == ((1u << CKPT_REGION_RECORDS) | (1u << CKPT_REGION_ROWSTORE)),
          "records + rowstore bits set");

    ckpt_clear_all();
    CHECK(ckpt_dirty_mask() == 0, "clear_all resets to 0 again");
}

/* ─── Scenario 3: mark_all_dirty ───────────────────────────────────── */
static void test_mark_all(void) {
    printf("--- Scenario 3: mark_all_dirty ---\n");
    ckpt_clear_all();
    ckpt_mark_all_dirty();
    CHECK(ckpt_dirty_mask() == ((1u << CKPT_NUM_REGIONS) - 1), "all 16 bits set");
}

/* ─── Scenario 4: compaction counter ───────────────────────────────── */
static void test_compaction(void) {
    printf("--- Scenario 4: compaction interval ---\n");
    ckpt_reset_incremental_count();
    CHECK(ckpt_incremental_count() == 0, "counter starts at 0 after reset");
    CHECK(!ckpt_should_force_full(), "not due for full checkpoint at 0");

    /* Simulate 7 incremental checkpoints */
    for (int i = 0; i < 7; i++) ckpt_clear_all();  /* clear_all increments counter */
    CHECK(ckpt_incremental_count() == 7, "counter is 7 after 7 clears");
    CHECK(!ckpt_should_force_full(), "not yet at interval (7 < 8)");

    ckpt_clear_all();  /* 8th */
    CHECK(ckpt_incremental_count() == 8, "counter is 8");
    CHECK(ckpt_should_force_full(), "should force full at interval 8");

    ckpt_reset_incremental_count();
    CHECK(ckpt_incremental_count() == 0, "reset brings it back to 0");
    CHECK(!ckpt_should_force_full(), "no longer due");
}

/* ─── Scenario 5: out-of-range region ignored ──────────────────────── */
static void test_bounds(void) {
    printf("--- Scenario 5: bounds checking ---\n");
    ckpt_clear_all();
    ckpt_mark_dirty(99);  /* out of range — should be no-op */
    CHECK(ckpt_dirty_mask() == 0, "out-of-range mark is a no-op");
    CHECK(!ckpt_is_dirty(99), "out-of-range query returns 0");
}

/* ─── Scenario 6: simulate incremental checkpoint flow ─────────────── */
static void test_flow(void) {
    printf("--- Scenario 6: incremental flow ---\n");
    ckpt_reset_incremental_count();
    ckpt_mark_all_dirty();  /* first checkpoint is full */

    /* "Full checkpoint": all regions persisted, then clear */
    uint32_t full_mask = ckpt_dirty_mask();
    CHECK(full_mask == ((1u << CKPT_NUM_REGIONS) - 1), "full: all dirty");
    ckpt_clear_all();
    ckpt_reset_incremental_count();

    /* Only catalog and records mutated */
    ckpt_mark_dirty(CKPT_REGION_CATALOG);
    ckpt_mark_dirty(CKPT_REGION_RECORDS);
    uint32_t incr_mask = ckpt_dirty_mask();
    CHECK(incr_mask == ((1u << CKPT_REGION_CATALOG) | (1u << CKPT_REGION_RECORDS)),
          "incremental: only 2 regions dirty");

    /* Only those 2 regions would be persisted; everything else is skipped.
     *
     * Derived from CKPT_NUM_REGIONS rather than hardcoded. This assertion read
     * `skipped == 14` with the message "out of 16" -- correct when it was
     * written and wrong the moment a 17th region was added, which is what it
     * was failing on. The substance of the scenario is "an incremental
     * checkpoint touches only the dirty regions", and that is true at any
     * dimension; pinning the dimension only made the test break when the thing
     * it does not test changed. */
    int skipped = 0;
    for (int i = 0; i < CKPT_NUM_REGIONS; i++) {
        if (!(incr_mask & (1u << i))) skipped++;
    }
    char skip_msg[96];
    snprintf(skip_msg, sizeof skip_msg,
             "%d of %d regions skipped -- everything not dirtied",
             CKPT_NUM_REGIONS - 2, CKPT_NUM_REGIONS);
    CHECK(skipped == CKPT_NUM_REGIONS - 2, skip_msg);

    ckpt_clear_all();
    CHECK(ckpt_incremental_count() == 1, "incremental counter advanced to 1");
}

int main(void) {
    test_initial_dirty();
    test_mark_clear();
    test_mark_all();
    test_compaction();
    test_bounds();
    test_flow();

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
