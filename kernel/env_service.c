/* env_service.c — the kernel side of the environment-manager control channel
 * (POSIX-Environments E4). See env_service.h. */

#include "env_service.h"
#include "env_console.h"
#include "env_proto.h"
#include "env_ckpt.h"
#include "cap.h"
#include "kernel_io.h"
#include "smp.h"
#include "process.h"   /* E1: kernel_yield_to_ring3 (the unified boot's hand-over) */
#include "checkpoint_mgr.h"   /* P1a: the checkpoint sequence a registration belongs to */
#include "env_payload.h"     /* P1a: the payload pour that follows a replay */

extern volatile uint64_t kernel_tick_counter;   /* ~10 ms per tick (net_event.c) */

#define ENV_SLOT_NONE 0xFFFFu
/* A generous wall-clock deadline: creating an environment has init allocate
 * three frame-pool regions and create two sidecars (each copies a shared
 * image), so it is not instantaneous — but a stuck/dead env manager must never
 * hang the HTTP server, so the round trip is bounded. ~10 ms/tick → ~3 s. */
#define ENV_CREATE_TIMEOUT_TICKS 300u

static uint16_t g_env_k_rd = ENV_SLOT_NONE;
static uint16_t g_env_k_wr = ENV_SLOT_NONE;
static int      g_env_ready = 0;
static uint32_t g_env_tag = 1;

void env_service_register(uint16_t kernel_chan_r, uint16_t kernel_chan_w) {
    g_env_k_rd = kernel_chan_r;
    g_env_k_wr = kernel_chan_w;
    g_env_ready = 1;
    kernel_serial_printf(
        "[ENV] control channel wired: kernel ends rd=%u wr=%u\n",
        (unsigned)kernel_chan_r, (unsigned)kernel_chan_w);
}

int env_service_reply_slot(uint16_t slot) {
    return g_env_ready && slot == g_env_k_rd;
}

/* One ENV round trip, the raw form: send `ty` with `body`, wait for the reply,
 * and hand the caller the reply's bytes (frame included). Create, destroy and
 * register differ only in the opcode, the body they carry and how the reply is
 * read, so the stale-reply drain and the bounded wait whose yield is what lets
 * init run at all (E4; see env_service.h) live here once. */
