/*
 * fault_report_host_test.c — verification for kernel/fault_report.h's two
 * fault-classification predicates, against the REAL, unmodified header.
 *
 * ─── Why this test exists ───────────────────────────────────────────────
 * A node printed exactly this and nothing else:
 *
 *     [FAULT] Kernel fault  cs=0x8  error=0x0  rip=0xcdcdcdcdcdcdcdcd — Halting.
 *
 * Reading that line correctly requires two mechanical facts, and getting
 * either one wrong sends the investigation to the wrong subsystem:
 *
 *   - 0xcdcdcdcdcdcdcdcd is non-canonical, so the CPU never fetched from it.
 *     The #GP happened while LOADING rip, which is what a smashed return
 *     address looks like -- not a jump into corrupted code, and not something
 *     wrong with the code at that address, because there is no such address.
 *     This is also why the error code is 0.
 *   - The value is one byte eight times, so it is data, and 0xCD identified
 *     the uploaded payload in a single step.
 *
 * The kernel now says both out loud. This test pins the boundaries, because a
 * predicate that is almost right here is worse than none: it would confidently
 * mislabel a real address as corruption, or a corrupted one as a real address,
 * at precisely the moment nobody can afford to double-check it.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -o /tmp/fault_report_host_test \
 *       tests/fault_report_host_test.c
 *   /tmp/fault_report_host_test
 */
#include "kernel/fault_report.h"
#include <stdio.h>
#include <stdint.h>

static int checks_passed = 0, checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (cond) { checks_passed++; printf("  ok   %s\n", msg); } \
    else      { checks_failed++; printf("  FAIL %s\n", msg); } \
} while (0)

