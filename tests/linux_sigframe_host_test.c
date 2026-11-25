/*
 * linux_sigframe_host_test.c — Phase E7 increment 26 (P1-C, "the frame"):
 * the signal frame's byte layout and its three decisions, against the REAL
 * kernel/linux_abi.c.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * The frame is the one artifact in this kernel that a GUEST's libc reads
 * unaided: musl's handler trampoline takes rsp, trusts the offsets, calls
 * the handler, and returns through `pretcode`, and `rt_sigreturn` reads the
 * same bytes back. A number wrong by eight is not a crash in the kernel —
 * it is a handler that runs with a corrupt `rip` and a signal mask that
 * never applied, and it shows up as a guest dying in a place with no
 * relation to the cause. So the offsets are asserted HERE, twice: against
 * the literals the header publishes (what the builder uses) and against the
 * x86-64 kernel UAPI they came from, — and both against a frame the builder
 * actually produced, field by field.
 *
 * ─── What it deliberately does not prove ──────────────────────────────────
 * Nothing here touches a user page, a CR2, a return-to-user path or a
 * channel: increment 26 is the pure half, and this test is the reason it can
 * be reviewed without a boot. The delivery hook, the rt_sigreturn table row
 * (after a real census run) and the sidecar's -EINTR answer are increment
 * 27's, and the live arm follows those.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_sigframe_host_test \
 *       tests/linux_sigframe_host_test.c kernel/linux_abi.c
 *   /tmp/linux_sigframe_host_test
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

/* A spec that is valid in every field, so each test can break exactly one. */
static struct LinuxSigFrameSpec base_spec(void)
{
    struct LinuxSigFrameSpec s;
    int i;

    memset(&s, 0, sizeof(s));
    s.signo = 11;                  /* SIGSEGV */
    s.code = 1;                    /* SEGV_MAPERR */
    s.addr = 0x00007fffffff0000ull;
    s.handler = 0x0000000000401000ull;
    s.restorer = 0x0000000000401200ull;
    /* The interrupted stack sits BELOW the alternate stack these tests
     * install, so "did the handler move to the altstack?" is answerable by
     * comparing rsp against alt_sp rather than by trusting a flag. */
    s.user_rsp = 0x00007fff00000000ull;
    for (i = 0; i < (int)LINUX_SIGREG_COUNT; i++)
        s.regs.r[i] = 0x1000u + (uint64_t)i;
    s.regs.eflags = 0x202;
    return s;
}

