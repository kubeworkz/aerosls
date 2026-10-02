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
 * ─── P1a (v0.2): the create path now also REGISTERS a checkpoint record ─────
 * A create is followed by a second round trip — ENV_REGISTER — and the clauses
 * below are the evidence for it. They are here, not in a test of their own,
 * because the property that matters is a property of the CREATE: every
 * environment the control plane creates ends up in env_ckpt_table[], or the
 * one place that makes environments cannot claim to be checkpointable.
 *
 *  12. a create issues ENV_REGISTER after its reply, carrying the (env_id,
 *      partition) the reply named — not a hardcoded pair;
 *  13. the registration init answers with lands in the REAL
 *      env_ckpt_table[]: the record names the environment, carries its three
 *      kind-labelled regions and four channels, and is stamped with the
 *      kernel's own sequence and console binding;
 *  14. each task's entry is the kernel's — the sidecar's `user_rip` from the
 *      process table, resolved by the name init sent — because where a sidecar
 *      was released is not a fact init holds.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -o /tmp/env_service_wait_host_test \
 *       tests/env_service_wait_host_test.c kernel/env_service.c kernel/env_ckpt.c
 *   /tmp/env_service_wait_host_test
 */
#include "kernel/cap.h"
#include "kernel/env_proto.h"
#include "kernel/env_service.h"
#include "kernel/env_ckpt.h"      /* P1a: the record the create path registers */
#include "tests/partition_host_stubs.h"  /* P1a quiesce: env_ckpt.c's partition_pause/_resume/_is_paused/_exists */
#include "kernel/checkpoint_mgr.h" /* P1a: checkpoint_last_sequence()'s prototype */
#include "kernel/process.h"       /* P1a: proc_table, where a sidecar's entry lives */

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
/* The tag of the last request the fake manager answered. Keyed on the TAG, not
 * a boolean: a create is now TWO round trips (ENV_CREATE, then ENV_REGISTER),
 * each with its own tag, and a one-shot flag would leave the second
 * unanswered — which is exactly the failure mode this test exists to catch in
 * the kernel, so it must not be built into the fake. */
static uint32_t g_answered_tag = 0;

/* P1a: the two sidecars the fake manager names, and where the kernel "released"
 * them — the entries the registration must NOT be able to supply itself. */
#define FAKE_RD_PID   0x51u
#define FAKE_PX_PID   0x52u
#define FAKE_RD_ENTRY 0x0000000040001000ull
#define FAKE_PX_ENTRY 0x0000000040002000ull
#define FAKE_SEQUENCE 7ull

/* Every request sent since the last reset_manager(), in order. A create is
 * two requests now (ENV_CREATE then ENV_REGISTER), so "the request" is no
 * longer a single slot: the clauses below name which one they mean. */
#define MAX_LOGGED 8
static uint8_t  g_log[MAX_LOGGED][ENV_REQ_MAX];
static uint32_t g_log_len[MAX_LOGGED];
static int      g_log_n = 0;

/* What the fake manager last created, so its ENV_REGISTER answer can name the
 * same environment the create named — the real manager keeps exactly this in
 * `struct Environment`, and the kernel now refuses a registration whose index
 * disagrees with the one the console was bound under. */
static uint32_t g_created_partition = 0;
static uint32_t g_created_index = 0;
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
    g_answered_tag = 0;
    g_inbox_len = 0;
    g_inbox_tag = 0;
    g_inbox_full = 0;
    g_log_n = 0;
    g_created_partition = 0;
    g_created_index = 0;
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

/* The registration the fake environment manager answers ENV_REGISTER with: a
 * planner's shape — two sidecars, three kind-labelled regions, four messenger
 * ends — exactly what user/init/src/env_manager.rs builds. The kernel's half of
 * it (each task's ENTRY) is deliberately absent here: the kernel must fill it
 * from proc_table, and a registration that could supply it would hide that. */