static int env_service_rpc_raw(uint16_t ty, const uint8_t* body, uint32_t body_len,
                               uint8_t* out, uint32_t out_cap, uint32_t* out_len) {
    if (out_len) *out_len = 0;
    if (!g_env_ready || !out || out_cap < ENV_FRAME_SIZE) return -1;

    /* Drain any stale reply left by a previous request that timed out, so this
     * round trip cannot mistake it for its own answer (single-flight channel). */
    {
        uint8_t stale[ENV_REPLY_MAX];
        uint32_t sp = 0, stag = 0, sfl = 0; uint16_t sn = 0;
        while (cap_recv_msg(0, g_env_k_rd, stale, sizeof(stale), &sp,
                            0, 0, &sn, &stag, &sfl) == 0) { /* drop */ }
    }

    uint8_t req[ENV_REQ_MAX];
    if (ENV_FRAME_SIZE + body_len > sizeof(req)) return -1;
    env_frame_encode(req, ty, 0);
    for (uint32_t i = 0; i < body_len; i++) {
        req[ENV_FRAME_SIZE + i] = body[i];
    }

    uint32_t tag = g_env_tag++;
    if (cap_send_msg(0, g_env_k_wr, req, ENV_FRAME_SIZE + body_len, 0, 0, tag, 0) != 0)
        return -1;

    /* Poll for the reply, handing the CPU to Ring-3 while we wait (below). The
     * send woke init; the yield lets it run, process the request, and reply.
     * The deadline keeps a stuck env manager from blocking the HTTP server. */
    uint64_t deadline = kernel_tick_counter + ENV_CREATE_TIMEOUT_TICKS;
    while ((int64_t)(kernel_tick_counter - deadline) < 0) {
        uint8_t reply[ENV_REPLY_MAX];
        uint32_t plen = 0, rtag = 0, flags = 0;
        uint16_t n_caps = 0;
        uint32_t cap = out_cap < (uint32_t)sizeof(reply) ? out_cap : (uint32_t)sizeof(reply);
        int r = cap_recv_msg(0, g_env_k_rd, reply, cap,
                             &plen, 0, 0, &n_caps, &rtag, &flags);
        if (r == 0 && plen >= ENV_FRAME_SIZE) {
            uint16_t rty = 0;
            if (env_frame_parse(reply, plen, &rty)) {
                // The channel is single-flight (one request in flight, drained
                // above), so the frame that parses IS this request's answer;
                // how much of it is usable is the caller's question.
                if (plen > out_cap) return -1;
                for (uint32_t i = 0; i < plen; i++) out[i] = reply[i];
                if (out_len) *out_len = plen;
                return 0;
            }
        }
        /* Keep the kernel's periodic service work ticking while we wait, then
         * hand the CPU to Ring-3.
         *
         * POSIX-Environments E1: the unified boot's Ring-0 path never
         * schedules — Ring-3 work runs only while the control plane yields —
         * and smp_uniprocessor_tick() is a no-op once an AP is online, so
         * without this call the spin below is a hole in the cooperative
         * schedule: init cannot even read the request just queued, the
         * deadline expires, and its reply arrives after we stopped looking.
         * That is exactly what the deferred E4 boot check found (6.25 M polls,
         * 0 messages received, with init's own trace printing `req recv'd /
         * reply built / send OK` only after the deadline line). Ring-3 answers
         * within a few budgets when it is alive; a dead env manager never
         * answers, which is what the deadline is for. Same call the shell and
         * HTTP loops' idle points make, and a no-op on every non-unified
         * boot. */
        smp_uniprocessor_tick();
        kernel_yield_to_ring3(PROC_CONTROL_PLANE_BUDGET_TICKS);
        __asm__ volatile("pause");
    }
    return -1;   /* init did not reply before the deadline */
}

/* The uniform-reply form: the create and destroy paths both answer with
 * { status, pad, env_id, partition }. A reply too short to carry that body is
 * not an answer, exactly as before -- the raw form already bounded the wait. */
static int env_service_rpc(uint16_t ty, const uint8_t* body, uint32_t body_len,
                           uint16_t* out_status, uint32_t* out_env_id,
                           uint32_t* out_partition) {
    if (out_status) *out_status = ENV_ERR_INVAL;
    if (out_env_id) *out_env_id = 0;
    if (out_partition) *out_partition = 0;

    uint8_t reply[ENV_REPLY_MAX];
    uint32_t plen = 0;
    if (env_service_rpc_raw(ty, body, body_len, reply, (uint32_t)sizeof(reply), &plen) != 0)
        return -1;
    if (plen < (ENV_FRAME_SIZE + ENV_REPLY_BODY_SIZE)) return -1;
    env_reply_body_parse(reply + ENV_FRAME_SIZE, out_status, out_env_id, out_partition);
    return 0;
}

