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

    /* ═══ E7 increment 5 (the fd class): the kernel copy ═════════════════
     * The byte crossing §5.6's read/write row names — "the copy's bulk
     * plus the console; sidecar fd table" — asserted where its siblings
     * live: the PLAN (pure arithmetic over the captured registers: how
     * many bytes cross, from where, or why not) and the COPY (a read-only
     * walk of a page-table root this test itself builds — the security
     * argument of §3.5 is that the copy can read only what ring-3 could
     * read itself, so every flag refusal below is a boundary, not a
     * nicety, and none of them can page-fault in ring 0 over a tenant's
     * pointer). ═══════════════════════════════════════════════════════ */

    {   /* the plan: total, bounded, honest about count == 0 */
        struct LinuxFdWritePlan p;
        uint64_t wargs[6];

        memset(&wargs, 0, sizeof wargs);
        p = linux_fd_write_plan(40, wargs);   /* lseek, not write */
        CHECK(p.src == 0 && p.len == 0 && p.refuse == 0,
              "a non-write number plans nothing: no bytes, no refusal —\n"
              "      the rest of the fd class still forwards as the plain\n"
              "      72-byte description it has always been");

        wargs[0] = 1;
        wargs[1] = 0x0000400000001000ULL;
        wargs[2] = 5;
        p = linux_fd_write_plan(1, wargs);
        CHECK(p.refuse == 0 && p.len == 5 && p.src == wargs[1],
              "count 5 plans exactly 5 bytes, from the buffer address");

        wargs[1] = 0;
        wargs[2] = 0;
        p = linux_fd_write_plan(1, wargs);
        CHECK(p.refuse == 0 && p.len == 0,
              "count 0 plans nothing BEFORE canonicality — write(fd, NULL,\n"
              "      0) is legal, and inventing -EFAULT there would be a\n"
              "      rule no kernel the candidate was compiled against\n"
              "      enforces (the §5.3 bound's own discipline)");

        wargs[1] = 0x0000400000001000ULL;
        wargs[2] = 1ULL << 30;
        p = linux_fd_write_plan(1, wargs);
        CHECK(p.refuse == 0 && p.len == LINUX_FD_WRITE_CHUNK,
              "a huge count clamps to the chunk — a short write is\n"
              "      POSIX-legal, and the clamp rides IN args[2] so header,\n"
              "      payload and the count the task will see are one truth\n"
              "      (the guard's M clause pins the chunk against cap.h)");

        wargs[1] = 0x0000800000000000ULL;   /* kernel-canonical */
        wargs[2] = 8;
        p = linux_fd_write_plan(1, wargs);
        CHECK(p.refuse == -LINUX_EFAULT,
              "a non-canonical buffer refuses -EFAULT at the PLAN — before\n"
              "      the post, with no channel and no arm (first-trap\n"
              "      refusal, §3.3)");

        wargs[1] = 0x00007FFFFFFFFFFFULL - 2;
        wargs[2] = 16;
        p = linux_fd_write_plan(1, wargs);
        CHECK(p.refuse == -LINUX_EFAULT,
              "a span running off the top of the user range refuses\n"
              "      -EFAULT — the end check is (span_end - 1) > MAX, so\n"
              "      one byte past the boundary is caught, not rounded");

        CHECK(LINUX_EFAULT == 14,
              "EFAULT is 14 — Linux's own number, returned by the plan and\n"
              "      copy paths DIRECTLY: the task's truth about the task's\n"
              "      own pointer never crosses §5.7's CAP map");
    }

    {   /* the copy: fake page tables, 4 KiB-aligned like the real ones.
         * Host addresses stand in for physical frames — the walker masks
         * them with the same frame mask the identity map implies, and
         * aligned(4096) keeps every flag bit clear in the pointer itself. */
        static uint64_t f_pml4[512] __attribute__((aligned(4096)));
        static uint64_t f_pdpt[512] __attribute__((aligned(4096)));
        static uint64_t f_pd[512]   __attribute__((aligned(4096)));
        static uint64_t f_pt[512]   __attribute__((aligned(4096)));
        static uint8_t  f_page0[4096] __attribute__((aligned(4096)));
        static uint8_t  f_page1[4096] __attribute__((aligned(4096)));
        const uint64_t P = 1ULL, U = 4ULL, PS = 0x80ULL;
        const uint64_t VA = 0x0000000123456000ULL;
        unsigned i4 = (unsigned)((VA >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((VA >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((VA >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((VA >> 12) & 0x1FF);
        uint64_t root = (uint64_t)(uintptr_t)f_pml4;
        uint8_t out[32];
        int r;
        unsigned k;

        memset(f_pml4, 0, sizeof f_pml4);
        memset(f_pdpt, 0, sizeof f_pdpt);
        memset(f_pd, 0, sizeof f_pd);
        memset(f_pt, 0, sizeof f_pt);
        for (k = 0; k < 4096; k++) { f_page0[k] = (uint8_t)(0xA0 ^ k);
                                     f_page1[k] = (uint8_t)(0x5A ^ k); }
        f_pml4[i4] = (uint64_t)(uintptr_t)f_pdpt | P | U;
        f_pdpt[i3] = (uint64_t)(uintptr_t)f_pd   | P | U;
        f_pd[i2]   = (uint64_t)(uintptr_t)f_pt   | P | U;
        f_pt[i1]   = (uint64_t)(uintptr_t)f_page0 | P | U;
        f_pt[i1 + 1] = (uint64_t)(uintptr_t)f_page1 | P | U;

        r = linux_user_copy_out(out, root, VA + 0x40, 8);
        CHECK(r == 0 && memcmp(out, f_page0 + 0x40, 8) == 0,
              "the copy reads through a present+user chain, 4 KiB frames,\n"
              "      identity frames — the bytes the task wrote are the\n"
              "      bytes the sidecar will see");

        CHECK(linux_user_copy_out(out, 0, 0, 0) == 0,
              "n == 0 succeeds without reading anything (root may be 0) —\n"
              "      the walker dereferences only inside the loop");

        f_pml4[i4] = 0;
        r = linux_user_copy_out(out, root, VA + 0x40, 8);
        f_pml4[i4] = (uint64_t)(uintptr_t)f_pdpt | P | U;
        CHECK(r == -LINUX_EFAULT,
              "a missing PML4 entry refuses -EFAULT — an unmapped span is\n"
              "      an errno into rax, never a page fault in ring 0");

        f_pdpt[i3] = (uint64_t)(uintptr_t)f_pd | P;   /* supervisor mid-level */
        r = linux_user_copy_out(out, root, VA + 0x40, 8);
        f_pdpt[i3] = (uint64_t)(uintptr_t)f_pd | P | U;
        CHECK(r == -LINUX_EFAULT,
              "a supervisor bit at any MID level refuses — the copy reads\n"
              "      exactly what ring 3 could read, which is the whole\n"
              "      security argument for letting the sidecar see it");

        f_pt[i1] = (uint64_t)(uintptr_t)f_page0 | P;   /* supervisor leaf  */
        r = linux_user_copy_out(out, root, VA + 0x40, 8);
        f_pt[i1] = (uint64_t)(uintptr_t)f_page0 | P | U;
        CHECK(r == -LINUX_EFAULT,
              "a supervisor leaf refuses — a kernel page smuggled into the\n"
              "      child's span cannot reach the sidecar's console");

        f_pt[i1] = 0;                                 /* leaf not present */
        r = linux_user_copy_out(out, root, VA + 0x40, 8);
        f_pt[i1] = (uint64_t)(uintptr_t)f_page0 | P | U;
        CHECK(r == -LINUX_EFAULT,
              "an absent leaf refuses — the page the task just unmapped\n"
              "      mid-write is an errno, not a fault");

        f_pd[i2] = (uint64_t)(uintptr_t)f_pt | P | U | PS;   /* 2 MiB page */
        r = linux_user_copy_out(out, root, VA + 0x40, 8);
        f_pd[i2] = (uint64_t)(uintptr_t)f_pt | P | U;
        CHECK(r == -LINUX_EFAULT,
              "a huge page (PS at the PDE) refuses by name — not this\n"
              "      shim's user shape, and a 2 MiB stride through a 4 KiB\n"
              "      frame assumption is how a copy reads the wrong bytes");

        r = linux_user_copy_out(out, root, VA + 0xFF8, 16);
        CHECK(r == 0 && memcmp(out, f_page0 + 0xFF8, 8) == 0 &&
              memcmp(out + 8, f_page1, 8) == 0,
              "a span crossing a page boundary copies BOTH pages in order —\n"
              "      each page is re-walked from the PML4, leaf frame never\n"
              "      mistaken for a root");
    }

    /* ═══ E7 increment 6 (the mem class, §5.4): the budget span ══════════
     * brk's row — "brk as a bump pointer inside a reserved span" — and
     * mprotect's row — the W|X refusal H2's Node exclusion IS. The
     * verdict is the re-check the resume arm performs AT the reply (the
     * §5.3 posture one level up: a page-table write instead of an MSR
     * write), and every branch below is a way a grant can be a LIE the
     * kernel must refuse without mapping a single page. The frame reader
     * is the unwind's read-back, over the same fake tables the copy's
     * cases already build. ═══════════════════════════════════════════ */

    CHECK(LINUX_BRK == 12 && LINUX_MPROTECT == 10 &&
              LINUX_MMAP == 9 && LINUX_MUNMAP == 11,
          "the mem numbers are the Linux ABI's — the census's own rows: "
          "      mmap 9, mprotect 10, munmap 11, brk 12");
    CHECK(linux_syscall_class(LINUX_BRK) == LSC_MEM &&
              linux_syscall_class(LINUX_MPROTECT) == LSC_MEM,
          "both landed rows are LSC_MEM — their semantics live in §5.4's "
          "      budget policy, not in a number chain");
    CHECK(LINUX_PROT_WRITE == 0x2u && LINUX_PROT_EXEC == 0x4u,
          "the PROT bits are Linux mman.h's ABI: WRITE 0x2, EXEC 0x4 — "
          "      the literals the sidecar's twin repeats");

    /* ─── mprotect's W|X refusal, as a total predicate ───────────────── */
    CHECK(linux_mprotect_refuses_wx(0x6) == 1 &&
              linux_mprotect_refuses_wx(0x7) == 1,
          "W|X (0x6) and RWX (0x7) are refused — the shim's own rule "
          "      (§5.4), the refusal H2's Node exclusion rests on, decided "
          "      from the prot word alone (no object, so the first trap "
          "      can say it)");
    CHECK(linux_mprotect_refuses_wx(0x0) == 0 &&
              linux_mprotect_refuses_wx(0x1) == 0 &&
              linux_mprotect_refuses_wx(0x2) == 0 &&
              linux_mprotect_refuses_wx(0x4) == 0 &&
              linux_mprotect_refuses_wx(0x5) == 0 &&
              linux_mprotect_refuses_wx(~0x6ULL) == 0,
          "anything without BOTH write and execute is not refused by this "
          "      predicate — R|X stays possible; the rest is the sidecar's "
          "      ENOSYS, not a fake grant");

    /* ─── the brk verdict: the re-check at the reply ──────────────────── */
    {   const uint64_t BASE  = 0x0000400000100000ULL;
        const uint64_t LIMIT = BASE + 0x10000;   /* 64 KiB, page-rounded */
        struct LinuxMemBrkVerdict v;

        /* the denial echo: over-budget and shrink asks both arrive as
         * rax == the current break (Linux's brk failure shape) */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE + 0x2000,
                                  BASE + 0x20000, BASE + 0x2000);
        CHECK(v.malformed == 0 && v.map_pages == 0 &&
                  v.map_va == 0 && v.new_cur == BASE + 0x2000,
              "the denial echo (rax == the current break) is NOT "
              "      malformed: the break stays, nothing maps — the "
              "      over-budget ask's honest answer");

        /* a no-op ask: brk(cur) is granted as itself, mapping nothing */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE + 0x2000,
                                  BASE + 0x2000, BASE + 0x2000);
        CHECK(v.malformed == 0 && v.map_pages == 0 &&
                  v.new_cur == BASE + 0x2000,
              "a no-op ask (req == cur) maps nothing and is not "
              "      malformed — brk(current) is a legal no-op");

        /* the grant: two pages, from align_up(cur) — the base is
         * page-aligned, so the range starts exactly at it */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE, BASE + 0x2000,
                                  BASE + 0x2000);
        CHECK(v.malformed == 0 && v.map_va == BASE &&
                  v.map_pages == 2 && v.new_cur == BASE + 0x2000,
              "a granted growth maps exactly the pages between the old "
              "      and new breaks — two for a 2-page ask");

        /* a grant to exactly the limit is legal (the span is inclusive) */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE, LIMIT, LIMIT);
        CHECK(v.malformed == 0 && v.map_va == BASE &&
                  v.map_pages == 0x10000 / 4096 && v.new_cur == LIMIT,
              "a grant to exactly the span's limit is inside the span — "
              "      the whole budget is the task's to take");

        /* an UNALIGNED grant maps ceil(new) pages — and only those */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE,
                                  BASE + 0x1001, BASE + 0x1001);
        CHECK(v.malformed == 0 && v.map_va == BASE &&
                  v.map_pages == 2 && v.new_cur == BASE + 0x1001,
              "an unaligned grant maps the pages it TOUCHES (ceil), not "
              "      the bytes it asked for — brk is byte-granular, "
              "      mappings are not");

        /* an unaligned current break: the page CONTAINING cur is already
         * mapped, so the next grant starts at align_up(cur) — a re-map
         * would leak the frame underneath and zero the bytes on it */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE + 0x1500,
                                  BASE + 0x2500, BASE + 0x2500);
        CHECK(v.malformed == 0 && v.map_va == BASE + 0x2000 &&
                  v.map_pages == 1 && v.new_cur == BASE + 0x2500,
              "after an unaligned break, the next grant maps only the "
              "      pages ABOVE the one the break already sits in — the "
              "      live page is never re-mapped");

        /* a grant whose reply escaped the span: the kernel's own bound,
         * whatever the policy side believed */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE,
                                  LIMIT + 8, LIMIT + 8);
        CHECK(v.malformed == 1 && v.map_pages == 0 &&
                  v.new_cur == BASE,
              "a grant past the stamped limit is malformed — the span is "
              "      the kernel's truth, not the grant's");

        /* a "granted" shrink: rax echoes a request below the current
         * break — v1 grows only, and nothing may ever unmap */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE + 0x2000,
                                  BASE + 0x1000, BASE + 0x1000);
        CHECK(v.malformed == 1 && v.map_pages == 0 &&
                  v.new_cur == BASE + 0x2000,
              "a granted SHRINK is malformed: the break never moves "
              "      backward and no page is ever unmapped by a grant");

        /* a reply that is neither the grant's echo nor the refusal's */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE + 0x2000,
                                  BASE + 0x3000, BASE + 0x9000);
        CHECK(v.malformed == 1 && v.map_pages == 0 &&
                  v.new_cur == BASE + 0x2000,
              "an rax that is neither the request's echo nor the current "
              "      break is a malformed grant — nothing maps, the break "
              "      stays, the caller logs it");

        /* a grant below the span's base (a bogus low request) */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT, BASE, BASE - 0x1000,
                                  BASE - 0x1000);
        CHECK(v.malformed == 1 && v.map_pages == 0,
              "a grant below the stamped base is malformed — the span "
              "      bounds the break from BOTH ends");

        /* E7 increment 16: the ceiling is the span's limit CAPPED by
         * the anonymous-map cursor — an arena a granted mmap backed is
         * a page a brk growth may never map over (a re-map leaks the
         * frame and zeros live bytes). The sidecar's ledger answers
         * the DENIAL ECHO (rax == the current break) for such an ask:
         * not malformed, nothing moves. A reply that granted it
         * ANYWAY is a lie the verdict refuses. */
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT - 0x1000,
                                  BASE, LIMIT, BASE);
        CHECK(v.malformed == 0 && v.map_pages == 0 &&
                  v.new_cur == BASE,
              "a brk growth past the mmap cursor answers the denial "
              "      echo — the arena's page is not brk's to map");
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT - 0x1000,
                                  BASE, LIMIT, LIMIT);
        CHECK(v.malformed == 1 && v.map_pages == 0 &&
                  v.new_cur == BASE,
              "a reply that granted a brk growth past the mmap cursor "
              "      anyway is malformed — the capped span is the "
              "      kernel's truth, not the grant's");
        v = linux_mem_brk_verdict(BASE, LIMIT, LIMIT - 0x1000,
                                  BASE, LIMIT - 0x1000, LIMIT - 0x1000);
        CHECK(v.malformed == 0 && v.map_va == BASE &&
                  v.map_pages == (0x10000 - 0x1000) / 4096 &&
                  v.new_cur == LIMIT - 0x1000,
              "a brk growth to exactly the mmap cursor is inside the "
              "      capped span — the arena's first page is the "
              "      cursor, and brk may reach it");
        /* a zero cursor (a task stamped without one) leaves the span's
         * own limit standing: the cap is min(), never a surprise 0 */
        v = linux_mem_brk_verdict(BASE, LIMIT, 0, BASE, LIMIT, LIMIT);
        CHECK(v.malformed == 0 && v.new_cur == LIMIT,
              "mmap_top == 0 (no cursor stamped) leaves the span's own "
              "      limit as brk's ceiling");
    }

    /* ─── the mmap seam (E7 increment 16, §5.4): first-trap ladder ─── */
    {   struct LinuxMmapReq req;
        const uint64_t BASE  = 0x0000400000100000ULL;
        const uint64_t LIMIT = BASE + 0x10000;

        /* the measured allocator's guard page: PROT_NONE at the span's
         * base, FIXED|ANON|PRIVATE — refused by nothing */
        req.addr = BASE; req.len = 4096; req.prot = 0;
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == 0,
              "the guard shape (PROT_NONE FIXED at the base) passes the "
              "      first trap — it is the request this increment exists "
              "      to serve");
        /* the RW arena: NULL, RW, ANON|PRIVATE — refused by nothing */
        req.addr = 0; req.prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == 0,
              "the arena shape (RW anonymous private) passes the first "
              "      trap");
        /* the hints are accepted and ignored (documented no-ops) */
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_STACK;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == 0,
              "MAP_STACK — a documented no-op hint on x86-64 — passes: "
              "      ignoring it IS Linux's behavior, not a shortcut");
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_NORESERVE;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == 0,
              "MAP_NORESERVE passes for the same reason");
        /* W|X: EPERM — mprotect's §5.4 rule over a prot word */
        req.prot = LINUX_PROT_WRITE | LINUX_PROT_EXEC;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_EPERM,
              "W|X is refused EPERM at the first trap — mprotect's own "
              "      §5.4 rule over a prot word, twin at both ends");
        /* PROT_EXEC alone: ENOSYS by name */
        req.prot = LINUX_PROT_EXEC;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_ENOSYS,
              "executable anonymous memory is refused ENOSYS by name — "
              "      its own increment, never a fake grant");
        /* an unknown prot bit */
        req.prot = 0x8;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_EINVAL,
              "an unknown prot bit is EINVAL");
        /* zero length */
        req.prot = LINUX_PROT_READ; req.len = 0;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_EINVAL,
              "a zero length is EINVAL — Linux's own number");
        /* file-backed, shared, unknown flag bits: ENOSYS by name */
        req.len = 4096; req.flags = LINUX_MAP_PRIVATE;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_ENOSYS,
              "a file-backed map (no ANONYMOUS) is refused ENOSYS by "
              "      name — the file class's own half of the path plan");
        req.flags = LINUX_MAP_ANONYMOUS | LINUX_MAP_SHARED;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_ENOSYS,
              "a shared map is refused ENOSYS by name");
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | 0x40;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_ENOSYS,
              "an unknown flag bit is refused ENOSYS by name — the "
              "      allowlist is closed");
        /* FIXED: alignment, then the span bound */
        req.addr = BASE + 1;
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_EINVAL,
              "a MAP_FIXED address that is not page-aligned is EINVAL");
        req.addr = BASE - 0x1000;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_ENOMEM,
              "a MAP_FIXED address below the stamped span is ENOMEM — "
              "      the budget shape §5.4 names");
        req.addr = LIMIT;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == -LINUX_ENOMEM,
              "a MAP_FIXED range that starts at the limit is ENOMEM");
        req.addr = LIMIT - 0x1000;
        CHECK(linux_mem_mmap_refusal(&req, BASE, LIMIT) == 0,
              "a MAP_FIXED page exactly at the span's top is inside the "
              "      span — the whole budget is the task's to take");
    }

    /* ─── the mmap verdict: the re-check at the reply ─────────────── */
    {   const uint64_t BASE  = 0x0000400000100000ULL;
        const uint64_t LIMIT = BASE + 0x10000;
        struct LinuxMmapReq req;
        struct LinuxMemMmapVerdict v;

        /* the refusal reply: not malformed, nothing maps — its errno
         * rides the §5.7 map as usual */
        req.addr = 0; req.len = 4096;
        req.prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS;
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT, &req,
                                   (uint64_t)(int64_t)(-LINUX_ENOSYS));
        CHECK(v.malformed == 0 && v.range_pages == 0 && v.map_pages == 0 &&
                  v.new_mmap_top == LIMIT,
              "a refusal reply maps nothing and is not malformed — the "
              "      errno rides the map, the cursor stands");

        /* the granted arena: the cursor's pick, in range — one page
         * backed, the cursor descends past it */
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT, &req,
                                   LIMIT - 0x1000);
        CHECK(v.malformed == 0 && v.replace == 0 &&
                  v.map_va == LIMIT - 0x1000 &&
                  v.range_pages == 1 && v.map_pages == 1 &&
                  v.prot_write == 1 &&
                  v.new_mmap_top == LIMIT - 0x1000,
              "a granted arena maps exactly the pages the cursor gave "
              "      and descends the kernel's own cursor beside it");

        /* PROT_NONE: the reservation maps no frames */
        req.prot = LINUX_PROT_NONE;
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT - 0x1000,
                                   &req, LIMIT - 0x2000);
        CHECK(v.malformed == 0 && v.range_pages == 1 && v.map_pages == 0 &&
                  v.prot_write == 0,
              "a PROT_NONE grant backs no frames — a reservation whose "
              "      every access faults is its whole contract");
        req.prot = LINUX_PROT_READ | LINUX_PROT_WRITE;

        /* the forged grants: each is a lie the kernel must refuse */
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT, &req, 0);
        CHECK(v.malformed == 1 && v.range_pages == 0,
              "a grant of address 0 is malformed — Linux never hands it");
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT, &req,
                                   LIMIT - 0x800);   /* unaligned */
        CHECK(v.malformed == 1 && v.range_pages == 0,
              "a non-FIXED grant that is not page-aligned is malformed");
        v = linux_mem_mmap_verdict(BASE, LIMIT, LIMIT - 0x1000,
                                   LIMIT, &req, LIMIT - 0x2000);
        CHECK(v.malformed == 1 && v.range_pages == 0,
              "a non-FIXED grant BELOW the break is malformed — it "
              "      would land under brk's own pages");
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT - 0x1000,
                                   &req, LIMIT - 0x1000);
        CHECK(v.malformed == 1 && v.range_pages == 0,
              "a non-FIXED grant AT the cursor (not below it) is "
              "      malformed — the cursor is where the NEXT grant "
              "      starts, never inside a handed-out range");

        /* FIXED: the echo rule on an address, and the span re-checked
         * against the kernel's OWN stamp */
        req.addr = BASE; req.len = 4096; req.prot = LINUX_PROT_NONE;
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED;
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE + 0x2000, LIMIT,
                                   &req, BASE);
        CHECK(v.malformed == 0 && v.replace == 1 &&
                  v.map_va == BASE && v.range_pages == 1 &&
                  v.map_pages == 0 && v.new_mmap_top == LIMIT,
              "the guard echo: FIXED PROT_NONE at the brk heap's base "
              "      replaces one page, backs none, moves no cursor — "
              "      the measured allocator's guard, as Linux maps it");
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT, &req,
                                   BASE + 0x1000);   /* wrong echo */
        CHECK(v.malformed == 1 && v.range_pages == 0 && v.replace == 0,
              "a FIXED grant that echoes a different address is "
              "      malformed — the one-truth rule over an address");
        req.addr = LIMIT + 0x1000;
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT, &req,
                                   LIMIT + 0x1000);
        CHECK(v.malformed == 1 && v.range_pages == 0,
              "a FIXED grant outside the kernel's OWN span is "
              "      malformed, whatever the policy side believed");

        /* the verdict is total: the refusal ladder re-runs over the
         * staged words, so a shape the first trap would have refused
         * maps nothing here either */
        req.addr = 0; req.len = 4096;
        req.prot = LINUX_PROT_WRITE | LINUX_PROT_EXEC;
        req.flags = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS;
        v = linux_mem_mmap_verdict(BASE, LIMIT, BASE, LIMIT, &req,
                                   LIMIT - 0x1000);
        CHECK(v.malformed == 1 && v.range_pages == 0,
              "a W|X request reaching the verdict maps nothing — the "
              "      ladder is re-run, the verdict is total on any input");
    }

    /* ─── the frame reader: the unwind's read-back ────────────────────── */
    {   static uint64_t p_pml4[512] __attribute__((aligned(4096)));
        static uint64_t p_pdpt[512] __attribute__((aligned(4096)));
        static uint64_t p_pd[512]   __attribute__((aligned(4096)));
        static uint64_t p_pt[512]   __attribute__((aligned(4096)));
        static uint8_t  p_page[4096] __attribute__((aligned(4096)));
        const uint64_t P = 1ULL, U = 4ULL, PS = 0x80ULL;
        const uint64_t VA = 0x0000400000102000ULL;  /* the heap's own range */
        unsigned i4 = (unsigned)((VA >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((VA >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((VA >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((VA >> 12) & 0x1FF);
        uint64_t root = (uint64_t)(uintptr_t)p_pml4;

        memset(p_pml4, 0, sizeof p_pml4);
        memset(p_pdpt, 0, sizeof p_pdpt);
        memset(p_pd, 0, sizeof p_pd);
        memset(p_pt, 0, sizeof p_pt);
        p_pml4[i4] = (uint64_t)(uintptr_t)p_pdpt | P | U;
        p_pdpt[i3] = (uint64_t)(uintptr_t)p_pd   | P | U;
        p_pd[i2]   = (uint64_t)(uintptr_t)p_pt   | P | U;
        p_pt[i1]   = (uint64_t)(uintptr_t)p_page | P | U;

        CHECK(linux_user_page_frame(root, VA) ==
                  ((uint64_t)(uintptr_t)p_page & 0x000FFFFFFFFFF000ULL),
              "the reader returns the leaf frame behind a user mapping — "
              "      the address the unwind returns to the partition");

        p_pt[i1] = (uint64_t)(uintptr_t)p_page | P;   /* supervisor leaf */
        CHECK(linux_user_page_frame(root, VA) == 0,
              "a supervisor leaf reads as 0 — the unwind can never free "
              "      a frame the copy's boundaries would refuse");
        p_pt[i1] = (uint64_t)(uintptr_t)p_page | P | U;

        p_pt[i1] = 0;                                 /* absent leaf */
        CHECK(linux_user_page_frame(root, VA) == 0,
              "an absent leaf reads as 0 — nothing mapped, nothing freed");
        p_pt[i1] = (uint64_t)(uintptr_t)p_page | P | U;

        p_pd[i2] = (uint64_t)(uintptr_t)p_pt | P | U | PS;   /* 2 MiB */
        CHECK(linux_user_page_frame(root, VA) == 0,
              "a huge page reads as 0 by name — not this shim's user "
              "      shape, exactly as the copy refuses it");
    }

    /* ─── the sig copy plan: the signal seam's byte crossing (§10.13 step 2) ──
     * The mechanism half of the split: the KERNEL decides WHAT crosses
     * (act/set bytes out, both first-trap refusals) and the SIDECAR
     * decides WHAT IT MEANS. Every refusal is a NEGATIVE Linux errno
     * decided before any channel post — the first-trap discipline. */
    {
        uint64_t a[6];
        struct LinuxSigCopyPlan p;

        memset(a, 0, sizeof a);
        a[0] = 6;                       /* SIGABRT, the firstuser's */
        a[1] = 0x00007FFFFFFFE000ULL;   /* act on the child's stack */
        a[3] = LINUX_SIGSET_BYTES;
        p = linux_sig_copy_plan(LINUX_RT_SIGACTION, a);
        CHECK(p.refuse == 0 && p.len == LINUX_SIGACT_BYTES &&
              p.src == a[1],
              "a well-formed rt_sigaction plans its 32 act bytes out of "
              "      the child with no refusal — the crossing SIGABRT "
              "      delivery needs");

        a[1] = 0x00007FFFFFFFE100ULL;
        p = linux_sig_copy_plan(LINUX_RT_SIGPROCMASK, a);
        CHECK(p.refuse == 0 && p.len == LINUX_SIGSET_BYTES &&
              p.src == a[1],
              "a well-formed rt_sigprocmask plans its 8 set bytes — the "
              "      mask abort(3) sequences through before raise(3)");

        a[3] = 4;   /* a width the kernel-ABI sigset does not have */
        p = linux_sig_copy_plan(LINUX_RT_SIGACTION, a);
        CHECK(p.refuse == -LINUX_EINVAL && p.len == 0,
              "a sigsetsize other than 8 refuses -EINVAL at the first "
              "      trap — a caller claiming another width would get its "
              "      bytes silently reinterpreted");

        a[3] = LINUX_SIGSET_BYTES;
        a[2] = 0x00007FFFFFFFE200ULL;   /* oldact: increment 12 landed the
                                        * copy-IN — the sig plan's business
                                        * is unchanged by it */
        p = linux_sig_copy_plan(LINUX_RT_SIGACTION, a);
        CHECK(p.refuse == 0 && p.len == LINUX_SIGACT_BYTES &&
              p.src == a[1],
              "an oldact out-pointer no longer refuses this plan — the "
              "      act bytes still cross out exactly as before, and the "
              "      destination's span is linux_fd_reply_plan's (step 3)");

        p = linux_sig_copy_plan(LINUX_RT_SIGPROCMASK, a);
        CHECK(p.refuse == 0 && p.len == LINUX_SIGSET_BYTES,
              "an oldset out-pointer likewise — the plan carries the set "
              "      bytes out, never a verdict about the reply");

        a[3] = 7;   /* BOTH defects: which refusal wins must be pinned */
        p = linux_sig_copy_plan(LINUX_RT_SIGACTION, a);
        CHECK(p.refuse == -LINUX_EINVAL,
              "a foreign sigsetsize still outranks everything — the "
              "      width check runs first, deterministically");

        a[1] = 0;
        a[2] = 0;
        a[3] = LINUX_SIGSET_BYTES;
        p = linux_sig_copy_plan(LINUX_RT_SIGPROCMASK, a);
        CHECK(p.refuse == 0 && p.len == 0 && p.src == 0,
              "a NULL set with no out-pointer plans nothing and refuses "
              "      nothing — the query-shape no-op the sidecar answers 0");

        a[1] = 0xFFFF800000000000ULL;   /* the kernel/non-canonical half */
        p = linux_sig_copy_plan(LINUX_RT_SIGACTION, a);
        CHECK(p.refuse == -LINUX_EFAULT,
              "a non-canonical act address refuses -EFAULT at the first "
              "      trap — ring 3 could not have that span read");

        a[1] = 0x00007FFFFFFFFFE0ULL;   /* span ends on the LAST valid
                                        * byte: MAX - 32 + 32 - 1 */
        p = linux_sig_copy_plan(LINUX_RT_SIGACTION, a);
        CHECK(p.refuse == 0 && p.len == LINUX_SIGACT_BYTES,
              "a span ending exactly on the user max's last byte is "
              "      allowed — the bound is inclusive (a >= here would "
              "      refuse the frame that holds MAX itself)");

        a[1] = 0x00007FFFFFFFFFFBULL;   /* first byte in, tail past MAX */
        p = linux_sig_copy_plan(LINUX_RT_SIGACTION, a);
        CHECK(p.refuse == -LINUX_EFAULT,
              "an act span crossing the user-canonical max refuses "
              "      -EFAULT — the bound is checked on the whole span, "
              "      not its first byte");

        a[1] = 0x00007FFFFFFFE000ULL;
        CHECK(linux_sig_copy_plan(218, a).len == 0 &&
              linux_sig_copy_plan(218, a).refuse == 0 &&
              linux_sig_copy_plan(1, a).len == 0 &&
              linux_sig_copy_plan(60, a).refuse == 0,
              "a number outside the sig class plans nothing and refuses "
              "      nothing — the plan is total over the number space");

        p = linux_sig_copy_plan(LINUX_RT_SIGACTION, NULL);
        CHECK(p.src == 0 && p.len == 0 && p.refuse == 0,
              "a NULL args array is the zero plan — total, no fault");
    }

    /* ═══ E7 increment 11 (§10.13's step-2 coda): poll (7) — the run's
     * LAST ENOSYS answer ═════════════════════════════════════════════
     * The firstuser's num=00000007 line is POLL, not munmap (11 never
     * fires before the kill — §10.13's annotation took the census row's
     * COUNT, `11 munmap 7`, for the number). Rust std's
     * lang_start_internal polls fds {0,1,2} with a ZERO timeout before
     * any signal handler exists, and an errno outside its tolerated set
     * aborts the task — which is why the run died at raise(SIGABRT).
     * The pure refusal below is the first trap's half (a WAITING poll
     * is refused before any post); the sidecar's 7 arm carries the
     * same rule as its twin and answers the zero-timeout probe 0
     * events. Every case below is a way that refusal drifts into either
     * silent lie: granting a wait whose wake can never come, or
     * refusing the probe (the wall back by name). ═════════════════ */
    CHECK(LINUX_POLL == 7,
          "poll is Linux's own number 7 (the census's poll row) — the "
          "      number §10.13's log line carried, once mislabelled munmap");
    CHECK(linux_syscall_class(LINUX_POLL) == LSC_FD,
          "poll is the fd class — it forwards to the sidecar's fd table, "
          "      never a mem-class stage");
    CHECK(linux_poll_refuses_blocking(LINUX_POLL, 0) == 0,
          "a zero-timeout poll is NOT refused — the snapshot the sidecar "
          "      answers 0 events (the firstuser's own call shape)");
    CHECK(linux_poll_refuses_blocking(LINUX_POLL, 1) == 1 &&
              linux_poll_refuses_blocking(LINUX_POLL, 5000) == 1,
          "a timed poll is refused — waiting needs console-input state "
          "      this v1 has never had (§10.13's named input class)");
    CHECK(linux_poll_refuses_blocking(LINUX_POLL,
                                     (uint64_t)(int64_t)-1) == 1,
          "an infinite poll (-1, sign-extended as the register carries "
          "      it) is refused — the same wait, with no end to it");
    CHECK(linux_poll_refuses_blocking(LINUX_POLL, 0x100000000ULL) == 0,
          "the timeout is Linux's INT: the low 32 bits decide "
          "      (0x1_00000000 truncates to a zero timeout — allowed)");
    CHECK(linux_poll_refuses_blocking(LINUX_BRK, 7) == 0 &&
              linux_poll_refuses_blocking(218, 7) == 0 &&
              linux_poll_refuses_blocking(0, (uint64_t)(int64_t)-1) == 0,
          "a number that is not poll never waits as one — the predicate "
          "      is total over the number space (brk's break, 218's echo)");

    /* ═══ E7 increment 12 (the reply payload, §10.13's step 3): the copy-IN ═
     * The reply half of the fd class's byte crossing — §10.10's named
     * gap closing. Three pure pieces, each with the branches a boot can
     * only observe once: the first-trap destination plan, the two-phase
     * copy (writable at every level, refusal mutates NOTHING), and the
     * ret-shape predicate that keeps header, payload and rax one truth.
     * Every case below is a way this seam drifts into the silent lie the
     * refusal discipline exists to prevent: bytes into a destination no
     * first trap validated, a write laundered through a read-only page,
     * a half-written span, or a ret that contradicts the number. ════ */
    CHECK(LINUX_READ == 0,
          "read is Linux's own number 0 (the census's read row) — the "
          "      count-truth number whose ret must equal its payload n");

    {   /* the reply plan: the destination's first-trap validation */
        uint64_t a[6];
        struct LinuxFdReplyPlan r;

        memset(a, 0, sizeof a);
        a[0] = 6;
        a[2] = 0x00007FFFFFFFE200ULL;   /* oldact on the child's stack */
        r = linux_fd_reply_plan(LINUX_RT_SIGACTION, a);
        CHECK(r.refuse == 0 && r.len == LINUX_SIGACT_BYTES &&
              r.dst == a[2],
              "an oldact pointer plans its 32-byte destination — the "
              "      clamp the first trap will stage and the deliver "
              "      bound the copy to");
        r = linux_fd_reply_plan(LINUX_RT_SIGPROCMASK, a);
        CHECK(r.refuse == 0 && r.len == LINUX_SIGSET_BYTES &&
              r.dst == a[2],
              "an oldset pointer plans 8 bytes — the fixed size this "
              "      number owes, decided from num alone");

        a[2] = 0;
        r = linux_fd_reply_plan(LINUX_RT_SIGACTION, a);
        CHECK(r.refuse == 0 && r.len == 0 && r.dst == 0,
              "a NULL out-pointer plans NOTHING — no stage, no refusal: "
              "      nothing is owed back, and a staged destination with "
              "      no payload would make a plain success -EPROTO");

        a[2] = 0x00007FFFFFFFE200ULL;
        r = linux_fd_reply_plan(LINUX_WRITE, a);
        CHECK(r.refuse == 0 && r.len == 0 && r.dst == 0,
              "write has no reply payload — the plan is total over the "
              "      number space; only copy-IN numbers plan a span");
        r = linux_fd_reply_plan(218, a);
        CHECK(r.len == 0 && r.refuse == 0,
              "218's echo likewise — a number that owes no bytes plans "
              "      nothing (total, like every plan here)");

        a[2] = 0xFFFF800000000000ULL;   /* non-canonical destination */
        r = linux_fd_reply_plan(LINUX_RT_SIGACTION, a);
        CHECK(r.refuse == -LINUX_EFAULT,
              "a non-canonical oldact refuses -EFAULT at the FIRST trap "
              "      — before any post, so no channel ever carries bytes "
              "      toward a destination ring 3 could not write");

        a[2] = 0x00007FFFFFFFFFE0ULL;    /* ends on MAX's last byte */
        r = linux_fd_reply_plan(LINUX_RT_SIGACTION, a);
        CHECK(r.refuse == 0 && r.len == LINUX_SIGACT_BYTES,
              "a destination ending exactly on the user max's last byte "
              "      is allowed — the inclusive bound the write plan pins"
              "");

        a[2] = 0x00007FFFFFFFFFFBULL;    /* first byte in, tail past MAX */
        r = linux_fd_reply_plan(LINUX_RT_SIGACTION, a);
        CHECK(r.refuse == -LINUX_EFAULT,
              "a destination crossing the user-canonical max refuses — "
              "      the WHOLE span is validated, not its first byte");

        r = linux_fd_reply_plan(LINUX_RT_SIGACTION, NULL);
        CHECK(r.dst == 0 && r.len == 0 && r.refuse == 0,
              "a NULL args array is the zero plan — total, no fault");
    }

    {   /* the copy-in: fake tables with the RW bit, and the two-phase
         * refusal — the security property the IN direction adds. Host
         * addresses stand in for physical frames exactly as the copy's
         * cases do; P=1 R=2 U=4 are the same own literals. */
        static uint64_t c_pml4[512] __attribute__((aligned(4096)));
        static uint64_t c_pdpt[512] __attribute__((aligned(4096)));
        static uint64_t c_pd[512]   __attribute__((aligned(4096)));
        static uint64_t c_pt[512]   __attribute__((aligned(4096)));
        static uint8_t  c_page0[4096] __attribute__((aligned(4096)));
        static uint8_t  c_page1[4096] __attribute__((aligned(4096)));
        const uint64_t P = 1ULL, R = 2ULL, U = 4ULL, PS = 0x80ULL;
        const uint64_t VA = 0x00000002468AF000ULL;
        unsigned i4 = (unsigned)((VA >> 39) & 0x1FF);
        unsigned i3 = (unsigned)((VA >> 30) & 0x1FF);
        unsigned i2 = (unsigned)((VA >> 21) & 0x1FF);
        unsigned i1 = (unsigned)((VA >> 12) & 0x1FF);
        uint64_t root = (uint64_t)(uintptr_t)c_pml4;
        uint8_t src[64];
        unsigned k;

        memset(c_pml4, 0, sizeof c_pml4);
        memset(c_pdpt, 0, sizeof c_pdpt);
        memset(c_pd, 0, sizeof c_pd);
        memset(c_pt, 0, sizeof c_pt);
        for (k = 0; k < 64; k++) src[k] = (uint8_t)(0x3C ^ k);
        c_pml4[i4] = (uint64_t)(uintptr_t)c_pdpt | P | R | U;
        c_pdpt[i3] = (uint64_t)(uintptr_t)c_pd   | P | R | U;
        c_pd[i2]   = (uint64_t)(uintptr_t)c_pt   | P | R | U;
        c_pt[i1]   = (uint64_t)(uintptr_t)c_page0 | P | R | U;
        c_pt[i1 + 1] = (uint64_t)(uintptr_t)c_page1 | P | R | U;
        memset(c_page0, 0x11, sizeof c_page0);
        memset(c_page1, 0x22, sizeof c_page1);

        CHECK(linux_user_copy_in(src, root, VA + 0x40, 32) == 0 &&
              memcmp(c_page0 + 0x40, src, 32) == 0,
              "the copy-IN writes through a present+user+WRITABLE chain —\n"
              "      the bytes the sidecar decided are the bytes the task\n"
              "      reads back from its own buffer");

        CHECK(linux_user_copy_in(src, 0, 0, 0) == 0,
              "n == 0 succeeds without touching anything (root may be 0) —\n"
              "      both phases run zero iterations");

        CHECK(linux_user_copy_in(NULL, root, VA + 0x40, 8) == -LINUX_EFAULT,
              "a NULL source refuses — the mirror of copy_out's NULL\n"
              "      destination, decided before the walk");

        /* ── refusals-before-mutation: the IN direction's new property ── */
        memset(c_page0 + 0x100, 0x77, 32);
        c_pt[i1] = (uint64_t)(uintptr_t)c_page0 | P | U;   /* read-only leaf */
        CHECK(linux_user_copy_in(src, root, VA + 0x100, 32) == -LINUX_EFAULT &&
              c_page0[0x100] == 0x77 && c_page0[0x107] == 0x77,
              "a READ-ONLY leaf refuses -EFAULT and mutates NOTHING —\n"
              "      the sidecar must never launder a write through a page\n"
              "      the child itself could not have written");
        c_pt[i1] = (uint64_t)(uintptr_t)c_page0 | P | R | U;

        c_pd[i2] = (uint64_t)(uintptr_t)c_pt | P | U;      /* RO mid-level */
        CHECK(linux_user_copy_in(src, root, VA + 0x100, 32) == -LINUX_EFAULT &&
              c_page0[0x100] == 0x77,
              "a read-only MID level refuses — effective writability is\n"
              "      the conjunction of all four entries, checked at every\n"
              "      level like the USER bit");
        c_pd[i2] = (uint64_t)(uintptr_t)c_pt | P | R | U;

        c_pd[i2] = (uint64_t)(uintptr_t)c_pt | P | R | U | PS;   /* 2 MiB */
        CHECK(linux_user_copy_in(src, root, VA + 0x100, 32) == -LINUX_EFAULT &&
              c_page0[0x100] == 0x77,
              "a huge page refuses by name, bytes untouched — the same\n"
              "      4-KiB-frame assumption the copy refuses it under");
        c_pd[i2] = (uint64_t)(uintptr_t)c_pt | P | R | U;

        c_pt[i1] = (uint64_t)(uintptr_t)c_page0 | P;       /* supervisor leaf */
        CHECK(linux_user_copy_in(src, root, VA + 0x100, 32) == -LINUX_EFAULT &&
              c_page0[0x100] == 0x77,
              "a supervisor leaf refuses, bytes untouched — the same\n"
              "      boundary copy_out reads with");
        c_pt[i1] = (uint64_t)(uintptr_t)c_page0 | P | R | U;

        c_pt[i1] = 0;                                     /* absent leaf */
        CHECK(linux_user_copy_in(src, root, VA + 0x100, 32) == -LINUX_EFAULT &&
              c_page0[0x100] == 0x77,
              "an absent leaf refuses, bytes untouched — an unmapped\n"
              "      destination is an errno, never a fault in ring 0");
        c_pt[i1] = (uint64_t)(uintptr_t)c_page0 | P | R | U;

        /* ── the two-phase shape across a page boundary: page 1 refuses,
         * so page 0 must not have been written either. This is the case
         * a single-pass "walk then write" implementation fails (it would
         * half-write the span), and the reason phase 1 exists. */
        memset(c_page0 + 0xFF0, 0x55, 16);
        c_pt[i1 + 1] = 0;   /* the SECOND page is absent */
        CHECK(linux_user_copy_in(src, root, VA + 0xFF0, 32) == -LINUX_EFAULT &&
              c_page0[0xFF0] == 0x55 && c_page0[0xFF7] == 0x55,
              "a span whose SECOND page refuses leaves the FIRST page\n"
              "      untouched — validate the whole span, THEN write: no\n"
              "      half-written reply payload ever lands");
        c_pt[i1 + 1] = (uint64_t)(uintptr_t)c_page1 | P | R | U;

        CHECK(linux_user_copy_in(src, root, VA + 0xFF0, 32) == 0 &&
              memcmp(c_page0 + 0xFF0, src, 16) == 0 &&
              memcmp(c_page1, src + 16, 16) == 0,
              "the happy cross-page span writes BOTH pages in order —\n"
              "      each page re-walked from the PML4, leaf frame never\n"
              "      mistaken for a root (copy_out's own rule)");
    }

    {   /* the ret-shape predicate: header, payload and rax one truth */
        CHECK(linux_reply_shape_ok(LINUX_RT_SIGACTION, 0, 32) == 1 &&
              linux_reply_shape_ok(LINUX_RT_SIGPROCMASK, 0, 8) == 1,
              "the sig numbers answer 0 with their payload — a success\n"
              "      the caller checks as success, never a byte count\n"
              "      musl would read as an errno");
        CHECK(linux_reply_shape_ok(LINUX_RT_SIGACTION, 32, 32) == 0 &&
              linux_reply_shape_ok(LINUX_RT_SIGPROCMASK, 8, 8) == 0,
              "ret == n on a sig number is REFUSED — that is read's rule\n"
              "      worn as a sigaction's rax would be a lie the assert\n"
              "      in the run turns into a crash");
        CHECK(linux_reply_shape_ok(LINUX_READ, 32, 32) == 1 &&
              linux_reply_shape_ok(LINUX_READ, 0, 0) == 1,
              "read echoes its count — the count-truth shape (step 4's\n"
              "      number, its rule already pinned here)");
        CHECK(linux_reply_shape_ok(LINUX_READ, 31, 32) == 0 &&
              linux_reply_shape_ok(LINUX_READ, 0, 32) == 0,
              "a read whose ret misses n by one (or answers 0) is refused\n"
              "      — payload and rax must be the same truth");
        CHECK(linux_reply_shape_ok(218, 0, 0) == 1 &&
              linux_reply_shape_ok(1, 0, 4) == 1 &&
              linux_reply_shape_ok(0xFFFF, 0, 0) == 1,
              "the predicate is total — a non-read number with a zero ret\n"
              "      is its success shape, whatever the number");
        CHECK(linux_reply_shape_ok(218, 5, 0) == 0,
              "and refused where the ret contradicts it — total means\n"
              "      every input has ONE answer, not a default that\n"
              "      guesses");
    }

    printf("linux_personality_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
