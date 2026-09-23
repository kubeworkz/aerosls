/*
 * env_service_wait_host_test.c — the E4 env-create round trip must hand the
 * CPU to Ring-3 while it waits for init's reply (kernel/env_service.c).
 *
 * Links the REAL kernel/env_service.c; the channel pairs and the timer are
 * modelled by the stubs below, and kernel_yield_to_ring3() is the seam that
 * decides whether the fake env manager ever runs.
 *
 * ─── The bug this exists to prevent from returning ─────────────────────────
 * env_service_create() sends ENV_CREATE and then polls for the reply. Its wait
 * loop called smp_uniprocessor_tick() — which kernel/smp.h documents as a no-op
 * the moment an AP is online — and never yielded. In the unified boot Ring-3
 * work runs ONLY while the Ring-0 control plane yields
 * (kernel_yield_to_ring3, kernel/process.h), so during that spin init was
 * never scheduled: it could not even read the request just queued, the
 * deadline expired, and its reply arrived after the kernel had stopped
 * looking. Measured on a live unified boot: 6.25 M polls, 0 messages
 * received, with init's own trace (`req recv'd / reply built / send OK`)
 * printing after the deadline line. Every POST /api/partition/{id}/env then
 * answered `environment manager unavailable or timed out` while init created
 * the environment anyway — and because the next call's stale-reply drain
 * discards that late reply, a retry created a SECOND environment.
 *
 * The fix is the yield in the wait loop. This test's fake manager is faithful
 * to that: it answers only once the control plane has yielded (manager_step()
 * runs from the yield stub), so on the pre-fix loop the reply can never arrive
 * and check 2/3 fail on the deadline.
 *
 * ─── What this test asserts ────────────────────────────────────────────────
 *   1. an unregistered service is refused without sending (registry guard);
 *   2. reply_slot() marks only the registered reply channel — the skip
 *      console_service_tick() relies on to leave ENV replies alone;
 *   3. a manager that only progresses when the control plane yields still
 *      answers: rc == 0, ENV_OK, the env id from the reply (THE DISCRIMINATOR);
 *   4. the wait therefore yielded at least once before the reply arrived,
 *      and finished well inside the deadline (the timeout path was not taken);
 *   5. the request really is the E4 create frame: ENV_CREATE + (partition,
 *      index) in the body;
 *   6. the wait polls only its own reply channel;
 *   7. a DEAD manager cannot hang the HTTP server: the deadline still bounds
 *      it, tick for tick, and the bounded wait keeps yielding to Ring-3;
 *   8. a stale reply left by an earlier timed-out request is drained, not
 *      mistaken for this request's answer;
 *   9. a reply shorter than frame+body is not accepted as success;
 *  10. the E5 destroy round trip shares that wait and yield, and its request
 *      carries the env_id AND the partition the manager enforces;
 *  11. the destroy reply's own env_id/partition are handed back to the caller.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -o /tmp/env_service_wait_host_test \
 *       tests/env_service_wait_host_test.c kernel/env_service.c
 *   /tmp/env_service_wait_host_test
 */
#include "kernel/cap.h"
#include "kernel/env_proto.h"
#include "kernel/env_service.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The kernel's tick source. On a real boot the timer ISR advances it and the
 * spin loop only drains; here smp_uniprocessor_tick() stands in for the ISR so
 * the deadline is deterministic (one iteration == one tick). */
volatile uint64_t kernel_tick_counter = 0;

/* The service's own deadline (kernel/env_service.c). Mirrored so the bound can
 * be asserted exactly; if that constant moves, this line moves with it. */
#define ENV_CREATE_TIMEOUT_TICKS 300u

#define ENV_K_RD 2u
#define ENV_K_WR 3u
#define ENV_ENV_ID 4242u
#define STALE_ENV_ID 999u

enum manager_mode { MANAGER_ALIVE, MANAGER_DEAD, MANAGER_TRUNCATED };

static int      g_mode = MANAGER_ALIVE;
static int      g_sends = 0;
static int      g_yields = 0;
static int      g_foreign_polls = 0;
static uint8_t  g_req[ENV_REQ_MAX];
static uint32_t g_req_len = 0;
static uint32_t g_req_tag = 0;
static uint32_t g_reply_ready = 0;
/* The frame type the fake manager replies with — the service echoes the request
 * type, so a destroy round trip answers with an ENV_DESTROY frame. */
static uint16_t g_reply_ty = ENV_CREATE;

/* The single-slot inbox the service polls (one reply in flight). */
static uint8_t  g_inbox[ENV_REPLY_MAX];
static uint32_t g_inbox_len = 0;
static uint32_t g_inbox_tag = 0;
static int      g_inbox_full = 0;

