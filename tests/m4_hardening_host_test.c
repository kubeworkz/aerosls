/*
 * m4_hardening_host_test.c — M4.8's pure first-trap contracts: clone3's
 * 88-byte copy plan, requeue's two addrs/CMP snapshot, and the robust-list
 * offset arithmetic. These are the decisions the kernel must make before a
 * post; the sidecar's queue policy is driven by linux.rs's own unit tests
 * and the live fixture.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/m4_hardening_host_test \
 *       tests/m4_hardening_host_test.c kernel/linux_abi.c
 *   /tmp/m4_hardening_host_test
 */
#include <stdio.h>
#include <stdint.h>

#include "linux_abi.h"

static int passed = 0, failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failed++; } \
    else         { printf("ok:   %s\n", msg); passed++; } \
} while (0)

int main(void)
{
    uint64_t a[6];
    struct LinuxClone3Plan c;
    struct LinuxFutexPlan f;

    /* clone3: only the one v1 size, one whole user span. */
    a[0] = 0x4000; a[1] = LINUX_CLONE_ARGS_BYTES; a[2] = 0;
    a[3] = 0; a[4] = 0; a[5] = 0;
    c = linux_clone3_plan(LINUX_CLONE3, a);
    CHECK(c.refuse == 0 && c.src == 0x4000 &&
          c.payload_len == LINUX_CLONE_ARGS_BYTES,
          "clone3 copies exactly 88 bytes from the child's clone_args");
    a[1] = LINUX_CLONE_ARGS_BYTES - 1;
    CHECK(linux_clone3_plan(LINUX_CLONE3, a).refuse == -LINUX_EINVAL,
          "a short clone_args is EINVAL, not a prefix parse");
    a[1] = LINUX_CLONE_ARGS_BYTES + 1;
    CHECK(linux_clone3_plan(LINUX_CLONE3, a).refuse == -LINUX_EINVAL,
          "a future/longer clone_args is EINVAL, not silently truncated");
    a[0] = 0; a[1] = LINUX_CLONE_ARGS_BYTES;
    CHECK(linux_clone3_plan(LINUX_CLONE3, a).refuse == -LINUX_EFAULT,
          "a NULL clone_args is EFAULT before any post");
    a[0] = LINUX_USER_ADDR_MAX - 4; a[1] = LINUX_CLONE_ARGS_BYTES;
    CHECK(linux_clone3_plan(LINUX_CLONE3, a).refuse == -LINUX_EFAULT,
          "a clone_args span crossing the user ceiling is EFAULT");
    a[0] = 0x4000; a[1] = LINUX_CLONE_ARGS_BYTES;
    CHECK(linux_clone3_plan(LINUX_CLONE, a).payload_len == 0,
          "clone(56) plans no clone_args payload");

    /* Requeue: both queue keys are checked; only CMP attaches a word. */
    a[0] = 0x1000; a[1] = LINUX_FUTEX_REQUEUE | LINUX_FUTEX_PRIVATE_FLAG;
    a[2] = 1; a[3] = 2; a[4] = 0x2000; a[5] = 0;
    f = linux_futex_plan(LINUX_FUTEX, a);
    CHECK(f.refuse == 0 && f.payload_len == 0 && f.observed_src == 0,
          "FUTEX_REQUEUE checks uaddr2 but attaches no payload");
    a[1] = LINUX_FUTEX_CMP_REQUEUE | LINUX_FUTEX_PRIVATE_FLAG;
    a[5] = 7;
    f = linux_futex_plan(LINUX_FUTEX, a);
    CHECK(f.refuse == 0 && f.payload_len == LINUX_FUTEX_REQUEUE_PAYLOAD_BYTES &&
          f.observed_src == 0x2000,
          "FUTEX_CMP_REQUEUE attaches uaddr2's four-byte snapshot");
    a[4] = 0;
    CHECK(linux_futex_plan(LINUX_FUTEX, a).refuse == -LINUX_EFAULT,
          "a NULL CMP_REQUEUE destination is EFAULT");
    a[4] = LINUX_USER_ADDR_MAX - 2;
    CHECK(linux_futex_plan(LINUX_FUTEX, a).refuse == -LINUX_EFAULT,
          "a CMP_REQUEUE destination crossing the user ceiling is EFAULT");

    /* Robust offset: signed, overflow-checked, canonical. */
    CHECK(linux_robust_futex_addr(0x1000, 16) == 0x1010,
          "a positive robust offset lands at entry+offset");
    CHECK(linux_robust_futex_addr(0x1000, -16) == 0x0ff0,
          "a negative robust offset lands before the entry");
    CHECK(linux_robust_futex_addr(0x8, -16) == 0,
          "a negative offset below the user base is refused");
    CHECK(linux_robust_futex_addr(LINUX_USER_ADDR_MAX - 2, 16) == 0,
          "a positive offset above the user ceiling is refused");

    CHECK(linux_syscall_class(LINUX_CLONE3) == LSC_LOCAL,
          "clone3 is a local (thread-create) table row");
    CHECK(linux_syscall_class(LINUX_KILL) == LSC_SIG &&
          linux_syscall_class(LINUX_TGKILL) == LSC_SIG,
          "kill and tgkill are signal-class rows");
    CHECK(linux_syscall_class(LINUX_SET_ROBUST_LIST) == LSC_SIG &&
          linux_syscall_class(LINUX_GET_ROBUST_LIST) == LSC_SIG,
          "robust-list numbers are signal-class rows");

    printf("m4_hardening_host_test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}