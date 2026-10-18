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
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE + 0x2000,
                                  BASE + 0x20000, BASE + 0x2000);
        CHECK(v.malformed == 0 && v.map_pages == 0 &&
                  v.map_va == 0 && v.new_cur == BASE + 0x2000,
              "the denial echo (rax == the current break) is NOT "
              "      malformed: the break stays, nothing maps — the "
              "      over-budget ask's honest answer");

        /* a no-op ask: brk(cur) is granted as itself, mapping nothing */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE + 0x2000,
                                  BASE + 0x2000, BASE + 0x2000);
        CHECK(v.malformed == 0 && v.map_pages == 0 &&
                  v.new_cur == BASE + 0x2000,
              "a no-op ask (req == cur) maps nothing and is not "
              "      malformed — brk(current) is a legal no-op");

        /* the grant: two pages, from align_up(cur) — the base is
         * page-aligned, so the range starts exactly at it */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE, BASE + 0x2000,
                                  BASE + 0x2000);
        CHECK(v.malformed == 0 && v.map_va == BASE &&
                  v.map_pages == 2 && v.new_cur == BASE + 0x2000,
              "a granted growth maps exactly the pages between the old "
              "      and new breaks — two for a 2-page ask");

        /* a grant to exactly the limit is legal (the span is inclusive) */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE, LIMIT, LIMIT);
        CHECK(v.malformed == 0 && v.map_va == BASE &&
                  v.map_pages == 0x10000 / 4096 && v.new_cur == LIMIT,
              "a grant to exactly the span's limit is inside the span — "
              "      the whole budget is the task's to take");

        /* an UNALIGNED grant maps ceil(new) pages — and only those */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE,
                                  BASE + 0x1001, BASE + 0x1001);
        CHECK(v.malformed == 0 && v.map_va == BASE &&
                  v.map_pages == 2 && v.new_cur == BASE + 0x1001,
              "an unaligned grant maps the pages it TOUCHES (ceil), not "
              "      the bytes it asked for — brk is byte-granular, "
              "      mappings are not");

        /* an unaligned current break: the page CONTAINING cur is already
         * mapped, so the next grant starts at align_up(cur) — a re-map
         * would leak the frame underneath and zero the bytes on it */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE + 0x1500,
                                  BASE + 0x2500, BASE + 0x2500);
        CHECK(v.malformed == 0 && v.map_va == BASE + 0x2000 &&
                  v.map_pages == 1 && v.new_cur == BASE + 0x2500,
              "after an unaligned break, the next grant maps only the "
              "      pages ABOVE the one the break already sits in — the "
              "      live page is never re-mapped");

        /* a grant whose reply escaped the span: the kernel's own bound,
         * whatever the policy side believed */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE,
                                  LIMIT + 8, LIMIT + 8);
        CHECK(v.malformed == 1 && v.map_pages == 0 &&
                  v.new_cur == BASE,
              "a grant past the stamped limit is malformed — the span is "
              "      the kernel's truth, not the grant's");

        /* a "granted" shrink: rax echoes a request below the current
         * break — v1 grows only, and nothing may ever unmap */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE + 0x2000,
                                  BASE + 0x1000, BASE + 0x1000);
        CHECK(v.malformed == 1 && v.map_pages == 0 &&
                  v.new_cur == BASE + 0x2000,
              "a granted SHRINK is malformed: the break never moves "
              "      backward and no page is ever unmapped by a grant");

        /* a reply that is neither the grant's echo nor the refusal's */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE + 0x2000,
                                  BASE + 0x3000, BASE + 0x9000);
        CHECK(v.malformed == 1 && v.map_pages == 0 &&
                  v.new_cur == BASE + 0x2000,
              "an rax that is neither the request's echo nor the current "
              "      break is a malformed grant — nothing maps, the break "
              "      stays, the caller logs it");

        /* a grant below the span's base (a bogus low request) */
        v = linux_mem_brk_verdict(BASE, LIMIT, BASE, BASE - 0x1000,
                                  BASE - 0x1000);
        CHECK(v.malformed == 1 && v.map_pages == 0,
              "a grant below the stamped base is malformed — the span "
              "      bounds the break from BOTH ends");
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

    printf("linux_personality_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
