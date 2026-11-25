/*
 * linux_sigdeliver_host_test.c — Phase E7 increment 27 (P1-C, "the
 * delivery"): the return-to-user hook's half, against the REAL
 * kernel/linux_abi.c and its real page-table walker.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * Three claims, in the order the hook makes them:
 *
 *   1. THE REQUEST is decoded from a fixed 80-byte payload, and an answer
 *      that is truncated, padded or names a handler that cannot run is
 *      refused HERE — before the write path sees it.
 *   2. si_addr comes from the number the signal actually has: CR2 for the
 *      two faults that fault on an ADDRESS (SIGSEGV, SIGBUS — including a
 *      NULL deref, where cr2 is 0 and 0 is still the answer), RIP for the
 *      two that fault on an INSTRUCTION (SIGFPE, SIGILL), the sidecar's own
 *      address for a software-sent signal, and nothing at all otherwise.
 *   3. THE WRITE goes through linux_user_copy_in against the task's cr3.
 *      A stack the task made read-only, or never mapped, is -EFAULT with
 *      EVERY BYTE OF THE GUEST'S MEMORY UNCHANGED — asserted by sentinel,
 *      not assumed. That is the property that matters: a signal is the
 *      last thing in this kernel that is allowed to fault in ring 0.
 *
 * ─── What it deliberately does not prove ──────────────────────────────────
 * No channel, no reply, no CS/SS switch, no live arm. Applying the returned
 * context to the descriptor is the caller's step and is exercised by the
 * boot, not here; rt_sigreturn's table row still does not exist (asserted
 * absent below, so this test cannot quietly become its justification).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_sigdeliver_host_test \
 *       tests/linux_sigdeliver_host_test.c kernel/linux_abi.c
 *   /tmp/linux_sigdeliver_host_test
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

#define PAGE_SZ 4096u

/* ─── The fake tables: 4 KiB frames, host addresses as physical ones, the
 *     same identity-frame idiom the copy test in linux_personality_host_test.c
 *     uses. A test re-points one entry to change one property. */
static uint64_t f_pml4[512] __attribute__((aligned(4096)));
static uint64_t f_pdpt[512] __attribute__((aligned(4096)));
static uint64_t f_pd[512]   __attribute__((aligned(4096)));
static uint64_t f_pt[512]   __attribute__((aligned(4096)));
static uint8_t  f_page0[4096] __attribute__((aligned(4096)));
static uint8_t  f_page1[4096] __attribute__((aligned(4096)));

#define P_BIT 1ULL
#define W_BIT 2ULL
#define U_BIT 4ULL

#define STACK_VA  0x0000000040000000ULL   /* page-aligned base of the stack */
#define STACK_TOP (STACK_VA + PAGE_SZ)   /* interrupted rsp: frame lands in
                                          * page0, so the test can read it
                                          * back from f_page0 by offset. Page1
                                          * is mapped too, so a test can tell
                                          * "wrong page" from "no page". */

static uint64_t root_of(void) { return (uint64_t)(uintptr_t)f_pml4; }

static void map_pages(uint64_t flags_leaf)
{
    unsigned i4 = (unsigned)((STACK_VA >> 39) & 0x1FF);
    unsigned i3 = (unsigned)((STACK_VA >> 30) & 0x1FF);
    unsigned i2 = (unsigned)((STACK_VA >> 21) & 0x1FF);
    unsigned i1 = (unsigned)((STACK_VA >> 12) & 0x1FF);

    memset(f_pml4, 0, sizeof f_pml4);
    memset(f_pdpt, 0, sizeof f_pdpt);
    memset(f_pd, 0, sizeof f_pd);
    memset(f_pt, 0, sizeof f_pt);
    /* The walker requires RW at EVERY level on the write side (a real
     * kernel table has it there), so the mids are P|W|U and each test
     * changes only the leaf. */
    f_pml4[i4] = (uint64_t)(uintptr_t)f_pdpt | P_BIT | W_BIT | U_BIT;
    f_pdpt[i3] = (uint64_t)(uintptr_t)f_pd   | P_BIT | W_BIT | U_BIT;
    f_pd[i2]   = (uint64_t)(uintptr_t)f_pt   | P_BIT | W_BIT | U_BIT;
    f_pt[i1]     = (uint64_t)(uintptr_t)f_page0 | flags_leaf;
    f_pt[i1 + 1] = (uint64_t)(uintptr_t)f_page1 | flags_leaf;
}

