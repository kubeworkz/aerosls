/*
 * tcache_identity_host_test.c — the QEMU-SLS translation cache's build stamp,
 * against the REAL qemu_tcache_identity() from kernel/qemu_sls_tcache.h.
 *
 * ─── What this guards, and why a magic number was not enough ───────────────
 * The translation cache stores HOST MACHINE CODE on NVMe and restores it
 * across reboots. Its header carried only QEMU_TCACHE_MAGIC, which proves that
 * *some* build of this code wrote the region -- not that *this* build did.
 *
 * Restoring a cache produced by a different compiler, a different TCG
 * revision, or different code-generation flags hands the CPU instructions
 * assembled against assumptions that no longer hold. Nothing faults. It simply
 * executes, and the failure appears somewhere unrelated, later, as data
 * corruption or a wild jump.
 *
 * That is the most expensive failure shape this project has met: a wrong state
 * indistinguishable from a right one. So the identity is checked before a
 * single cached byte is restored, and this file checks the identity.
 *
 * ─── Why GOLDEN VALUES and not a recomputation ─────────────────────────────
 * A test that recomputes the hash with the same algorithm asserts only that
 * the algorithm equals itself -- it passes no matter what the algorithm is,
 * including a version that returns a constant. The expected values below were
 * computed once, independently, and are written down. If the mixing changes,
 * if a structural constant changes, or if someone "simplifies" the function,
 * these numbers move and the test says so.
 *
 * AEROSLS_BUILD_ID is forced to a known string by the build line so the
 * goldens are reproducible; production takes it from the Makefile (git hash).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel \
 *       -DAEROSLS_BUILD_ID='"aerosls-test-build-A"' \
 *       -o /tmp/tcache_identity_host_test \
 *       tests/tcache_identity_host_test.c
 *   /tmp/tcache_identity_host_test
 */
#include <stdio.h>
#include <stdint.h>

#include "kernel/qemu_sls_tcache.h"

static int checks_passed = 0, checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* Computed independently of the implementation, for
 * AEROSLS_BUILD_ID="aerosls-test-build-A", CODEBUF_SIZE=4 MiB, MAX_TBS=4096,
 * 64-bit host.
 *
 * Updated for QEMU_TCACHE_FORMAT_VERSION 2, which added a version term to the
 * mixing (and the multiply after the pointer-width xor that the term needs to
 * be distinguishable). This test failed on that change, which is the outcome
 * it was written for -- its own message says any change to the mixing moves
 * the value.
 *
 * The new goldens were NOT read out of the implementation. Copying whatever
 * the code produces makes this assertion vacuous: it would pass by
 * construction even if the mixing were wrong. They were derived from a
 * separate reimplementation of the documented steps, in a different language,
 * which was first checked against the v1 goldens below and reproduced both
 * exactly -- so the reimplementation is faithful and its v2 output is
 * independent evidence rather than an echo.
 *
 *   v1 (pre-format-version): A = 0x393db903c46df50a, B = 0x30943e03bf85f65f
 *
 * This test caught two real defects when it was finally re-run, both of them
 * introduced by the commit that added the softmmu term and never re-ran this
 * file:
 *
 *   1. The goldens were stale, so the suite was red and nobody had looked.
 *   2. The softmmu ON and OFF tags differed in ONE BIT, so the two sides of
 *      the A/B produced identities one bit apart. That passes an equality
 *      check and fails the standard this file argues for twenty lines below:
 *      "a hash that changed a single bit would still differ, and would still
 *      be a bad guard." Adding a multiply took it to 8 bits; separating the
 *      two tags took it to 26.
 */
#define GOLDEN_A 0x413d6c1fa3ed892fULL
#define GOLDEN_B 0xe40e480212dc0928ULL

int main(void) {
    printf("tcache_identity_host_test\n=========================\n\n");

    uint64_t id = qemu_tcache_identity();

    CHECK(id == GOLDEN_A,
          "*** the identity matches its independently-computed value -- any "
          "change to the mixing, or to CODEBUF_SIZE / MAX_TBS / pointer width, "
          "moves it ***");
    CHECK(id == qemu_tcache_identity(),
          "...and is deterministic across calls, so a cache written and read by "
          "the same build always matches itself");
    CHECK(id != 0,
          "...and is not zero, which is what a blank NVMe page reads as");
    CHECK(id != QEMU_TCACHE_MAGIC,
          "...and is not the magic. Two header fields that could hold the same "
          "value would let a corrupt header pass both checks");

    /* The property that matters: a DIFFERENT build id must produce a different
     * identity. Asserted against the second golden rather than by recomputing,
     * for the same reason as above -- see the header note. Build id B differs
     * from A by one character, which is the hardest case: a hash that
     * discriminates only on length or first byte would pass a bigger
     * difference and fail here. */
    printf("\n-- discrimination --\n");
    CHECK(qemu_tcache_identity_of("aerosls-test-build-A") == GOLDEN_A &&
          qemu_tcache_identity_of("aerosls-test-build-B") == GOLDEN_B,
          "*** a build id differing by ONE character yields a different "
          "identity, so a rebuild cannot be mistaken for the same build ***");

    /* The check that a constant-returning implementation cannot survive.
     * Mutation testing found that every assertion above passes if the function
     * simply RETURNS the expected value -- determinism, non-zero, matching the
     * golden, all true of a constant. Only calling it with two different inputs
     * and requiring two different outputs proves it depends on its input at
     * all. This is why the function was parameterised. */
    CHECK(qemu_tcache_identity_of("aerosls-test-build-A") !=
          qemu_tcache_identity_of("aerosls-test-build-B"),
          "*** ...and the function actually DEPENDS on the build id: two calls "
          "with different ids return different values, which no constant "
          "implementation can do ***");

    /* Avalanche: a one-character input change should move most output bits,
     * not one. A hash that changed a single bit would still "differ", and
     * would still be a bad guard -- neighbouring build ids would collide
     * across the space that matters. */
    uint64_t diff = GOLDEN_A ^ GOLDEN_B;
    int bits = 0;
    for (int i = 0; i < 64; i++) if (diff & (1ULL << i)) bits++;
    printf("      (%d of 64 bits differ)\n", bits);
    CHECK(bits >= 16,
          "*** ...and it differs in many bits, not one. 'Different' is not "
          "enough: near-identical build ids must land far apart ***");

    printf("\n=========================\n");
    printf("passed %d, failed %d\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