static int checks_passed = 0;
static int checks_failed = 0;

#define CHECK(cond, msg) do {                                                \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); }                \
    else      { checks_failed++; printf("FAIL: %s\n", msg); }                \
} while (0)

static void reset_manager(int mode) {
    g_mode = mode;
    g_reply_ty = ENV_CREATE;
    g_sends = 0;
    g_yields = 0;
    g_foreign_polls = 0;
    g_req_len = 0;
    g_req_tag = 0;
    g_reply_ready = 0;
    g_inbox_len = 0;
    g_inbox_tag = 0;
    g_inbox_full = 0;
    kernel_tick_counter = 0;
}

/* Build one reply into the inbox: a valid ENV frame + the 12-byte reply body.
 * `env_id` distinguishes a fresh answer from a stale one. When `truncate` is
 * set the payload is the frame alone, which the service must not accept. */
static void enqueue_reply(uint32_t env_id, uint32_t tag, int truncate) {
    uint8_t f[ENV_REPLY_MAX];
    uint32_t len = ENV_REPLY_MAX;
    env_frame_encode(f, g_reply_ty, 0);
    env_put_u16(f, ENV_FRAME_SIZE + 0, ENV_OK);
    env_put_u16(f, ENV_FRAME_SIZE + 2, 0);
    env_put_u32(f, ENV_FRAME_SIZE + 4, env_id);
    env_put_u32(f, ENV_FRAME_SIZE + 8, 1u);
    if (truncate) len = ENV_FRAME_SIZE;
    memcpy(g_inbox, f, len);
    g_inbox_len = len;
    g_inbox_tag = tag;
    g_inbox_full = 1;
}

/* One step of the fake env manager. It runs from the yield stub because that
 * is when Ring-3 work actually runs on a unified boot (and never otherwise). */
static void manager_step(void) {
    if (g_reply_ready || g_req_len == 0) return;   /* one reply per request */
    if (g_mode == MANAGER_DEAD) return;            /* a wedged env manager */
    enqueue_reply(ENV_ENV_ID, g_req_tag, g_mode == MANAGER_TRUNCATED);
    g_reply_ready = 1;
}

/* ─── kernel seams ──────────────────────────────────────────────────────── */
int cap_send_msg(uint32_t pid, uint16_t ch_w_idx, const void* payload,
                 uint32_t payload_len, const struct SLSCapDesc* descs,
                 uint16_t n_caps, uint32_t tag, uint32_t flags) {
    (void)pid; (void)descs; (void)n_caps; (void)flags;
    if (ch_w_idx != ENV_K_WR) return -1;
    g_sends++;
    g_req_len = payload_len <= sizeof g_req ? payload_len : (uint32_t)sizeof g_req;
    memcpy(g_req, payload, g_req_len);
    g_req_tag = tag;
    return 0;
}

int cap_recv_msg(uint32_t pid, uint16_t ch_r_idx, void* buf, uint32_t buf_len,
                 uint32_t* out_payload_len, uint16_t max_caps,
                 struct SLSCapDesc* out_caps, uint16_t* out_n_caps,
                 uint32_t* out_tag, uint32_t* out_flags) {
    (void)pid; (void)max_caps; (void)out_caps;
    if (ch_r_idx != ENV_K_RD) { g_foreign_polls++; return -1; }
    if (!g_inbox_full) return -1;
    uint32_t n = g_inbox_len < buf_len ? g_inbox_len : buf_len;
    memcpy(buf, g_inbox, n);
    if (out_payload_len) *out_payload_len = n;
    if (out_n_caps) *out_n_caps = 0;
    if (out_tag) *out_tag = g_inbox_tag;
    if (out_flags) *out_flags = 0;
    g_inbox_full = 0;
    return 0;
}

void smp_uniprocessor_tick(void) {
    kernel_tick_counter++;   /* stands in for the timer ISR */
}

void kernel_yield_to_ring3(uint32_t budget_ticks) {
    (void)budget_ticks;
    g_yields++;
    manager_step();
}