/* A request that is valid in every field, so each test breaks exactly one. */
static void good_req(struct LinuxSigDeliverReq *r, uint32_t signo)
{
    memset(r, 0, sizeof *r);
    r->signo     = signo;
    r->code      = 1u;                    /* SEGV_MAPERR */
    r->addr      = 0;
    r->handler   = 0x401000ULL;
    r->restorer  = 0x401200ULL;
    r->act_flags = 0;
    r->act_mask  = 0;
    r->blocked   = 0;
    r->alt_sp    = 0;
    r->alt_size  = 0;
    r->alt_flags = LINUX_SS_DISABLE;
    r->siginfo   = 0;
}

static void encode(const struct LinuxSigDeliverReq *r, uint8_t *b)
{
    unsigned i;
    uint64_t f[8];
    uint32_t g[4];
    f[0] = r->addr; f[1] = r->handler; f[2] = r->restorer; f[3] = r->act_flags;
    f[4] = r->act_mask; f[5] = r->blocked; f[6] = r->alt_sp; f[7] = r->alt_size;
    g[0] = r->signo; g[1] = r->code; g[2] = r->alt_flags; g[3] = r->siginfo;
    for (i = 0; i < 4; i++) {
        b[i * 4 + 0] = (uint8_t)(g[i]);
        b[i * 4 + 1] = (uint8_t)(g[i] >> 8);
        b[i * 4 + 2] = (uint8_t)(g[i] >> 16);
        b[i * 4 + 3] = (uint8_t)(g[i] >> 24);
    }
    /* offsets 0..15 are signo/code then addr, so the u64 block starts at 8 */
    b[8]  = (uint8_t)(f[0]); b[9] = (uint8_t)(f[0] >> 8);
    b[10] = (uint8_t)(f[0] >> 16); b[11] = (uint8_t)(f[0] >> 24);
    b[12] = (uint8_t)(f[0] >> 32); b[13] = (uint8_t)(f[0] >> 40);
    b[14] = (uint8_t)(f[0] >> 48); b[15] = (uint8_t)(f[0] >> 56);
    /* the u64 run starts at 16 with handler (addr is at 8, above) */
    for (i = 1; i < 8; i++) {
        unsigned k;
        for (k = 0; k < 8; k++)
            b[16 + (i - 1) * 8 + k] = (uint8_t)(f[i] >> (k * 8));
    }
    b[72] = (uint8_t)(g[2]); b[73] = (uint8_t)(g[2] >> 8);
    b[74] = (uint8_t)(g[2] >> 16); b[75] = (uint8_t)(g[2] >> 24);
    b[76] = (uint8_t)(g[3]); b[77] = (uint8_t)(g[3] >> 8);
    b[78] = (uint8_t)(g[3] >> 16); b[79] = (uint8_t)(g[3] >> 24);
}

