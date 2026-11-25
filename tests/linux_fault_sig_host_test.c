/*
 * linux_fault_sig_host_test.c — E7 increment 31 (P1-C, "the fault's own
 * delivery"): the #PF path's decision and frame, against the REAL
 * kernel/linux_abi.c.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * A ring-3 #PF with a handler installed must run that handler instead of
 * killing the thread, and the frame it runs on must record the fault: the
 * interrupted registers, the mask Linux installs, and — the field a crash
 * reporter reads — si_addr, which for SIGSEGV is CR2 and for SIGILL is the
 * faulting RIP. Every one of those is decided here, with no page tables, no
 * trap frame and no sidecar: if this file is green and the boot still kills,
 * the defect is the wiring (stub, capture, or record), not the decision.
 *
 * ─── What it deliberately does not prove ──────────────────────────────────
 * The register file's journey through isr14_stub's stack (clause D3 pins the
 * push order and the struct order against each other, and the live arm reads
 * what a real handler actually saw), the validated copy into the task's
 * stack (linux_user_copy_in's own tests), and the sidecar's half — a
 * fault-delivered handler cannot return through rt_sigreturn yet, which is
 * named in §10.26 rather than pretended here.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_fault_sig_host_test \
 *       tests/linux_fault_sig_host_test.c kernel/linux_abi.c
 *   /tmp/linux_fault_sig_host_test
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

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

/* A context with every register distinct, so a swapped pair cannot pass. */
static struct LinuxSigRegs ctx_fixture(void)
{
    struct LinuxSigRegs c;
    for (int i = 0; i < (int)LINUX_SIGREG_COUNT; i++)
        c.r[i] = 0x11110000ull + (uint64_t)i * 0x1000ull;
    c.r[LINUX_SIGREG_RSP] = 0x400000480000ull;   /* a plausible user stack */
    c.r[LINUX_SIGREG_RIP] = 0x400000001234ull;
    c.eflags = 0x246;
    return c;
}

static struct LinuxFaultAct act_fixture(uint64_t flags)
{
    struct LinuxFaultAct a;
    a.handler  = 0x400000005000ull;
    a.flags    = flags;
    a.mask     = (uint64_t)1u << 6;        /* SIGUSR1 in the handler's mask */
    a.restorer = 0x400000005100ull;
    return a;
}

