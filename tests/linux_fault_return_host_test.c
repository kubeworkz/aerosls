/*
 * linux_fault_return_host_test.c — E7 increment 32 (P1-C, "the fault
 * handler's return"): rt_sigreturn's answer for a frame the TRAP built,
 * against the REAL kernel/linux_abi.c.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * Increment 31 enters a handler from a ring-3 #PF, but nothing could come
 * back: the sidecar never saw that delivery, so its rt_sigreturn arm answers
 * EINVAL ("no handler was entered") — true for its own bookkeeping, and the
 * last word on a return the kernel owes. The kernel therefore owns that one
 * return, and two decisions make it safe:
 *
 *   linux_sig_fault_return_ok() — WHICH return the kernel may take over.
 *     Only a frame that is installed AND came from the trap, only against a
 *     refusal (never a consent or a -EPROTO), and only when the errno the
 *     task would have heard is exactly -EINVAL. Everything else keeps the
 *     answer it always had, so the existing "15 with no handler" refusal
 *     still reaches the task as -EINVAL.
 *
 *   the frame the trap wrote — WHAT that return restores. The same 440 bytes
 *     linux_sig_frame_build produced, decoded by the same
 *     linux_sig_frame_restore the return uses. That is what lets a handler
 *     FIX its fault in the ucontext and have the retried instruction see the
 *     fix: the live arm rewrites the interrupted r13 through its ucontext
 *     pointer, and this file proves the offset it writes is the offset the
 *     restore reads.
 *
 * ─── What it deliberately does not prove ──────────────────────────────────
 * The trap stack (isr14_stub's push order — clause AA's job), the frame
 * landing in the task's real pages (linux_user_copy_in's own tests), and the
 * resume itself (cap_resume_linux_sig, whose asm no host test can run). The
 * live arm, tests/linux_fault_return_check.sh, is where those three meet a
 * running machine.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_fault_return_host_test \
 *       tests/linux_fault_return_host_test.c kernel/linux_abi.c
 *   /tmp/linux_fault_return_host_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (cond) { checks_passed++; } \
    else { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* A context with every register distinct, so a swapped pair cannot pass. */
static struct LinuxSigRegs ctx_fixture(void)
{
    struct LinuxSigRegs c;
    for (int i = 0; i < (int)LINUX_SIGREG_COUNT; i++)
        c.r[i] = 0x11110000ull + (uint64_t)i * 0x1000ull;
    c.r[LINUX_SIGREG_RSP] = 0x400000480000ull;
    c.r[LINUX_SIGREG_RIP] = 0x400000001234ull;   /* the faulting instruction */
    c.eflags = 0x246;
    return c;
}

static struct LinuxFaultAct act_fixture(void)
{
    struct LinuxFaultAct a;
    a.handler  = 0x400000005000ull;
    a.flags    = LINUX_SA_SIGINFO;
    a.mask     = 0;
    a.restorer = 0x400000005100ull;
    return a;
}

/* The frame offset of a greg, as the builder writes it and the restore
 * reads it: the sigcontext starts at LINUX_SIGFRAME_MCONTEXT, gregs are in
 * Linux's order (r8 first), so r13 is index 5. */
static uint64_t greg_off(int reg)
{
    return (uint64_t)LINUX_SIGFRAME_MCONTEXT + (uint64_t)reg * 8u;
}

