/*
 * linux_futex_host_test.c — E7 TG-1 increment I3: private futex wait/wake.
 *
 * What this proves without a boot, a descriptor or a page table:
 *   - the served op set is exactly WAIT/WAKE with the optional PRIVATE bit;
 *     bitset ops, requeue, PI commands and CLOCK_REALTIME are refused ENOSYS
 *     by name rather than degraded to a nearby op;
 *   - a relative timespec becomes an ABSOLUTE tick deadline, rounding UP
 *     (never early), clamping at a day, and refusing a bad nsec;
 *   - a record is keyed by (group_id, uaddr): two processes that map the same
 *     virtual address never wake each other, and a wake takes at most the
 *     requested count;
 *   - enqueue refuses a full table without changing it, a duplicate tid
 *     replaces its record, and cancel removes exactly one tid;
 *   - expiry takes only finite waiters whose deadline has passed, never a
 *     forever waiter;
 *   - teardown's forget_group drops one group's records and nobody else's;
 *   - futex (202) is LSC_FUTEX in the real ABI table and its name is "futex".
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/linux_futex_host_test \
 *       tests/linux_futex_host_test.c kernel/linux_futex.c kernel/linux_abi.c
 *   /tmp/linux_futex_host_test
 *
 * The privileged halves live in process.c (the park/resume switch) and
 * syscall_dispatch.c (the ABI parse and the cr3 read); they are pinned by
 * their source guards and the live probe, not reachable here.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kernel/linux_futex.h"
#include "kernel/linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

#define TICK_NS 10000000ull   /* KERNEL_TICK_NS: 100 Hz */

