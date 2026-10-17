/*
 * linux_personality_host_test.c — Phase E7 increment 2, §3.3/§3.4/§10.1:
 * the personality predicate and the forwarded request against the REAL
 * kernel/linux_abi.c.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * The personality branch is the ONE thing standing between a Linux task's
 * rax=105 (setuid) and the SLS arm it collides with (SYS_SLS_ALLOCATE) —
 * §2.2's number-space property, and §10.3 names inverting this predicate
 * as the decisive sabotage tooth. So the predicate itself is asserted
 * here: SLS (the zero value — every slot at boot, every reused slot) does
 * not intercept, LINUX does, and no other value claims a trap. The
 * request struct's ABI is asserted as literals (size, field offsets,
 * capture order), because increment 3 posts it across a channel and the
 * sidecar's Rust mirror (user/proto) will be built against these numbers;
 * and describe() is asserted to be the pure copy it claims — six captured
 * values in, ret/status zeroed out, so what a caller reads before an
 * answer exists is defined rather than stale.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_personality_host_test \
 *       tests/linux_personality_host_test.c kernel/linux_abi.c
 *   /tmp/linux_personality_host_test
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

int main(void)
{
    /* ─── The predicate (§3.3 step 3, §10.3's tooth) ────────────────────── */
    CHECK(linux_personality_intercepts(PROC_PERSONALITY_SLS) == 0,
          "SLS tasks do NOT intercept: the existing chain is their only path");
    CHECK(linux_personality_intercepts(PROC_PERSONALITY_LINUX) == 1,
          "LINUX tasks intercept: describe-and-refuse before any number chain");
    CHECK(linux_personality_intercepts(0) == 0,
          "personality 0 (a zeroed slot: boot, reuse, kernel context) is SLS");
    CHECK(linux_personality_intercepts(2) == 0 &&
          linux_personality_intercepts(0xFFFFFFFFu) == 0,
          "an unrecognised personality value intercepts NOTHING — a value no\n"
          "      branch claims must not claim a trap either");

    /* ─── The enum's load-bearing value: SLS is 0 ───────────────────────── */
    CHECK(PROC_PERSONALITY_SLS == 0,
          "PROC_PERSONALITY_SLS is 0: BSS zero-init IS the safe default, so a\n"
          "      slot that was never stamped is an SLS task, never a Linux one");
    CHECK(PROC_PERSONALITY_LINUX == 1,
          "PROC_PERSONALITY_LINUX is 1");

    /* ─── The request struct's ABI (§3.4) — literals, because increment 3's
     * channel post and the Rust mirror are built against these numbers ──── */
    CHECK(sizeof(struct SLSLinuxSyscallRequest) == 72,
          "the request is 72 bytes (num 8 + args 48 + ret 8 + status 4 + pad 4)");
    CHECK(offsetof(struct SLSLinuxSyscallRequest, num) == 0, "num at offset 0");
    CHECK(offsetof(struct SLSLinuxSyscallRequest, args) == 8, "args at offset 8");
    CHECK(offsetof(struct SLSLinuxSyscallRequest, ret) == 56, "ret at offset 56");
    CHECK(offsetof(struct SLSLinuxSyscallRequest, status) == 64, "status at offset 64");
    CHECK(sizeof(((struct SLSLinuxSyscallRequest*)0)->args) == 48,
          "args is exactly six u64s — the six Linux argument registers");

    /* ─── describe(): the pure copy, with defined pre-answer state ──────── */
    {
        struct SLSLinuxSyscallRequest req;
        uint64_t captured[6] = { 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666 };
        size_t i;
        int all = 1;

        memset(&req, 0xCD, sizeof(req));   /* poison: describe writes everything
                                            * it claims to own */
        linux_syscall_describe(&req, 158, captured);   /* arch_prctl's number */
        CHECK(req.num == 158, "describe() carries the syscall number through");
        for (i = 0; i < 6; i++)
            if (req.args[i] != captured[i]) all = 0;
        CHECK(all, "describe() copies all six captured values, in order\n"
                   "      (rdi, rsi, rdx, r10, r8, r9 — the capture order is the ABI)");
        CHECK(req.args[0] == 0x1111 && req.args[3] == 0x4444,
              "arg positions are rdi=args[0] and r10=args[3] — Linux puts arg4\n"
              "      in r10 because SYSCALL itself clobbers RCX with the return RIP");
        CHECK(req.ret == 0,
              "ret is 0 before any answer exists — defined, not stale (poison cleared)");
        CHECK(req.status == 0, "status is 0 before any answer exists");
    }

    /* ─── describe() with no capture (pure kernel context) is total ─────── */
    {
        struct SLSLinuxSyscallRequest req;
        memset(&req, 0xCD, sizeof(req));
        linux_syscall_describe(&req, 9, NULL);
        CHECK(req.num == 9 && req.args[0] == 0 && req.args[5] == 0,
              "describe() with a NULL capture is defined (zeros), not a crash");
    }

    /* ─── The refusal contract's units: ENOSYS is a LINUX errno ─────────── */
    CHECK(LINUX_ENOSYS == 38,
          "LINUX_ENOSYS is 38 — the number a musl-built binary was compiled\n"
          "      against; the branch returns -38, never a CAP_E* family value");

    printf("linux_personality_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
