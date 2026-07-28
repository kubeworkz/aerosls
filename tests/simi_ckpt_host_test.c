/*
 * simi_ckpt_host_test.c — verification for kernel/simi_ckpt.c, checkpoint
 * and restore of a live SIMI execution context. Links the REAL, unmodified
 * kernel/simi_ckpt.c and kernel/simi_interp.c.
 *
 * ─── The failure mode this file exists to catch ──────────────────────
 * Checkpointing fails by OMISSION. Forget one field in the serialiser and
 * nothing crashes: the checkpoint writes, the restore succeeds, and the
 * resumed computation quietly produces a different answer -- or the same
 * answer on the programs you happened to try. That is a silent-wrong-result
 * class, so a handful of round-trip spot checks is not adequate coverage.
 *
 * Scenario 2 is therefore the centrepiece: checkpoint at EVERY instruction
 * boundary of a program, restore each one into a fresh context, run it to
 * completion, and require every single one to produce the identical result.
 * A field that is live only during a narrow window -- mid-loop, one frame
 * deep, between a CALL and its RET -- is exactly what that catches and what
 * spot checks miss.
 *
 * Scenario 3 covers the other half: every rejection path. A checkpoint
 * layer that accepts a corrupt or mismatched payload is worse than one that
 * refuses to restore at all, because it resumes execution against state
 * that means something else.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I tools/simi \
 *       -o /tmp/simi_ckpt_host_test \
 *       tests/simi_ckpt_host_test.c kernel/simi_ckpt.c kernel/simi_interp.c tools/simi/simi_obj.c
 *   /tmp/simi_ckpt_host_test
 */
#include "tools/simi/simi_isa.h"
#include "tools/simi/simi_obj.h"
#include "kernel/simi_interp.h"
#include "kernel/simi_ckpt.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* Catalog bindings: same mock the reference interpreter uses, so corpus
 * programs touching RESOLVE/OBJSIZE/OBJTYPE behave identically. */
static const struct { const char* name; uint64_t base; uint32_t size; uint32_t type; }
g_mock[] = { { "simi_add_test2", 0x2000, 303, 1 }, { "simi_verify3", 0x3000, 100, 2 } };
#define MOCK_N (sizeof(g_mock)/sizeof(g_mock[0]))
uint64_t simi_rt_resolve(const char* n) {
    for (size_t i = 0; i < MOCK_N; i++) if (strcmp(g_mock[i].name, n) == 0) return g_mock[i].base;
    return 0;
}
uint64_t simi_rt_objsize(uint64_t v) {
    for (size_t i = 0; i < MOCK_N; i++) if (g_mock[i].base == v) return g_mock[i].size;
    return 0;
}
uint64_t simi_rt_objtype(uint64_t v) {
    for (size_t i = 0; i < MOCK_N; i++) if (g_mock[i].base == v) return g_mock[i].type;
    return 0xFFFFFFFFu;
}
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

static struct SimiContext ctx_a, ctx_b;
static uint8_t ckbuf[512 * 1024];

/* Runs a context to completion, returning its final status. */
static SimiStatus finish(struct SimiContext* c) {
    SimiStatus st = SIMI_STATUS_OK;
    for (int g = 0; g < 200000 && st == SIMI_STATUS_OK; g++) st = simi_interp_run(c, 64);
    return st;
}