int main(void) {
    printf("=== linux_futex: E7 TG-1 I3 private wait/wake state ===\n\n");

    linux_futex_init();

    /* ── 1. The op allowlist ─────────────────────────────────────────────── */
    {
        CHECK(linux_futex_op_supported(LINUX_FUTEX_OP_WAIT) == 0 &&
              linux_futex_op_supported(LINUX_FUTEX_OP_WAIT |
                                       LINUX_FUTEX_PRIVATE_FLAG) == 0 &&
              linux_futex_op_supported(LINUX_FUTEX_OP_WAKE) == 0 &&
              linux_futex_op_supported(LINUX_FUTEX_OP_WAKE |
                                       LINUX_FUTEX_PRIVATE_FLAG) == 0,
              "WAIT/WAKE, with or without PRIVATE, are the four served words (the census's 128/129 included)");
        CHECK(linux_futex_op_supported(9) == -LINUX_ENOSYS &&
              linux_futex_op_supported(10) == -LINUX_ENOSYS &&
              linux_futex_op_supported(2) == -LINUX_ENOSYS &&
              linux_futex_op_supported(3) == -LINUX_ENOSYS &&
              linux_futex_op_supported(6) == -LINUX_ENOSYS,
              "WAIT_BITSET, WAKE_BITSET, REQUEUE, CMP_REQUEUE and LOCK_PI are refused ENOSYS by name");
        CHECK(linux_futex_op_supported(LINUX_FUTEX_OP_WAIT |
                                       LINUX_FUTEX_CLOCK_REALTIME) ==
              -LINUX_ENOSYS,
              "CLOCK_REALTIME is refused: v1's timeouts are relative only");
        CHECK(linux_futex_op_supported(0x40000000u) == -LINUX_ENOSYS,
              "an unknown high flag bit is refused, never masked off");
    }

    /* ── 2. Relative timespec -> absolute deadline ───────────────────────── */
    {
        uint64_t d = 0;
        CHECK(linux_futex_deadline(0, 0, 500, TICK_NS, &d) == 0 && d == 500,
              "a zero timespec yields zero ticks (the caller answers ETIMEDOUT without parking)");
        CHECK(linux_futex_deadline(0, 1, 500, TICK_NS, &d) == 0 && d == 501,
              "one nanosecond rounds UP to one tick (a wait never ends early)");
        CHECK(linux_futex_deadline(0, TICK_NS, 500, TICK_NS, &d) == 0 &&
              d == 501,
              "exactly one tick is one tick");
        CHECK(linux_futex_deadline(0, TICK_NS + 1, 500, TICK_NS, &d) == 0 &&
              d == 502,
              "one tick plus a nanosecond rounds up to two");
        CHECK(linux_futex_deadline(1, 0, 500, TICK_NS, &d) == 0 && d == 600,
              "one second is KERNEL_TICK_HZ ticks");
        CHECK(linux_futex_deadline(1000000000000ull, 0, 500, TICK_NS, &d) ==
              0 && d == 500 + LINUX_FUTEX_MAX_SECONDS * 100ull,
              "an absurd timeout clamps at the day bound instead of overflowing");
        CHECK(linux_futex_deadline(0, 1000000000ull, 500, TICK_NS, &d) ==
              -LINUX_EINVAL,
              "nsec >= 1e9 is Linux's EINVAL");
        CHECK(linux_futex_deadline(0, 0, 500, 0, &d) == -LINUX_EINVAL,
              "a zero tick interval is refused");
        CHECK(linux_futex_deadline(0, 0, 500, TICK_NS, 0) == -LINUX_EINVAL,
              "a NULL output is refused");
    }

    /* ── 3. The key is (group_id, uaddr) ─────────────────────────────────── */
    {
        const uint64_t A = 0x0000700000000000ull;
        uint32_t tids[4];
        uint32_t got;

        linux_futex_init();
        CHECK(linux_futex_wait_enqueue(100, A, 101, 550) == 0,
              "a waiter enqueues under its group and address");
        CHECK(linux_futex_pending(100, A) == 1,
              "the record is visible on its own key");
        CHECK(linux_futex_wait_enqueue(200, A, 201, 550) == 0 &&
              linux_futex_pending(200, A) == 1,
              "another process may wait on the same virtual address independently");

        got = (uint32_t)linux_futex_wake_collect(100, A, 1, tids, 4);
        CHECK(got == 1 && tids[0] == 101,
              "a group's wake takes exactly that group's waiter");
        CHECK(linux_futex_pending(100, A) == 0 &&
              linux_futex_pending(200, A) == 1,
              "the other process's waiter is untouched — the key is not the bare address");

        got = (uint32_t)linux_futex_wake_collect(100, A, 1, tids, 4);
        CHECK(got == 0,
              "a wake with no waiter answers 0");

        /* At most `count`, and a different address never matches. */
        CHECK(linux_futex_wait_enqueue(100, A, 102, 550) == 0 &&
              linux_futex_wait_enqueue(100, A, 103, 550) == 0 &&
              linux_futex_wait_enqueue(100, A + 4, 104, 550) == 0,
              "three more waiters enqueue on two adjacent words");
        got = (uint32_t)linux_futex_wake_collect(100, A, 1, tids, 4);
        CHECK(got == 1 && linux_futex_pending(100, A) == 1 &&
              linux_futex_pending(100, A + 4) == 1,
              "a count of one wakes one waiter and leaves the address's other waiter parked");
        got = (uint32_t)linux_futex_wake_collect(100, A, 8, tids, 4);
        CHECK(got == 1 && linux_futex_pending(100, A) == 0 &&
              linux_futex_pending(100, A + 4) == 1,
              "a larger count drains the same word but not a neighbouring one");

        CHECK(linux_futex_wait_cancel(104) == 1 &&
              linux_futex_wait_cancel(104) == 0,
              "cancel removes one tid exactly once (the two at A were already woken)");
        CHECK(linux_futex_pending(200, A) == 1,
              "cancelling a tid leaves another group's waiter on the same address alone");
    }

    /* ── 4. Duplicate tid replaces, full table refuses ───────────────────── */
    {
        const uint64_t A = 0x0000700000001000ull;

        linux_futex_init();
        CHECK(linux_futex_wait_enqueue(300, A, 301, 700) == 0 &&
              linux_futex_wait_enqueue(300, A + 4, 301, 700) == 0,
              "re-enqueueing the same tid is accepted as an update");
        CHECK(linux_futex_pending(300, A) == 0 &&
              linux_futex_pending(300, A + 4) == 1,
              "the update moves the record instead of leaving a ghost");

        CHECK(linux_futex_wait_cancel(301) == 1,
              "the updated record can be cancelled");
        {
            int all = 1;
            for (uint32_t i = 0; i < LINUX_FUTEX_WAITERS; i++) {
                if (linux_futex_wait_enqueue(300, A + 8 + i * 8u,
                                             400u + i, 700) != 0)
                    all = 0;
            }
            CHECK(all, "the table fills to LINUX_FUTEX_WAITERS records");
        }
        CHECK(linux_futex_wait_enqueue(300, A + 0x100, 999, 700) ==
              -LINUX_ENOMEM,
              "one waiter past the cap is refused ENOMEM");
        CHECK(linux_futex_wait_cancel(400) == 1 &&
              linux_futex_wait_enqueue(300, A + 0x100, 999, 700) == 0,
              "a cancelled slot is reusable");
    }

    /* ── 5. Expiry: finite only, at the deadline, never forever ──────────── */
    {
        const uint64_t A = 0x0000700000002000ull;
        uint32_t tids[8];
        int got;

        linux_futex_init();
        CHECK(linux_futex_wait_enqueue(500, A, 501, 610) == 0 &&
              linux_futex_wait_enqueue(500, A, 502, 0) == 0,
              "a timed waiter and a forever waiter coexist");
        got = linux_futex_expire(609, tids, 8);
        CHECK(got == 0 && linux_futex_pending(500, A) == 2,
              "before the deadline nothing expires");
        got = linux_futex_expire(610, tids, 8);
        CHECK(got == 1 && tids[0] == 501,
              "at the deadline the timed waiter is taken");
        CHECK(linux_futex_pending(500, A) == 1,
              "the forever waiter is still parked (deadline 0 is not 0 ticks)");
        got = linux_futex_expire(100000, tids, 8);
        CHECK(got == 0 && linux_futex_pending(500, A) == 1,
              "no amount of time expires a forever waiter");

        CHECK(linux_futex_forget_group(500) == 1,
              "teardown's forget_group reports what it dropped");
        CHECK(linux_futex_pending(500, A) == 0,
              "and the group has no waiters afterwards");
        CHECK(linux_futex_forget_group(0) == 0 &&
              linux_futex_wait_enqueue(0, A, 1, 5) == -LINUX_EINVAL &&
              linux_futex_wait_enqueue(500, A, 0, 5) == -LINUX_EINVAL,
              "a zero group or tid is refused rather than aliased");
    }

    /* ── 6. The ABI row ──────────────────────────────────────────────────── */
    CHECK(linux_syscall_class(LINUX_FUTEX) == LSC_FUTEX,
          "futex (202) has the LSC_FUTEX class in the real ABI table");
    CHECK(strcmp(linux_syscall_class_name(LSC_FUTEX), "futex") == 0,
          "LSC_FUTEX's class name is \"futex\"");

    printf("\nlinux_futex_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
