/*
 * linux_sig_mask_host_test.c — E7 increment 33 (P1-C, "the mask inside a
 * trap-delivered handler"): whose copy answers rt_sigprocmask(14), and with
 * what arithmetic, against the REAL kernel/linux_abi.c.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * Increment 31 delivers a ring-3 #PF to its handler by building the frame in
 * the KERNEL, and it moves `linux_sig_mask` to the handler's set while doing
 * it. The sidecar is never told: a trap has no syscall behind it, so there is
 * no round trip to carry the mask. Its copy therefore still holds the
 * pre-delivery set, and a forwarded 14 from inside the handler reported that
 * narrower mask — while a BLOCK/UNBLOCK there did its arithmetic on the wrong
 * base. Increment 32 named the split; this increment closes it by making the
 * kernel the mask's authority for exactly that window.
 *
 *   linux_sig_mask_fault_owned() — WHOSE copy answers. Both flags, so the
 *     tkill path (a frame the SIDECAR entered, whose mask it moved before it
 *     answered, and whose reply clears fault_active) keeps its forward. That
 *     is the "without disturbing the tkill path" half, and it is a fact this
 *     file drives directly rather than by inspecting a call site.
 *
 *   linux_sig_mask_step() — the arithmetic, the sidecar's own: BLOCK unions,
 *     UNBLOCK differences, SETMASK replaces, a foreign `how` refuses with the
 *     output untouched, and no KILL/STOP bit is filtered because the sidecar
 *     filters none.
 *
 * The numbers are the live fixture's: the fault handler is entered with mask
 * 0x400 (SIGSEGV's bit, the blocked set 0 plus sa_mask 0 plus the signal),
 * and the pre-delivery set is 0 — which is exactly the pair the old code
 * swapped, and exactly what the boot line
 * `[E7] rt_sigprocmask in a fault handler: oldset 400 -> 400` reports.
 *
 * ─── What it deliberately does not prove ──────────────────────────────────
 * The dispatch branch itself (clause AC greps its shape and order), the copy
 * walkers that move the set and the oldset (their own tests), and the pending
 * reveal the sidecar owns — which this window deliberately does not perform
 * (a reveal is a delivery, and it stays pending until the first forwarded 14
 * after the return; shim doc §10.28).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/linux_sig_mask_host_test \
 *       tests/linux_sig_mask_host_test.c kernel/linux_abi.c
 *   /tmp/linux_sig_mask_host_test
 */
#include <stdio.h>
#include <stdint.h>

