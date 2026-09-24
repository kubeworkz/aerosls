/* env_service.c — the kernel side of the environment-manager control channel
 * (POSIX-Environments E4). See env_service.h. */

#include "env_service.h"
#include "env_console.h"
#include "env_proto.h"
#include "cap.h"
#include "kernel_io.h"
#include "smp.h"
#include "process.h"   /* E1: kernel_yield_to_ring3 (the unified boot's hand-over) */

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

/* One ENV round trip: send `ty` with `body`, then wait for the reply. The
 * create and destroy paths differ only in the opcode and the body they carry,
 * so they share this — including the stale-reply drain and the bounded wait
 * whose yield is what lets init run at all (E4; see env_service.h). */
static int env_service_rpc(uint16_t ty, const uint8_t* body, uint32_t body_len,
                           uint16_t* out_status, uint32_t* out_env_id,
                           uint32_t* out_partition) {
    if (out_status) *out_status = ENV_ERR_INVAL;
    if (out_env_id) *out_env_id = 0;
    if (out_partition) *out_partition = 0;
    if (!g_env_ready) return -1;

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
        int r = cap_recv_msg(0, g_env_k_rd, reply, (uint32_t)sizeof(reply),
                             &plen, 0, 0, &n_caps, &rtag, &flags);
        if (r == 0 && plen >= (ENV_FRAME_SIZE + ENV_REPLY_BODY_SIZE)) {
            uint16_t rty = 0;
            if (env_frame_parse(reply, plen, &rty)) {
                env_reply_body_parse(reply + ENV_FRAME_SIZE,
                                     out_status, out_env_id, out_partition);
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
        if (!env_console_bind_env(partition, index, *out_env_id)) {
            kernel_serial_printf(
                "[ENV] environment id %u (partition %u, index %u) has no "
                "console to bind — attach will answer no such environment\n",
                (unsigned)*out_env_id, (unsigned)partition, (unsigned)index);
        }
    }
    return rc;
}

int env_service_destroy(uint32_t env_id, uint32_t partition,
                        uint16_t* out_status, uint32_t* out_env_id,
                        uint32_t* out_partition) {
    uint8_t body[ENV_DESTROY_BODY_SIZE];
    env_destroy_body_encode(body, env_id, partition);
    return env_service_rpc(ENV_DESTROY, body, (uint32_t)sizeof(body),
                           out_status, out_env_id, out_partition);
}