int env_service_create(uint32_t partition, uint32_t index,
                       uint16_t* out_status, uint32_t* out_env_id) {
    uint8_t body[ENV_CREATE_BODY_SIZE];
    env_create_body_encode(body, partition, index);
    int rc = env_service_rpc(ENV_CREATE, body, (uint32_t)sizeof(body),
                             out_status, out_env_id, 0);
    /* POSIX-Environments E6: the environment's POSIX sidecar — and so its
     * console, registered by cap.c from the sidecar's own name — was created
     * INSIDE the round trip above, and the id that names it in the control
     * plane only arrives with this reply. Completing the mapping here is what
     * lets attach address a console by (partition, env_id), the same pair
     * create and destroy speak, instead of exposing the internal index. A
     * refusal to bind is not fatal to the create: the environment exists, and
     * env_service.h's contract is about the create, not the console. */
    if (rc == 0 && out_status && *out_status == ENV_OK &&
        out_env_id && *out_env_id) {
        /* E6: the environment's POSIX sidecar — and so its console, registered
         * by cap.c from the sidecar's own name — was created INSIDE the round
         * trip above, and the id that names it in the control plane only
         * arrives with this reply. Completing the mapping here is what lets
         * attach address a console by (partition, env_id), the same pair create
         * and destroy speak, instead of exposing the internal index. A refusal
         * to bind is not fatal to the create: the environment exists, and
         * env_service.h's contract is about the create, not the console.
         *
         * P1a: the binding is ALSO the checkpoint record's `console_id` -- the
         * console registry's key within the partition, 0 when there is no
         * console -- so it is computed here and handed on rather than looked up
         * twice. Then the environment manager is asked for its half of the
         * record and the record is stored. A refused registration is likewise
         * not a refused create (the environment exists), but it IS said out
         * loud: an environment that is live and absent from the checkpoint is
         * precisely the silent vacuity this phase exists to remove. */
        uint32_t console_id = 0;
        if (env_console_bind_env(partition, index, *out_env_id)) {
            console_id = index;
        } else {
            kernel_serial_printf(
                "[ENV] environment id %u (partition %u, index %u) has no "
                "console to bind — attach will answer no such environment\n",
                (unsigned)*out_env_id, (unsigned)partition, (unsigned)index);
        }
        (void)env_service_register_env(partition, index, *out_env_id, console_id);
    }
    return rc;
}

int env_service_destroy(uint32_t env_id, uint32_t partition,
                        uint16_t* out_status, uint32_t* out_env_id,
                        uint32_t* out_partition) {
    uint8_t body[ENV_DESTROY_BODY_SIZE];
    env_destroy_body_encode(body, env_id, partition);
    int rc = env_service_rpc(ENV_DESTROY, body, (uint32_t)sizeof(body),
                             out_status, out_env_id, out_partition);
    /* P1a: a destroyed environment's checkpoint record must not outlive it.
     * Without this the next restore resurrects an environment an operator
     * removed -- the stale-versus-absent sin this phase exists to police. Only
     * on a destroy the manager actually acknowledged: a failed destroy leaves
     * the environment live, so its record is still the truth about it. */
    if (rc == 0 && out_status && *out_status == ENV_OK) {
        if (env_ckpt_drop_env(partition, env_id) == 0) {
            kernel_serial_printf(
                "[ENV] checkpoint record for env %u (partition %u) dropped"
                " with its environment\n",
                (unsigned)env_id, (unsigned)partition);
        }
    }
    return rc;
}

/* P1a: where the kernel put the sidecar `name` registered under in `partition`
 * -- its entry point, read off the process descriptor cap_create_sidecar
 * populated (`user_rip = image_vbase + image_entry`, set at create and never
 * clobbered afterwards; a park saves the resume RIP in park_ctx, not here).
 * 0 when the name does not resolve, which the record layer refuses rather than
 * storing a task no restore could place. */
static uint64_t env_service_task_entry(const char* name, uint32_t partition) {
    uint32_t pid = sidecar_registry_resolve(name, partition);
    if (pid == 0) return 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid)
            return proc_table[i].user_rip;
    }
    return 0;
}

