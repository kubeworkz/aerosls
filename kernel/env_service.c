/* env_service.c — the kernel side of the environment-manager control channel
 * (POSIX-Environments E4). See env_service.h. */

#include "env_service.h"
#include "env_proto.h"
#include "cap.h"
#include "kernel_io.h"
#include "smp.h"

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

int env_service_create(uint32_t partition, uint32_t index,
                       uint16_t* out_status, uint32_t* out_env_id) {
    if (out_status) *out_status = ENV_ERR_INVAL;
    if (out_env_id) *out_env_id = 0;
    if (!g_env_ready) return -1;

    /* Drain any stale reply left by a previous request that timed out, so this
     * round trip cannot mistake it for its own answer (single-flight channel). */
    {
        uint8_t stale[ENV_REPLY_MAX];
        uint32_t sp = 0, stag = 0, sfl = 0; uint16_t sn = 0;
        while (cap_recv_msg(0, g_env_k_rd, stale, sizeof(stale), &sp,
                            0, 0, &sn, &stag, &sfl) == 0) { /* drop */ }
    }

    uint8_t req[ENV_FRAME_SIZE + ENV_CREATE_BODY_SIZE];
    env_frame_encode(req, ENV_CREATE, 0);
    env_create_body_encode(req + ENV_FRAME_SIZE, partition, index);

    uint32_t tag = g_env_tag++;
    if (cap_send_msg(0, g_env_k_wr, req, (uint32_t)sizeof(req), 0, 0, tag, 0) != 0)
        return -1;

    /* Spin for the reply. The send woke init; it runs under timer preemption
     * (interrupts stay enabled here), processes the request, and replies. The
     * deadline keeps a stuck env manager from blocking the HTTP server. */
    uint64_t deadline = kernel_tick_counter + ENV_CREATE_TIMEOUT_TICKS;
    while ((int64_t)(kernel_tick_counter - deadline) < 0) {
        uint8_t reply[ENV_REPLY_MAX];
        uint32_t plen = 0, rtag = 0, flags = 0;
        uint16_t n_caps = 0;
        int r = cap_recv_msg(0, g_env_k_rd, reply, (uint32_t)sizeof(reply),
                             &plen, 0, 0, &n_caps, &rtag, &flags);
        if (r == 0 && plen >= (ENV_FRAME_SIZE + ENV_REPLY_BODY_SIZE)) {
            uint16_t ty = 0;
            if (env_frame_parse(reply, plen, &ty)) {
                env_reply_body_parse(reply + ENV_FRAME_SIZE,
                                     out_status, out_env_id, 0);
                return 0;
            }
        }
        /* Keep the kernel's periodic service work ticking while we wait, and
         * yield the pipeline so the preempting scheduler can run init. */
        smp_uniprocessor_tick();
        __asm__ volatile("pause");
    }
    return -1;   /* init did not reply before the deadline */
}
