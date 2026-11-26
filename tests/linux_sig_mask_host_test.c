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
 * base. Increment 32 named the split; increment 33 closed the reading half
 * by making the kernel the mask's authority for exactly that window, and
 * increment 34 closes the second half — the REVEAL an unmask owes.
 *
 * Sections 1–5 are increment 33's (whose copy answers, with what
 * arithmetic). Section 6 is increment 34's: whether moving to a mask FREES
 * anything (only then can a pending signal be revealed), the single plan the
 * dispatch runs for every rt_sigprocmask, the tag the probe rides, and that
 * the kernel's copy of the mask follows the thread's own blocks — which is
 * what lets a signal blocked BEFORE a fault be freed inside its handler at
 * all.
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

    /* ═══ 6. The unmask that must REVEAL (increment 34) ════════════════ */
    int probe = 0;
    const uint64_t term_bit = 1ull << (15 - 1);   /* SIGTERM's bit, 0x4000 */

    CHECK(term_bit == 0x4000ull,
          "SIGTERM is bit 14 — the bit the live arm frees from a handler");

    /* A bit can only be revealed if it WAS blocked, so the rule is
     * old & ~new: what left the mask. (The inverse — new & ~old — would
     * call every BLOCK a reveal and every UNBLOCK a non-event.) */
    CHECK(linux_sig_mask_reveals(0x400ull, 0x400ull) == 0,
          "nothing left the mask: nothing can be revealed");
    CHECK(linux_sig_mask_reveals(0x400ull, 0ull) == 1,
          "freeing the handler's own signal is a reveal");
    CHECK(linux_sig_mask_reveals(0x400ull, 0x402ull) == 0,
          "SIG_BLOCK only ADDS to the mask, so it never reveals");
    CHECK(linux_sig_mask_reveals(0x402ull, 0x400ull) == 1,
          "SIG_UNBLOCK of a blocked bit is a reveal");
    CHECK(linux_sig_mask_reveals(0ull, ~0ull) == 0,
          "a mask that only grows frees nothing");

    /* The live arm's numbers: the kernel's mask now tracks the thread's own
     * blocked set (0x4000 — SIGTERM blocked BEFORE the fault), the delivery
     * adds SIGSEGV's bit, and the handler frees SIGTERM again. */
    CHECK(linux_sig_mask_reveals(0x4400ull, 0x400ull) == 1,
          "SIGTERM blocked before the fault, freed inside the handler, IS a\n"
          "      reveal — the case the increment exists for");
    CHECK(linux_sig_mask_step(0ull, 0, term_bit, &new_mask) == 1 &&
          new_mask == 0x4000ull,
          "the pre-fault block is a mask the kernel now tracks (it used to\n"
          "      move only at deliveries, so the fault frame lost it)");

    CHECK(linux_sig_mask_fault_plan(0x4400ull, 1, term_bit, &new_mask,
                                    &probe) == 1 &&
          new_mask == 0x400ull && probe == 1,
          "the live arm's unblock moves to 0x400 and owes a probe");
    CHECK(linux_sig_mask_fault_plan(0x400ull, 0, 0x2ull, &new_mask,
                                    &probe) == 1 &&
          new_mask == 0x402ull && probe == 0,
          "BLOCK is arithmetic only: no probe, so no round trip is spent");
    CHECK(linux_sig_mask_fault_plan(0x400ull, 2, 0x400ull, &new_mask,
                                    &probe) == 1 &&
          new_mask == 0x400ull && probe == 0,
          "SETMASK to the mask already in force owes nothing");
    CHECK(linux_sig_mask_fault_plan(0x4400ull, 2, term_bit, &new_mask,
                                    &probe) == 1 &&
          new_mask == term_bit && probe == 1,
          "a SETMASK that drops blocked bits owes a reveal for them");

    new_mask = 0xDEADBEEFull;
    probe = 7;
    CHECK(linux_sig_mask_fault_plan(0x4400ull, 9, 0ull, &new_mask, &probe) == 0 &&
          new_mask == 0xDEADBEEFull && probe == 7,
          "a foreign `how` refuses and writes NEITHER output, so the caller\n"
          "      answers -EINVAL with the mask unmoved");
    CHECK(linux_sig_mask_fault_plan(0x4400ull, 1, term_bit, NULL, NULL) == 0,
          "no output pointer is a refusal, never a write through NULL");
    CHECK(linux_sig_mask_fault_plan(0x4400ull, 1, term_bit, &new_mask, NULL) == 1 &&
          new_mask == 0x400ull,
          "the probe flag is optional; the arithmetic is not");
    {
        uint64_t a = 0, b = 0;
        CHECK(linux_sig_mask_step(0x4400ull, 1, term_bit, &a) == 1 &&
              linux_sig_mask_fault_plan(0x4400ull, 1, term_bit, &b, NULL) == 1 &&
              a == b,
              "the plan's mask IS step()'s — one arithmetic, not two");
    }
    CHECK(LINUX_SIG_REVEAL_TAG == 0x53475256ull,
          "the probe's tag is the one number both ends carry (the sidecar's\n"
          "      SIG_MASK_REVEAL_TAG, held to it by clause AD)");

    /* ═══ 7. The frame a handler LEFT (increment 35) ═══════════════════ */
    /* A delivery enters the handler with rsp == the frame's address and a
     * handler only grows DOWN, so rsp <= frame_rsp is "still inside". A
     * longjmp pops rsp back to the sigsetjmp caller — the interrupted
     * context's side of the frame, strictly ABOVE it — and copies nothing
     * back into the frame, which is why the ADDRESS comparison, not the
     * frame's bytes, is what names the abandonment a 15 will never name.
     * Its one false positive, named and not solved: a handler that runs on a
     * stack ABOVE its frame (makecontext/swapcontext) looks identical here,
     * so it would close the window early — geometry cannot tell the two. */
    CHECK(linux_sig_frame_abandoned(0x7000ull, 0x7000ull) == 0,
          "rsp exactly at the frame is the handler's ENTRY point, not an exit");
    CHECK(linux_sig_frame_abandoned(0x7000ull, 0x6f00ull) == 0,
          "a handler that pushed below its frame (rsp < frame_rsp) is still\n"
          "      inside it — every ordinary 14 of a live handler looks like\n"
          "      this, and none of them may close the window");
    CHECK(linux_sig_frame_abandoned(0x7000ull, 0x7008ull) == 1,
          "one word above the frame is already the setjmp caller: the frame\n"
          "      is behind the task and the window closes with no 15 coming");
    CHECK(linux_sig_frame_abandoned(0ull, 0x7008ull) == 0,
          "no recorded frame is nothing to abandon — an ordinary task has\n"
          "      none. A tkill-delivered frame DOES record one (reply_apply\n"
          "      stores frame_rsp); it is out of the closure because its\n"
          "      fault_active is 0, not because the address is missing");
    CHECK(linux_sig_frame_abandoned(0x7000ull, 0x7040ull) == 1,
          "the live shape: siglongjmp has popped rsp 0x40 above the frame");

    printf("\nlinux_sig_mask_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
