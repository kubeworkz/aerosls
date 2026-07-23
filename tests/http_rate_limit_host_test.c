/*
 * http_rate_limit_host_test.c — Multitenant Isolation Gap Analysis §5 item
 * 4 / §7 item 1 verification: a standalone host-buildable test for the
 * REAL, unmodified net/http_rate_limit.c, not a reimplementation of it.
 *
 * net/http.c's own dependency graph (rowstore, sql_exec, vec_join, dozens
 * more) is far too heavy to link for a host test -- exactly why the
 * rate-limiting mechanism itself was split into its own tiny file
 * (net/http_rate_limit.h/.c) rather than left inline in http.c, the same
 * "pull the testable logic out of the too-heavy file" move LPAR Phase 5
 * made for net/dspp.c and Phase 12 made for process.c's
 * pick_next_partition()/pick_next_process_in_partition() helpers. This
 * file proves that move paid off: http_partition_rate_check()'s real,
 * unmodified logic gets real execution here, not just compile-check.
 *
 * partition_get_for_uid() is stubbed as a settable fake (the same
 * technique tests/partition_host_test.c's g_fake_local_node_id
 * established for cluster_local_node_id() in the Multi-Node Partition
 * Scaling Roadmap's Phase 2) -- a tiny uid->partition_id table the test
 * populates directly, avoiding kernel/partition.c's own much heavier
 * persist/journal/lock_mgr dependency graph, which this test has no
 * interest in. FAKE_UID_MAX must exceed every uid literal used below
 * (300/301/400/etc.) -- a too-small bound was caught here the hard way:
 * both fake_set_partition() and partition_get_for_uid() silently fall
 * back to treating an out-of-range uid as PARTITION_DEFAULT, so a uid
 * that overflows the table doesn't error, it just quietly maps to
 * partition 0 -- corrupting shared state for whichever scenario
 * genuinely means to test partition 0 (unassigned uids). kernel_tick_counter
 * is the real global definition, exactly
 * matching kernel/timer.c's own (volatile uint64_t, BSS-zero-initialized)
 * -- the same pattern tests/auth_host_test.c already established for
 * testing another tick-based TTL/window mechanism (AUTH_TOKEN_TTL_TICKS),
 * driven directly by the test the same way that file's own Scenario 4
 * advances time to prove expiry.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/http_rate_limit_host_test tests/http_rate_limit_host_test.c \
 *       net/http_rate_limit.c
 *   /tmp/http_rate_limit_host_test
 */
#include "net/http_rate_limit.h"
#include "kernel/partition.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* kernel_tick_counter -- real definition, matching kernel/timer.c's own
 * (volatile uint64_t, BSS-zero-initialized), driven directly by this test
 * to simulate time passing across rate-limit windows. */
volatile uint64_t kernel_tick_counter = 0;

/* ─── partition_get_for_uid() -- settable fake ────────────────────────────
 * A small uid->partition_id table this test populates directly via
 * fake_set_partition(), rather than linking the real kernel/partition.c
 * (persist.h/journal.h/lock_mgr.h/etc. -- a much heavier graph this test
 * has no interest in exercising; that's already covered on its own by
 * tests/partition_host_test.c). Defaults every uid to PARTITION_DEFAULT
 * (0) until explicitly set, matching partition_get_for_uid()'s own real
 * "unassigned -> PARTITION_DEFAULT" contract. */
