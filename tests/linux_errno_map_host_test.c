/*
 * linux_errno_map_host_test.c — Phase E7 increment 1, §5.7: the errno map
 * against the REAL kernel/linux_abi.c.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * The errno map is the one place two error vocabularies meet: a tenant
 * binary was compiled against Linux's numbers, and the kernel's answers
 * are the CAP_E* and CAP_ERR_* codes. A wrong entry is not a crash — it is a plausible
 * wrong number in a program's error path (a retry that should have been
 * an exit, an absence reported as a permission failure). So this test is
 * exhaustive in the direction that can be exhaustive (every code in both
 * families, plus the CAP_NONE sentinel, plus the range law) and precise
 * in the direction that cannot (the reverse map is defined only where a
 * Linux errno has one preimage; the collapse set is asserted to return
 * 0 rather than guess).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_errno_map_host_test \
 *       tests/linux_errno_map_host_test.c kernel/linux_abi.c
 *   /tmp/linux_errno_map_host_test
 */
#include <stdio.h>
#include <stdint.h>

#include "linux_abi.h"
#include "cap.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

int main(void)
{
    /* ─── The forward map, exhaustively over the Phase-3 CAP_E* family ────
     * Each expectation is the Linux errno a musl/glibc binary's error path
     * was compiled against; the comment is the reason the pairing is the
     * right one, so a future edit argues with the comment, not with taste. */
    struct { int32_t cap; int32_t want; const char *why; } fwd[] = {
        { CAP_EINVAL,        -LINUX_EINVAL,   "same word, same meaning" },
        { CAP_EBADF,         -LINUX_EBADF,    "same word, same meaning" },
        { CAP_EAGAIN,        -LINUX_EAGAIN,   "same word, same meaning" },
        { CAP_ETABLEFULL,    -LINUX_ENFILE,   "a fixed KERNEL table is system-scoped (ENFILE), not fd-scoped (EMFILE)" },
        { CAP_ECAPREVOKED,   -LINUX_EIO,      "the handle died under us mid-operation" },
        { CAP_EALREADY,      -LINUX_EALREADY, "same word, same meaning" },
        { CAP_ENOMEM,        -LINUX_ENOMEM,   "same word, same meaning" },
        { CAP_ENOSPC,        -LINUX_ENOSPC,   "same word, same meaning" },
        { CAP_ERANGE,        -LINUX_ERANGE,   "same word, same meaning" },
        { CAP_ENOSYS,        -LINUX_ENOSYS,   "same word, same meaning" },
        { CAP_ECONFLICT,     -LINUX_EBUSY,    "state conflict reads as busy to a POSIX caller" },
        { CAP_EPERM,         -LINUX_EPERM,    "same word, same meaning" },
    };
    size_t i;
    for (i = 0; i < sizeof(fwd) / sizeof(fwd[0]); i++) {
        int32_t got = linux_errno_from_cap(fwd[i].cap);
        char msg[192];
        snprintf(msg, sizeof(msg), "forward: CAP code %d -> %d (%s)",
                 (int)fwd[i].cap, (int)fwd[i].want, fwd[i].why);
        CHECK(got == fwd[i].want, msg);
    }

    /* ─── The forward map, exhaustively over the Phase-5 CAP_ERR_* family ── */
    struct { int32_t cap; int32_t want; const char *why; } fwd5[] = {
        { CAP_ERR_OK,        0,               "success is success in both vocabularies" },
        { CAP_ERR_NOTFOUND,  -LINUX_ENOENT,   "object absent" },
        { CAP_ERR_REVOKED,   -LINUX_EIO,      "died under us mid-op" },
        { CAP_ERR_RIGHTS,    -LINUX_EPERM,    "a rights check is a permission decision" },
        { CAP_ERR_RANGE,     -LINUX_ERANGE,   "same word, same meaning" },
        { CAP_ERR_BUDGET,    -LINUX_ENOMEM,   "the partition's frame wall reads as out-of-memory to the task" },
        { CAP_ERR_SPACE,     -LINUX_ENOBUFS,  "a full CHANNEL is ENOBUFS, not ENOMEM — memory is a different truth" },
        { CAP_ERR_TARGET,    -LINUX_ESRCH,    "the peer is gone: no such process" },
        { CAP_ERR_STATE,     -LINUX_EBUSY,    "wrong state for the operation" },
        { CAP_ERR_TYPE,      -LINUX_EINVAL,   "a malformed object word is the caller's bad argument" },
        { CAP_ERR_PROTO,     -LINUX_EPROTO,   "the conversation broke" },
        { CAP_ERR_NOMEM,     -LINUX_ENOMEM,   "same word, same meaning" },
        { CAP_ERR_BUFSZ,     -LINUX_EINVAL,   "a request that will not fit the message is the caller's bad argument" },
        { CAP_ERR_TIMEOUT,   -LINUX_ETIMEDOUT,"same word, same meaning" },
    };
    for (i = 0; i < sizeof(fwd5) / sizeof(fwd5[0]); i++) {
        int32_t got = linux_errno_from_cap(fwd5[i].cap);
        char msg[192];
        snprintf(msg, sizeof(msg), "forward: CAP_ERR_* %d -> %d (%s)",
                 (int)fwd5[i].cap, (int)fwd5[i].want, fwd5[i].why);
        CHECK(got == fwd5[i].want, msg);
    }

    /* ─── Status 0 is success, in the forward map ────────────────────────── */
    CHECK(linux_errno_from_cap(0) == 0, "forward: status 0 is 0 (success), not an errno");

    /* ─── The sentinel, and everything that is not an enumerated code ─────
     * §5.7's load-bearing rule: translated, never leaked. CAP_NONE as a
     * status means the kernel answered with an out-of-band value; the only
     * honest translation is "the kernel did not produce a result" (EIO),
     * never a plausible errno a binary might retry or treat as absence. */
    CHECK(linux_errno_from_cap((int32_t)CAP_NONE) == -LINUX_EIO,
          "forward: the CAP_NONE (0xFFFF) out-param sentinel is translated to -EIO, never leaked");
    CHECK(linux_errno_from_cap(99) == -LINUX_EIO,
          "forward: an unenumerated positive code outside CAP_ERR_* is refused as -EIO");
    CHECK(linux_errno_from_cap(-99) == -LINUX_EIO,
          "forward: an unenumerated negative code below CAP_EPERM is refused as -EIO");
    CHECK(linux_errno_from_cap(65536) == -LINUX_EIO,
          "forward: a value beyond the sentinel width is refused as -EIO");

    /* ─── The range law: a Linux task may only ever see [-4095, -1] or 0 ──
     * A value outside that window is not an errno to musl; it looks like
     * success (positive) or a wildly wrong number. Assert it over every
     * input this function accepts. */
    {
        int all_in_range = 1;
        int32_t probe;
        int32_t specials[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                               (int32_t)CAP_NONE, 99, -99, 65536, -1, -12 };
        for (i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
            int32_t got = linux_errno_from_cap(specials[i]);
            if (got != 0 && (got < -4095 || got > -1))
                all_in_range = 0;
        }
        for (probe = -12; probe <= 13; probe++) {
            int32_t got = linux_errno_from_cap(probe);
            if (got != 0 && (got < -4095 || got > -1))
                all_in_range = 0;
        }
        CHECK(all_in_range, "range: every forward result is 0 or inside [-4095, -1] (musl's errno window)");
    }

    /* ─── The reverse map, exactly where it is defined ───────────────────── */
    struct { int32_t linux_errno; int32_t want; } rev[] = {
        { -LINUX_ENOENT,    CAP_ERR_NOTFOUND },
        { -LINUX_ESRCH,     CAP_ERR_TARGET },
        { -LINUX_EBADF,     CAP_EBADF },
        { -LINUX_EAGAIN,    CAP_EAGAIN },
        { -LINUX_ENFILE,    CAP_ETABLEFULL },
        { -LINUX_EALREADY,  CAP_EALREADY },
        { -LINUX_ENOSPC,    CAP_ENOSPC },
        { -LINUX_ERANGE,    CAP_ERANGE },
        { -LINUX_ENOSYS,    CAP_ENOSYS },
        { -LINUX_ENOBUFS,   CAP_ERR_SPACE },
        { -LINUX_EPROTO,    CAP_ERR_PROTO },
        { -LINUX_ETIMEDOUT, CAP_ERR_TIMEOUT },
    };
    for (i = 0; i < sizeof(rev) / sizeof(rev[0]); i++) {
        int32_t got = cap_code_from_linux_errno(rev[i].linux_errno);
        char msg[128];
        snprintf(msg, sizeof(msg), "reverse: %d -> CAP code %d (unique preimage)",
                 (int)rev[i].linux_errno, (int)rev[i].want);
        CHECK(got == rev[i].want, msg);
    }

    /* ─── The collapse set: several CAP codes, one errno, no guess ────────
     * EPERM (CAP_EPERM, CAP_ERR_RIGHTS), ENOMEM (CAP_ENOMEM, BUDGET, NOMEM),
     * EINVAL (CAP_EINVAL, TYPE, BUFSZ), EIO (ECAPREVOKED, REVOKED) and
     * EBUSY (ECONFLICT, STATE) each have 2-3 preimages. The reverse MUST
     * return 0 — "not defined" — because choosing one would invent a
     * kernel-side truth the errno does not carry. */
    CHECK(cap_code_from_linux_errno(-LINUX_EPERM) == 0,
          "reverse: EPERM has two preimages (CAP_EPERM, CAP_ERR_RIGHTS) — returns 0, no guess");
    CHECK(cap_code_from_linux_errno(-LINUX_ENOMEM) == 0,
          "reverse: ENOMEM has three preimages — returns 0, no guess");
    CHECK(cap_code_from_linux_errno(-LINUX_EINVAL) == 0,
          "reverse: EINVAL has three preimages — returns 0, no guess");
    CHECK(cap_code_from_linux_errno(-LINUX_EIO) == 0,
          "reverse: EIO has two preimages — returns 0, no guess");
    CHECK(cap_code_from_linux_errno(-LINUX_EBUSY) == 0,
          "reverse: EBUSY has two preimages — returns 0, no guess");
    CHECK(cap_code_from_linux_errno(0) == 0,
          "reverse: 0 (success) has no CAP code preimage — returns 0");

    /* ─── Round trip, exactly over the defined half ────────────────────────
     * For every input whose reverse IS defined, reverse(forward(x)) == x.
     * This is the property that keeps a future edit of either map honest:
     * changing a unique-preimage forward entry silently redefines the
     * reverse, and this loop is where that shows up by name. */
    {
        int roundtrip_ok = 1;
        int32_t defined_set[] = { CAP_ERR_NOTFOUND, CAP_ERR_TARGET, CAP_EBADF,
                                  CAP_EAGAIN, CAP_ETABLEFULL, CAP_EALREADY,
                                  CAP_ENOSPC, CAP_ERANGE, CAP_ENOSYS,
                                  CAP_ERR_SPACE, CAP_ERR_PROTO, CAP_ERR_TIMEOUT };
        for (i = 0; i < sizeof(defined_set) / sizeof(defined_set[0]); i++) {
            int32_t c = defined_set[i];
            if (cap_code_from_linux_errno(linux_errno_from_cap(c)) != c)
                roundtrip_ok = 0;
        }
        CHECK(roundtrip_ok,
              "round trip: reverse(forward(x)) == x over every code with a unique preimage");
    }

    printf("linux_errno_map_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
