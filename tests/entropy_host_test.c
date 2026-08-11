/*
 * entropy_host_test.c — kernel/entropy.c: DRBG correctness, health tests,
 * source-failure handling, and the fail-closed contract.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -DENTROPY_HOST_TEST -DENTROPY_TEST_HOOKS \
 *       -I . -I kernel -o /tmp/entropy_host_test \
 *       tests/entropy_host_test.c kernel/entropy.c kernel/sha256.c
 *   /tmp/entropy_host_test
 *
 * ─── What this test is for, and what it cannot do ──────────────────────────
 * It cannot tell you the random numbers are random. Nothing can, from inside
 * the process generating them. What it CAN do is check the two things that
 * actually break in practice:
 *
 *   1. the DRBG is the algorithm it claims to be   -> §1, against vectors
 *   2. the machinery around it fails safely        -> §2-4, against behaviour
 *
 * The gate this test does NOT implement is the third one from design doc
 * §2.5: boot N nodes from an identical image and assert their first DRBG
 * output differs pairwise. That is the test that would have caught Debian, it
 * needs run-cluster.sh and real nodes, and it belongs in a check script rather
 * than here. It is owed. Recorded so that a green run of this file is not
 * mistaken for a verified entropy source.
 *
 * ─── Provenance of the §1 vectors ──────────────────────────────────────────
 * V1 is believed to be NIST CAVP HMAC_DRBG SHA-256, no-reseed, no prediction
 * resistance, COUNT=0. "Believed" is doing real work in that sentence:
 * csrc.nist.gov's drbgtestvectors.zip could not be retrieved from the
 * environment this was written in (403 through the proxy), so the value was
 * not copied out of the .rsp file.
 *
 * What it has instead is triangulation. An HMAC-DRBG was implemented
 * separately in Python directly from the SP 800-90A §10.1.2 algorithm text,
 * and it reproduces all 1024 bits of V1 exactly. Two independent derivations
 * agreeing on 1024 bits is not chance. V2-V5 were then generated from that
 * same Python reference and are therefore weaker evidence -- they check this
 * C code against another implementation of the spec, not against NIST.
 *
 * ─── Two mutations this file does NOT kill, recorded rather than hidden ────
 *   M7  making rd_seed64() report success while returning no data survives,
 *       because the host running this test has a working RDSEED -- the
 *       instruction still fills the register, so "claims success with no data"
 *       never produces no data here. Its CONSEQUENCE (an all-constant source)
 *       is covered by the stuck-source tests in §5; the specific line is not
 *       reachable without simulating instruction-level failure.
 *   M10 changing RDRAND's credit from a quarter to full survives: nothing
 *       asserts the crediting ratio. It is a policy number with real effect on
 *       the seeding threshold and it is currently untested.
 *
 * TODO, and it is a real one: when drbgtestvectors.zip is reachable, diff V1
 * against the published COUNT=0 entry and replace V2-V5 with genuine CAVP
 * entries covering reseed and additional input. Until then this file proves
 * "agrees with a careful second reading", not "validated".
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "entropy.h"

static int checks = 0, fails = 0;

static void hexs(const uint8_t* p, size_t n, char* out) {
    static const char* h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2*i] = h[p[i]>>4]; out[2*i+1] = h[p[i]&15]; }
    out[2*n] = 0;
}
static size_t unhex(const char* s, uint8_t* b) {
    size_t n = 0;
    for (; s[0] && s[1]; s += 2) {
        uint8_t hi = (uint8_t)(s[0] <= '9' ? s[0]-'0' : (s[0]|32)-'a'+10);
        uint8_t lo = (uint8_t)(s[1] <= '9' ? s[1]-'0' : (s[1]|32)-'a'+10);
        b[n++] = (uint8_t)((hi<<4)|lo);
    }
    return n;
}
static void ok(int cond, const char* what) {
    checks++;
    if (cond) printf("ok:   %s\n", what);
    else { printf("  FAIL  %s\n", what); fails++; }
}
static void eq_hex(const uint8_t* got, size_t n, const char* want, const char* what) {
    char g[600];
    checks++;
    hexs(got, n, g);
    if (strcmp(g, want) == 0) printf("ok:   %s\n", what);
    else { printf("  FAIL  %s\n         got  %s\n         want %s\n", what, g, want); fails++; }
}

static const char* EI    = "ca851911349384bffe89de1cbdc46e6831e44d34a4fb935ee285dd14b71a7488";
static const char* NONCE = "659ba96c601dc69fc902940805ec0ca8";

int main(void) {
    uint8_t ei[64], nonce[64], out[128];
    size_t eil, nl;

    eil = unhex(EI, ei);
    nl  = unhex(NONCE, nonce);

    printf("-- 1: HMAC-DRBG against vectors --\n");

    /* V1: instantiate, discard one generate (as CAVP does), compare the
     * second. The discard is not decoration -- it is what makes this a test of
     * the update() path between generates rather than of instantiate alone. */
    entropy_test_reset();
    entropy_test_drbg_instantiate(ei, eil, nonce, nl, 0, 0);
    entropy_test_drbg_generate(out, 128, 0, 0);
    entropy_test_drbg_generate(out, 128, 0, 0);
    eq_hex(out, 128,
        "e528e9abf2dece54d47c7e75e5fe302149f817ea9fb4bee6f4199697d04d5b89"
        "d54fbb978a15b5c443c9ec21036d2460b6f73ebad0dc2aba6e624abf07745bc1"
        "07694bb7547bb0995f70de25d6b29e2d3011bb19d27676c07162c8b5ccde0668"
        "961df86803482cb37ed6d5c0bb8d50cf1f50d476aa0458bdaba806f48be9dcb8",
        "V1: CAVP no-reseed COUNT=0, second generate");

    /* V2: the reseed path. A DRBG whose reseed is wrong still produces
     * excellent-looking output -- it just never absorbs the new entropy,
     * which is the entire point of reseeding. */
    entropy_test_reset();
    {
        uint8_t e2[64], n2[64], r2[64];
        size_t e2l = unhex("79737479ba4e7642a221fcfd1b820b134e9e3540a35bb48ffae29c20f5418ea3", e2);
        size_t n2l = unhex("3593259c092bef4129bc2c6c9e19f343", n2);
        size_t r2l = unhex("c7215b5b56c60ec5b6a4e30e4f2a11e05ab5ab5eb2a4d09c5f1f1c1f3b64d55d", r2);
        entropy_test_drbg_instantiate(e2, e2l, n2, n2l, 0, 0);
        entropy_test_drbg_reseed(r2, r2l, 0, 0);
        entropy_test_drbg_generate(out, 128, 0, 0);
        entropy_test_drbg_generate(out, 128, 0, 0);
        eq_hex(out, 128,
            "a40d3d6db5ec26e2a3f012ed414552e1e5def89194a07453a7882a7e99cb36d5"
            "26d5299aa212593f25bde08fe02a5231b4f968ff9d6cff34bff1067567e6a181"
            "887e8dffc2c8388941a389588ed047fe7221155deb1043060512f9b8bb07fccf"
            "e4869cf79f9ed2351db00525cf8c819cf11d7a95ccc2cda3b50629214ad10dab",
            "V2: reseed then generate");
    }

    /* V3: additional input on both generates. Note the spec calls update()
     * TWICE per generate when addl is present -- once before, once after. An
     * implementation that does it once is a different, wrong generator. */
    entropy_test_reset();
    {
        uint8_t a1[64], a2[64];
        size_t a1l = unhex("2b790052f09b364d4a8267a0a7de63b8f0f10a1d9fadb3f0a2f5b6d1e2c3b4a5", a1);
        size_t a2l = unhex("9a8b7c6d5e4f30211213141516171819a1b2c3d4e5f60718293a4b5c6d7e8f90", a2);
        entropy_test_drbg_instantiate(ei, eil, nonce, nl, 0, 0);
        entropy_test_drbg_generate(out, 128, a1, a1l);
        entropy_test_drbg_generate(out, 128, a2, a2l);
        eq_hex(out, 128,
            "5e6eb10a37ee5b16e2a4644aec5b6dd7efaf55941efb39d4fd59cb62db86e269"
            "4e5aca9ab5c470cafb873a5c528c362b9a05abb8129f8ab220be6b40f114b99f"
            "ee6806d911848a39f0e0001dbf38ed154b438785afd271c3b31833436b4786d4"
            "17ebd999d093e7c864c85a5f5c7566e25519d3e26caf966bbf3548abdb00576e",
            "V3: additional input on each generate");
    }

    /* V4: 20 bytes -- not a multiple of the 32-byte digest, so the last block
     * is truncated. Off-by-one here yields the right length and wrong bytes. */
    entropy_test_reset();
    entropy_test_drbg_instantiate(ei, eil, nonce, nl, 0, 0);
    entropy_test_drbg_generate(out, 20, 0, 0);
    eq_hex(out, 20, "591adfe6e6ee9ba3e7d11ed51db04b3bf9600c17",
        "V4: 20-byte request (partial final block)");

    /* V5: with the personalisation string entropy.c actually uses, so the
     * seeding path's constant is covered and not just the generic one. */
    entropy_test_reset();
    entropy_test_drbg_instantiate(ei, eil, nonce, nl,
        (const uint8_t*)"AeroSLS-entropy-v1", 18);
    entropy_test_drbg_generate(out, 32, 0, 0);
    eq_hex(out, 32,
        "8307dd967dfb761af555315f806912073faefd444d31019763ed495cf621434b",
        "V5: with the personalisation string entropy.c uses");

    printf("\n-- 2: the fail-closed contract --\n");

    entropy_test_reset();
    ok(entropy_get(out, 32) == ENTROPY_E_NOT_SEEDED,
       "entropy_get() before init refuses");
    ok(entropy_is_ready() == 0, "entropy_is_ready() is false before init");

    /* The buffer must be untouched on refusal. A caller that ignores the
     * return code should get its own recognisable bytes back, not something
     * that could pass for a key. */
    entropy_test_reset();
    for (int i = 0; i < 32; i++) out[i] = 0xA5;
    entropy_get(out, 32);
    {
        int untouched = 1;
        for (int i = 0; i < 32; i++) if (out[i] != 0xA5) untouched = 0;
        ok(untouched, "a refused entropy_get() leaves the buffer untouched");
    }

    entropy_test_reset();
    ok(entropy_get(0, 32)   == ENTROPY_E_BADARG, "null buffer is rejected");
    entropy_test_reset();
    ok(entropy_get(out, 0)  == ENTROPY_E_BADARG, "zero length is rejected");

    printf("\n-- 3: source failure is survivable, and honest about it --\n");

    /* Every hardware source dead. On x86 this leaves jitter; the node must
     * still come up, because that is the ARM64 case permanently. */
    entropy_test_reset();
    entropy_test_force_source_failure(1, 1, 0);
    ok(entropy_init() == ENTROPY_OK,
       "with RDSEED and RDRAND both dead, jitter alone still seeds");
    ok(entropy_get(out, 32) == ENTROPY_OK, "and entropy_get() then works");

    /* Everything dead, including jitter. This is the case that must refuse:
     * there is genuinely nothing, and proceeding would be the whole bug. */
    entropy_test_reset();
    entropy_test_force_source_failure(1, 1, 1);
    ok(entropy_init() == ENTROPY_E_NOT_SEEDED,
       "with every source dead, init REFUSES rather than seeding from nothing");
    ok(entropy_is_ready() == 0, "and the node reports not-ready");
    ok(entropy_get(out, 32) == ENTROPY_E_NOT_SEEDED,
       "and entropy_get() still refuses afterwards");

    /* Status must reflect what actually happened, not what was hoped. */
    entropy_test_reset();
    entropy_test_force_source_failure(1, 1, 1);
    entropy_init();
    {
        entropy_status_t st;
        entropy_get_status(&st);
        ok(st.ready == 0, "status reports not-ready when seeding failed");
        ok(st.pool_bits < 8u * ENTROPY_SEED_BYTES,
           "status reports the pool below threshold rather than rounding up");
    }

    printf("\n-- 4: output actually varies --\n");

    /* Weak, but it catches the stupidest and most catastrophic failure: a
     * generator that returns the same bytes every time. */
    entropy_test_reset();
    entropy_init();
    {
        uint8_t a[32], b[32];
        entropy_get(a, 32);
        entropy_get(b, 32);
        ok(memcmp(a, b, 32) != 0, "two successive entropy_get() calls differ");

        int all_same = 1;
        for (int i = 1; i < 32; i++) if (a[i] != a[0]) all_same = 0;
        ok(!all_same, "output is not a single repeated byte");

        int nonzero = 0;
        for (int i = 0; i < 32; i++) if (a[i]) nonzero = 1;
        ok(nonzero, "output is not all zero");
    }

    /* Two independently seeded generators must diverge. This is the in-process
     * shadow of the boot-diversity gate -- much weaker, since both draw from
     * the same machine in the same second, but it fails loudly if seeding is
     * a constant. */
    {
        uint8_t a[32], b[32];
        entropy_test_reset(); entropy_init(); entropy_get(a, 32);
        entropy_test_reset(); entropy_init(); entropy_get(b, 32);
        ok(memcmp(a, b, 32) != 0,
           "two independent init()+get() sequences differ (weak proxy for the "
           "cross-boot gate, which is still owed)");
    }

    printf("\n-- 5: health tests reject a stuck source --\n");

    /* A source emitting a constant must be caught by the repetition count
     * test. Fed through entropy_add() so the public path is exercised. */
    entropy_test_reset();
    {
        entropy_status_t before, after;
        entropy_get_status(&before);
        uint8_t stuck[256];
        for (int i = 0; i < 256; i++) stuck[i] = 0x00;
        /* entropy_add credits nothing by default, so a stuck source cannot
         * inflate the pool even before the health tests see it. */
        entropy_add(stuck, sizeof stuck, 0);
        entropy_get_status(&after);
        ok(after.pool_bits == before.pool_bits,
           "entropy_add() with a zero estimate credits zero bits");
    }

    /* A stuck source: RDSEED reports success and returns the same value every
     * time. This is the common real hardware failure and the one "the source
     * returns nothing" does not simulate at all -- a stuck source looks
     * perfectly healthy to everything downstream of the conditioner, because
     * SHA-256 of a constant is uniform-looking garbage.
     *
     * Added because mutation testing found nothing covered it: making
     * rd_seed64() claim success while returning no data killed 0 of 22 checks.
     * A hardware source that lies is exactly the case this subsystem exists to
     * survive, and it had no test. */
    entropy_test_reset();
    entropy_test_force_stuck_source(1, 0);
    entropy_init();
    {
        entropy_status_t st;
        entropy_get_status(&st);
#if defined(__x86_64__)
        ok(st.health_failures > 0,
           "a stuck RDSEED trips a health test rather than being absorbed");
        ok(st.have_rdseed == 0,
           "and the stuck source is dropped, not left contributing");
#else
        /* No RDSEED to be stuck on this architecture; asserting otherwise
         * would be asserting the test harness, not the kernel. */
        ok(st.have_rdseed == 0, "no RDSEED on this architecture (nothing to stick)");
        ok(1, "stuck-source test is x86-only");
#endif
    }

    /* The pool must not credit a stuck source's bytes. Catching it in the
     * health test and then counting the bits anyway would be the same bug
     * wearing a hat. */
    entropy_test_reset();
    entropy_test_force_stuck_source(1, 1);
    entropy_test_force_source_failure(0, 0, 1);     /* jitter off: isolate it */
    ok(entropy_init() == ENTROPY_E_NOT_SEEDED,
       "with only stuck hardware sources and no jitter, init REFUSES");

    printf("\n=== %d passed, %d failed ===\n", checks - fails, fails);
    return fails == 0 ? 0 : 1;
}