int main(void)
{
    /* ═══ 1. WHICH return the kernel may take over ═════════════════════ */
    const uint64_t inval = (uint64_t)(int64_t)(-LINUX_EINVAL);
    const uint64_t enosys = (uint64_t)(int64_t)(-LINUX_ENOSYS);

    CHECK(LINUX_EINVAL == 22, "EINVAL is 22 on x86-64");
    CHECK(inval == (uint64_t)0xFFFFFFFFFFFFFFEAull,
          "the errno the task hears is sign-extended -22, not a status code");

    CHECK(linux_sig_fault_return_ok(1, 1, inval, -1) == 1,
          "a trap frame + the sidecar's EINVAL refusal is the kernel's return");
    CHECK(linux_sig_fault_return_ok(1, 1, inval, -LINUX_EINVAL) == 1,
          "…whether the status carries the cap code (-1) or the errno (-22):\n"
          "      the gate reads the refusal, not its spelling");

    CHECK(linux_sig_fault_return_ok(1, 0, inval, -1) == 0,
          "a SIDECAR-delivered frame keeps the sidecar's gate — if its 15\n"
          "      arm ever refuses its own delivery, that refusal is the task's");
    CHECK(linux_sig_fault_return_ok(0, 1, inval, -1) == 0,
          "no frame installed means nothing to restore: -EINVAL is correct\n"
          "      and the live probe's SIGRET-NOHAND-OK arm depends on it");
    CHECK(linux_sig_fault_return_ok(0, 0, inval, -1) == 0,
          "no frame and no fault flag is the refusal that always existed");

    CHECK(linux_sig_fault_return_ok(1, 1, inval, 0) == 0,
          "a CONSENT is never the kernel's case — the gate above handles\n"
          "      status 0, and taking it here would skip its -EPROTO check");
    CHECK(linux_sig_fault_return_ok(1, 1, enosys, -1) == 0,
          "a different refusal (-ENOSYS) stays the task's answer by name");
    CHECK(linux_sig_fault_return_ok(1, 1, 0, -1) == 0,
          "a zero rax with a nonzero status is not a return either");
    CHECK(linux_sig_fault_return_ok(1, 1, (uint64_t)(int64_t)(-LINUX_EPROTO), -1) == 0,
          "a reply that never applied (-EPROTO) is refused here too");

    /* ═══ 2. WHAT that return restores ═════════════════════════════════ */
    struct LinuxFaultAct act = act_fixture();
    struct LinuxSigRegs ctx = ctx_fixture();
    struct LinuxSigRegs entry;
    uint8_t frame[LINUX_SIGFRAME_SIZE];
    uint64_t mask = 0;

    /* Increment 31's own planner, unchanged: the frame a #PF writes. */
    int d = linux_fault_sig_plan(&act, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                 0x400000480000ull - 0x1000ull, &ctx, frame,
                                 &entry, &mask);
    CHECK(d == 1, "a handler plus a restorer plans a delivery for a #PF");

    /* The address the return will read: the rsp the handler was entered
     * with. The live boot records it in linux_sig_frame_rsp; here it is the
     * entry's, and nothing else in the context is allowed to be it. */
    uint64_t frame_rsp = entry.r[LINUX_SIGREG_RSP];
    CHECK(frame_rsp == ((ctx.r[LINUX_SIGREG_RSP] -
                         (uint64_t)LINUX_SIGFRAME_SIZE) & ~(uint64_t)0xF) - 8u,
          "the frame sits at the entry rsp, aligned as Linux aligns it");
    CHECK(frame_rsp < ctx.r[LINUX_SIGREG_RSP] &&
          frame_rsp + LINUX_SIGFRAME_SIZE <= ctx.r[LINUX_SIGREG_RSP],
          "the whole frame fits below the interrupted stack pointer");
    CHECK(frame_rsp != ctx.r[LINUX_SIGREG_RSP],
          "the frame address is not the interrupted rsp — reading there\n"
          "      would restore the caller's frame, not the signal frame");

    /* &ucontext, as the handler receives it in rdx (SA_SIGINFO). */
    CHECK(entry.r[LINUX_SIGREG_RDX] == frame_rsp + LINUX_SIGFRAME_UC,
          "the handler's ucontext argument points INTO the frame (rdx)");
    CHECK(entry.r[LINUX_SIGREG_RSI] == frame_rsp + LINUX_SIGFRAME_INFO,
          "the handler's siginfo argument points at the frame's siginfo");

    /* The handler's pointer plus the greg offset is where the live arm
     * writes: prove the two-number agreement the fixture depends on. */
    uint64_t uctx_off = entry.r[LINUX_SIGREG_RDX] - frame_rsp;
    CHECK(uctx_off == (uint64_t)LINUX_SIGFRAME_UC,
          "ucontext is one pointer into the frame (pretcode first)");
    CHECK(uctx_off + 40u + (uint64_t)LINUX_SIGREG_R13 * 8u ==
              greg_off(LINUX_SIGREG_R13),
          "uc + 40 + REG_R13*8 IS the frame offset of r13 — the live arm\n"
          "      writes 80(%rdx) because uc_mcontext sits 40 bytes in");

    /* The return trip, on exactly that frame: what 15 restores must be the
     * interrupted context, register for register. */
    struct LinuxSigRegs back;
    uint64_t back_mask = 0;
    CHECK(linux_sig_frame_restore(frame, LINUX_SIGFRAME_SIZE, &back,
                                  &back_mask) == 0,
          "the trap's frame decodes through the rt_sigreturn decoder");
    int same = 1;
    for (int i = 0; i < (int)LINUX_SIGREG_COUNT; i++)
        if (back.r[i] != ctx.r[i]) same = 0;
    CHECK(same, "every interrupted register comes back exactly — the retried\n"
                "      instruction runs in the context that faulted");
    CHECK(back.r[LINUX_SIGREG_RIP] == 0x400000001234ull,
          "including rip: the resume is the FAULTING instruction, so a\n"
          "      handler that fixes the fault makes progress");
    CHECK(back.eflags == ctx.eflags, "and eflags, which the iretq carries");
    CHECK(back_mask == 0,
          "uc_sigmask is the PRE-handler mask: the return unblocks what the\n"
          "      delivery blocked, or SIGSEGV stays blocked forever");

    /* Now the live arm's mechanism, at the offset asserted above: a handler
     * that points the interrupted r13 at a good page and returns. */
    put64(frame + greg_off(LINUX_SIGREG_R13), 0x400000481000ull);
    CHECK(linux_sig_frame_restore(frame, LINUX_SIGFRAME_SIZE, &back,
                                  &back_mask) == 0,
          "the edited frame still decodes");
    CHECK(back.r[LINUX_SIGREG_R13] == 0x400000481000ull,
          "a handler's greg write through the ucontext IS what the return\n"
          "      restores — the fix reaches the retried instruction");
    CHECK(back.r[LINUX_SIGREG_R12] == ctx.r[LINUX_SIGREG_R12] &&
          back.r[LINUX_SIGREG_RSP] == ctx.r[LINUX_SIGREG_RSP] &&
          back.r[LINUX_SIGREG_RIP] == ctx.r[LINUX_SIGREG_RIP],
          "…and nothing else in the frame moves with it");

    /* The same offset with the OLD value would fault again — which is why
     * the live arm counts its deliveries instead of assuming one. */
    CHECK(ctx.r[LINUX_SIGREG_R13] == 0x11110000ull +
              (uint64_t)LINUX_SIGREG_R13 * 0x1000ull,
          "the interrupted r13 (the trap page) is what the frame held before\n"
          "      the edit, so a return without the edit re-faults");

    /* ═══ 3. The refusal this must NOT have changed ════════════════════ */
    struct LinuxFaultAct none;
    none.handler = 0;          /* SIG_DFL */
    none.flags   = 0;
    none.mask    = 0;
    none.restorer = 0;
    struct LinuxSigRegs e2;
    uint64_t m2 = 0;
    CHECK(linux_fault_sig_plan(&none, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                               0xdead0000ull, &ctx, frame, &e2, &m2) == 0,
          "with no handler there is no frame — so no 15 can be a fault\n"
          "      return, and the task still hears EINVAL");

    printf("\nlinux_fault_return_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
