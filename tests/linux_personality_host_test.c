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
#include "cap.h"       /* the CAP_E* / CAP_ERR_* codes the reply may carry —
                        * the same families the errno-map host test pins */

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

    /* ═══ E7 increment 3 (§3.3 steps 5-7): the reply, the in-flight state,
     * the channel shape — all pure, all asserted here because the park that
     * consumes them is exactly what no host can run ═════════════════════ */

    /* ─── The resume sentinel and the in-flight predicate ──────────────── */
    CHECK(SYS_LINUX_TRAP_RESUME == 0x4C494E52u,
          "the resume sentinel is \"LINR\": outside Linux's number space,\n"
          "      outside every SYS_SLS_*, and it fits the u32 park_syscall");
    CHECK(sizeof(SYS_LINUX_TRAP_RESUME) <= sizeof(uint32_t),
          "the sentinel fits park_syscall — a value cap_recv_resume passes\n"
          "      through its %edi re-entry could truncate would strand a\n"
          "      parked forward on a different number");
    CHECK(linux_forward_resume_is_live(PROC_PERSONALITY_LINUX,
                                       SYS_LINUX_TRAP_RESUME) == 1,
          "an armed Linux task at the sentinel IS the in-flight resume\n"
          "      re-run — the only arm that reads a reply");
    CHECK(linux_forward_resume_is_live(PROC_PERSONALITY_SLS,
                                       SYS_LINUX_TRAP_RESUME) == 0,
          "an SLS task at the sentinel intercepts nothing (its number\n"
          "      space is its own — §2.2 applied to the resume number too)");
    CHECK(linux_forward_resume_is_live(PROC_PERSONALITY_LINUX, 0) == 0 &&
          linux_forward_resume_is_live(PROC_PERSONALITY_LINUX,
                                       311 /* SYS_SLS_CHAN_WAIT */) == 0,
          "a disarmed Linux task is never a resume: a stray sentinel trap\n"
          "      is refused by name, never parked");
    CHECK(linux_forward_resume_is_live(0xFFFFFFFFu, SYS_LINUX_TRAP_RESUME) == 0,
          "an unrecognised personality asserts no in-flight state either");
    CHECK(linux_syscall_class(SYS_LINUX_TRAP_RESUME) == LSC_UNKNOWN,
          "the census table has no row for the sentinel: it can never be\n"
          "      mistaken for a describable syscall's number");

    /* ─── §3.5's channel shape as a pure rule ──────────────────────────── */
    CHECK(linux_channel_shape_ok(1, 1) == 1,
          "exactly one sendable CHAN_W and one recv-able CHAN_R is the\n"
          "      shape a Linux task forwards on");
    CHECK(linux_channel_shape_ok(0, 1) == 0 &&
          linux_channel_shape_ok(1, 0) == 0 &&
          linux_channel_shape_ok(0, 0) == 0,
          "a task WITHOUT its channel (§10.3's no-cap-channel tooth) can\n"
          "      never forward — the refusal is structural");
    CHECK(linux_channel_shape_ok(2, 1) == 0 &&
          linux_channel_shape_ok(1, 2) == 0 &&
          linux_channel_shape_ok(3, 3) == 0,
          "two candidates are refused, not guessed between: routing a\n"
          "      task's syscalls to the wrong channel is a decision the\n"
          "      kernel may not make (§3.6: describe, never interpret)");

    /* ─── apply_reply: the reply contract, total and by name ───────────── */
    {
        struct SLSLinuxSyscallRequest base;
        struct SLSLinuxSyscallRequest reply;
        uint64_t captured[6] = { 1, 2, 3, 4, 5, 6 };
        uint64_t r;
        int i;

        /* success: status 0 means ret IS the rax (brk-class values pass
         * through untouched — a huge positive, not an errno) */
        linux_syscall_describe(&base, 158, captured);   /* the in-flight num */
        memset(&reply, 0, sizeof(reply));
        reply.num    = 158;               /* the echo */
        reply.args[0] = 0xDEAD;           /* ignored by apply — ret/status only */
        reply.ret    = 0x7F0000000000ULL;
        reply.status = 0;
        r = linux_syscall_apply_reply(&base, &reply, sizeof(reply));
        CHECK(r == 0x7F0000000000ULL,
              "status 0: the sidecar's ret IS the rax — a big positive\n"
              "      passes through un-mangled (the brk/mmap contract)");
        CHECK(base.ret == 0x7F0000000000ULL && base.status == 0 &&
              base.num == 158 && base.args[0] == 0xDEAD,
              "apply overlays the reply onto the request in place — the\n"
              "      dispatcher reads the final rax from its return value");

        /* nonzero status: the CAP code is MAPPED, never leaked (§5.7) —
         * both families, exactly as the errno-map test pins them */
        {   struct { int32_t status; int32_t want; } map[] = {
                { CAP_EINVAL,    -LINUX_EINVAL },
                { CAP_EAGAIN,    -LINUX_EAGAIN },
                { CAP_ENOSYS,    -LINUX_ENOSYS },
                { CAP_ERR_NOTFOUND, -LINUX_ENOENT },   /* the Phase-5 family */
                { CAP_ERR_TIMEOUT,  -LINUX_ETIMEDOUT },
                { CAP_NONE,      -LINUX_EIO },         /* the sentinel:\n translated, never leaked */
                { 0x7FFF,        -LINUX_EIO },         /* out of both families */
            };
            size_t mi;
            for (mi = 0; mi < sizeof(map) / sizeof(map[0]); mi++) {
                linux_syscall_describe(&base, 1, NULL);
                memset(&reply, 0, sizeof(reply));
                reply.num = 1;
                reply.status = map[mi].status;
                r = linux_syscall_apply_reply(&base, &reply, sizeof(reply));
                CHECK(r == (uint64_t)(int64_t)map[mi].want,
                      "status maps through the errno table, never crosses\n"
                      "      as itself");
            }
        }

        /* refusals: an answer that is not provably THIS request's answer */
        linux_syscall_describe(&base, 9, NULL);
        memset(&reply, 0, sizeof(reply));
        reply.num = 9;
        CHECK(linux_syscall_apply_reply(&base, &reply, sizeof(reply) - 1) ==
                  (uint64_t)(int64_t)(-LINUX_EPROTO) &&
              linux_syscall_apply_reply(&base, &reply, 0) ==
                  (uint64_t)(int64_t)(-LINUX_EPROTO) &&
              linux_syscall_apply_reply(&base, &reply, sizeof(reply) + 1) ==
                  (uint64_t)(int64_t)(-LINUX_EPROTO),
              "a payload that is not exactly 72 bytes is not a reply:\n"
              "      -EPROTO, before anything in it is trusted");
        reply.num = 10;   /* a DIFFERENT syscall's answer */
        CHECK(linux_syscall_apply_reply(&base, &reply, sizeof(reply)) ==
                  (uint64_t)(int64_t)(-LINUX_EPROTO),
              "a num echo mismatch is a misrouted or malformed reply:\n"
              "      -EPROTO — another syscall's answer must never produce\n"
              "      this syscall's rax");
        reply.num = 9;
        for (i = 0; i < 2; i++) {
            void *null_arg = (i == 0) ? (void *)0 : (void *)&base;
            void *null_req = (i == 0) ? (void *)&reply : (void *)0;
            CHECK(linux_syscall_apply_reply(
                      (struct SLSLinuxSyscallRequest *)null_req, null_arg,
                      sizeof(reply)) == (uint64_t)(int64_t)(-LINUX_EPROTO),
                  "a NULL request or NULL payload is refused, defined");
        }
    }

    /* ═══ E7 §5.3 (the plan's increment 4): arch_prctl's pure bounds ═══════
     * The one Linux semantic the KERNEL owns, asserted where its siblings
     * live — without a boot: the canonicality bound every wrmsr of an FS
     * base is filtered by (the grant says WHETHER, this says whether the
     * VALUE can be written), and the stage/apply predicate both ends of the
     * forward seam share, so they cannot disagree about what got staged.
     * The census's own measured call — arch_prctl(ARCH_SET_FS, 0x47a1a8)
     * (tools/linux_syscall_census.txt) — must pass its own gate, or the
     * first user's first hundred syscalls die at a check we wrote. ══════ */

    CHECK(LINUX_ARCH_PRCTL == 158 && LINUX_ARCH_SET_FS == 0x1002u &&
              LINUX_ARCH_SET_GS == 0x1001u && LINUX_ARCH_GET_FS == 0x1003u,
          "arch_prctl's numbers are the Linux ABI's: 158, ARCH_SET_FS 0x1002,\n"
          "      SET_GS 0x1001, GET_FS 0x1003 — literals where the branch, the\n"
          "      stage and the sidecar's mirror each read them");
    CHECK(linux_syscall_class(LINUX_ARCH_PRCTL) == LSC_MSR,
          "158's census class is LSC_MSR — \"this one's semantics live in the\n"
          "      kernel\" — not UNKNOWN (§5.6's row, the strata D1 leans on)");
    CHECK(linux_fs_addr_is_user_canonical(0x47a1a8) == 1,
          "the census's measured FS base (0x47a1a8) is canonical — the one\n"
          "      call the first user actually makes passes its own gate");
    CHECK(linux_fs_addr_is_user_canonical(0) == 1,
          "0 is canonical — refusing it would invent a rule no Linux kernel\n"
          "      has (the degenerate zero-TLS case is legal, just useless)");
    CHECK(linux_fs_addr_is_user_canonical(0x0000400000000000ULL) == 1,
          "USER_PROC_CODE_BASE (bits 63..47 all zero) is canonical — the\n"
          "      probe's own SET_FS target passes");
    CHECK(linux_fs_addr_is_user_canonical(0x0000800000000000ULL) == 0 &&
              linux_fs_addr_is_user_canonical(0x00007FFFFFFFFFFFULL + 1) == 1 - 1,
          "bit 47 set — one past the user maximum — is NOT a user-canonical\n"
          "      address: the wrmsr refuses it even if the grant waved it through");
    CHECK(linux_fs_addr_is_user_canonical(0xFFFF800000000000ULL) == 0 &&
              linux_fs_addr_is_user_canonical(0xFFFFFFFFFFFFFFFFULL) == 0,
          "a kernel-canonical value and the all-ones value are both refused —\n"
          "      every-bit-set above all: an unvalidated wrmsr of -1 is how a\n"
          "      mechanism becomes a story about one garbage MSR");
    CHECK(linux_arch_prctl_is_set_fs(LINUX_ARCH_PRCTL, LINUX_ARCH_SET_FS) == 1,
          "158+0x1002 IS the stage/apply pair — ONE predicate at both ends of\n"
          "      the seam, so stage and apply can never disagree");
    CHECK(linux_arch_prctl_is_set_fs(LINUX_ARCH_PRCTL, LINUX_ARCH_SET_GS) == 0 &&
              linux_arch_prctl_is_set_fs(LINUX_ARCH_PRCTL, LINUX_ARCH_GET_FS) == 0,
          "SET_GS / GET_FS are NOT the granted pair (no GSR in this kernel;\n"
          "      GET_FS's value lives in the kernel's field — neither in census)");
    CHECK(linux_arch_prctl_is_set_fs(218, LINUX_ARCH_SET_FS) == 0 &&
              linux_arch_prctl_is_set_fs(LINUX_ARCH_PRCTL, 0xDEAD) == 0,
          "a cmd-shaped argument under another number, or a cmd word under no\n"
          "      real cmd, never stages a write — the predicate needs BOTH words");

    printf("linux_personality_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