static void enqueue_register_reply(uint32_t env_id, uint32_t partition, uint32_t tag) {
    struct EnvCkptRegister r;
    memset(&r, 0, sizeof r);
    r.partition_id = partition;
    r.index        = g_created_index;
    r.env_id       = env_id;
    r.n_regions    = 3;
    r.regions[0].base = 0x0000000040000000ull; r.regions[0].frames = 1024;
    r.regions[0].kind = ENV_CKPT_REGION_POSIX_HEAP;
    r.regions[1].base = 0x0000000050000000ull; r.regions[1].frames = 64;
    r.regions[1].kind = ENV_CKPT_REGION_RD_HEAP;
    r.regions[2].base = 0x0000000060000000ull; r.regions[2].frames = 256;
    r.regions[2].kind = ENV_CKPT_REGION_RD_STORAGE;
    r.n_chans = 4;
    r.chans[0] = 11; r.chans[1] = 12; r.chans[2] = 13; r.chans[3] = 14;
    r.n_tasks = 2;
    memcpy(r.task_name[0], "drv.ramdisk.3", 14);
    r.task_kind[0] = ENV_CKPT_TASK_RAMDISK_SIDECAR;
    memcpy(r.task_name[1], "aerosls.posix.3", 16);
    r.task_kind[1] = ENV_CKPT_TASK_POSIX_SIDECAR;

    uint8_t f[ENV_REPLY_MAX];
    uint32_t len = ENV_FRAME_SIZE + ENV_REGISTER_REPLY_BODY_SIZE;
    env_frame_encode(f, ENV_REGISTER, 0);
    env_register_reply_encode(f + ENV_FRAME_SIZE, &r);
    memcpy(g_inbox, f, len);
    g_inbox_len = len;
    g_inbox_tag = tag;
    g_inbox_full = 1;
}

/* One step of the fake env manager. It runs from the yield stub because that
 * is when Ring-3 work actually runs on a unified boot (and never otherwise).
 * It answers the request it was just sent, whatever its type — a create is two
 * requests now, and a manager that answered only the first would be modelling
 * the bug, not the system. */
static void manager_step(void) {
    if (g_req_len == 0 || g_req_tag == g_answered_tag) return;  /* one reply per request */
    if (g_mode == MANAGER_DEAD) return;                        /* a wedged env manager */
    uint16_t ty = 0;
    if (env_frame_parse(g_req, g_req_len, &ty) && ty == ENV_REGISTER) {
        uint32_t eid = 0, part = 0;
        if (!env_register_body_parse(g_req + ENV_FRAME_SIZE,
                                     g_req_len - ENV_FRAME_SIZE, &eid, &part)) return;
        enqueue_register_reply(eid, part, g_req_tag);
    } else {
        /* Remember what the create asked for, the way the real manager keeps
         * it in `struct Environment`: its ENV_REGISTER answer has to name the
         * environment the create named. */
        if (env_frame_parse(g_req, g_req_len, &ty) && ty == ENV_CREATE &&
            g_req_len >= ENV_FRAME_SIZE + ENV_CREATE_BODY_SIZE) {
            g_created_partition = env_read_u32(g_req, ENV_FRAME_SIZE);
            g_created_index     = env_read_u32(g_req, ENV_FRAME_SIZE + 4);
        }
        enqueue_reply(ENV_ENV_ID, g_req_tag, g_mode == MANAGER_TRUNCATED);
    }
    g_answered_tag = g_req_tag;
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
    if (g_log_n < MAX_LOGGED) {
        memcpy(g_log[g_log_n], g_req, g_req_len);
        g_log_len[g_log_n] = g_req_len;
        g_log_n++;
    }
    return 0;
}

