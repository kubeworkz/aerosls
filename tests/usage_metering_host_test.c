/*
 * usage_metering_host_test.c -- Multitenant Isolation Gap Analysis §5 item 6
 * / §7 item 6 verification: a standalone host-buildable test for the new
 * kernel/usage_metering.c, linked against the REAL, unmodified
 * usage_metering.c -- not a reimplementation.
 *
 * usage_metering.c's only real dependencies are: kernel_io.h's two logging
 * functions (faked, same pattern every other host test in this suite
 * uses), timer.h's kernel_tick_counter (bare global, settable directly --
 * this test drives it forward manually to simulate elapsed ticks the same
 * way frame_quota_host_test.c drives allocation counts directly),
 * frame_pool.h's partition_get_frame_usage() (settable fake, since this
 * test's whole point is proving frame-ticks = frame_usage * elapsed_ticks
 * accumulates correctly for chosen usage values), and partition.h's
 * partition_table[]/partition_get_for_uid() (bare storage + a small settable
 * fake, since usage_metering.c reads partition_table[] directly by index
 * and calls partition_get_for_uid() only from
 * usage_metering_record_request()).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -o /tmp/usage_metering_host_test \
 *       tests/usage_metering_host_test.c kernel/usage_metering.c
 *   /tmp/usage_metering_host_test
 */
#include "kernel/usage_metering.h"
#include "kernel/partition.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ── Bare storage for partition.h's extern tables (partition.c NOT linked) ── */
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
struct SLSPartitionOwner  partition_owner_table[PARTITION_MAX];

/* Minimal fake: uid == partition_id for this test's purposes (no assignment
 * table walk needed -- usage_metering_record_request() only cares that the
 * returned id round-trips into usage_table[] correctly). */
uint32_t partition_get_for_uid(uint32_t uid) {
    return uid;
}

/* ── Settable fake for frame_pool.h's partition_get_frame_usage() ────────── */
static uint64_t g_fake_frame_usage[PARTITION_MAX];
uint64_t partition_get_frame_usage(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0;
    return g_fake_frame_usage[partition_id];
}

/* ── kernel_tick_counter: bare global, driven forward manually ───────────── */
volatile uint64_t kernel_tick_counter = 0;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

/* Drives usage_metering_tick() exactly USAGE_METER_TICK_GATE times so its
 * internal gate fires exactly once -- matches the "every 100 calls, one
 * real sample" contract documented in usage_metering.h. */
static void tick_gate(void) {
    for (int i = 0; i < USAGE_METER_TICK_GATE; i++) usage_metering_tick();
}

