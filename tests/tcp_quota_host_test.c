/*
 * tcp_quota_host_test.c — Multitenant Isolation Gap Analysis §5 item 4 /
 * §7 item 1, Network Fairness Phase 2 verification: a standalone
 * host-buildable test for the REAL, unmodified net/tcp_quota.c, not a
 * reimplementation of it.
 *
 * Mirrors tests/http_rate_limit_host_test.c's own structure exactly, for
 * the identical reason: net/tcp_quota.c's real logic has exactly the same
 * two real dependencies http_rate_limit.c already named --
 * partition_get_for_uid() and PARTITION_MAX -- so it gets the same
 * settable-fake partition_get_for_uid() rather than linking kernel/
 * partition.c's much heavier persist/journal/lock_mgr graph, and the same
 * "pull the testable logic out of the too-heavy net/http.c" precedent.
 * kernel_serial_print/kernel_serial_printf are stubbed as no-ops, the same
 * pattern tests/vecstore_syscall_host_test.c and others already established
 * for kernel/kernel_io.h's console output during a host test.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net \
 *       -o /tmp/tcp_quota_host_test tests/tcp_quota_host_test.c \
 *       net/tcp_quota.c
 *   /tmp/tcp_quota_host_test
 */
#include "net/tcp_quota.h"
#include "net/tcp.h"
#include "kernel/partition.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ─── partition_get_for_uid() -- settable fake ────────────────────────────
 * Same technique and same FAKE_UID_MAX-overflow caveat as
 * tests/http_rate_limit_host_test.c's own copy of this fake -- see that
 * file's header comment for the full reasoning. */
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
    tcp_quota_init();

    /* ── Scenario 1: unlimited by default (0 = unlimited, BSS-zero-safe for
     * the quota array itself) -- many connections for the same partition
     * all succeed until an explicit quota is ever set. ─────────────────── */
    {
        fake_set_partition(100, 5);   // uid 100 -> partition 5
        int admitted = 0;
        for (int conn_id = 0; conn_id < 50; conn_id++) {
            if (tcp_conn_attribute(conn_id, 100)) admitted++;
        }
        CHECK(admitted == 50, "s1: with no quota configured, every distinct connection for partition 5 is admitted");
        CHECK(tcp_partition_get_conn_usage(5) == 50, "s1: partition 5's live usage count reflects all 50 attributed connections");
        CHECK(tcp_partition_get_conn_quota(5) == 0, "s1: partition 5's quota reads back 0 (unlimited), the default");
        for (int conn_id = 0; conn_id < 50; conn_id++) tcp_conn_release(conn_id);
        CHECK(tcp_partition_get_conn_usage(5) == 0, "s1: releasing every attributed connection brings usage back to 0");
    }

    /* ── Scenario 2: a configured quota is enforced -- the (quota+1)th
     * concurrent connection for that partition is denied. ──────────────── */
    {
        fake_set_partition(200, 9);
        CHECK(tcp_partition_set_conn_quota(9, 3) == 0, "s2: setup -- set partition 9's connection quota to 3");
        int admitted = 0, denied = 0;
        int conn_id = 100;
        for (int i = 0; i < 6; i++) {
            if (tcp_conn_attribute(conn_id + i, 200)) admitted++; else denied++;
        }
        CHECK(admitted == 3, "s2: exactly 3 concurrent connections were admitted before the quota kicked in");
        CHECK(denied == 3, "s2: every connection past the quota was denied");
        CHECK(tcp_partition_get_conn_usage(9) == 3, "s2: partition 9's usage reflects only the 3 admitted connections");
    }

    /* ── Scenario 3: releasing one attributed connection frees exactly one
     * slot in the quota, letting a new connection through -- proves this
     * is a live concurrency count, not a monotonic counter. ────────────── */
    {
        // Reuses partition 9's state from Scenario 2 (usage=3, quota=3).
        fake_set_partition(201, 9);   // a different uid, same partition 9 as Scenario 2's uid 200
        CHECK(tcp_conn_attribute(150, 201) == 0,
              "s3: setup -- partition 9 (uid 201, same partition) is still at quota, a new connection is denied");
        tcp_conn_release(100);   // release one of the 3 connections attributed in Scenario 2
        CHECK(tcp_partition_get_conn_usage(9) == 2, "s3: releasing one connection drops usage from 3 to 2");
        CHECK(tcp_conn_attribute(150, 201) == 1,
              "s3: with a slot freed, a new connection for the same partition is now admitted");
        CHECK(tcp_partition_get_conn_usage(9) == 3, "s3: usage is back at 3 after the new admission");
    }

    /* ── Scenario 4: idempotent re-attribution -- calling tcp_conn_attribute()
     * again for an already-attributed conn_id is a safe no-op success, not
     * a double-count, matching the documented "call every sweep" contract. ── */
    {
        fake_set_partition(300, 20);
        CHECK(tcp_conn_attribute(400, 300) == 1, "s4: setup -- conn_id 400 attributed to partition 20");
        CHECK(tcp_partition_get_conn_usage(20) == 1, "s4: usage is 1 after the first attribution");
        for (int i = 0; i < 10; i++) tcp_conn_attribute(400, 300);
        CHECK(tcp_partition_get_conn_usage(20) == 1,
              "s4: ten more calls for the SAME already-attributed conn_id do not inflate usage past 1");
    }

    /* ── Scenario 5: an unrelated partition is totally unaffected by
     * another partition being fully exhausted -- the real point of this
     * whole mechanism, mirroring http_rate_limit_host_test.c's own
     * Scenario 2. ───────────────────────────────────────────────────────── */
    {
        fake_set_partition(500, 30);
        CHECK(tcp_partition_get_conn_usage(30) == 0, "s5: a fresh, never-touched partition starts at usage 0");
        CHECK(tcp_conn_attribute(450, 500) == 1,
              "s5: partition 30's first connection is admitted even though partitions 9/20 above are at/near their own limits");
    }

    /* ── Scenario 6: releasing a never-attributed or already-released
     * conn_id is a safe no-op, and an out-of-range conn_id is rejected by
     * both functions rather than indexing out of bounds. ────────────────── */
    {
        tcp_conn_release(999);   // never attributed -- must not crash or underflow any counter
        CHECK(1, "s6: releasing a never-attributed conn_id does not crash");
        CHECK(tcp_conn_attribute(-1, 100) == 0, "s6: a negative conn_id is rejected");
        CHECK(tcp_conn_attribute(TCP_INBOUND_MAX_CONNS, 100) == 0,
              "s6: a conn_id at/past TCP_INBOUND_MAX_CONNS (the outbound-reserved range) is rejected -- this module never attributes outbound slots");
        tcp_conn_release(TCP_INBOUND_MAX_CONNS + 1);   // also must not crash
        CHECK(1, "s6: releasing an out-of-range conn_id does not crash");
    }

    /* ── Scenario 7: the defensive out-of-range partition_id guard --
     * can't happen via the real partition_get_for_uid()/partition_assign_
     * table[], but proves the guard itself denies cleanly rather than
     * indexing out of bounds, mirroring http_rate_limit_host_test.c's own
     * Scenario 6. ───────────────────────────────────────────────────────── */
    {
        fake_set_partition(600, 99999);   // deliberately out of PARTITION_MAX range
        CHECK(tcp_conn_attribute(500, 600) == 0,
              "s7: an out-of-range partition_id is denied (fails closed) rather than indexing out of bounds");
        CHECK(tcp_partition_get_conn_usage(99999) == 0xFFFFu, "s7: usage getter returns the 0xFFFF sentinel for an out-of-range partition_id");
        CHECK(tcp_partition_get_conn_quota(99999) == 0xFFFFu, "s7: quota getter returns the 0xFFFF sentinel for an out-of-range partition_id");
        CHECK(tcp_partition_set_conn_quota(99999, 5) == 1, "s7: setter fails (returns 1) for an out-of-range partition_id");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