void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ─── the test ──────────────────────────────────────────────────────────── */
int main(void) {
    uint16_t status = 0xFFFF;
    uint32_t env_id = 0;

    /* 1. an unregistered service cannot round-trip anything. */
    reset_manager(MANAGER_ALIVE);
    CHECK(env_service_create(1, 0, &status, &env_id) == -1 && g_sends == 0,
          "an unregistered env service is refused without sending a request");

    env_service_register((uint16_t)ENV_K_RD, (uint16_t)ENV_K_WR);

    /* 2. only the reply channel is marked, so console_service_tick() skips
     *    exactly one slot. */
    CHECK(env_service_reply_slot((uint16_t)ENV_K_RD) == 1 &&
          env_service_reply_slot((uint16_t)ENV_K_WR) == 0 &&
          env_service_reply_slot(0) == 0,
          "reply_slot() marks only the registered reply channel");

    /* 3-6. THE DISCRIMINATOR: a manager that only runs when the control plane
     *      yields still answers, inside the deadline. */
    reset_manager(MANAGER_ALIVE);
    int rc = env_service_create(7, 3, &status, &env_id);
    CHECK(rc == 0 && status == ENV_OK && env_id == ENV_ENV_ID,
          "a manager that only progresses on a yield still answers (rc 0, ENV_OK)");
    CHECK(g_yields >= 1,
          "the wait handed the CPU to Ring-3 instead of polling a no-op tick");
    CHECK(kernel_tick_counter < ENV_CREATE_TIMEOUT_TICKS,
          "the reply arrived well inside the deadline (the timeout path was not taken)");
    {
        uint16_t ty = 0;
        int framed = env_frame_parse(g_req, g_req_len, &ty);
        CHECK(framed && ty == ENV_CREATE &&
              env_read_u32(g_req, ENV_FRAME_SIZE) == 7 &&
              env_read_u32(g_req, ENV_FRAME_SIZE + 4) == 3,
              "the request is the E4 create frame for (partition 7, index 3)");
    }
    CHECK(g_foreign_polls == 0,
          "the wait polls only its own reply channel");

    /* 7. a wedged manager is still bounded by the deadline, tick for tick —
     *    and the bounded wait keeps yielding. */
    reset_manager(MANAGER_DEAD);
    CHECK(env_service_create(1, 0, &status, &env_id) == -1,
          "a dead env manager cannot hang the HTTP server: the deadline still bounds it");
    CHECK(kernel_tick_counter == ENV_CREATE_TIMEOUT_TICKS,
          "the bounded wait ran exactly ENV_CREATE_TIMEOUT_TICKS ticks");
    CHECK(g_yields >= 1,
          "the bounded wait kept handing the CPU to Ring-3 on its way to the deadline");

    /* 8. a reply left behind by an earlier timed-out request is drained, so it
     *    cannot be reported as THIS request's answer. */
    reset_manager(MANAGER_ALIVE);
    enqueue_reply(STALE_ENV_ID, 0xDEADu, 0);
    status = 0xFFFF; env_id = 0;
    rc = env_service_create(1, 0, &status, &env_id);
    CHECK(rc == 0 && env_id == ENV_ENV_ID,
          "a stale reply left by a timed-out request is drained, not mistaken for the answer");

    /* 9. a short/malformed reply is never taken as success. */
    reset_manager(MANAGER_TRUNCATED);
    CHECK(env_service_create(1, 0, &status, &env_id) == -1,
          "a reply shorter than frame + body is not accepted as an answer");

    /* 10. E5: the destroy round trip shares the same wait (and therefore the
     *     same yield), and its request carries BOTH fields the manager needs
     *     to enforce the route's partition. */
    reset_manager(MANAGER_ALIVE);
    g_reply_ty = ENV_DESTROY;
    uint32_t got_part = 0;
    status = 0xFFFF; env_id = 0;
    rc = env_service_destroy(ENV_ENV_ID, 7, &status, &env_id, &got_part);
    CHECK(rc == 0 && status == ENV_OK && env_id == ENV_ENV_ID,
          "the destroy round trip answers like the create one (rc 0, ENV_OK)");
    CHECK(g_yields >= 1,
          "the destroy wait hands the CPU to Ring-3 too — a destroy is work init does");
    {
        uint16_t rty = 0;
        int framed = env_frame_parse(g_req, g_req_len, &rty);
        CHECK(framed && rty == ENV_DESTROY &&
              env_read_u32(g_req, ENV_FRAME_SIZE) == ENV_ENV_ID &&
              env_read_u32(g_req, ENV_FRAME_SIZE + 4) == 7,
              "the destroy request is an ENV_DESTROY frame for (env_id, partition 7)");
        CHECK(g_req_len == ENV_FRAME_SIZE + ENV_DESTROY_BODY_SIZE,
              "the destroy request is exactly frame + body");
    }

    /* 11. E5: the reply's own fields come back out — the control plane reports
     *     the partition the environment really was in. */
    reset_manager(MANAGER_ALIVE);
    g_reply_ty = ENV_DESTROY;
    enqueue_reply(ENV_ENV_ID, 1u, 0);   /* partition 1 in the reply body */
    status = 0xFFFF; env_id = 0; got_part = 0;
    rc = env_service_destroy(ENV_ENV_ID, 1, &status, &env_id, &got_part);
    CHECK(rc == 0 && got_part == 1,
          "the destroy reply's env_id and partition are parsed for the caller");

    printf("\n%d checks passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