#include "linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (cond) { checks_passed++; } \
    else { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

int main(void)
{
    /* The live arm's whole handler set: SIGSEGV is bit 10 (signal 11). */
    const uint64_t segv_bit = 1ull << (LINUX_SIGSEGV - 1);
    uint64_t new_mask = 0;

    CHECK(LINUX_SIGSET_BYTES == 8,
          "the sigset this window answers with is the 8 bytes the ABI crosses");
    CHECK(segv_bit == 0x400ull,
          "SIGSEGV is bit 10, so the handler's set is 0x400 — the number the\n"
          "      boot's delivery line prints as `mask 400`");

    /* ═══ 1. WHOSE copy answers ════════════════════════════════════════ */
    CHECK(linux_sig_mask_fault_owned(1, 1) == 1,
          "an installed frame entered by the TRAP is the kernel's window");
    CHECK(linux_sig_mask_fault_owned(1, 0) == 0,
          "a tkill-delivered frame is NOT: the sidecar entered it and moved\n"
          "      its own mask before answering, so its copy is the true one");
    CHECK(linux_sig_mask_fault_owned(0, 1) == 0,
          "no installed frame means nothing to answer from");
    CHECK(linux_sig_mask_fault_owned(0, 0) == 0,
          "and the ordinary no-handler task keeps the forward it had");

    /* ═══ 2. The arithmetic is the sidecar's own ═══════════════════════ */
    /* The Rust unit test's worked example (linux.rs:
     * sigprocmask_is_exact_posix_arithmetic), replayed here so the two
     * languages cannot drift apart in silence. */
    CHECK(linux_sig_mask_step(0b101ull, 2, 0b101ull, &new_mask) == 1 &&
          new_mask == 0b101ull,
          "SETMASK is replacement");
    CHECK(linux_sig_mask_step(0b101ull, 0, 0b010ull, &new_mask) == 1 &&
          new_mask == 0b111ull,
          "BLOCK is union");
    CHECK(linux_sig_mask_step(0b111ull, 1, 0b001ull, &new_mask) == 1 &&
          new_mask == 0b110ull,
          "UNBLOCK is difference");

    /* The QUERY shape: `set` NULL never reaches step (Linux ignores `how`
     * then), and the identity that matters is that the answer is the mask
     * the handler runs under — 0x400 — not the sidecar's stale 0. */
    CHECK(linux_sig_mask_step(0x400ull, 2, 0x400ull, &new_mask) == 1 &&
          new_mask == 0x400ull,
          "a handler-set mask passes through SETMASK unchanged — the query\n"
          "      answers 0x400, the set the frame installed");
    CHECK(linux_sig_mask_step(0ull, 2, 0ull, &new_mask) == 1 &&
          new_mask == 0ull,
          "the PRE-delivery 0 the sidecar still holds is the narrower answer\n"
          "      the old code reported; it is not what this window returns");

    /* ═══ 3. The two bases disagree, which is the whole point ══════════ */
    CHECK(linux_sig_mask_step(0x400ull, 0, 0x2ull, &new_mask) == 1 &&
          new_mask == 0x402ull,
          "BLOCK from the handler's 0x400 yields 0x402");
    CHECK(linux_sig_mask_step(0ull, 0, 0x2ull, &new_mask) == 1 &&
          new_mask == 0x2ull,
          "…while BLOCK from the sidecar's stale 0 yields 0x2: two bases, two\n"
          "      masks, so forwarding this call would have been wrong too");
    CHECK(linux_sig_mask_step(0x402ull, 1, 0x2ull, &new_mask) == 1 &&
          new_mask == 0x400ull,
          "UNBLOCK from the handler's set keeps SIGSEGV blocked, as the\n"
          "      handler's own frame says it is");
    CHECK(linux_sig_mask_step(0x2ull, 1, 0x2ull, &new_mask) == 1 &&
          new_mask == 0ull,
          "…and the same call on the narrow base would have cleared everything");

    /* ═══ 4. Refusals move nothing ═════════════════════════════════════ */
    new_mask = 0xDEADBEEFull;
    CHECK(linux_sig_mask_step(0x400ull, 3, 0, &new_mask) == 0 &&
          new_mask == 0xDEADBEEFull,
          "a `how` Linux does not have (3) refuses with the output untouched");
    CHECK(linux_sig_mask_step(0x400ull, 9, 0, &new_mask) == 0 &&
          new_mask == 0xDEADBEEFull,
          "…and 9 refuses the same way (the sidecar's own `how` twin)");
    CHECK(linux_sig_mask_step(0x400ull, ~0ull, 0, &new_mask) == 0 &&
          new_mask == 0xDEADBEEFull,
          "…and so does any other word");
    CHECK(linux_sig_mask_step(0x400ull, 2, 0x400ull, NULL) == 0,
          "no output pointer is a refusal, never a write through NULL");

    /* ═══ 5. The twin rule: no KILL/STOP filtering ═════════════════════ */
    CHECK(linux_sig_mask_step(0ull, 2, ~0ull, &new_mask) == 1 &&
          new_mask == ~0ull,
          "SETMASK does not strip SIGKILL/SIGSTOP bits — the sidecar's arm\n"
          "      strips none, and a window that filtered would answer a mask\n"
          "      the other end never holds");
    CHECK(linux_sig_mask_step(0x400ull, 1, 0ull, &new_mask) == 1 &&
          new_mask == 0x400ull,
          "UNBLOCK of nothing is the identity");
    CHECK(linux_sig_mask_step(0x400ull, 2, 0ull, &new_mask) == 1 &&
          new_mask == 0ull,
          "SETMASK 0 clears the mask — the handler may lift its own signal");

    printf("\nlinux_sig_mask_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