int main(void) {
    printf("=== SIMI checkpoint / restore ===\n\n");

    SimiObject obj = {0};
    if (simi_obj_read("tools/simi/tests/loop_sum.tmo", &obj) != 0) {
        printf("FAIL: cannot read corpus program\n");
        return 1;
    }
    /* A second, DIFFERENT program, for the cross-image rejection test. */
    SimiObject other = {0};
    int have_other = (simi_obj_read("tools/simi/tests/call_ret.tmo", &other) == 0);

    /* Ground truth: one uninterrupted run. */
    simi_interp_init(&ctx_a, &obj, "main");
    SimiStatus st = finish(&ctx_a);
    uint64_t truth = ctx_a.result;
    CHECK(st == SIMI_STATUS_HALTED, "baseline: program runs to completion uninterrupted");
    printf("      (uninterrupted result: %lld in %llu steps)\n",
           (long long)(int64_t)truth, (unsigned long long)ctx_a.steps);

    /* ── Scenario 1: size accounting is exact and depth-proportional ─── */
    {
        simi_interp_init(&ctx_a, &obj, "main");
        uint32_t sz = simi_ckpt_size(&ctx_a);
        uint32_t written = 0;
        CHECK(simi_ckpt_save(&ctx_a, ckbuf, sizeof ckbuf, &written) == SIMI_CKPT_OK,
              "save succeeds into a large-enough buffer");
        CHECK(written == sz, "simi_ckpt_size() predicts the written length exactly");
        CHECK(sz < sizeof(struct SimiContext),
              "a shallow context checkpoints to far less than sizeof(SimiContext) -- depth is paid for, not reserved");
        printf("      (depth-1 checkpoint: %u B = %u NVMe frames; struct is %u B)\n",
               sz, (sz + 4095) / 4096, (unsigned)sizeof(struct SimiContext));

        /* One byte short must be refused, not truncated silently. */
        CHECK(simi_ckpt_save(&ctx_a, ckbuf, sz - 1, NULL) == SIMI_CKPT_ERR_BUFFER_TOO_SMALL,
              "a buffer one byte too small is refused rather than partially written");
    }

    /* ── Scenario 2: THE proof -- checkpoint at every instruction ─────
     * If any execution state lives outside what the serialiser writes,
     * some boundary will expose it. Restoring into a context that has
     * been deliberately POISONED first is what makes this strict: the
     * restored state must come entirely from the checkpoint, not survive
     * in ctx_b from a previous iteration. */
    {
        int all_ok = 1, first_bad = -1, boundaries = 0;

        for (uint64_t cut = 0; cut < 200; cut++) {
            simi_interp_init(&ctx_a, &obj, "main");
            SimiStatus s1 = SIMI_STATUS_OK;
            for (uint64_t i = 0; i < cut && s1 == SIMI_STATUS_OK; i++)
                s1 = simi_interp_run(&ctx_a, 1);
            if (s1 != SIMI_STATUS_OK) break;      /* ran past the end of the program */
            boundaries++;

            uint32_t n = 0;
            if (simi_ckpt_save(&ctx_a, ckbuf, sizeof ckbuf, &n) != SIMI_CKPT_OK) {
                all_ok = 0; if (first_bad < 0) first_bad = (int)cut; continue;
            }
            memset(&ctx_b, 0xA5, sizeof(ctx_b));   /* poison: nothing may survive */
            if (simi_ckpt_load(&ctx_b, ckbuf, n, &obj) != SIMI_CKPT_OK) {
                all_ok = 0; if (first_bad < 0) first_bad = (int)cut; continue;
            }

            /* IDENTITY, not just equivalence. Comparing only the final
             * result is too weak: loop_sum recomputes 55 even if `pc` is
             * lost, because restarting from the top re-initialises its
             * accumulators. An injected "forget to restore pc" bug passed
             * a result-only version of this test. The restored context
             * must equal the saved one field for field. `obj` is excluded
             * -- it is a host pointer, deliberately re-bound rather than
             * serialised (see simi_ckpt.h). */
            struct SimiContext expect_ctx = ctx_a;
            expect_ctx.obj = ctx_b.obj;
            if (memcmp(&expect_ctx, &ctx_b, sizeof(expect_ctx)) != 0) {
                all_ok = 0; if (first_bad < 0) first_bad = (int)cut; continue;
            }

            if (finish(&ctx_b) != SIMI_STATUS_HALTED || ctx_b.result != truth) {
                all_ok = 0; if (first_bad < 0) first_bad = (int)cut;
            }
        }
        CHECK(boundaries > 5, "the program has multiple instruction boundaries to checkpoint at");
        CHECK(all_ok,
              "STATE COMPLETENESS: at EVERY instruction boundary, the restored context is byte-identical to the saved one AND finishes with the identical result");
        if (!all_ok) printf("      (first divergent boundary: instruction %d)\n", first_bad);
        printf("      (%d boundaries checkpointed and resumed, all -> %lld)\n",
               boundaries, (long long)(int64_t)truth);
    }

    /* ── Scenario 3: every rejection path ────────────────────────────
     * Each of these, if accepted, resumes execution against state that
     * means something other than what it says. */
    {
        simi_interp_init(&ctx_a, &obj, "main");
        simi_interp_run(&ctx_a, 5);
        uint32_t n = 0;
        simi_ckpt_save(&ctx_a, ckbuf, sizeof ckbuf, &n);

        static uint8_t tmp[sizeof(ckbuf)];
        struct SimiCkptHeader* h = (struct SimiCkptHeader*)tmp;

        memcpy(tmp, ckbuf, n);
        CHECK(simi_ckpt_load(&ctx_b, tmp, n, &obj) == SIMI_CKPT_OK,
              "control: an untampered checkpoint loads");

        memcpy(tmp, ckbuf, n); h->magic ^= 1;
        CHECK(simi_ckpt_load(&ctx_b, tmp, n, &obj) == SIMI_CKPT_ERR_BAD_MAGIC,
              "rejects a buffer that is not a checkpoint");

        memcpy(tmp, ckbuf, n); h->format_version += 1;
        CHECK(simi_ckpt_load(&ctx_b, tmp, n, &obj) == SIMI_CKPT_ERR_BAD_VERSION,
              "rejects a checkpoint written by an incompatible build");

        memcpy(tmp, ckbuf, n); h->isa_fingerprint ^= 0xFF;
        CHECK(simi_ckpt_load(&ctx_b, tmp, n, &obj) == SIMI_CKPT_ERR_ISA_MISMATCH,
              "rejects a checkpoint whose ISA numbering differs -- pc is an instruction INDEX, so this would resume executing something else entirely");

        memcpy(tmp, ckbuf, n); h->frame_bytes += 8;
        CHECK(simi_ckpt_load(&ctx_b, tmp, n, &obj) == SIMI_CKPT_ERR_LAYOUT_MISMATCH,
              "rejects a changed SimiFrame layout (e.g. a different NREGS)");

        memcpy(tmp, ckbuf, n); h->program_hash ^= 0x1234;
        CHECK(simi_ckpt_load(&ctx_b, tmp, n, &obj) == SIMI_CKPT_ERR_PROGRAM_MISMATCH,
              "rejects restoring against a different program image");

        memcpy(tmp, ckbuf, n); tmp[n - 1] ^= 0xFF;
        CHECK(simi_ckpt_load(&ctx_b, tmp, n, &obj) == SIMI_CKPT_ERR_CHECKSUM,
              "rejects a corrupt payload -- a single flipped byte in memory is caught");

        memcpy(tmp, ckbuf, n);
        CHECK(simi_ckpt_load(&ctx_b, tmp, n - 1, &obj) == SIMI_CKPT_ERR_TRUNCATED,
              "rejects a truncated buffer");
        CHECK(simi_ckpt_load(&ctx_b, tmp, 4, &obj) == SIMI_CKPT_ERR_TRUNCATED,
              "rejects a buffer too short even for the header");

        memcpy(tmp, ckbuf, n); h->num_frames += 1;
        CHECK(simi_ckpt_load(&ctx_b, tmp, n, &obj) == SIMI_CKPT_ERR_BAD_STATE,
              "rejects a header whose frame_top and num_frames disagree");

        /* A real, well-formed checkpoint of a DIFFERENT program must be
         * refused against this image -- the cross-image hazard, using two
         * genuine programs rather than a tampered field. */
        if (have_other) {
            simi_interp_init(&ctx_a, &other, "main");
            simi_interp_run(&ctx_a, 3);
            uint32_t m = 0;
            simi_ckpt_save(&ctx_a, tmp, sizeof tmp, &m);
            CHECK(simi_ckpt_load(&ctx_b, tmp, m, &obj) == SIMI_CKPT_ERR_PROGRAM_MISMATCH,
                  "a genuine checkpoint of ANOTHER program is refused against this one");
        }
    }

    /* ── Scenario 4: a rejected load leaves the target untouched ──────
     * Otherwise a failed restore would half-destroy a live context. */
    {
        simi_interp_init(&ctx_b, &obj, "main");
        simi_interp_run(&ctx_b, 4);
        struct SimiContext before;
        memcpy(&before, &ctx_b, sizeof(before));

        static uint8_t bad[256];
        memset(bad, 0, sizeof bad);
        SimiCkptStatus s = simi_ckpt_load(&ctx_b, bad, sizeof bad, &obj);
        CHECK(s != SIMI_CKPT_OK, "setup: a garbage buffer is rejected");
        CHECK(memcmp(&before, &ctx_b, sizeof(before)) == 0,
              "VERIFY-BEFORE-COMMIT: a rejected restore leaves the destination context byte-identical, not half-overwritten");
    }

    /* ── Scenario 5: a halted context round-trips as halted ───────────
     * A finished computation must not come back runnable. */
    {
        simi_interp_init(&ctx_a, &obj, "main");
        finish(&ctx_a);
        CHECK(ctx_a.status == SIMI_STATUS_HALTED, "setup: context has halted");
        uint32_t n = 0;
        simi_ckpt_save(&ctx_a, ckbuf, sizeof ckbuf, &n);
        memset(&ctx_b, 0xA5, sizeof(ctx_b));
        CHECK(simi_ckpt_load(&ctx_b, ckbuf, n, &obj) == SIMI_CKPT_OK, "halted context checkpoints and restores");
        CHECK(ctx_b.status == SIMI_STATUS_HALTED && ctx_b.result == truth,
              "...and comes back HALTED with its result intact, not runnable again");
        CHECK(simi_interp_run(&ctx_b, 100) == SIMI_STATUS_HALTED && ctx_b.steps == ctx_a.steps,
              "...retiring zero further instructions when run");
    }

    /* ── Scenario 6: deep call state survives ─────────────────────────
     * The frame stack is the part most likely to be serialised wrongly,
     * since only LIVE frames are written. */
    {
        if (have_other) {
            simi_interp_init(&ctx_a, &other, "main");
            SimiStatus s = SIMI_STATUS_OK;
            int found_depth = 0;
            for (int i = 0; i < 200 && s == SIMI_STATUS_OK; i++) {
                s = simi_interp_run(&ctx_a, 1);
                if (ctx_a.frame_top > 0) { found_depth = 1; break; }
            }
            CHECK(found_depth, "setup: reached a call depth greater than 1");
            if (found_depth) {
                uint32_t n = 0;
                CHECK(simi_ckpt_save(&ctx_a, ckbuf, sizeof ckbuf, &n) == SIMI_CKPT_OK,
                      "a context inside a call frame checkpoints");
                uint32_t expect = (uint32_t)sizeof(struct SimiCkptHeader)
                                + (uint32_t)(ctx_a.frame_top + 1) * (uint32_t)sizeof(struct SimiFrame)
                                + (uint32_t)sizeof(ctx_a.mem);
                CHECK(n == expect, "...writing exactly the live frames, not the whole frame array");

                simi_interp_init(&ctx_b, &other, "main");
                uint64_t other_truth = 0;
                finish(&ctx_b); other_truth = ctx_b.result;

                memset(&ctx_b, 0xA5, sizeof(ctx_b));
                CHECK(simi_ckpt_load(&ctx_b, ckbuf, n, &other) == SIMI_CKPT_OK, "...restores");
                CHECK(finish(&ctx_b) == SIMI_STATUS_HALTED && ctx_b.result == other_truth,
                      "...and resuming from mid-call produces the correct final result");
            }
        }
    }

    simi_obj_free(&obj);
    if (have_other) simi_obj_free(&other);

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