int env_service_register_env(uint32_t partition, uint32_t index,
                             uint32_t env_id, uint32_t console_id) {
    uint8_t body[ENV_REGISTER_BODY_SIZE];
    env_register_body_encode(body, env_id, partition);

    uint8_t reply[ENV_REPLY_MAX];
    uint32_t plen = 0;
    if (env_service_rpc_raw(ENV_REGISTER, body, (uint32_t)sizeof(body),
                            reply, (uint32_t)sizeof(reply), &plen) != 0) {
        kernel_serial_printf(
            "[ENV] env %u (partition %u, index %u): no ENV_REGISTER reply -- "
            "the environment is live and NOT checkpointable\n",
            (unsigned)env_id, (unsigned)partition, (unsigned)index);
        return -1;
    }
    if (plen < (ENV_FRAME_SIZE + ENV_REGISTER_REPLY_BODY_SIZE)) {
        kernel_serial_printf(
            "[ENV] env %u (partition %u): ENV_REGISTER reply carries %u bytes, "
            "short of the %u-byte registration -- refused, not half-applied\n",
            (unsigned)env_id, (unsigned)partition, (unsigned)plen,
            (unsigned)(ENV_FRAME_SIZE + ENV_REGISTER_REPLY_BODY_SIZE));
        return -1;
    }

    struct EnvCkptRegister reg;
    if (!env_register_reply_parse(reply + ENV_FRAME_SIZE,
                                  plen - ENV_FRAME_SIZE, &reg)) {
        kernel_serial_printf(
            "[ENV] env %u (partition %u): ENV_REGISTER reply body did not parse\n",
            (unsigned)env_id, (unsigned)partition);
        return -1;
    }

    /* The reply must be about the environment the REQUEST named, under the same
     * index the create used. The channel is single-flight and its stale answers
     * are drained, so this is belt-and-braces -- but the failure it prevents is
     * filing a record under the wrong identity, and `index` is the half of that
     * identity the sidecar names are built from (E2's partition-scoped
     * registry), so a registration that disagreed about it would name sidecars
     * that belong to a different environment. A restore would re-register
     * those. Refused, not stored. */
    if (reg.partition_id != partition || reg.env_id != env_id ||
        reg.index != index) {
        kernel_serial_printf(
            "[ENV] ENV_REGISTER reply names env %u (partition %u, index %u), not "
            "the requested env %u (partition %u, index %u) -- refused\n",
            (unsigned)reg.env_id, (unsigned)reg.partition_id, (unsigned)reg.index,
            (unsigned)env_id, (unsigned)partition, (unsigned)index);
        return -1;
    }

    /* The kernel's half of the task list: where it actually released each named
     * sidecar. Done here, not in init, because only the kernel knows it -- and
     * only for the tasks the manager named, so an unasked-for slot stays 0. */
    uint64_t entries[ENV_CKPT_MAX_TASKS];
    for (uint32_t i = 0; i < ENV_CKPT_MAX_TASKS; i++) {
        entries[i] = (i < reg.n_tasks)
                   ? env_service_task_entry(reg.task_name[i], partition) : 0;
    }

    int rc = env_ckpt_register_from(&reg, entries, console_id,
                                    checkpoint_last_sequence());
    if (rc != 0) {
        struct EnvCkptRefusal ref = env_ckpt_last_refusal();
        char why[96];
        env_ckpt_refusal_text(&ref, why, (uint32_t)sizeof why);
        kernel_serial_printf(
            "[ENV] checkpoint registration for env %u (partition %u, index %u) "
            "REFUSED (%d): %s -- the environment is live and NOT checkpointed\n",
            (unsigned)env_id, (unsigned)partition, (unsigned)index,
            (int)rc, why);
        return -1;
    }
    kernel_serial_printf(
        "[ENV] checkpoint record registered: env %u (partition %u, index %u) "
        "regions=%u chans=%u tasks=%u console=%u\n",
        (unsigned)env_id, (unsigned)partition, (unsigned)index,
        (unsigned)reg.n_regions, (unsigned)reg.n_chans, (unsigned)reg.n_tasks,
        (unsigned)console_id);
    return 0;
}