int main(void) {
    memset(partition_table, 0, sizeof(partition_table));
    memset(g_fake_frame_usage, 0, sizeof(g_fake_frame_usage));

    /* Two active partitions, one inactive (never sampled). */
    partition_table[1].partition_id = 1;
    partition_table[1].active = 1;
    strcpy(partition_table[1].name, "tenant-a");

    partition_table[2].partition_id = 2;
    partition_table[2].active = 1;
    strcpy(partition_table[2].name, "tenant-b");

    partition_table[5].partition_id = 5;
    partition_table[5].active = 0;   /* inactive -- must never accumulate */
    strcpy(partition_table[5].name, "tenant-inactive");

    /* ── Scenario 1: HTTP request counting is cumulative and per-partition,
     * independent of frame-tick sampling. ───────────────────────────────── */
    CHECK(usage_metering_get_requests(1) == 0, "scenario 1: partition 1 starts with 0 requests");
    for (int i = 0; i < 7; i++) usage_metering_record_request(1);
    for (int i = 0; i < 3; i++) usage_metering_record_request(2);
    CHECK(usage_metering_get_requests(1) == 7, "scenario 1: partition 1 recorded exactly 7 requests");
    CHECK(usage_metering_get_requests(2) == 3, "scenario 1: partition 2 recorded exactly 3 requests, independent of partition 1");
    CHECK(usage_metering_get_requests(5) == 0, "scenario 1: an untouched partition still reads 0 requests");

    /* ── Scenario 2: first observation per partition seeds the clock without
     * accumulating -- no bogus frame-tick jump for time before the first
     * sample. ────────────────────────────────────────────────────────────── */
    kernel_tick_counter = 1000;
    g_fake_frame_usage[1] = 50;
    tick_gate();
    CHECK(usage_metering_get_frame_ticks(1) == 0, "scenario 2: first-ever sample seeds the clock, accumulates ZERO frame-ticks");
    CHECK(usage_metering_get_frame_ticks(2) == 0, "scenario 2: partition 2's first sample also seeds without accumulating");

    /* ── Scenario 3: a real elapsed interval accumulates frame_usage *
     * elapsed_ticks exactly. ─────────────────────────────────────────────── */
    kernel_tick_counter += USAGE_METER_TICK_GATE;   /* advance by the gate's own sampling interval */
    tick_gate();
    /* elapsed = USAGE_METER_TICK_GATE ticks, frame_usage = 50 -> += 50 * USAGE_METER_TICK_GATE */
    uint64_t expected = (uint64_t)50 * USAGE_METER_TICK_GATE;
    CHECK(usage_metering_get_frame_ticks(1) == expected, "scenario 3: frame-ticks accumulate exactly frame_usage * elapsed_ticks");
    CHECK(usage_metering_get_frame_ticks(2) == 0, "scenario 3: partition 2 (frame_usage still 0) accumulates 0, independent of partition 1's growth");

    /* ── Scenario 4: changing frame usage mid-stream accumulates the NEW
     * value over the NEXT interval, not retroactively. ──────────────────── */
    g_fake_frame_usage[1] = 200;
    kernel_tick_counter += USAGE_METER_TICK_GATE;
    tick_gate();
    uint64_t expected2 = expected + (uint64_t)200 * USAGE_METER_TICK_GATE;
    CHECK(usage_metering_get_frame_ticks(1) == expected2, "scenario 4: a frame-usage change is applied only to the interval after it took effect");

    /* ── Scenario 5: an inactive partition is never sampled, even though its
     * table slot exists and its fake frame usage is nonzero. ────────────── */
    g_fake_frame_usage[5] = 999;
    kernel_tick_counter += USAGE_METER_TICK_GATE;
    tick_gate();
    CHECK(usage_metering_get_frame_ticks(5) == 0, "scenario 5: an inactive partition accumulates nothing regardless of its fake frame usage");
    CHECK(usage_metering_get_requests(5) == 0, "scenario 5: an inactive partition's request counter also stays untouched");

    /* ── Scenario 6: out-of-range partition_id reads/records are safe
     * no-ops, mirroring frame_pool.c's own "never fails" posture. ───────── */
    CHECK(usage_metering_get_requests(PARTITION_MAX + 10) == 0, "scenario 6: out-of-range partition_id read returns 0, not a crash");
    CHECK(usage_metering_get_frame_ticks(PARTITION_MAX + 10) == 0, "scenario 6: out-of-range partition_id frame-tick read returns 0");
    usage_metering_record_request(PARTITION_MAX + 10);   /* must not crash or corrupt other entries */
    CHECK(usage_metering_get_requests(1) == 7, "scenario 6: an out-of-range record_request() call leaves real partitions' counters untouched");

    /* ── Scenario 7: the tick gate really is gated -- fewer than
     * USAGE_METER_TICK_GATE calls must not trigger a sample. ────────────── */
    uint64_t before = usage_metering_get_frame_ticks(2);
    g_fake_frame_usage[2] = 42;
    kernel_tick_counter += 1000000;   /* huge elapsed time, but... */
    for (int i = 0; i < USAGE_METER_TICK_GATE - 1; i++) usage_metering_tick();  /* ...one short of the gate */
    CHECK(usage_metering_get_frame_ticks(2) == before, "scenario 7: fewer than USAGE_METER_TICK_GATE ticks never triggers a sample, no matter how much time elapsed");
    usage_metering_tick();   /* the one call that crosses the gate */
    CHECK(usage_metering_get_frame_ticks(2) > before, "scenario 7: the Nth call (crossing the gate) finally samples, accumulating the full elapsed jump");

    printf("\n%d check(s) failed.\n", g_fail);
    return g_fail ? 1 : 0;
}