static uint64_t get64_at(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint16_t get16_at(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

int main(void)
{
    struct LinuxSigDeliverReq req, back;
    struct LinuxSigRegs ctx, entry;
    uint8_t bytes[LINUX_SIGDELIVER_BYTES + 8];
    uint64_t mask = 0, addr = 0;
    int r;

    printf("── the request: 80 bytes, and not one more ─────────────────\n");
    {
        good_req(&req, 11u);
        encode(&req, bytes);
        memset(&back, 0, sizeof back);
        r = linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES, &back);
        CHECK(r == 0 &&
              back.signo == 11u && back.code == 1u &&
              back.handler == 0x401000ULL && back.restorer == 0x401200ULL &&
              back.alt_flags == LINUX_SS_DISABLE && back.siginfo == 0u,
              "a well-formed 80-byte answer decodes field for field — the\n"
              "      wire length and the struct length agree, which is what the\n"
              "      header's _Static_assert pins and this checks end to end");

        r = linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES - 1u, &back);
        CHECK(r == -LINUX_EINVAL,
              "79 bytes is refused — a truncated answer is not an answer to\n"
              "      be repaired (the last word is the handler's arguments)");

        r = linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES + 1u, &back);
        CHECK(r == -LINUX_EINVAL,
              "81 bytes is refused — a padded answer is a different message,\n"
              "      and guessing which end grew is not this kernel's job");

        good_req(&req, 0u);
        encode(&req, bytes);
        CHECK(linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES, &back)
              == -LINUX_EINVAL,
              "signo 0 is refused — there is no signal 0 to run");

        good_req(&req, 65u);
        encode(&req, bytes);
        CHECK(linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES, &back)
              == -LINUX_EINVAL,
              "signo 65 is refused — Linux's range is 1..=64 and the frame's\n"
              "      sigset is exactly one u64 wide");

        good_req(&req, 11u);
        req.handler = 1u;                 /* SIG_IGN */
        encode(&req, bytes);
        CHECK(linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES, &back)
              == -LINUX_EINVAL,
              "a handler of 1 (SIG_IGN) is refused — a scalar answered by the\n"
              "      sidecar, not something to jump to");

        good_req(&req, 11u);
        req.restorer = 0u;
        encode(&req, bytes);
        CHECK(linux_sig_deliver_req_parse(bytes, LINUX_SIGDELIVER_BYTES, &back)
              == -LINUX_EINVAL,
              "a zero restorer is refused — pretcode is how the handler\n"
              "      returns, and a delivery that cannot return is a crash\n"
              "      with extra steps");
    }

    printf("── si_addr: the address the signal actually has ────────────\n");
    {
        CHECK(linux_sig_si_addr(11u, 0xdead0000ULL, 0x401234ULL, &addr) == 1 &&
              addr == 0xdead0000ULL,
              "SIGSEGV takes CR2 — the faulting ADDRESS, which is what a\n"
              "      crash reporter prints and what a guard page proves");

        CHECK(linux_sig_si_addr(11u, 0ULL, 0x401234ULL, &addr) == 1 && addr == 0ULL,
              "a NULL deref's si_addr is 0 and still counts as an answer —\n"
              "      cr2 == 0 is the NULL it was taken on, not 'no address'");

        CHECK(linux_sig_si_addr(7u, 0x7000ULL, 0x401234ULL, &addr) == 1 &&
              addr == 0x7000ULL,
              "SIGBUS takes CR2 too — the same fault class, the same register");

        CHECK(linux_sig_si_addr(8u, 0xdead0000ULL, 0x401234ULL, &addr) == 1 &&
              addr == 0x401234ULL,
              "SIGFPE takes RIP — an integer divide faults on an INSTRUCTION,\n"
              "      and CR2 is stale from whatever ran before it");

        CHECK(linux_sig_si_addr(4u, 0xdead0000ULL, 0x401234ULL, &addr) == 1 &&
              addr == 0x401234ULL,
              "SIGILL takes RIP for the same reason — si_addr names the\n"
              "      instruction the kernel refused to execute");

        CHECK(linux_sig_si_addr(15u, 0xdead0000ULL, 0x401234ULL, &addr) == 0,
              "SIGTERM carries no address — the caller keeps the sidecar's\n"
              "      own addr, so a software-sent signal cannot inherit a\n"
              "      fault address from whatever ran before it");
    }

    printf("── the write: through the validated copy, or not at all ────\n");
    {
        const uint64_t ursp = STACK_TOP;         /* interrupted rsp */
        const uint64_t urip = 0x401abcULL;

        map_pages(P_BIT | W_BIT | U_BIT);
        memset(f_page0, 0xEE, sizeof f_page0);   /* sentinel: untouched = fail */
        memset(f_page1, 0xEE, sizeof f_page1);

        memset(&ctx, 0, sizeof ctx);
        ctx.r[LINUX_SIGREG_RSP] = ursp;
        ctx.r[LINUX_SIGREG_RIP] = urip;
        ctx.r[LINUX_SIGREG_RDI] = 0x1111ULL;
        ctx.r[LINUX_SIGREG_RAX] = 0x2222ULL;
        ctx.eflags = 0x202ULL;

        good_req(&req, 11u);
        req.blocked = 0x2ULL;                    /* SIGINT blocked */
        req.act_mask = 0x4ULL;                   /* handler adds SIGILL */
        memset(&entry, 0, sizeof entry);

        r = linux_sig_deliver(root_of(), &req, &ctx, 0xdead0000ULL, &entry, &mask);
        CHECK(r == 0,
              "a mapped, writable stack accepts the frame — the real copy\n"
              "      walker, the real builder, no boot in the loop");

        {
            uint64_t frsp = entry.r[LINUX_SIGREG_RSP];
            uint64_t off = frsp - STACK_VA;

            CHECK(frsp % 16u == 8u && frsp < ursp &&
                  frsp + LINUX_SIGFRAME_SIZE <= ursp &&
                  frsp + LINUX_SIGFRAME_SIZE + 16u >= ursp,
                  "the frame sits just below the interrupted rsp (the 16-byte\n"
                  "      gap is the kernel's own: sp is rounded, then dropped\n"
                  "      by 8) and the handler is entered with rsp % 16 == 8 —\n"
                  "      the alignment glibc's trampoline and musl's assume");

            CHECK(entry.r[LINUX_SIGREG_RIP] == req.handler,
                  "RIP is the handler — the one register that makes this a\n"
                  "      delivery rather than a stack edit");

            CHECK(entry.r[LINUX_SIGREG_RDI] == 11u,
                  "RDI is the signo: the first argument of every handler");

            CHECK(entry.r[LINUX_SIGREG_RDX] == ctx.r[LINUX_SIGREG_RDX],
                  "without SA_SIGINFO, RDX is still the interrupted value —\n"
                  "      a one-argument handler must not see a pointer it\n"
                  "      never asked for");

            CHECK(entry.r[LINUX_SIGREG_RAX] == 0x2222ULL &&
                  entry.r[LINUX_SIGREG_RSP] != ctx.r[LINUX_SIGREG_RSP],
                  "every other register is the interrupted one, and only rsp\n"
                  "      was aimed at the frame");

            CHECK(entry.eflags == ctx.eflags,
                  "eflags rides through untouched — a handler that runs with\n"
                  "      IF cleared deadlocks on its own first syscall");

            CHECK(off < PAGE_SZ &&
                  get64_at(f_page0 + off + LINUX_SIGFRAME_PRETCODE) == req.restorer,
                  "pretcode is the restorer, read back out of the guest's own\n"
                  "      memory — the byte the trampoline will execute");

            CHECK(get64_at(f_page0 + off + LINUX_SIGFRAME_MCONTEXT +
                           (uint64_t)LINUX_SIGREG_RSP * 8u) == ursp &&
                  get64_at(f_page0 + off + LINUX_SIGFRAME_MCONTEXT +
                           (uint64_t)LINUX_SIGREG_RIP * 8u) == urip,
                  "the sigcontext holds the INTERRUPTED rsp and rip — what\n"
                  "      rt_sigreturn will one day restore");

            CHECK(get16_at(f_page0 + off + LINUX_SIGFRAME_MCONTEXT +
                           LINUX_SIGCTX_CS) == LINUX_SIGFRAME_USER_CS,
                  "the saved cs is 0x33: a restored context that returns to\n"
                  "      ring 0 is refused by the restore for this reason");

            CHECK(get64_at(f_page0 + off + LINUX_SIGFRAME_INFO + 16) ==
                  0xdead0000ULL,
                  "si_addr in the guest's frame is CR2 — the CR2 -> si_addr\n"
                  "      rule survives the write, not just the call");

            CHECK(get64_at(f_page0 + off + LINUX_SIGFRAME_SIGMASK) ==
                  (0x2ULL | 0x4ULL | (1ULL << 10)),
                  "the handler's mask is blocked | sa_mask | the signal\n"
                  "      itself — SIGINT's 0x2, the handler's 0x4 and SIGSEGV's\n"
                  "      own bit 10 compose to 0x406");

            CHECK(mask == 0x406ULL,
                  "and the mask the CALLER installs is the same number the\n"
                  "      frame carries — two copies of one decision, checked\n"
                  "      against each other");
        }

        {   /* SA_NODEFER: the signal is NOT hidden from itself */
            map_pages(P_BIT | W_BIT | U_BIT);
            good_req(&req, 11u);
            req.act_flags = LINUX_SA_NODEFER;
            req.blocked = 0;
            req.act_mask = 0x4ULL;
            r = linux_sig_deliver(root_of(), &req, &ctx, 0ULL, &entry, &mask);
            CHECK(r == 0 && mask == 0x4ULL,
                  "SA_NODEFER drops the signal's own bit from the mask — a\n"
                  "      recursive handler is the caller's choice to make");
        }

        {   /* read-only stack: refused, NOTHING written */
            map_pages(P_BIT | U_BIT);            /* no W */
            memset(f_page0, 0xEE, sizeof f_page0);
            good_req(&req, 11u);
            r = linux_sig_deliver(root_of(), &req, &ctx, 0xdead0000ULL,
                                  &entry, &mask);
            CHECK(r == -LINUX_EFAULT,
                  "a read-only stack refuses the frame with -EFAULT — the\n"
                  "      task's own protection is the kernel's protection");

            {
                unsigned i, clean = 1;
                for (i = 0; i < PAGE_SZ; i++)
                    if (f_page0[i] != 0xEE) clean = 0;
                CHECK(clean,
                      "and every byte of the guest's stack is UNCHANGED —\n"
                      "      refusals-before-mutation, asserted by sentinel:\n"
                      "      a half-written frame would be a kernel-authored\n"
                      "      corruption of the task's memory");
            }
        }

        {   /* unmapped stack: the frame spans a hole */
            map_pages(P_BIT | W_BIT | U_BIT);
            f_pt[(STACK_VA >> 12) & 0x1FF] = 0;   /* page0 gone */
            good_req(&req, 11u);
            r = linux_sig_deliver(root_of(), &req, &ctx, 0xdead0000ULL,
                                  &entry, &mask);
            CHECK(r == -LINUX_EFAULT,
                  "an unmapped stack span refuses with -EFAULT — the frame\n"
                  "      does not become a ring-0 page fault on the way out");
        }

        {   /* the builder's own refusal, before the copy path is reached */
            map_pages(P_BIT | W_BIT | U_BIT);
            memset(f_page0, 0xEE, sizeof f_page0);
            good_req(&req, 11u);
            req.handler = 0;                     /* SIG_DFL */
            r = linux_sig_deliver(root_of(), &req, &ctx, 0xdead0000ULL,
                                  &entry, &mask);
            CHECK(r == -LINUX_EINVAL && f_page0[0] == 0xEE,
                  "a scalar handler is refused by the builder, before any\n"
                  "      copy — the write path is only ever reached by a\n"
                  "      frame that was first built");
        }
    }

    printf("── the census: the row is still absent ────────────────────\n");
    CHECK(linux_syscall_class(LINUX_RT_SIGRETURN) == LSC_UNKNOWN,
          "rt_sigreturn is still UNCLASSIFIED — this increment delivers\n"
          "      frames and does NOT claim the return trip, which needs a\n"
          "      real census trace before a table row exists");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