int main(void) {
    printf("=== fault_report.h: classifying a kernel fault's rip ===\n\n");

    /* ─── The incident value ──────────────────────────────────────────── */
    printf("-- the value from the real fault --\n");
    CHECK(fault_poison_byte(0xcdcdcdcdcdcdcdcdULL) == 0xCD,
          "*** 0xcdcdcdcdcdcdcdcd is recognised as the byte 0xCD repeated ***");
    CHECK(fault_noncanonical(0xcdcdcdcdcdcdcdcdULL) == 1,
          "*** ...and as non-canonical, so the CPU never fetched from it ***");

    /* ─── Poison detection: it must not fire on real addresses ────────── */
    printf("\n-- poison detection --\n");
    CHECK(fault_poison_byte(0x00000000001041a0ULL) == -1,
          "*** a plausible kernel text address is NOT reported as a pattern ***");
    CHECK(fault_poison_byte(0xffffffffffffffffULL) == 0xFF,
          "all-ones is the pattern 0xFF (freed/uninitialised memory)");
    CHECK(fault_poison_byte(0x0000000000000000ULL) == 0x00,
          "zero is reported as the pattern 0x00, not as 'no pattern' -- a null "
          "return address is a corruption report too");
    CHECK(fault_poison_byte(0xdeadbeefdeadbeefULL) == -1,
          "a repeating 4-byte motif is not an 8-byte one-byte pattern");
    CHECK(fault_poison_byte(0xcdcdcdcdcdcdcdceULL) == -1,
          "*** one byte off in the LOW byte breaks the pattern ***");
    CHECK(fault_poison_byte(0xcecdcdcdcdcdcdcdULL) == -1,
          "*** one byte off in the HIGH byte breaks it too -- the loop really "
          "covers all eight lanes ***");
    CHECK(fault_poison_byte(0x4141414141414141ULL) == 0x41,
          "'A' repeated is found (a classic overflow filler)");

    /* ─── Canonicality: the two boundaries are the whole content ──────── */
    printf("\n-- canonical address boundaries --\n");
    CHECK(fault_noncanonical(0x0000000000100000ULL) == 0,
          "the kernel's own load address (1 MiB) is canonical");
    CHECK(fault_noncanonical(0x00007fffffffffffULL) == 0,
          "*** 0x0000_7fff_ffff_ffff -- the LAST canonical low-half address ***");
    CHECK(fault_noncanonical(0x0000800000000000ULL) == 1,
          "*** 0x0000_8000_0000_0000 -- the FIRST non-canonical address ***");
    CHECK(fault_noncanonical(0xffff800000000000ULL) == 0,
          "*** 0xffff_8000_0000_0000 -- the first canonical high-half address ***");
    CHECK(fault_noncanonical(0xffff7fffffffffffULL) == 1,
          "*** one below that is non-canonical -- the high half's own boundary ***");
    CHECK(fault_noncanonical(0xffffffffffffffffULL) == 0,
          "all-ones is canonical (sign-extended), so 'poisoned' and "
          "'non-canonical' are genuinely independent tests");
    CHECK(fault_noncanonical(0x0000000000000000ULL) == 0,
          "...and so is zero -- a null rip faults on the fetch, not on the load");

    /* The two predicates answering differently on the same value is the point:
     * reporting only one of them would have left half the diagnosis on the
     * floor in the incident that prompted this file. */
    printf("\n-- the two are independent --\n");
    CHECK(fault_poison_byte(0xffffffffffffffffULL) == 0xFF &&
          fault_noncanonical(0xffffffffffffffffULL) == 0,
          "*** 0xffff...ff is poisoned AND canonical ***");
    CHECK(fault_poison_byte(0x0000900000000000ULL) == -1 &&
          fault_noncanonical(0x0000900000000000ULL) == 1,
          "*** a non-poisoned non-canonical value is still reported as the "
          "smashed-pointer case ***");

    /* ─── Locating the interrupt frame ────────────────────────────────── */
    printf("\n-- finding the interrupt frame by its rip/cs pair --\n");
    {
        /* A plausible handler stack: some prologue junk, then the five-qword
         * frame x86-64 pushes on every interrupt. */
        uint64_t stk[16] = {
            0x0000000007000000ULL, 0x000000000010029aULL, 0x000000000000000cULL,
            0x0000000000000000ULL,                       /* error code */
            0xcdcdcdcdcdcdcdcdULL,                       /* [4] RIP */
            0x0000000000000008ULL,                       /* [5] CS */
            0x0000000000000206ULL,                       /* [6] RFLAGS */
            0x0000000007ff6920ULL,                       /* [7] RSP  <- the prize */
            0x0000000000000010ULL,                       /* [8] SS */
            0, 0, 0, 0, 0, 0, 0
        };
        const uint64_t* f = fault_find_iret_frame(stk, stk + 16,
                                                  0xcdcdcdcdcdcdcdcdULL, 0x8ULL);
        CHECK(f == &stk[4],
              "*** the frame is found by the adjacent rip/cs pair ***");
        CHECK(f && f[3] == 0x0000000007ff6920ULL,
              "*** ...and rip_slot[3] really is the interrupted rsp -- the value the "
              "first version of this dump stopped three qwords short of ***");

        CHECK(fault_find_iret_frame(stk, stk + 16, 0xdeadbeefULL, 0x8ULL) == 0,
              "*** a rip that is not on the stack returns 0 rather than a guess ***");
        CHECK(fault_find_iret_frame(stk, stk + 16, 0xcdcdcdcdcdcdcdcdULL, 0x1BULL) == 0,
              "...a matching rip with the WRONG cs is not accepted -- the pair is the "
              "signature, not the rip alone");
        CHECK(fault_find_iret_frame(stk + 5, stk + 16, 0xcdcdcdcdcdcdcdcdULL, 0x8ULL) == 0,
              "...searching from past the frame does not find it (the scan is upward "
              "only, so it can never report a frame below the handler)");

        /* The last qword in range cannot be a frame start: reading its CS would
         * be an over-read. The bound has to exclude it. */
        uint64_t edge[2] = { 0xcdcdcdcdcdcdcdcdULL, 0x8ULL };
        CHECK(fault_find_iret_frame(edge, edge + 1, 0xcdcdcdcdcdcdcdcdULL, 0x8ULL) == 0,
              "*** a candidate whose CS would fall outside the limit is rejected, not "
              "read past the end ***");
        CHECK(fault_find_iret_frame(edge, edge + 2, 0xcdcdcdcdcdcdcdcdULL, 0x8ULL) == edge,
              "...and is accepted once the limit actually covers the pair");
    }

    /* ─── Measuring the overrun ───────────────────────────────────────── */
    printf("\n-- the poison run, which is what names the buffer --\n");
    {
        /* A 4 KiB page buffer that overran into the frame above it: 512 qwords
         * of payload, then the smashed [saved rbp][return address] pair, and
         * real stack contents below the buffer's base. */
        enum { N = 600, BASE = 40, TOP = BASE + 512 };
        static uint64_t stk[N];
        for (int i = 0; i < N; i++) stk[i] = 0x1111111111111111ULL * (uint64_t)(i + 1);
        for (int i = BASE; i < TOP + 2; i++) stk[i] = 0xcdcdcdcdcdcdcdcdULL;

        /* sp is where the epilogue left it: just past the popped pair. */
        const uint64_t* sp = &stk[TOP + 2];
        const uint64_t* base = fault_poison_run_base(sp - 1, stk, 0xcdcdcdcdcdcdcdcdULL);
        CHECK(base == &stk[BASE],
              "*** the run's low end is the base of the overflowing buffer ***");
        CHECK((const char*)sp - (const char*)base == (512 + 2) * 8,
              "*** ...and its length measures the overrun: 4096 bytes of buffer plus "
              "the 16 bytes of frame it destroyed ***");

        /* A single poisoned qword is a run of one -- an overwrite that reached
         * exactly the return address and no further. */
        for (int i = 0; i < N; i++) stk[i] = 0x2222222222222222ULL;
        stk[100] = 0xcdcdcdcdcdcdcdcdULL;
        CHECK(fault_poison_run_base(&stk[100], stk, 0xcdcdcdcdcdcdcdcdULL) == &stk[100],
              "*** a lone poisoned qword reports itself, not the qword below it ***");

        /* The floor must hold even when everything below is the pattern -- this
         * runs on a stack that is already known-bad, so walking off the bottom
         * would mean faulting while diagnosing a fault. */
        for (int i = 0; i < N; i++) stk[i] = 0xcdcdcdcdcdcdcdcdULL;
        CHECK(fault_poison_run_base(&stk[N - 1], stk, 0xcdcdcdcdcdcdcdcdULL) == stk,
              "*** an all-poison stack stops exactly at the floor ***");
        CHECK(fault_poison_run_base(&stk[N - 1], &stk[300], 0xcdcdcdcdcdcdcdcdULL)
                  == &stk[300],
              "...and at a raised floor, so the bound is the argument and not a "
              "coincidence of the data");
        CHECK(fault_poison_run_base(stk, stk, 0xcdcdcdcdcdcdcdcdULL) == stk,
              "...starting at the floor reads nothing below it");
    }

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