/* The nth request sent since the last reset, or NULL. */
static const uint8_t* logged_request(int n, uint32_t* out_len) {
    if (n < 0 || n >= g_log_n) return 0;
    if (out_len) *out_len = g_log_len[n];
    return g_log[n];
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

/* POSIX-Environments E6: env_service_create() binds the created environment's
 * console (kernel/env_console.c) once the manager's reply names it. This test
 * links the REAL kernel/env_service.c but not the console registry — its
 * subject is the create round trip's yield-and-deadline, and no console exists
 * in host context, so the binding is an inert stand-in. Its own behaviour is
 * covered by tests/env_console_host_test.c. */
int env_console_bind_env(uint32_t partition, uint32_t index, uint32_t env_id) {
    (void)partition; (void)index; (void)env_id;
    return 0;
}

/* ─── POSIX-Environments P1a: the register round trip's kernel seams ────────
 * The test links the REAL kernel/env_ckpt.c, so the record the create path
 * registers is a real record in the real table — the clauses below read it
 * back through env_ckpt_find(). Only the three sources it draws on are stubs:
 * the dirty mark (a no-op here; the bit is env_ckpt_host_test.c's subject),
 * the checkpoint epoch, and the process table.
 *
 * ckpt_mark_dirty(): env_ckpt.c calls it on every mutation. */
void ckpt_mark_dirty(uint32_t region) { (void)region; }

/* checkpoint_last_sequence(): the epoch a registration belongs to. A fixed
 * value so the record's `sequence` field can be asserted exactly. */
uint64_t checkpoint_last_sequence(void) { return FAKE_SEQUENCE; }

/* The sidecar registry: name + partition -> pid. Same signature as cap.c's, so
 * the real kernel and this stand-in are interchangeable at the call site. */
uint32_t sidecar_registry_resolve(const char* name, uint32_t partition_id) {
    (void)partition_id;
    if (strcmp(name, "drv.ramdisk.3") == 0) return FAKE_RD_PID;
    if (strcmp(name, "aerosls.posix.3") == 0) return FAKE_PX_PID;
    return 0;
}

/* The process table: where the kernel RELEASED each sidecar. Only the fields
 * env_service_task_entry() reads are set — a parked sidecar's resume RIP lives
 * in park_ctx, so `user_rip` stays the entry point cap_create_sidecar set. */
struct ProcessDescriptor proc_table[PROC_MAX];
static void seat_sidecars(void) {
    memset(proc_table, 0, sizeof proc_table);
    proc_table[0].pid = FAKE_RD_PID; proc_table[0].active = 1;
    proc_table[0].user_rip = FAKE_RD_ENTRY;
    proc_table[1].pid = FAKE_PX_PID; proc_table[1].active = 1;
    proc_table[1].user_rip = FAKE_PX_ENTRY;
}

/* ─── the test ──────────────────────────────────────────────────────────── */
int main(void) {
    uint16_t status = 0xFFFF;
    uint32_t env_id = 0;

    /* P1a: the two sidecars the fake manager will name exist in the process
     * table before anything asks for them, and the record table starts empty. */
    seat_sidecars();
    env_ckpt_reset();

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
        /* The create is the FIRST of the two requests a create now sends. */
        uint16_t ty = 0;
        uint32_t len = 0;
        const uint8_t* req = logged_request(0, &len);
        CHECK(req && env_frame_parse(req, len, &ty) && ty == ENV_CREATE &&
              env_read_u32(req, ENV_FRAME_SIZE) == 7 &&
              env_read_u32(req, ENV_FRAME_SIZE + 4) == 3,
              "the first request a create sends is the E4 create frame for "
              "(partition 7, index 3)");
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

    /* ── P1a 12-14: a create REGISTERS the environment's checkpoint record ──
     * The property is a property of the create: the one path that makes
     * environments is the one that puts them in env_ckpt_table[], so there is
     * no window in which an environment exists and is invisible to a
     * checkpoint. */
    reset_manager(MANAGER_ALIVE);
    status = 0xFFFF; env_id = 0;
    rc = env_service_create(7, 3, &status, &env_id);
    CHECK(rc == 0 && status == ENV_OK,
          "a create still answers as before with registration added");
    {
        uint16_t ty = 0;
        uint32_t len = 0;
        const uint8_t* req = logged_request(1, &len);
        uint32_t rid = 0, rpart = 0;
        CHECK(g_log_n == 2,
              "a create sends exactly one further request (the registration)");
        CHECK(req && env_frame_parse(req, len, &ty) && ty == ENV_REGISTER &&
              len == ENV_FRAME_SIZE + ENV_REGISTER_BODY_SIZE,
              "and it is an ENV_REGISTER, frame + an 8-byte body");
        CHECK(req && env_register_body_parse(req + ENV_FRAME_SIZE,
                                            len - ENV_FRAME_SIZE, &rid, &rpart) &&
              rid == env_id && rpart == 7,
              "the registration asks about the environment the CREATE named — "
              "the id from the reply and the partition from the request");
    }

    {
        const struct EnvCkptRecord* rec = env_ckpt_find(7, 3);
        CHECK(rec != 0,
              "the environment is now IN the checkpoint table, registered by the "
              "create that made it");
        if (rec) {
            CHECK(env_ckpt_valid(rec) == ENV_CKPT_REFUSE_NONE,
                  "and its record passes the gate every other path goes through");
            CHECK(rec->env_id == env_id && rec->index == 3 && rec->partition_id == 7,
                  "with the identity the create and the registration agreed on");
            CHECK(rec->sequence == FAKE_SEQUENCE,
                  "stamped with the checkpoint epoch the KERNEL read, not one init sent");
            CHECK(rec->n_regions == 3 && rec->n_chans == 4 && rec->n_tasks == 2,
                  "carrying the manager's three regions, four channels and two sidecars");
            CHECK(rec->regions[0].base == 0x40000000ull &&
                  rec->regions[0].kind == ENV_CKPT_REGION_POSIX_HEAP &&
                  rec->regions[2].kind == ENV_CKPT_REGION_RD_STORAGE,
                  "each region with the kind it was reported under");
            CHECK(rec->chans[0] == 11 && rec->chans[3] == 14,
                  "all four messenger endpoints crossed");
            CHECK(rec->tasks[0].entry == FAKE_RD_ENTRY &&
                  rec->tasks[1].entry == FAKE_PX_ENTRY,
                  "each task's ENTRY is the kernel's (the sidecar's user_rip), "
                  "resolved by the name init sent");
            CHECK(rec->tasks[0].entry != rec->regions[0].base &&
                  rec->tasks[0].entry != rec->regions[1].base,
                  "and is demonstrably not a region base the manager supplied");
        }
    }

    printf("\n%d checks passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