int main(void)
{
    uint8_t frame[LINUX_SIGFRAME_SIZE + 64];
    struct LinuxSigRegs entry;
    uint64_t entry_mask = 0;
    struct LinuxSigFrameSpec spec;
    int i;

    /* ═══ The layout, as literals the header publishes ═══════════════════
     * These are the x86-64 kernel's rt_sigframe (arch/x86/include/asm/
     * ucontext.h): pretcode, ucontext (uc_flags/uc_link/uc_stack/
     * uc_mcontext/uc_sigmask), then siginfo, then fpstate. linux_abi.c
     * _Static_asserts the same numbers against a mirror struct, so both a
     * compiler that packs differently and an edit here stop the build. */
    CHECK(LINUX_SIGFRAME_PRETCODE == 0,
          "pretcode is at offset 0 — the handler's rsp points AT it, and `ret` from the handler must land on the restorer");
    CHECK(LINUX_SIGFRAME_UC == 8 && LINUX_SIGFRAME_UC_FLAGS == 8,
          "the ucontext starts 8 bytes in (uc_flags), so pretcode is exactly one pointer");
    CHECK(LINUX_SIGFRAME_UC_LINK == 16, "uc_link follows the flags (unused, zeroed)");
    CHECK(LINUX_SIGFRAME_UC_STACK == 24,
          "uc_stack is at 24: ss_sp 24, ss_flags(+pad) 32, ss_size 40 — stack_t's own ABI");
    CHECK(LINUX_SIGFRAME_MCONTEXT == 48,
          "uc_mcontext is at 48: 8 (pretcode-relative 8) + 40 bytes of flags/link/stack");
    CHECK(LINUX_SIGFRAME_MCONTEXT_BYTES == 256,
          "the sigcontext is 256 bytes: 17 gregs + eflags + selectors + err/trapno/oldmask/cr2 + fpstate + reserved1[8]");
    CHECK(LINUX_SIGFRAME_SIGMASK == 304,
          "uc_sigmask is at 304 — immediately after the 256-byte sigcontext");
    CHECK(LINUX_SIGFRAME_INFO == 312,
          "siginfo is at 312: one 8-byte sigset after the sigcontext");
    CHECK(LINUX_SIGFRAME_SIZE == 440,
          "the frame is 440 bytes — siginfo's 128 bytes end it (no fpstate in this increment)");
    CHECK(LINUX_SIGFRAME_MCONTEXT + LINUX_SIGFRAME_MCONTEXT_BYTES + LINUX_SIGSET_BYTES ==
          LINUX_SIGFRAME_INFO,
          "the sections tile without a hole: sigcontext at 48 + its 256 bytes + the 8-byte sigset lands exactly on siginfo at 312");

    /* ─── sigcontext's fields, relative to the mcontext ───────────────── */
    CHECK(LINUX_SIGCTX_EFLAGS == 136,
          "eflags is at 136: 17 gregs x 8 bytes — the frame's first non-greg word");
    CHECK(LINUX_SIGCTX_CS == 144 && LINUX_SIGCTX_GS == 146 &&
          LINUX_SIGCTX_FS == 148,
          "cs/gs/fs are u16s at 144/146/148, then a pad — reading cs at a u64 alignment would pick up gs and fs");
    CHECK(LINUX_SIGCTX_ERR == 152 && LINUX_SIGCTX_TRAPNO == 160 &&
          LINUX_SIGCTX_OLDMASK == 168 && LINUX_SIGCTX_CR2 == 176,
          "err/trapno/oldmask/cr2 are four u64s after the selectors' pad");
    CHECK(LINUX_SIGCTX_FPSTATE == 184 && LINUX_SIGCTX_RESERVED == 192,
          "the fpstate pointer is at 184 (NULL here) and reserved1[8] runs 192..256");
    CHECK(LINUX_SIGREG_COUNT == 17 && LINUX_SIGREG_R8 == 0 &&
          LINUX_SIGREG_RDI == 8 && LINUX_SIGREG_RSP == 15 &&
          LINUX_SIGREG_RIP == 16,
          "gregs are the KERNEL's order — r8..r15 first, then rdi/rsi/rbp/rbx/rdx/rax/rcx/rsp/rip — not the SYSCALL capture's order");

    /* ═══ The builder: a frame that a libc can actually run ══════════════ */
    spec = base_spec();
    memset(frame, 0xAA, sizeof(frame));   /* poison: the builder owns every byte it claims */
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask) == 0,
          "a valid spec builds: the SIGSEGV handler install a real program makes");
    CHECK(((uint64_t *)frame)[LINUX_SIGFRAME_PRETCODE / 8] == spec.restorer,
          "pretcode is the caller's sa_restorer — the only way back from the handler");
    CHECK((entry.r[LINUX_SIGREG_RSP] & 0xFu) == 8u,
          "the handler is entered with rsp % 16 == 8 — Linux's align_sigframe shape, which every compiler prologue assumes");
    CHECK(entry.r[LINUX_SIGREG_RIP] == spec.handler,
          "rip is the handler address, not the interrupted rip");
    CHECK(entry.r[LINUX_SIGREG_RDI] == spec.signo,
          "rdi is the signal number: the handler's first argument");
    CHECK(entry.r[LINUX_SIGREG_RSI] == spec.regs.r[LINUX_SIGREG_RSI] &&
          entry.r[LINUX_SIGREG_RDX] == spec.regs.r[LINUX_SIGREG_RDX],
          "without SA_SIGINFO rsi/rdx keep the INTERRUPTED values — a one-argument handler that reads them must not see frame addresses");
    CHECK(frame[LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_CS] == 0x33 &&
          frame[LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_GS] == 0x00,
          "cs is the ring-3 selector 0x33 and gs is 0 — a frame that carried another cs could return to ring 0");
    CHECK(((uint64_t *)(frame + LINUX_SIGFRAME_MCONTEXT))[LINUX_SIGREG_RIP] ==
          spec.regs.r[LINUX_SIGREG_RIP],
          "the interrupted rip is preserved in the sigcontext (the handler needs to return to it)");
    CHECK(((uint64_t *)(frame + LINUX_SIGFRAME_MCONTEXT))[LINUX_SIGREG_RSP] ==
          spec.regs.r[LINUX_SIGREG_RSP],
          "the interrupted rsp is preserved in the sigcontext, distinct from the handler's rsp");
    CHECK(*(uint64_t *)(frame + LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_EFLAGS) ==
          spec.regs.eflags,
          "eflags crosses into the frame (a handler that returns into a flags-less context resumes with the wrong DF)");
    CHECK(*(uint64_t *)(frame + LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_FPSTATE) == 0,
          "the fpstate pointer is NULL — this increment's NAMED limit: a handler that clobbers xmm keeps the clobbering after sigreturn");
    CHECK(*(uint32_t *)(frame + LINUX_SIGFRAME_INFO + 0) == spec.signo &&
          *(uint32_t *)(frame + LINUX_SIGFRAME_INFO + 8) == spec.code &&
          *(uint64_t *)(frame + LINUX_SIGFRAME_INFO + 16) == spec.addr,
          "siginfo carries si_signo, si_code and si_addr at 0/8/16 — a SIGSEGV handler that reads si_addr is how a real crash reporter runs");

    /* ─── The mask: blocked | sa_mask | the signal itself ──────────────── */
    CHECK((entry_mask & (1ull << (spec.signo - 1))) != 0,
          "the delivered signal is blocked inside its own handler by default — recursion is opt-in");
    spec = base_spec();
    spec.act_flags = LINUX_SA_NODEFER;
    (void)linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask);
    CHECK((entry_mask & (1ull << (spec.signo - 1))) == 0,
          "SA_NODEFER leaves the signal unblocked — a handler that must see its own re-raise");
    spec = base_spec();
    spec.blocked = (1ull << 1) | (1ull << 9);   /* SIGINT, SIGKILL blocked */
    spec.act_mask = (1ull << 19);               /* SIGTSTP masked by the act */
    (void)linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask);
    CHECK((entry_mask & (1ull << 1)) != 0 && (entry_mask & (1ull << 9)) != 0,
          "the thread's own blocked set is carried into the handler's mask");
    CHECK((entry_mask & (1ull << 19)) != 0,
          "sa_mask is OR'd in — an action that masks SIGTSTP while it runs");

    /* ─── SA_SIGINFO: three arguments, pointing into this frame ────────── */
    spec = base_spec();
    spec.siginfo = 1;
    spec.act_flags = LINUX_SA_SIGINFO;
    (void)linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask);
    CHECK(entry.r[LINUX_SIGREG_RDI] == spec.signo &&
          entry.r[LINUX_SIGREG_RSI] == entry.r[LINUX_SIGREG_RSP] + LINUX_SIGFRAME_INFO &&
          entry.r[LINUX_SIGREG_RDX] == entry.r[LINUX_SIGREG_RSP] + LINUX_SIGFRAME_UC,
          "SA_SIGINFO passes (signo, &siginfo, &ucontext) — the three-argument ABI a real handler is compiled for");

    /* ─── sigaltstack: SA_ONSTACK runs on the alternate stack ──────────── */
    spec = base_spec();
    spec.act_flags = LINUX_SA_ONSTACK;
    spec.alt_sp = 0x00007ffffff00000ull;
    spec.alt_size = 0x2000;
    (void)linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask);
    CHECK(entry.r[LINUX_SIGREG_RSP] >= spec.alt_sp &&
          entry.r[LINUX_SIGREG_RSP] < spec.alt_sp + spec.alt_size,
          "SA_ONSTACK with an installed stack runs the handler INSIDE that stack — the whole point of a SIGSEGV handler over a blown stack");
    CHECK(*(uint64_t *)(frame + LINUX_SIGFRAME_UC_STACK + 0) == spec.alt_sp &&
          *(uint32_t *)(frame + LINUX_SIGFRAME_UC_STACK + 8) == LINUX_SS_ONSTACK &&
          *(uint64_t *)(frame + LINUX_SIGFRAME_UC_STACK + 16) == spec.alt_size,
          "uc_stack names the alternate stack with SS_ONSTACK — a handler that calls sigaltstack(NULL) must see it");
    spec = base_spec();
    spec.act_flags = LINUX_SA_ONSTACK;      /* asked for it ... */
    spec.alt_flags = LINUX_SS_DISABLE;      /* ... but the stack is disabled */
    spec.alt_sp = 0x00007ffffff00000ull;
    spec.alt_size = 0x2000;
    (void)linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask);
    CHECK(entry.r[LINUX_SIGREG_RSP] < spec.alt_sp,
          "a DISABLED altstack is not used even when SA_ONSTACK asks — the flag alone is not the state");
    spec = base_spec();
    spec.act_flags = LINUX_SA_ONSTACK;
    spec.alt_sp = 0x00007ffffff00000ull;
    spec.alt_size = 64;                     /* smaller than a frame */
    (void)linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask);
    CHECK(entry.r[LINUX_SIGREG_RSP] < spec.alt_sp,
          "an altstack too small for the frame is refused by falling back to the interrupted stack, not by overrunning it");

    /* ─── The refusals: each one is a place a frame must not exist ─────── */
    spec = base_spec();
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE - 8u, &spec,
                                &entry, &entry_mask) == -LINUX_EINVAL,
          "a buffer shorter than the frame is EINVAL — writing anyway would run off the caller's staging page");
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask) == 0 &&
          linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, 0,
                                &entry, &entry_mask) == -LINUX_EINVAL,
          "a NULL spec is EINVAL (the builder is total over bad input, not a crash)");
    spec = base_spec(); spec.handler = 0;
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask) == -LINUX_EINVAL,
          "SIG_DFL is not a frame — delivering one would jump to address 0");
    spec = base_spec(); spec.handler = 1;
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask) == -LINUX_EINVAL,
          "SIG_IGN is not a frame either — the honest answer is to ignore, not to build");
    spec = base_spec(); spec.restorer = 0;
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask) == -LINUX_EINVAL,
          "a zero restorer is refused: the handler's `ret` would pop a NULL pretcode and fault in the handler");
    spec = base_spec(); spec.signo = 0;
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask) == -LINUX_EINVAL,
          "signal 0 is tkill's existence probe, never a delivery — and 1..64 is the only range that has a bit");
    spec = base_spec(); spec.signo = 65;
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask) == -LINUX_EINVAL,
          "signal 65 is outside the kernel-ABI sigset (64 signals) and is refused, not shifted out of range");
    spec = base_spec(); spec.user_rsp = 0x100;   /* no room under it */
    CHECK(linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask) == -LINUX_EINVAL,
          "an interrupted rsp with no room for the frame plus its 8-byte alignment step is EINVAL — the alternative is a frame written over the guest's own code");

    /* ═══ rt_sigreturn: the inverse, and what it must refuse ═════════════ */
    spec = base_spec();
    (void)linux_sig_frame_build(frame, (uint32_t)LINUX_SIGFRAME_SIZE, &spec,
                                &entry, &entry_mask);
    {
        struct LinuxSigRegs back;
        uint64_t back_mask = 0;
        int same = 1;
        memset(&back, 0, sizeof(back));
        CHECK(linux_sig_frame_restore(frame, (uint32_t)LINUX_SIGFRAME_SIZE,
                                      &back, &back_mask) == 0,
              "a built frame restores");
        for (i = 0; i < (int)LINUX_SIGREG_COUNT; i++)
            if (back.r[i] != spec.regs.r[i]) same = 0;
        CHECK(same && back.eflags == spec.regs.eflags,
              "every greg and eflags comes back exactly — build/restore is a round trip, which is the property rt_sigreturn depends on");
        CHECK(back_mask == spec.blocked,
              "the restored mask is the PRE-handler blocked set (uc_sigmask),\n"
              "      not the mask the handler runs under — increment 29's\n"
              "      return trip restores exactly this field, and a frame that\n"
              "      stored entry_mask would leave the signal blocked forever");
        CHECK(entry_mask != back_mask,
              "and the two are different numbers in this case, so the\n"
              "      assertion above can fail: the handler runs with\n"
              "      blocked|sa_mask|sig while the frame remembers blocked");
        CHECK(linux_sig_frame_restore(frame, (uint32_t)LINUX_SIGFRAME_SIZE - 1u,
                                      &back, &back_mask) == -LINUX_EINVAL,
              "a frame shorter than the header says is refused — a truncated sigreturn must not restore half a context");
        frame[LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_CS] = 0x10;   /* ring 0 */
        CHECK(linux_sig_frame_restore(frame, (uint32_t)LINUX_SIGFRAME_SIZE,
                                      &back, &back_mask) == -LINUX_EINVAL,
              "a frame whose cs is not 0x33 is refused: restore is the one place a forged frame could return to ring 0");
    }

    /* ═══ The default actions, and the two decisions ═════════════════════ */
    CHECK(linux_sig_default_action(11) == LINUX_SIGDEF_CORE,
          "SIGSEGV's default is a terminating core — the path a guest takes today with no handler installed");
    CHECK(linux_sig_default_action(17) == LINUX_SIGDEF_IGNORE,
          "SIGCHLD's default is to ignore — what makes wait4-based programs possible later");
    CHECK(linux_sig_default_action(19) == LINUX_SIGDEF_STOP &&
          linux_sig_default_action(20) == LINUX_SIGDEF_STOP,
          "SIGSTOP and SIGTSTP default to a stop, not a kill");
    CHECK(linux_sig_default_action(18) == LINUX_SIGDEF_CONTINUE,
          "SIGCONT's default is to continue");
    CHECK(linux_sig_default_action(15) == LINUX_SIGDEF_TERMINATE,
          "SIGTERM terminates without a core");
    CHECK(linux_sig_default_action(0) == LINUX_SIGDEF_NONE &&
          linux_sig_default_action(65) == LINUX_SIGDEF_NONE,
          "0 and 65 have no action — the table is total and says so instead of defaulting to a kill");

    CHECK(linux_sig_delivery_needed(0, 11, 0x401000) == 1,
          "an unmasked signal with a real handler needs a frame");
    CHECK(linux_sig_delivery_needed(1ull << 10, 11, 0x401000) == 0,
          "a blocked signal does not — it stays pending until the mask opens");
    CHECK(linux_sig_delivery_needed(0, 11, 0) == 0 &&
          linux_sig_delivery_needed(0, 11, 1) == 0,
          "SIG_DFL and SIG_IGN are the action/ignore path, never a frame");

    CHECK(linux_sig_park_is_eintr(11, 0, 0x401000, 0) == 1,
          "an installed handler with no SA_RESTART interrupts a blocked syscall — Linux's EINTR rule");
    CHECK(linux_sig_park_is_eintr(11, 0, 0x401000, LINUX_SA_RESTART) == 0,
          "SA_RESTART means the syscall is restarted instead — the distinction a program's read loop depends on");
    CHECK(linux_sig_park_is_eintr(11, 1ull << 10, 0x401000, 0) == 0,
          "a blocked signal cannot interrupt the park — it is not being delivered");
    CHECK(linux_sig_park_is_eintr(11, 0, 1, 0) == 0 &&
          linux_sig_park_is_eintr(11, 0, 1, LINUX_SA_RESTART) == 0,
          "SIG_IGN never interrupts: ignored is 0, not a wake");

    /* ═══ The absence that is load-bearing ═══════════════════════════════
     * This increment builds frames; it dispatches nothing. rt_sigreturn's
     * table row (15) stays UNKNOWN until a real census run says the number
     * is what a guest actually traps (§5.6: the table is BUILT from the
     * trace), and increment 27 adds the row with that trace in hand. The
     * assertion is here so the frame cannot quietly grow a dispatch nobody
     * censused. */
    CHECK(LINUX_RT_SIGRETURN == 15, "rt_sigreturn is 15 on x86-64");
    CHECK(linux_syscall_class(LINUX_RT_SIGRETURN) == LSC_SIG,
          "rt_sigreturn (15) IS in the table now — the absence this test used\n"
          "      to assert was the right assertion until a trace existed;\n"
          "      tools/linux_rt_sigreturn_census.txt is that trace (a handler\n"
          "      returning, count 1), and the row cites it");

    printf("\nlinux_sigframe_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