#define FAKE_UID_MAX 1024
static uint32_t fake_partition_for_uid[FAKE_UID_MAX];
static void fake_set_partition(uint32_t uid, uint32_t partition_id) {
    if (uid < FAKE_UID_MAX) fake_partition_for_uid[uid] = partition_id;
}
uint32_t partition_get_for_uid(uint32_t uid) {
    if (uid >= FAKE_UID_MAX) return PARTITION_DEFAULT;
    return fake_partition_for_uid[uid];
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    for (uint32_t u = 0; u < FAKE_UID_MAX; u++) fake_partition_for_uid[u] = PARTITION_DEFAULT;

    /* ── Scenario 1: a fresh partition may make exactly
     * HTTP_PARTITION_RATE_LIMIT requests before being denied, and the
     * (LIMIT+1)th request in the same window is denied. ───────────────────── */
    {
        fake_set_partition(100, 5);   // uid 100 -> partition 5
        int allowed = 0, denied = 0;
        for (uint32_t i = 0; i < HTTP_PARTITION_RATE_LIMIT + 5; i++) {
            if (http_partition_rate_check(100)) allowed++; else denied++;
        }
        CHECK(allowed == HTTP_PARTITION_RATE_LIMIT,
              "s1: exactly HTTP_PARTITION_RATE_LIMIT requests were allowed before denial started");
        CHECK(denied == 5, "s1: every request past the limit in the same window was denied");
    }

    /* ── Scenario 2: a completely different partition is totally
     * unaffected by partition 5 already being exhausted -- the real
     * point of this whole mechanism. ───────────────────────────────────────── */
    {
        fake_set_partition(200, 9);   // uid 200 -> partition 9, unrelated to partition 5
        CHECK(http_partition_rate_check(200) == 1,
              "s2: an unrelated partition's first request succeeds even though partition 5 is fully exhausted");
        int allowed = 1;   // the check above already counted as one
        for (uint32_t i = 1; i < HTTP_PARTITION_RATE_LIMIT; i++) {
            if (http_partition_rate_check(200)) allowed++;
        }
        CHECK(allowed == HTTP_PARTITION_RATE_LIMIT,
              "s2: partition 9 independently gets its own full budget of HTTP_PARTITION_RATE_LIMIT requests");
        CHECK(http_partition_rate_check(200) == 0,
              "s2: partition 9 is denied once IT (independently) exhausts its own budget");
    }

    /* ── Scenario 3: two different uids sharing the SAME partition share
     * ONE budget, not two -- matching every other multitenancy choke
     * point's own "keyed on partition, not uid" convention. ─────────────────── */
    {
        fake_set_partition(300, 3);
        fake_set_partition(301, 3);   // a different uid, same partition as 300
        int total_allowed = 0;
        for (uint32_t i = 0; i < HTTP_PARTITION_RATE_LIMIT; i++) {
            if (http_partition_rate_check(300)) total_allowed++;
        }
        CHECK(total_allowed == HTTP_PARTITION_RATE_LIMIT,
              "s3: setup -- uid 300 alone can exhaust partition 3's entire budget");
        CHECK(http_partition_rate_check(301) == 0,
              "s3: uid 301, a DIFFERENT uid in the SAME partition, is denied too -- the budget is shared, not per-uid");
    }

    /* ── Scenario 4: the window genuinely resets after
     * HTTP_PARTITION_RATE_WINDOW_TICKS elapse -- a partition denied in one
     * window is allowed again once real time (kernel_tick_counter) moves
     * past the window boundary, the same "drive the real global forward"
     * technique tests/auth_host_test.c's own TTL-expiry scenario uses. ──────── */
    {
        fake_set_partition(400, 7);
        for (uint32_t i = 0; i < HTTP_PARTITION_RATE_LIMIT; i++) http_partition_rate_check(400);
        CHECK(http_partition_rate_check(400) == 0, "s4: setup -- partition 7 is exhausted in its current window");

        kernel_tick_counter += HTTP_PARTITION_RATE_WINDOW_TICKS - 1;
        CHECK(http_partition_rate_check(400) == 0,
              "s4: one tick before the window fully elapses, partition 7 is STILL denied (no premature reset)");

        kernel_tick_counter += 2;   // now safely past the window boundary
        CHECK(http_partition_rate_check(400) == 1,
              "s4: once the window has genuinely elapsed, partition 7's very next request is allowed again");

        int allowed_in_new_window = 1;   // the check above already counted as one
        for (uint32_t i = 1; i < HTTP_PARTITION_RATE_LIMIT; i++) {
            if (http_partition_rate_check(400)) allowed_in_new_window++;
        }
        CHECK(allowed_in_new_window == HTTP_PARTITION_RATE_LIMIT,
              "s4: the new window grants a genuinely fresh full budget, not a partial carryover");
    }

    /* ── Scenario 5: PARTITION_SYSTEM/PARTITION_DEFAULT (0) -- every
     * never-explicitly-assigned uid, including kernel uid 0 -- is rate
     * limited exactly like any other real partition_id, not silently
     * exempted. This is deliberate, not an oversight: see
     * http_rate_limit.h's own header comment on why PARTITION_SYSTEM gets
     * no special-case bypass here. ──────────────────────────────────────────── */
    {
        // uid 50 was never given an explicit fake_set_partition() call,
        // so partition_get_for_uid(50) returns PARTITION_DEFAULT (0),
        // exactly matching the real function's own unassigned-uid contract.
        // (Must stay well under FAKE_UID_MAX -- this test's own fake
        // partition_get_for_uid() silently falls back to PARTITION_DEFAULT
        // for any uid >= FAKE_UID_MAX too, which would make this scenario
        // look right for the wrong reason instead of actually exercising
        // the real "unassigned uid" path.)
        int allowed = 0;
        for (uint32_t i = 0; i < HTTP_PARTITION_RATE_LIMIT + 3; i++) {
            if (http_partition_rate_check(50)) allowed++;
        }
        CHECK(allowed == HTTP_PARTITION_RATE_LIMIT,
              "s5: an unassigned uid (falling back to PARTITION_DEFAULT/PARTITION_SYSTEM, id 0) is rate limited too, not exempted");
    }

    /* ── Scenario 6: the defensive out-of-range guard
     * (pid >= PARTITION_MAX -> always allowed) -- can't happen via the
     * real partition_get_for_uid()/partition_assign_table[], but this
     * proves the guard itself works rather than just trusting the
     * comment, the same "never let a bad index gate-crash the fairness
     * check itself" property http_rate_limit.c's own comment names. ─────── */
    {
        fake_set_partition(600, 999);   // deliberately out of PARTITION_MAX range
        int allowed = 0;
        for (int i = 0; i < 500; i++) {
            if (http_partition_rate_check(600)) allowed++;
        }
        CHECK(allowed == 500,
              "s6: an out-of-range partition_id never gets rate limited at all -- the defensive fallback always allows, exactly as documented, rather than indexing out of bounds");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