// ─── P1a: restore-through-create (see env_service.h for the contract) ────────
// Every record this boot adopted is replayed through env_service_create(), the
// same round trip the HTTP create route makes. The pause dance is the record
// layer's (env_ckpt_restore_resume_for_create/_repause) so all the
// partition-interaction stays in one translation unit; this function owns the
// order, which is the part that cannot be delegated:
//
//   resume (if the snapshot left it paused) -> create -> identity check
//   -> settle or refuse -> ... -> repause the ledger
//
// and no `return` may appear between the first of those and the repause. A
// pass that bailed out there would leave a partition the snapshot had frozen
// RUNNING, which is the restore side of the same failure the capture's quiesce
// interval refuses (v0.2 §4's `leak-unpause` tooth).
/* P1a payload: whether this pass pours the captured contents back after
 * each replay. The restore route sets it on EVERY invocation (no sticky
 * state across requests); every other caller sees the default, on. */
static int es_restore_payload = 1;

void env_service_set_restore_payload(int on) {
    es_restore_payload = on ? 1 : 0;
}

int env_service_restore_pending(struct EnvSvcRestoreReport* out) {
    if (!out) return -1;
    out->n_pending   = env_ckpt_restore_pending();
    out->n_replayed  = 0;
    out->n_refused   = 0;
    out->n_remaining = out->n_pending;
    out->n_resumed   = 0;
    out->n_repaused  = 0;
    out->n_payloads      = 0;
    out->n_payload_skips = 0;

    if (out->n_pending == 0) return 0;   /* nothing to replay is not a failure */
    // No manager, no attempt: refusing every record because init is not up yet
    // would turn "not yet" into "never", and the records are still the truth.
    if (!g_env_ready) return -1;

    uint32_t refused_before = env_ckpt_restore_refused();

    /* What this pass replayed, so the payload pour below can run AFTER the
     * repause — the pour must not yield, and yielding is only illegal from
     * there on (env_payload.h's rule). */
    struct { uint32_t p, idx, env_id; uint64_t seq; } done[ENV_CKPT_MAX];
    uint32_t n_done = 0;

    while (env_ckpt_restore_pending() > 0) {
        uint32_t p = 0, idx = 0, old_id = 0;
        if (!env_ckpt_restore_next(&p, &idx, &old_id)) break;

        // The record the create is about to overwrite (env_ckpt_register_from()
        // upserts on (partition, index), which is the point -- the replay
        // refreshes the record rather than keeping a stale one).
        const struct EnvCkptRecord* want = env_ckpt_find(p, idx);
        if (!want) {
            // Unreachable while the pending set is kept in step with the table
            // (reset/drop/adopt all maintain both, and nothing in this loop can
            // remove a record). Kept as a stop rather than a `continue`: a
            // record that is not there cannot be replayed, and a pass that
            // spun on it forever would be worse than one that stopped with the
            // record still pending.
            break;
        }
        struct EnvCkptRecord w = *want;

        int can = env_ckpt_restore_resume_for_create(p);
        if (can < 0) {
            env_ckpt_restore_refuse(p, idx, ENV_CKPT_REFUSE_NO_PARTITION, idx, p);
            continue;
        }

        uint16_t status = ENV_ERR_INVAL;
        uint32_t new_id = 0;
        int rc = env_service_create(p, idx, &status, &new_id);
        if (rc != 0) {
            env_ckpt_restore_refuse(p, idx, ENV_CKPT_REFUSE_CREATE, idx, 0);
            continue;
        }
        if (status != ENV_OK) {
            env_ckpt_restore_refuse(p, idx, ENV_CKPT_REFUSE_CREATE, idx, status);
            continue;
        }

        if (!env_ckpt_restore_same_identity(&w, env_ckpt_find(p, idx))) {
            // Refusal over partial application, applied to the replay: the
            // create made SOMETHING and it is not the environment that was
            // written down, so it is ended rather than left running as a
            // half-restore nobody could account for. The refusal is recorded
            // first, so the destroy's own record bookkeeping finds nothing
            // pending to undo.
            env_ckpt_restore_refuse(p, idx, ENV_CKPT_REFUSE_IDENTITY, idx, new_id);
            uint16_t dst = ENV_ERR_INVAL;
            uint32_t d_id = 0, d_part = 0;
            (void)env_service_destroy(new_id, p, &dst, &d_id, &d_part);
            kernel_serial_printf(
                "[ENV_RESTORE] record (partition %u, index %u) came back as a "
                "different environment (env %u) -- refused, and that "
                "environment ended\n",
                (unsigned)p, (unsigned)idx, (unsigned)new_id);
            continue;
        }

        env_ckpt_restore_settle(p, idx);
        out->n_replayed++;
        /* P1a payload: wait (bounded, yielding — legal here, inside the
         * resume/repause interval) until this environment's sidecars have
         * run to their parked idle point, which is the only posture the
         * pour can restore into. A timeout is not a failure of the replay:
         * the pour re-checks and refuses BY NAME if they never got there. */
        if (es_restore_payload)
            (void)env_payload_wait_parked(&w);
        if (n_done < ENV_CKPT_MAX) {
            done[n_done].p = p;      done[n_done].idx = idx;
            done[n_done].env_id = new_id; done[n_done].seq = w.sequence;
            n_done++;
        }
        kernel_serial_printf(
            "[ENV_RESTORE] record (partition %u, index %u) replayed through "
            "create: env %u -> %u, identity intact\n",
            (unsigned)p, (unsigned)idx, (unsigned)old_id, (unsigned)new_id);
    }

    // Put back exactly the pauses this pass stepped out of -- never "every
    // partition that has a record" -- and clear the ledger. On every path above
    // `continue` was used, never `return`: see the comment at the top.
    out->n_resumed   = env_ckpt_restore_resumed();
    out->n_repaused  = env_ckpt_restore_repause();

    /* P1a payload: pour the captured contents into every environment this
     * pass replayed, now that each partition is back in the state the
     * snapshot recorded. Straight-line and NO yield from here to the last
     * memcpy (env_payload.h's rule): Ring-3 runs only while the control
     * plane yields, so nothing can execute over the memory being poured. */
    for (uint32_t d = 0; d < n_done; d++) {
        if (!es_restore_payload) {
            out->n_payload_skips++;
            kernel_serial_printf(
                "[ENV_PAYLOAD] restore asked for metadata only (partition %u, "
                "index %u): the replay stands, the contents are withheld\n",
                (unsigned)done[d].p, (unsigned)done[d].idx);
            continue;
        }
        uint32_t pages = 0, cons = 0;
        if (env_payload_restore(done[d].p, done[d].idx, done[d].env_id,
                                done[d].seq, &pages, &cons) == 0) {
            out->n_payloads++;
        } else {
            out->n_payload_skips++;
            char why[96];
            struct EnvPayloadRefusal pr = env_payload_last_refusal();
            env_payload_refusal_text(&pr, why, (uint32_t)sizeof(why));
            kernel_serial_printf(
                "[ENV_PAYLOAD] contents not restored for (partition %u, "
                "index %u): %s -- the replay stands; contents are not claimed\n",
                (unsigned)done[d].p, (unsigned)done[d].idx, why);
        }
    }

    out->n_remaining = env_ckpt_restore_pending();
    out->n_refused   = env_ckpt_restore_refused() - refused_before;

    if (out->n_refused > 0) {
        char why[96];
        struct EnvCkptRefusal r = env_ckpt_last_refusal();
        env_ckpt_refusal_text(&r, why, (uint32_t)sizeof(why));
        kernel_serial_printf("[ENV_RESTORE] last refusal: %s.\n", why);
    }
    kernel_serial_printf(
        "[ENV_RESTORE] %u pending, %u replayed, %u refused, %u still pending "
        "(%u partition(s) resumed for a create, %u re-paused).\n",
        (unsigned)out->n_pending, (unsigned)out->n_replayed,
        (unsigned)out->n_refused, (unsigned)out->n_remaining,
        (unsigned)out->n_resumed, (unsigned)out->n_repaused);
    return 0;
}