int main(void)
{
    /* ─── 1. The table: exactly the four signals a trap can raise ───────── */
    CHECK(linux_fault_act_index(LINUX_SIGILL)  == 0, "SIGILL is slot 0");
    CHECK(linux_fault_act_index(LINUX_SIGBUS)  == 1, "SIGBUS is slot 1");
    CHECK(linux_fault_act_index(LINUX_SIGFPE)  == 2, "SIGFPE is slot 2");
    CHECK(linux_fault_act_index(LINUX_SIGSEGV) == 3, "SIGSEGV is slot 3");
    CHECK(linux_fault_act_index(1u)  == -1, "SIGHUP is not a fault class");
    CHECK(linux_fault_act_index(15u) == -1, "SIGTERM is not a fault class");
    CHECK(linux_fault_act_index(65u) == -1, "a number above 64 has no slot");

    /* ─── 2. Recording: the guest's 32 bytes, field for field ───────────── */
    {
        struct LinuxFaultAct slots[LINUX_FAULT_SIG_COUNT];
        uint8_t bytes[LINUX_SIGACT_BYTES];
        memset(slots, 0, sizeof slots);
        /* The tree's wire order: handler, flags, mask, restorer — the same
         * bytes the sidecar records (its act[2]/act[3]). */
        put64(bytes + 0,  0xAAAA0000ull);      /* handler  */
        put64(bytes + 8,  LINUX_SA_SIGINFO);   /* flags    */
        put64(bytes + 16, 0xCCCC0000ull);      /* mask     */
        put64(bytes + 24, 0xBBBB0000ull);      /* restorer */
        CHECK(linux_fault_act_record(slots, LINUX_SIGSEGV, bytes) == 1,
              "a SIGSEGV act is recorded");
        CHECK(slots[3].handler  == 0xAAAA0000ull, "handler word lands in slot 3");
        CHECK(slots[3].flags    == LINUX_SA_SIGINFO, "flags word lands");
        CHECK(slots[3].restorer == 0xBBBB0000ull, "restorer word lands");
        CHECK(slots[3].mask     == 0xCCCC0000ull, "mask word lands");
        CHECK(linux_fault_act_record(slots, 15u, bytes) == 0,
              "a non-fault signal is not recorded");
        CHECK(slots[3].handler == 0xAAAA0000ull,
              "and the refusal did not disturb the table");
    }

    /* ─── 3. No frame: every answer that must keep the kill ─────────────── */
    {
        struct LinuxSigRegs ctx = ctx_fixture();
        struct LinuxSigRegs entry;
        uint64_t mask = 0;
        uint8_t frame[LINUX_SIGFRAME_SIZE];
        struct LinuxFaultAct act;

        memset(frame, 0xAA, sizeof frame);

        act = act_fixture(0);
        act.handler = 0;                          /* SIG_DFL */
        CHECK(linux_fault_sig_plan(&act, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                   0xdead0000ull, &ctx, frame, &entry,
                                   &mask) == 0,
              "SIG_DFL asks for no frame: the attributed kill stands");

        act = act_fixture(0);
        act.handler = 1;                          /* SIG_IGN */
        CHECK(linux_fault_sig_plan(&act, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                   0xdead0000ull, &ctx, frame, &entry,
                                   &mask) == 0,
              "SIG_IGN asks for no frame");

        act = act_fixture(0);
        CHECK(linux_fault_sig_plan(&act, (uint64_t)1u << (LINUX_SIGSEGV - 1u),
                                   LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                   0xdead0000ull, &ctx, frame, &entry,
                                   &mask) == 0,
              "a blocked SIGSEGV asks for no frame (this path has no queue)");

        act = act_fixture(0);
        act.restorer = 0;
        CHECK(linux_fault_sig_plan(&act, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                   0xdead0000ull, &ctx, frame, &entry,
                                   &mask) == 0,
              "no restorer means no frame: a handler with nothing to return "
              "through is refused, not run");

        CHECK(frame[0] == 0xAA && frame[LINUX_SIGFRAME_SIZE - 1] == 0xAA,
              "a refused plan wrote NOTHING into the frame buffer");
    }

    /* ─── 4. The delivery: handler, alignment, mask, context ───────────── */
    {
        struct LinuxSigRegs ctx = ctx_fixture();
        struct LinuxSigRegs entry, back;
        uint64_t mask = 0;
        uint8_t frame[LINUX_SIGFRAME_SIZE];
        struct LinuxFaultAct act = act_fixture(LINUX_SA_SIGINFO);
        uint64_t cr2 = 0x400000400000ull;
        int d;

        d = linux_fault_sig_plan(&act, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                 cr2, &ctx, frame, &entry, &mask);
        CHECK(d == 1, "a handler + restorer is a delivery");

        CHECK(entry.r[LINUX_SIGREG_RIP] == act.handler,
              "the handler's entry rip IS the handler");
        CHECK(entry.r[LINUX_SIGREG_RSP] < ctx.r[LINUX_SIGREG_RSP],
              "the handler's stack is below the interrupted one");
        CHECK((entry.r[LINUX_SIGREG_RSP] & 0xFu) == 8u,
              "the handler is entered with rsp % 16 == 8 (the psABI shape)");
        CHECK(entry.r[LINUX_SIGREG_RSP] + LINUX_SIGFRAME_SIZE <=
              ctx.r[LINUX_SIGREG_RSP],
              "the whole frame fits below the interrupted rsp");
        CHECK(mask == (act.mask | ((uint64_t)1u << (LINUX_SIGSEGV - 1u))),
              "the handler's mask is sa_mask | the signal, on the thread's "
              "own blocked set (empty here)");

        /* SA_SIGINFO's three arguments, and the siginfo the handler reads. */
        CHECK(entry.r[LINUX_SIGREG_RDI] == LINUX_SIGSEGV,
              "rdi carries the signal number");
        CHECK(entry.r[LINUX_SIGREG_RSI] ==
              entry.r[LINUX_SIGREG_RSP] + LINUX_SIGFRAME_INFO,
              "rsi aims at the frame's siginfo");
        CHECK(entry.r[LINUX_SIGREG_RDX] ==
              entry.r[LINUX_SIGREG_RSP] + LINUX_SIGFRAME_UC,
              "rdx aims at the frame's ucontext");
        CHECK(get32(frame + LINUX_SIGFRAME_INFO + 0) == LINUX_SIGSEGV,
              "si_signo is the signal");
        CHECK(get32(frame + LINUX_SIGFRAME_INFO + 8) == LINUX_SEGV_MAPERR,
              "si_code is what the #PF error code chose");
        CHECK(get64(frame + LINUX_SIGFRAME_INFO + 16) == cr2,
              "si_addr is CR2 — the field a crash reporter reads");
        CHECK(get64(frame + LINUX_SIGFRAME_PRETCODE) == act.restorer,
              "pretcode carries the recorded restorer");

        /* The interrupted context, through the frame's own inverse. */
        memset(&back, 0, sizeof back);
        CHECK(linux_sig_frame_restore(frame, LINUX_SIGFRAME_SIZE,
                                      &back, NULL) == 0,
              "the built frame decodes back through linux_sig_frame_restore");
        CHECK(back.r[LINUX_SIGREG_RIP] == ctx.r[LINUX_SIGREG_RIP] &&
              back.r[LINUX_SIGREG_RSP] == ctx.r[LINUX_SIGREG_RSP] &&
              back.r[LINUX_SIGREG_RBX] == ctx.r[LINUX_SIGREG_RBX] &&
              back.r[LINUX_SIGREG_R15] == ctx.r[LINUX_SIGREG_R15] &&
              back.r[LINUX_SIGREG_RDI] == ctx.r[LINUX_SIGREG_RDI],
              "the frame records the INTERRUPTED context, not the handler's");
        CHECK(back.eflags == ctx.eflags, "and its rflags");

        /* SA_NODEFER: the handler must be able to take the same signal. */
        {
            struct LinuxFaultAct nd = act_fixture(LINUX_SA_SIGINFO |
                                                  LINUX_SA_NODEFER);
            uint64_t ndmask = 0;
            CHECK(linux_fault_sig_plan(&nd, 0, LINUX_SIGSEGV,
                                       LINUX_SEGV_ACCERR, cr2, &ctx, frame,
                                       &entry, &ndmask) == 1,
                  "SA_NODEFER is still a delivery");
            CHECK((ndmask & ((uint64_t)1u << (LINUX_SIGSEGV - 1u))) == 0,
                  "SA_NODEFER leaves the delivered signal unblocked");
        }
    }

    /* ─── 5. Without SA_SIGINFO the handler takes one argument ──────────── */
    {
        struct LinuxSigRegs ctx = ctx_fixture();
        struct LinuxSigRegs entry;
        uint64_t mask = 0;
        uint8_t frame[LINUX_SIGFRAME_SIZE];
        struct LinuxFaultAct act = act_fixture(0);
        CHECK(linux_fault_sig_plan(&act, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                   0x4000ull, &ctx, frame, &entry,
                                   &mask) == 1,
              "a plain handler is still a delivery");
        CHECK(entry.r[LINUX_SIGREG_RDI] == LINUX_SIGSEGV,
              "a plain handler still gets the number in rdi");
        CHECK(entry.r[LINUX_SIGREG_RSI] == ctx.r[LINUX_SIGREG_RSI],
              "and NOT a siginfo pointer (SA_SIGINFO was not asked for)");
    }

    /* ─── 6. SIGFPE/SIGILL name the INSTRUCTION, not CR2 ────────────────── */
    {
        struct LinuxSigRegs ctx = ctx_fixture();
        struct LinuxSigRegs entry;
        uint64_t mask = 0;
        uint8_t frame[LINUX_SIGFRAME_SIZE];
        struct LinuxFaultAct act = act_fixture(LINUX_SA_SIGINFO);
        CHECK(linux_fault_sig_plan(&act, 0, LINUX_SIGILL, 2u,
                                   0xdead0000ull, &ctx, frame, &entry,
                                   &mask) == 1,
              "SIGILL is deliverable too");
        CHECK(get64(frame + LINUX_SIGFRAME_INFO + 16) ==
              ctx.r[LINUX_SIGREG_RIP],
              "SIGILL's si_addr is the faulting RIP (CR2 would be stale)");
    }

    /* ─── 7. CR2 == 0 is an ADDRESS, not \"no address\" ─────────────────── */
    {
        struct LinuxSigRegs ctx = ctx_fixture();
        struct LinuxSigRegs entry;
        uint64_t mask = 0;
        uint8_t frame[LINUX_SIGFRAME_SIZE];
        struct LinuxFaultAct act = act_fixture(LINUX_SA_SIGINFO);
        CHECK(linux_fault_sig_plan(&act, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                   0ull, &ctx, frame, &entry, &mask) == 1,
              "a NULL-pointer fault is still a delivery");
        CHECK(get64(frame + LINUX_SIGFRAME_INFO + 16) == 0ull,
              "a NULL-pointer fault reports si_addr = 0, not a substitute");
    }

    /* ─── 8. A frame that cannot fit is refused, not truncated ──────────── */
    {
        struct LinuxSigRegs ctx = ctx_fixture();
        struct LinuxSigRegs entry;
        uint64_t mask = 0;
        uint8_t frame[LINUX_SIGFRAME_SIZE];
        struct LinuxFaultAct act = act_fixture(LINUX_SA_SIGINFO);
        ctx.r[LINUX_SIGREG_RSP] = 128;      /* no room for a 440-byte frame */
        CHECK(linux_fault_sig_plan(&act, 0, LINUX_SIGSEGV, LINUX_SEGV_MAPERR,
                                   0x4000ull, &ctx, frame, &entry, &mask) ==
              -LINUX_EINVAL,
              "a stack with no room refuses by name (the caller keeps the "
              "kill) rather than writing a truncated frame");
    }

    printf("%s: %d passed, %d failed\n",
           checks_failed ? "linux_fault_sig_host_test"
                         : "linux_fault_sig_host_test (all)",
           checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
